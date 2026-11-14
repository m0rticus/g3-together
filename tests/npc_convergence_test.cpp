// SPDX-License-Identifier: GPL-3.0-or-later
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main

#include "../src/steam_send.h"
#include <array>
#include <deque>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr unsigned MaxCrowd = 60;
constexpr double SteamRate = 243000, OtherTrafficRate = 6000;
std::array<NpcRecord, MaxCrowd> shown{};
std::array<std::vector<pose::Bone>, MaxCrowd> shownBones{};
std::array<unsigned, MaxCrowd> displayed{};

unsigned Index(NpcRecord const& record)
{
    assert(record.guid[0] && record.guid[0] <= MaxCrowd);
    return record.guid[0] - 1;
}

int DisplayRecords(NpcRecord* records, int count, uint32_t, uint32_t, uint8_t, uint32_t)
{
    for (int i = 0; i < count; ++i) {
        const auto index = Index(records[i]);
        shown[index] = records[i];
        ++displayed[index];
    }
    return count;
}

void DisplayPose(NpcRecord const& record, pose::Bone const* bones, int count, uint8_t, bool essential, uint8_t)
{
    if (essential) assert(record.sent == 5000);
    shownBones[Index(record)].assign(bones, bones + count);
}

struct Flight {
    Transmission packet;
    std::vector<Transmission> companions;
    uint32_t serial = 0, at = 0, sentAt = 0, queuedAt = 0;
    size_t charge = 0;
    bool lost = false, serviced = false, charged = false, isRetry = false;
};

// A finite, byte-serviced, shared send buffer with result-25 refusals. Steam
// data stays unreliable; only the simulated ENet layer retries reliable loss.
class Link {
    std::array<std::deque<Flight>, 4> waiting_;
    std::deque<Flight> flights_, retries_;
    std::array<std::map<uint32_t, Flight>, ChannelCount> reordered_;
    std::array<uint32_t, ChannelCount> next_{}, received_{};
    size_t buffered_ = 0;
    double serviceAt_ = 0;
    unsigned submissions_ = 0;
    unsigned verified_ = 0;
    steam::send::Codec codec_;
    std::vector<uint8_t> datagram_, original_, decoded_;

    static unsigned Priority(Transmission const& packet)
    {
        return packet.channel == ChannelDefinitions ? 1u :
            packet.channel == ChannelPoses || packet.channel == ChannelPosesAlt ? 2u : 0u;
    }

    size_t Charge(Flight const& flight)
    {
        datagram_.assign({0x80, 0, 0, 0});
        auto append = [&](Transmission const& packet, unsigned offset) {
            const bool reliable = (packet.flags & ENET_PACKET_FLAG_RELIABLE) != 0;
            const uint16_t token = static_cast<uint16_t>(flight.serial + offset);
            datagram_.push_back(reliable ? 0x86 : 7);
            datagram_.push_back(packet.channel);
            datagram_.push_back(static_cast<uint8_t>(token >> 8));
            datagram_.push_back(static_cast<uint8_t>(token));
            if (!reliable) { datagram_.push_back(0); datagram_.push_back(1); }
            datagram_.push_back(static_cast<uint8_t>(packet.bytes.size() >> 8));
            datagram_.push_back(static_cast<uint8_t>(packet.bytes.size()));
            datagram_.insert(datagram_.end(), packet.bytes.begin(), packet.bytes.end());
        };
        append(flight.packet, 0);
        for (unsigned i = 0; i < flight.companions.size(); ++i) append(flight.companions[i], i + 1);
        const bool check = (++verified_ & 7u) == 1u;
        if (check) original_ = datagram_;
        codec_.Encode(datagram_);
        if (check) {
            assert(codec_.Decode(datagram_.data(), datagram_.size(), decoded_));
            assert(decoded_ == original_);
        }
        return datagram_.size() + 72; // Conservative framing beyond ENet, including the 16-byte relay envelope.
    }

    static size_t PlainBytes(Flight const& flight)
    {
        size_t bytes = flight.packet.bytes.size() + 8;
        for (auto const& packet : flight.companions) bytes += packet.bytes.size() + 8;
        return bytes;
    }
public:
    size_t peak = 0;
    unsigned refused = 0, losses = 0, delivered = 0, reliableRetries = 0;
    unsigned poseAckMessages = 0;
    uint64_t offeredCold = 0, offeredSteady = 0, steadySamples = 0;
    uint32_t captureMs = 22000, steadyFromMs = 16000;
    bool hostward = false;
    std::map<uint32_t, uint32_t> anchorsAt;

    void FinalizePending()
    {
        for (auto& queue : waiting_) {
            for (auto& flight : queue) {
                if (flight.charged) continue;
                flight.charge = Charge(flight);
                flight.charged = true;
                if (flight.queuedAt < 2000) offeredCold += flight.charge;
                if (flight.queuedAt >= steadyFromMs && flight.queuedAt < captureMs)
                    offeredSteady += flight.charge;
            }
        }
    }

    void Enqueue(Transmission packet, uint32_t now)
    {
        if (!hostward && packet.bytes[0] == MsgControlPose) {
            const size_t flagsAt = sizeof(NpcsHeader) + 2 +
                BoneWire16(packet.bytes.data() + sizeof(NpcsHeader));
            if (packet.bytes[flagsAt] & PoseAnchorFlag) {
                NpcsHeader header;
                std::memcpy(&header, packet.bytes.data(), sizeof(header));
                anchorsAt.emplace(header.publication, now);
            }
        }
        if (hostward && packet.bytes[0] == MsgPoseAck) ++poseAckMessages;
        const bool steady = now >= steadyFromMs && now < captureMs;
        if (steady && (packet.bytes[0] == MsgControlPose || packet.bytes[0] == MsgCompactPosedNpc))
            ++steadySamples;
        const auto priority = Priority(packet);
        auto& queue = waiting_[priority];
        const bool join = !queue.empty() && !queue.back().isRetry && queue.back().queuedAt == now &&
            queue.back().packet.channel == packet.channel &&
            queue.back().packet.flags == packet.flags &&
            PlainBytes(queue.back()) + packet.bytes.size() + 8 <= 1172;
        if (join) {
            queue.back().companions.push_back(std::move(packet));
        } else {
            Flight flight;
            flight.queuedAt = now;
            flight.serial = packet.flags & ENET_PACKET_FLAG_RELIABLE ? ++next_[packet.channel] : 0;
            flight.packet = std::move(packet);
            queue.push_back(std::move(flight));
        }
    }

    template<class Receive> void Step(uint32_t now, Receive receive)
    {
        FinalizePending();
        for (auto& flight : flights_) {
            if (!flight.serviced && flight.sentAt <= now) {
                buffered_ -= flight.charge;
                flight.serviced = true;
            }
        }
        while (!retries_.empty() && retries_.front().at <= now) {
            auto flight = std::move(retries_.front());
            retries_.pop_front();
            if (now < 2000) offeredCold += flight.charge;
            if (now >= steadyFromMs && now < captureMs) offeredSteady += flight.charge;
            flight.queuedAt = now;
            flight.isRetry = true;
            waiting_[Priority(flight.packet)].push_front(std::move(flight));
            ++reliableRetries;
        }
        while (!flights_.empty() && flights_.front().at <= now) {
            auto flight = std::move(flights_.front());
            flights_.pop_front();
            assert(flight.serviced);
            if (flight.lost) {
                ++losses;
                if (flight.serial) {
                    flight.at = now + 2100;
                    flight.isRetry = true;
                    retries_.push_back(std::move(flight));
                }
                continue;
            }
            if (!flight.serial) {
                receive(flight.packet);
                for (auto const& packet : flight.companions) receive(packet);
                ++delivered;
                continue;
            }
            const unsigned channel = flight.packet.channel;
            reordered_[channel].emplace(flight.serial, std::move(flight));
            for (;;) {
                auto found = reordered_[channel].find(received_[channel] + 1);
                if (found == reordered_[channel].end()) break;
                receive(found->second.packet);
                for (auto const& packet : found->second.companions) receive(packet);
                reordered_[channel].erase(found);
                ++received_[channel];
                ++delivered;
            }
        }
        constexpr size_t Capacity = 12 * 1024;
        // Reserve headroom for the observed non-posed controls and other traffic.
        // This models physical service, not a production NPC send limit.
        const double rate = !hostward && now < 3000 ? 16 * 1024.0 :
            hostward ? SteamRate : SteamRate - OtherTrafficRate;
        for (auto& queue : waiting_) {
            while (!queue.empty()) {
                auto const& pending = queue.front();
                const steam::send::Item item{{}, {}, 0, pending.serial != 0, 0};
                assert(!(item.Flags() & steam::send::Reliable));
                if (buffered_ + pending.charge > Capacity) {
                    ++refused;
                    break;
                }
                auto flight = std::move(queue.front());
                queue.pop_front();
                buffered_ += flight.charge;
                peak = (std::max)(peak, buffered_);
                serviceAt_ = (std::max)(serviceAt_, double(now));
                serviceAt_ += flight.charge * 1000.0 / rate;
                // ~2.1 s RTT, plus serialization. Keep arrival order here;
                // retries explicitly create cross-channel and sample reorder.
                flight.sentAt = static_cast<uint32_t>(std::ceil(serviceAt_));
                flight.serviced = false;
                flight.at = flight.sentAt + 1050;
                flight.lost = (++submissions_ % 20) == 7;
                flights_.push_back(std::move(flight));
            }
        }
    }

    bool Empty() const
    {
        for (auto const& queue : waiting_) if (!queue.empty()) return false;
        for (auto const& queue : reordered_) if (!queue.empty()) return false;
        return flights_.empty() && retries_.empty();
    }

    uint32_t OldestWaitingMs(uint32_t now) const
    {
        uint32_t oldest = 0;
        for (auto const& queue : waiting_)
            if (!queue.empty()) oldest = (std::max)(oldest, now - queue.front().queuedAt);
        return oldest;
    }
};

NpcRecord Record(unsigned number, unsigned crowd, unsigned tick)
{
    NpcRecord record{};
    record.guid[0] = static_cast<uint8_t>(number + 1);
    record.guid[7] = 0xa5;
    record.incarnation = 1;
    record.hitPoints = 100 - int(number);
    record.hitPointsMax = 100;
    const unsigned fightEnd = (std::max)(2u, crowd / 12);
    const unsigned walkEnd = crowd * 2 / 3;
    record.position[0] = float(number * 100) + (number >= fightEnd && number < walkEnd ? float(tick) * 2.0f : 0.0f);
    record.position[1] = float(number) * -0.25f;
    record.rotation[3] = 1;
    record.sent = fakeTime;
    record.heldRight = 1000 + number;
    record.attitude = static_cast<uint8_t>(number % 3);
    auto& motion = record.motions[0];
    motion.active = 1;
    motion.name = 100 + number;
    motion.instance = 1 + tick / 50;
    motion.speed = 1.0f + float(tick % 13) * 0.001f;
    motion.phase = float(tick % 50) / 50;
    motion.weight = 0.75f;
    motion.options[0] = number + 20;
    return record;
}

std::vector<pose::Bone> Rig(unsigned number, unsigned crowd, unsigned tick)
{
    std::vector<pose::Bone> bones(number % 3 == 0 ? 95 : number % 3 == 1 ? 73 : 64);
    const unsigned fightEnd = (std::max)(2u, crowd / 12);
    const unsigned walkEnd = crowd * 2 / 3;
    const unsigned idleEnd = crowd * 11 / 12;
    const bool fighting = number < fightEnd, walking = number >= fightEnd && number < walkEnd;
    const double amplitude = fighting ? .4 : walking ? .18 : number < idleEnd ? .012 : 0;
    const double time = double(tick) * .02;
    for (unsigned i = 0; i < bones.size(); ++i) {
        pose::Transform transform;
        transform.name = 1 + i;
        const double offset = double(i) * .17 + double(number) * .1;
        const double angle = std::sin(time * (fighting ? 6 : 3) + offset) * amplitude;
        transform.values[0] = transform.values[8] = float(std::cos(angle));
        transform.values[2] = float(std::sin(angle));
        transform.values[6] = -transform.values[2];
        transform.values[4] = 1;
        for (unsigned k = 9; k < 12; ++k)
            transform.values[k] = float(double(i) * .5 + amplitude * 10 * std::sin(time * 3 + offset + k));
        assert(pose::Encode(transform, bones[i]));
    }
    return bones;
}
}

int main(int argc, char** argv)
{
    bool fullSoak = false;
    for (int a = 1; a < argc; ++a)
        if (std::string(argv[a]) == "--full") fullSoak = true;
    const unsigned crowd = fullSoak ? 60u : 30u;
    const unsigned steadyWindowMs = fullSoak ? 10000u : 6000u;
    const unsigned captureMs = fullSoak ? 120000u : 22000u;
    const unsigned steadyFromMs = captureMs - steadyWindowMs;
    const unsigned drainMs = fullSoak ? 25000u : 12000u;

    ENetHost fakeHost{};
    ENetPeer peer{};
    ENetChannel channels[ChannelCount]{};
    peer.channels = channels;
    peer.data = reinterpret_cast<void*>(uintptr_t(2));
    peer.address.port = 12;
    fakeHost.peers = &peer;
    host = &fakeHost;
    peers[2] = server = &peer;
    peerAccounts[2] = 12;
    target.host = peer.address.host;
    isHost = true;
    localId = 1;
    inWorld = clientInWorld[2] = true;
    worldEpoch = 0x1234;
    fakeTime = 5000;
    ResetNpcReplication();
    SetNpcHandlers(DisplayRecords, nullptr);
    SetNpcPoseHandler(DisplayPose);
    Link outward, inward;
    outward.captureMs = inward.captureMs = captureMs;
    outward.steadyFromMs = inward.steadyFromMs = steadyFromMs;
    inward.hostward = true;
    bonewire::Dictionary hostDictionary, clientDictionary;
    std::array<NpcRecord, MaxCrowd> latest{};
    std::array<std::vector<pose::Bone>, MaxCrowd> latestBones{};
    unsigned samples = 0;
    unsigned maxAnchorAckDelay = 0;
    unsigned recoveredQueueAge = 0, recoveredDisplayAge = 0;
    auto collect = [&](Link& link, uint32_t now) {
        for (auto& packet : transmissions) link.Enqueue(std::move(packet), now);
        transmissions.clear();
        link.FinalizePending();
    };
    for (unsigned now = 0; now <= captureMs + drainMs; now += 20) {
        fakeTime = 5000 + now;
        isHost = true;
        boneLayouts = std::move(hostDictionary);
        if (now < captureMs || (now < captureMs + 7000 && now % 1000 == 0)) {
            const unsigned tick = now / 20;
            for (unsigned i = 0; i < crowd; ++i) {
                // Synthetic captured samples, 20 poses/s per NPC across the camp;
                // no send budget, replacement or new scheduler policy.
                if (now < captureMs) {
                    if (now && (tick + i) % 5 >= 2) continue;
                    latest[i] = Record(i, crowd, tick);
                    latestBones[i] = Rig(i, crowd, tick);
                } else {
                    // The existing unchanged-state keepalive repairs ordinary
                    // unreliable loss after animation becomes quiet.
                    latest[i].sent = fakeTime;
                }
                assert(SendPosedNpcTo(2, latest[i], latestBones[i].data(),
                                      static_cast<int>(latestBones[i].size()), 0, now == 0, 100));
                ++samples;
            }
        }
        collect(outward, now);
        inward.Step(now, [&](Transmission const& packet) {
            if (packet.bytes[0] == MsgPoseAck) {
                PoseAckMsg ack;
                std::memcpy(&ack, packet.bytes.data(), sizeof(ack));
                const auto found = outward.anchorsAt.find(ack.token);
                if (found != outward.anchorsAt.end())
                    maxAnchorAckDelay = (std::max)(maxAnchorAckDelay, now - found->second);
            }
            Deliver(&peer, packet);
            collect(outward, now);
        });
        hostDictionary = std::move(boneLayouts);
        isHost = false;
        boneLayouts = std::move(clientDictionary);
        outward.Step(now, [&](Transmission const& packet) {
            Deliver(&peer, packet);
            collect(inward, now);
        });
        clientDictionary = std::move(boneLayouts);
        if (now == captureMs - 20) {
            recoveredQueueAge = outward.OldestWaitingMs(now);
            assert(recoveredQueueAge < 250);
            for (unsigned i = 0; i < crowd; ++i)
                recoveredDisplayAge = (std::max)(recoveredDisplayAge, fakeTime - shown[i].sent);
            assert(recoveredDisplayAge < 1500);
        }
    }
    assert(outward.refused && outward.losses && outward.reliableRetries);
    assert(outward.peak <= 12 * 1024 && outward.Empty() && inward.Empty());
    assert(npcControlCreated == crowd && npcControlAckAccepted == crowd);
    assert(bandwidth.poseDeltas > 1000 && bandwidth.poseAcks < samples / 5);
    assert(inward.poseAckMessages >= npcAnchorsSent);
    assert(maxAnchorAckDelay > 2500);
    assert(bandwidth.poseAckMisses == 0 && bandwidth.poseDecodeFailures == 0);
    const double cold = outward.offeredCold / 2.0;
    const double steady = outward.offeredSteady / (steadyWindowMs / 1000.0);
    std::fprintf(stderr, "NPC compressed offered load: cold=%.0f B/s steady=%.0f B/s\n", cold, steady);
    assert(steady < cold * .70 && steady < SteamRate - OtherTrafficRate);
    assert(outward.steadySamples >= (crowd * 20u) * (steadyWindowMs / 1000u));
    for (unsigned i = 0; i < crowd; ++i) {
        assert(displayed[i] > 10);
        assert(shown[i].sent >= captureMs + 4000);
        assert(!std::memcmp(shown[i].guid, latest[i].guid, 16));
        assert(shown[i].hitPoints == latest[i].hitPoints && shown[i].heldRight == latest[i].heldRight);
        auto expectedMotion = latest[i].motions[0];
        expectedMotion.phase = shown[i].motions[0].phase;
        expectedMotion.weight = shown[i].motions[0].weight;
        assert(!std::memcmp(&shown[i].motions[0], &expectedMotion, sizeof(expectedMotion)));
        assert(std::fabs(shown[i].motions[0].phase - latest[i].motions[0].phase) <= .5f / 4095 + 1e-7f);
        assert(std::fabs(shown[i].motions[0].weight - latest[i].motions[0].weight) <= .5f / 255 + 1e-7f);
        for (unsigned k = 0; k < 3; ++k)
            assert(std::fabs(shown[i].position[k] - latest[i].position[k]) <= .5f);
        assert(shownBones[i].size() == latestBones[i].size());
        for (unsigned b = 0; b < shownBones[i].size(); ++b) {
            assert(shownBones[i][b].name == latestBones[i][b].name);
            for (unsigned k = 0; k < 12; ++k)
                assert(std::abs(int(shownBones[i][b].values[k]) - latestBones[i][b].values[k]) <=
                       (k < 9 ? 2 : 0));
        }
    }
    std::printf("NPC convergence: NPCs=%u samples=%u cold_Bps=%.0f steady_Bps=%.0f reduction=%.1f%% "
                "result25=%u loss=%u reliable_retries=%u anchors=%llu ACKs=%llu deltas=%llu "
                "max_anchor_ACK_ms=%u recovered_queue_ms=%u recovered_display_ms=%u peak_bytes=%zu\n",
                crowd, samples, cold, steady, 100 * (cold - steady) / cold,
                outward.refused, outward.losses, outward.reliableRetries,
                static_cast<unsigned long long>(npcAnchorsSent),
                static_cast<unsigned long long>(inward.poseAckMessages),
                static_cast<unsigned long long>(bandwidth.poseDeltas), maxAnchorAckDelay,
                recoveredQueueAge, recoveredDisplayAge, outward.peak);
    return 0;
}
