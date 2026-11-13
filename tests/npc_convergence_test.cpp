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
constexpr unsigned Crowd = 60;
constexpr unsigned CaptureMs = 120000, SteadyFromMs = CaptureMs - 10000;
constexpr double SteamRate = 243000, OtherTrafficRate = 6000;
std::array<NpcRecord, Crowd> shown{};
std::array<std::vector<pose::Bone>, Crowd> shownBones{};
std::array<unsigned, Crowd> displayed{};

unsigned Index(NpcRecord const& record)
{
    assert(record.guid[0] && record.guid[0] <= Crowd);
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
    bool lost = false, serviced = false;
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
    steam::send::Codec codec_;

    static unsigned Priority(Transmission const& packet)
    {
        return packet.channel == ChannelDefinitions ? 1u :
            packet.channel == ChannelPoses || packet.channel == ChannelPosesAlt ? 2u : 0u;
    }

    size_t Charge(Flight const& flight)
    {
        std::vector<uint8_t> datagram{0x80, 0, 0, 0};
        auto append = [&](Transmission const& packet, unsigned offset) {
            const bool reliable = (packet.flags & ENET_PACKET_FLAG_RELIABLE) != 0;
            const uint16_t token = static_cast<uint16_t>(flight.serial + offset);
            datagram.push_back(reliable ? 0x86 : 7);
            datagram.push_back(packet.channel);
            datagram.push_back(static_cast<uint8_t>(token >> 8));
            datagram.push_back(static_cast<uint8_t>(token));
            if (!reliable) { datagram.push_back(0); datagram.push_back(1); }
            datagram.push_back(static_cast<uint8_t>(packet.bytes.size() >> 8));
            datagram.push_back(static_cast<uint8_t>(packet.bytes.size()));
            datagram.insert(datagram.end(), packet.bytes.begin(), packet.bytes.end());
        };
        append(flight.packet, 0);
        for (unsigned i = 0; i < flight.companions.size(); ++i) append(flight.companions[i], i + 1);
        const auto original = datagram;
        codec_.Encode(datagram);
        std::vector<uint8_t> decoded;
        assert(codec_.Decode(datagram.data(), datagram.size(), decoded));
        assert(decoded == original);
        return datagram.size() + 72; // Conservative framing beyond ENet, including the 16-byte relay envelope.
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
    bool hostward = false;
    std::map<uint32_t, uint32_t> anchorsAt;

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
        const bool steady = now >= SteadyFromMs && now < CaptureMs;
        if (steady && (packet.bytes[0] == MsgControlPose || packet.bytes[0] == MsgCompactPosedNpc))
            ++steadySamples;
        const auto priority = Priority(packet);
        auto& queue = waiting_[priority];
        const bool join = !queue.empty() && queue.back().queuedAt == now &&
            queue.back().packet.channel == packet.channel &&
            queue.back().packet.flags == packet.flags &&
            PlainBytes(queue.back()) + packet.bytes.size() + 8 <= 1172;
        size_t before = 0;
        if (join) {
            before = queue.back().charge;
            queue.back().companions.push_back(std::move(packet));
        } else {
            Flight flight;
            flight.queuedAt = now;
            flight.serial = packet.flags & ENET_PACKET_FLAG_RELIABLE ? ++next_[packet.channel] : 0;
            flight.packet = std::move(packet);
            queue.push_back(std::move(flight));
        }
        auto& flight = queue.back();
        flight.charge = Charge(flight);
        auto account = [&](uint64_t& counter) {
            counter += flight.charge;
            counter -= before;
        };
        if (now < 2000) account(offeredCold);
        if (steady) account(offeredSteady);
    }

    template<class Receive> void Step(uint32_t now, Receive receive)
    {
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
            if (now >= SteadyFromMs && now < CaptureMs) offeredSteady += flight.charge;
            flight.queuedAt = now;
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

NpcRecord Record(unsigned number, unsigned tick)
{
    NpcRecord record{};
    record.guid[0] = static_cast<uint8_t>(number + 1);
    record.guid[7] = 0xa5;
    record.incarnation = 1;
    record.hitPoints = 100 - int(number);
    record.hitPointsMax = 100;
    record.position[0] = float(number * 100) + (number >= 5 && number < 40 ? float(tick) * 2.0f : 0.0f);
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

std::vector<pose::Bone> Rig(unsigned number, unsigned tick)
{
    std::vector<pose::Bone> bones(number % 3 == 0 ? 95 : number % 3 == 1 ? 73 : 64);
    const bool fighting = number < 5, walking = number >= 5 && number < 40;
    const double amplitude = fighting ? .4 : walking ? .18 : number < 55 ? .012 : 0;
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

int main()
{
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
    inward.hostward = true;
    bonewire::Dictionary hostDictionary, clientDictionary;
    std::array<NpcRecord, Crowd> latest{};
    std::array<std::vector<pose::Bone>, Crowd> latestBones{};
    unsigned samples = 0;
    unsigned maxAnchorAckDelay = 0;
    unsigned recoveredQueueAge = 0, recoveredDisplayAge = 0;
    auto collect = [&](Link& link, uint32_t now) {
        for (auto& packet : transmissions) link.Enqueue(std::move(packet), now);
        transmissions.clear();
    };
    for (unsigned now = 0; now <= CaptureMs + 25000; now += 20) {
        fakeTime = 5000 + now;
        isHost = true;
        boneLayouts = std::move(hostDictionary);
        if (now < CaptureMs || (now < CaptureMs + 7000 && now % 1000 == 0)) {
            const unsigned tick = now / 20;
            for (unsigned i = 0; i < Crowd; ++i) {
                // Synthetic captured samples, 1,200 poses/s for the 60-NPC camp;
                // no send budget, replacement or new scheduler policy.
                if (now < CaptureMs) {
                    if (now && (tick + i) % 5 >= 2) continue;
                    latest[i] = Record(i, tick);
                    latestBones[i] = Rig(i, tick);
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
        if (now == CaptureMs - 20) {
            recoveredQueueAge = outward.OldestWaitingMs(now);
            assert(recoveredQueueAge < 250);
            for (auto const& record : shown)
                recoveredDisplayAge = (std::max)(recoveredDisplayAge, fakeTime - record.sent);
            assert(recoveredDisplayAge < 1500);
        }
    }
    assert(outward.refused && outward.losses && outward.reliableRetries);
    assert(outward.peak <= 12 * 1024 && outward.Empty() && inward.Empty());
    assert(npcControlCreated == Crowd && npcControlAckAccepted == Crowd);
    assert(bandwidth.poseDeltas > 1000 && bandwidth.poseAcks < samples / 5);
    assert(inward.poseAckMessages >= npcAnchorsSent);
    assert(maxAnchorAckDelay > 2500);
    assert(bandwidth.poseAckMisses == 0 && bandwidth.poseDecodeFailures == 0);
    const double cold = outward.offeredCold / 2.0;
    const double steady = outward.offeredSteady / 10.0;
    std::fprintf(stderr, "NPC compressed offered load: cold=%.0f B/s steady=%.0f B/s\n", cold, steady);
    assert(steady < cold * .70 && steady < SteamRate - OtherTrafficRate);
    assert(outward.steadySamples >= 12000);
    for (unsigned i = 0; i < Crowd; ++i) {
        assert(displayed[i] > 10);
        assert(shown[i].sent >= CaptureMs + 4000);
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
                Crowd, samples, cold, steady, 100 * (cold - steady) / cold,
                outward.refused, outward.losses, outward.reliableRetries,
                static_cast<unsigned long long>(npcAnchorsSent),
                static_cast<unsigned long long>(inward.poseAckMessages),
                static_cast<unsigned long long>(bandwidth.poseDeltas), maxAnchorAckDelay,
                recoveredQueueAge, recoveredDisplayAge, outward.peak);
    return 0;
}
