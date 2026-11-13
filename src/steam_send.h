// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "relaywire.h"
#include <enet/enet.h>
#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace g3t::steam::send {
constexpr int Ok = 1, Timeout = 16, LimitExceeded = 25, Ignored = 41;
constexpr int Reliable = 8, NoNagle = 1, NoDelay = 4, AutoRestart = 32;
enum class Priority : unsigned { Critical, Definition, Pose, Bulk, Count };
constexpr unsigned Classes = static_cast<unsigned>(Priority::Count);
constexpr uint32_t MaxUnreliablePoseAgeMs = 250;
constexpr size_t MaxUnreliablePoseBytes = 128 * 1024;

inline bool Retryable(int result) { return result == LimitExceeded || result == Ignored; }

inline std::string ExtractNpcPoseKey(uint8_t const* payload, size_t size)
{
    if (!payload || size < 31) return {};
    const uint8_t type = payload[0];
    // NpcsHeader is 23 bytes: MsgPosedNpc(39) and MsgCompactPosedNpc(44) follow with NpcRecord::guid[16].
    if ((type == 39 || type == 44) && size >= 23 + 16) {
        std::string key(1, 'G');
        key.append(reinterpret_cast<char const*>(payload + 23), 16);
        return key;
    }
    // MsgControlPose(49): NpcsHeader(23) + controlBytes(2) + generation(4) + WireControl::id(2).
    if (type == 49 && size >= 23 + 2 + 4 + 2) {
        std::string key(1, 'C');
        key.append(reinterpret_cast<char const*>(payload + 25), 6);
        return key;
    }
    return {};
}

class Codec {
    void* context_ = enet_range_coder_create();
public:
    Codec() = default;
    ~Codec() { if (context_) enet_range_coder_destroy(context_); }
    Codec(Codec const&) = delete;
    Codec& operator=(Codec const&) = delete;
    bool Ready() const { return context_ != nullptr; }

    bool Decode(void const* packet, size_t size, std::vector<uint8_t>& plain, EnetPacketParse* result = nullptr)
    {
        if (result) *result = {};
        if (!context_ || !packet || size < 2 || size > RelayPayloadLimit) return false;
        auto const* bytes = static_cast<uint8_t const*>(packet);
        const size_t header = bytes[0] & 0x80u ? 4 : 2;
        if (size <= header) return false;
        plain.assign(bytes, bytes + header);
        if (bytes[0] & 0x40u) {
            plain.resize(RelayPayloadLimit);
            const size_t decoded = enet_range_coder_decompress(context_, bytes + header, size - header,
                                                               plain.data() + header, plain.size() - header);
            if (!decoded) return false;
            plain.resize(header + decoded);
            plain[0] &= ~0x40u;
        } else {
            plain.insert(plain.end(), bytes + header, bytes + size);
        }
        EnetPacketParse parsed;
        const bool valid = ParseEnetDataPacket(plain.data(), plain.size(), &parsed) && !parsed.compressed;
        if (result) *result = parsed;
        return valid;
    }

    void Encode(std::vector<uint8_t>& packet)
    {
        const size_t header = packet[0] & 0x80u ? 4 : 2;
        std::vector<uint8_t> packed(packet.size());
        ENetBuffer buffer{packet.size() - header, packet.data() + header};
        const size_t size = enet_range_coder_compress(context_, &buffer, 1, buffer.dataLength,
                                                     packed.data() + header, buffer.dataLength);
        if (!size || size >= buffer.dataLength) return;
        std::memcpy(packed.data(), packet.data(), header);
        packed[0] |= 0x40u;
        packed.resize(header + size);
        packet.swap(packed);
    }
};

struct Item {
    std::vector<uint8_t> bytes;
    std::vector<std::string> retransmissions;
    uint32_t at = 0;
    bool reliable = false;
    unsigned playerStates = 0;
    Priority priority = Priority::Critical;
    std::string npcKey;

    Item() = default;
    Item(std::vector<uint8_t> b, std::vector<std::string> r, uint32_t timestamp,
         bool isReliable, unsigned states, Priority prio = Priority::Critical, std::string key = {})
        : bytes(std::move(b)), retransmissions(std::move(r)), at(timestamp),
          reliable(isReliable), playerStates(states), priority(prio), npcKey(std::move(key)) {}

    uint16_t Lane() const { return static_cast<uint16_t>(priority); }
    int Flags() const
    {
        // ENet already owns ACKs/retransmission. Steam reliable would queue a
        // second copy and impose ordering across ENet's independent channels.
        // Retain NoDelay refusals here so priority stays under our control.
        return AutoRestart | NoDelay | NoNagle;
    }
};

struct Counters {
    uint64_t accepted[Classes] = {}, bytes[Classes] = {};
    uint64_t failed[128] = {};
    uint64_t reliableAccepted = 0, unreliableAccepted = 0;
    uint64_t reliableFailed = 0, unreliableFailed = 0;
    uint64_t retries = 0, duplicates = 0, plainBytes = 0, wireBytes = 0;
    uint64_t playerStates = 0;
    uint64_t unclassified = 0;
    uint64_t npcUpdatesDropped = 0;
    size_t peakDepth = 0, peakBytes = 0;
    uint32_t peakAge = 0;
};

class Queue {
    std::array<std::deque<Item>, Classes * 2> queues_;
    std::unordered_set<std::string> pendingReliable_;
    size_t bytes_ = 0, depth_ = 0, poseUnreliableBytes_ = 0;

    void PopFront(unsigned index)
    {
        auto& q = queues_[index];
        if (q.empty()) return;
        auto const& item = q.front();
        bytes_ -= item.bytes.size();
        if (index == static_cast<unsigned>(Priority::Pose) * 2 + 1)
            poseUnreliableBytes_ -= item.bytes.size();
        --depth_;
        for (auto const& key : item.retransmissions) pendingReliable_.erase(key);
        q.pop_front();
    }
public:
    Counters counters;
    size_t Bytes() const { return bytes_; }
    size_t Depth() const { return depth_; }
    uint32_t OldestAge(uint32_t now) const
    {
        uint32_t age = 0;
        for (auto const& q : queues_)
            if (!q.empty()) age = (std::max)(age, now - q.front().at);
        return age;
    }

    void EvictStaleUnreliablePoses(uint32_t now, size_t incomingBytes = 0)
    {
        constexpr unsigned poseQueueIdx = static_cast<unsigned>(Priority::Pose) * 2 + 1;
        auto& q = queues_[poseQueueIdx];
        while (!q.empty()) {
            const uint32_t age = now - q.front().at;
            const bool tooOld = age > MaxUnreliablePoseAgeMs;
            const bool overCap = poseUnreliableBytes_ + incomingBytes > MaxUnreliablePoseBytes;
            if (!tooOld && !overCap) break;
            PopFront(poseQueueIdx);
            ++counters.npcUpdatesDropped;
        }
    }

    bool Push(Codec& codec, void const* packet, size_t size, uint32_t now, EnetPacketParse* result = nullptr)
    {
        if (result) *result = {};
        if (!packet || !size || size > RelayPayloadLimit) return false;
        std::vector<uint8_t> plain;
        if (!codec.Decode(packet, size, plain, result)) {
            // Classification is not transport validation. Keep unknown datagrams
            // intact and let the receiving ENet endpoint validate them.
            auto const* bytes = static_cast<uint8_t const*>(packet);
            Item item;
            item.bytes.assign(bytes, bytes + size);
            item.at = now;
            item.priority = Priority::Bulk;
            queues_[static_cast<unsigned>(Priority::Bulk) * 2 + 1].push_back(std::move(item));
            bytes_ += size;
            ++depth_;
            ++counters.unclassified;
            counters.plainBytes += size;
            counters.wireBytes += size;
            counters.peakDepth = (std::max)(counters.peakDepth, depth_);
            counters.peakBytes = (std::max)(counters.peakBytes, bytes_);
            counters.peakAge = (std::max)(counters.peakAge, OldestAge(now));
            return true;
        }
        const size_t header = plain[0] & 0x80u ? 4 : 2;
        std::array<std::vector<uint8_t>, Classes * 2> groups;
        std::array<std::vector<std::string>, Classes * 2> retransmissions;
        std::array<std::string, Classes * 2> npcKeys;
        std::array<unsigned, Classes * 2> commandCounts{};
        std::array<unsigned, Classes * 2> playerStates{};
        for (size_t at = header; at < plain.size();) {
            const unsigned command = plain[at] & 15u;
            const unsigned channel = plain[at + 1];
            const size_t commandHeader = EnetCommandHeaderSize(command);
            size_t length = commandHeader;
            if (command == 6) length += RelayReadUint16(plain.data() + at + 4);
            if (command == 7 || command == 8 || command == 9 || command == 12)
                length += RelayReadUint16(plain.data() + at + 6);
            const bool reliable = (plain[at] & 0x80u) != 0 || command == 1;
            Priority priority = Priority::Critical;
            if (channel == 6 && command >= 6) priority = Priority::Bulk;
            else if (command >= 6 && (channel == 4 || channel == 5 || (channel == 2 && !reliable)))
                priority = Priority::Pose;
            else if (channel == 7 && command >= 6)
                priority = Priority::Definition;
            const unsigned groupId = static_cast<unsigned>(priority) * 2 + (reliable ? 0 : 1);
            if (reliable) {
                // An ENet retry may be packed beside different commands.
                // Match the command, not that incidental datagram grouping.
                std::string key(reinterpret_cast<char const*>(plain.data()), 2);
                key.append(reinterpret_cast<char const*>(plain.data() + at), length);
                if (!pendingReliable_.insert(key).second) {
                    ++counters.duplicates;
                    at += length;
                    continue;
                }
                retransmissions[groupId].push_back(std::move(key));
            }
            auto& group = groups[groupId];
            if ((command == 6 || command == 7) && channel == 0 &&
                length > commandHeader && plain[at + commandHeader] == 3)
                ++playerStates[groupId];
            if (!reliable && priority == Priority::Pose && command == 7 && length > commandHeader) {
                if (++commandCounts[groupId] == 1)
                    npcKeys[groupId] = ExtractNpcPoseKey(plain.data() + at + commandHeader, length - commandHeader);
                else
                    npcKeys[groupId].clear();
            } else {
                ++commandCounts[groupId];
                npcKeys[groupId].clear();
            }
            if (group.empty()) group.assign(plain.begin(), plain.begin() + header);
            group.insert(group.end(), plain.begin() + at, plain.begin() + at + length);
            at += length;
        }
        for (unsigned i = 0; i < groups.size(); ++i) {
            auto& group = groups[i];
            if (group.empty()) continue;
            const bool reliable = (i & 1u) == 0;
            const Priority priority = static_cast<Priority>(i / 2);
            auto& queue = queues_[i];
            counters.plainBytes += group.size();
            // Do not re-compress with ENet's range coder: Rice-coded poses gain only ~6.7%
            // while burning multiple passes of DIV/BST work on the relay and game threads.
            counters.wireBytes += group.size();
            if (!reliable && priority == Priority::Pose) {
                if (!npcKeys[i].empty()) {
                    bool replaced = false;
                    for (auto& existing : queue) {
                        if (existing.npcKey == npcKeys[i]) {
                            bytes_ -= existing.bytes.size();
                            poseUnreliableBytes_ -= existing.bytes.size();
                            existing.bytes = std::move(group);
                            existing.at = now;
                            bytes_ += existing.bytes.size();
                            poseUnreliableBytes_ += existing.bytes.size();
                            ++counters.npcUpdatesDropped;
                            replaced = true;
                            break;
                        }
                    }
                    if (replaced) {
                        EvictStaleUnreliablePoses(now, 0);
                        continue;
                    }
                }
                EvictStaleUnreliablePoses(now, group.size());
                poseUnreliableBytes_ += group.size();
            }
            bytes_ += group.size();
            ++depth_;
            queue.push_back({std::move(group), std::move(retransmissions[i]),
                             now, reliable, playerStates[i], priority, std::move(npcKeys[i])});
        }
        counters.peakDepth = (std::max)(counters.peakDepth, depth_);
        counters.peakBytes = (std::max)(counters.peakBytes, bytes_);
        counters.peakAge = (std::max)(counters.peakAge, OldestAge(now));
        return true;
    }

    template<class Submit>
    int Drain(Submit submit)
    {
        int blocked = Ok;
        for (unsigned i = 0; i < queues_.size(); ++i) {
            auto& queue = queues_[i];
            while (!queue.empty()) {
                auto const& item = queue.front();
                const int result = submit(item);
                if (result != Ok) {
                    ++counters.failed[result >= 0 && result < 128 ? result : 0];
                    ++(item.reliable ? counters.reliableFailed : counters.unreliableFailed);
                    if (!Retryable(result)) return result;
                    ++counters.retries;
                    if (blocked == Ok) blocked = result;
                    // A refused large packet must not prevent a small ACK,
                    // player state or independent channel from fitting.
                    break;
                }
                ++counters.accepted[i / 2];
                counters.bytes[i / 2] += item.bytes.size();
                counters.playerStates += item.playerStates;
                ++(item.reliable ? counters.reliableAccepted : counters.unreliableAccepted);
                PopFront(i);
            }
        }
        return blocked;
    }
};
}
