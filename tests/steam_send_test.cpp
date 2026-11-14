// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/steam_send.h"
#include "../src/steam_config.h"
#include "../src/net.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

extern "C" void* enet_malloc(size_t size) { return std::malloc(size); }
extern "C" void enet_free(void* memory) { std::free(memory); }
extern "C" void enet_host_compress(ENetHost*, ENetCompressor const*) {}

using namespace g3t::steam;
using namespace g3t::steam::send;

static std::vector<uint8_t> Command(unsigned channel, unsigned sequence, bool reliable,
                                    std::vector<uint8_t> const& payload)
{
    std::vector<uint8_t> packet{0x80, 1, 0, 9, static_cast<uint8_t>(reliable ? 0x86 : 7),
                                static_cast<uint8_t>(channel), 0, static_cast<uint8_t>(sequence)};
    if (!reliable) { packet.push_back(0); packet.push_back(static_cast<uint8_t>(sequence)); }
    packet.push_back(static_cast<uint8_t>(payload.size() >> 8));
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

static unsigned Channel(Codec& codec, Item const& item)
{
    std::vector<uint8_t> plain;
    assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain));
    return plain[5];
}

static void RetentionAndPriority()
{
    Codec codec;
    assert(codec.Ready());
    Queue queue;
    auto pose = Command(4, 1, false, {44,1,2,3,4});
    assert(queue.Push(codec, pose.data(), pose.size(), 100));
    assert(queue.Drain([](Item const&) { return LimitExceeded; }) == LimitExceeded);
    assert(queue.Depth() == 1 && queue.OldestAge(200) == 100);
    auto player = Command(0, 1, false, {3,5,6,7});
    auto baseline = Command(7, 1, true, {47,0,0,0});
    assert(queue.Push(codec, baseline.data(), baseline.size(), 150));
    assert(queue.Push(codec, player.data(), player.size(), 160));
    assert(queue.Drain([](Item const&) { return Ignored; }) == Ignored);
    assert(queue.Depth() == 3 && queue.counters.retries == 4);
    std::vector<unsigned> order;
    assert(queue.Drain([&](Item const& item) {
        order.push_back(Channel(codec, item));
        assert(item.Flags() & AutoRestart);
        assert(item.Flags() & NoDelay);
        assert(!(item.Flags() & Reliable)); // even ENet reliable: only one retransmission layer
        return Ok;
    }) == Ok);
    assert((order == std::vector<unsigned>{0,7,4}));
    assert(queue.Depth() == 0 && queue.Bytes() == 0);
    assert(queue.counters.reliableAccepted == 1 && queue.counters.unreliableAccepted == 2);
    assert(queue.counters.playerStates == 1);
    assert(queue.counters.failed[25] == 1 && queue.counters.failed[41] == 3);
    assert(!Retryable(Timeout)); // EResult 16 is not Ignored/NoDelay

    assert(queue.Push(codec, pose.data(), pose.size(), UINT32_MAX - 20));
    assert(queue.Push(codec, pose.data(), pose.size(), UINT32_MAX - 10));
    assert(queue.Depth() == 2 && queue.OldestAge(9) == 30);
    assert(queue.Drain([](Item const&) { return 3; }) == 3);
    assert(queue.Depth() == 2 && !Retryable(3) && !Retryable(8));
    assert(queue.Drain([](Item const&) { return Ok; }) == Ok);
    assert(queue.counters.accepted[2] == 3); // every NPC sample, including identical samples

    assert(queue.Push(codec, player.data(), player.size(), 300));
    auto heartbeat = Command(0, 2, true, {50,1});
    assert(queue.Push(codec, heartbeat.data(), heartbeat.size(), 301));
    unsigned submissions = 0;
    assert(queue.Drain([&](Item const& item) {
        ++submissions;
        if (item.reliable) return Ok;
        return Ignored;
    }) == Ignored);
    assert(submissions == 2 && queue.Depth() == 1); // delayed motion cannot block reliable activity/ACKs
}

static void CapacityConfiguration()
{
    using namespace g3t::steam::config;
    std::array<int32_t, 4> values{512 * 1024, 256 * 1024, 256 * 1024, 5000};
    unsigned writes = 0;
    auto get = [&](int id) { return Read{2, Int32, values[id - SendBufferSize]}; };
    auto set = [&](int id, int32_t value) {
        ++writes;
        values[id - SendBufferSize] = value;
        return true;
    };
    for (auto const& setting : Settings) {
        auto result = Apply(setting, get, set);
        assert(result.Verified());
    }
    assert(writes == 3);
    assert((values == std::array<int32_t, 4>{BufferCapacity, MinRateCapacity, MaxRateCapacity, 5000}));
    assert(Apply(Settings[0], get, set).Verified() && writes == 3);
    values[0] = 4 * 1024 * 1024;
    assert(Apply(Settings[0], get, set).Verified() && values[0] == BufferCapacity && writes == 4);
    values[0] = 1024;
    auto rejected = Apply(Settings[0], get, [](int, int32_t) { return false; });
    assert(!rejected.Verified() && rejected.after.value == 1024);
    auto unreadable = Apply(Settings[0], [](int) { return Read{-1, 0, 0}; }, set);
    assert(!unreadable.Verified() && writes == 4);
    auto wrongType = Apply(Settings[0], [](int) { return Read{1, 3, 1024}; }, set);
    assert(!wrongType.Verified() && writes == 4);
    auto unverified = Apply(Settings[0], get, [](int, int32_t) { return true; });
    assert(!unverified.Verified()); // accepted write alone is not an effective-value check
    values[1] = MinRateCapacity * 2;
    assert(Apply(Settings[1], get, set).Verified() && writes == 4);

    // Pinned Valve SNP_ClampSendRate, d534c19: the BBR probe cycle is a FIXME.
    auto valveClamp = [](int estimate, int minimum, int maximum) {
        minimum = std::clamp(minimum, 1024, 100 * 1024 * 1024);
        maximum = std::clamp(maximum, minimum, 100 * 1024 * 1024);
        return std::clamp(estimate, minimum, maximum);
    };
    int rate = 4380 * 1000 / 40; // Initial rate at 40 ms RTT, not a bandwidth measurement.
    for (int i = 0; i < 100; ++i) rate = valveClamp(rate, 256 * 1024, MaxRateCapacity);
    assert(rate == 256 * 1024); // Raising only max cannot make this sender probe upwards.
    rate = valveClamp(rate, MinRateCapacity, MaxRateCapacity);
    assert(rate == MinRateCapacity);
}

static void SplittingAndRetransmissions()
{
    Codec codec;
    Queue queue;
    auto pose = Command(5, 1, false, {44,1,2});
    auto critical = Command(1, 7, true, {6,8,9});
    auto bulk = Command(6, 3, true, {40,1,2});
    auto mixed = pose;
    mixed.insert(mixed.end(), critical.begin() + 4, critical.end());
    mixed.insert(mixed.end(), bulk.begin() + 4, bulk.end());
    codec.Encode(mixed);
    assert(queue.Push(codec, mixed.data(), mixed.size(), 10));
    assert(queue.Depth() == 3);
    auto resent = critical;
    resent[3] = 30;
    assert(queue.Push(codec, resent.data(), resent.size(), 20));
    assert(queue.Depth() == 3 && queue.counters.duplicates == 1);
    std::vector<unsigned> order;
    assert(queue.Drain([&](Item const& item) {
        const unsigned channel = Channel(codec, item);
        order.push_back(channel);
        std::vector<uint8_t> plain;
        assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain));
        auto const& expected = channel == 5 ? pose : channel == 1 ? critical : bulk;
        assert(plain == expected); // headers, sequence numbers and command bytes preserved
        return Ok;
    }) == Ok);
    assert((order == std::vector<unsigned>{1,5,6}));
    assert(queue.Push(codec, resent.data(), resent.size(), 30)); // a transmitted retry is not suppressed
    assert(queue.Depth() == 1);
    auto invalid = pose;
    invalid.pop_back();
    assert(queue.Push(codec, invalid.data(), invalid.size(), 40));
    assert(queue.Depth() == 2 && queue.counters.unclassified == 1);
    assert(!queue.Push(codec, nullptr, 0, 40));
}

static void UnclassifiedRetention()
{
    Codec codec;
    Queue queue;
    const std::vector<std::vector<uint8_t>> unknown = {
        {0x40, 0, 0xff},                    // compressed, invalid range coder stream
        {0, 0, 0x0d, 0, 0, 0},             // unsupported command
        {0x80, 0, 0, 1, 0x86, 0, 0, 1},   // truncated command
        {0x40}                              // too short even to decode
    };
    std::vector<uint8_t> plain;
    size_t bytes = 0;
    for (auto const& packet : unknown) {
        EnetPacketParse parse{};
        assert(!codec.Decode(packet.data(), packet.size(), plain, &parse));
        assert(queue.Push(codec, packet.data(), packet.size(), 10, &parse));
        assert(!parse.valid);
        bytes += packet.size();
    }
    assert(queue.Push(codec, unknown[1].data(), unknown[1].size(), 11)); // unknowns are never deduplicated
    bytes += unknown[1].size();
    assert(queue.Depth() == 5 && queue.Bytes() == bytes);
    assert(queue.counters.unclassified == 5 && queue.counters.duplicates == 0);
    assert(queue.counters.plainBytes == bytes && queue.counters.wireBytes == bytes);
    assert(!queue.Push(codec, nullptr, 1, 10));
    assert(!queue.Push(codec, unknown[0].data(), 0, 10));
    std::vector<uint8_t> oversized(RelayPayloadLimit + 1);
    assert(!queue.Push(codec, oversized.data(), oversized.size(), 10));
    assert(queue.Depth() == 5 && queue.counters.unclassified == 5);

    for (int result : {LimitExceeded, Ignored}) {
        assert(queue.Drain([&](Item const& item) {
            assert(!item.reliable && item.bytes == unknown[0]);
            return result;
        }) == result);
        assert(queue.Depth() == 5 && queue.Bytes() == bytes);
    }
    assert(queue.counters.retries == 2 && queue.counters.failed[25] == 1 &&
           queue.counters.failed[41] == 1 && queue.counters.unreliableFailed == 2);
    size_t index = 0;
    assert(queue.Drain([&](Item const& item) {
        assert(!item.reliable && item.bytes == unknown[index < 4 ? index : 1]);
        ++index;
        return Ok;
    }) == Ok);
    assert(index == 5 && queue.Depth() == 0 && queue.Bytes() == 0);
    assert(queue.counters.accepted[static_cast<unsigned>(Priority::Bulk)] == 5);
    assert(queue.counters.unreliableAccepted == 5 &&
           queue.counters.bytes[static_cast<unsigned>(Priority::Bulk)] == bytes);
}

static void RepackedRetransmissions()
{
    Codec codec;
    Queue queue;
    auto first = Command(1, 7, true, {6,8,9});
    auto second = Command(1, 8, true, {6,1,2});
    assert(queue.Push(codec, first.data(), first.size(), 10));
    auto repacked = first;
    repacked[3] = 30; // ENet's retransmit timestamp is not part of command identity
    repacked.insert(repacked.end(), second.begin() + 4, second.end());
    assert(queue.Push(codec, repacked.data(), repacked.size(), 20));
    assert(queue.Depth() == 2 && queue.counters.duplicates == 1);
    std::vector<std::vector<uint8_t>> decoded;
    assert(queue.Drain([&](Item const& item) {
        std::vector<uint8_t> plain;
        assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain));
        decoded.push_back(plain);
        return Ok;
    }) == Ok);
    auto expectedSecond = second;
    expectedSecond[3] = 30;
    assert((decoded == std::vector<std::vector<uint8_t>>{first, expectedSecond}));
    assert(queue.Push(codec, repacked.data(), repacked.size(), 30));
    assert(queue.Depth() == 1); // accepted commands may be retransmitted after real network loss
}

static void IndependentRefusals()
{
    Codec codec;
    Queue queue;
    auto save = Command(6, 1, true, {40,0});
    auto definition = Command(7, 1, true, {47,0});
    auto player = Command(0, 1, false, {3,0});
    auto action = Command(1, 1, true, {6,0});
    for (auto const& packet : {save, definition, player, action})
        assert(queue.Push(codec, packet.data(), packet.size(), 100));
    std::vector<unsigned> accepted;
    assert(queue.Drain([&](Item const& item) {
        const unsigned channel = Channel(codec, item);
        if (channel == 1) return LimitExceeded;
        if (channel == 7) return Ignored;
        accepted.push_back(channel);
        return Ok;
    }) == LimitExceeded);
    assert((accepted == std::vector<unsigned>{0,6}));
    assert(queue.Depth() == 2 && queue.counters.playerStates == 1);
    assert(queue.counters.peakDepth == 4 && queue.counters.peakBytes >= queue.Bytes());
    assert(queue.Drain([](Item const&) { return Ok; }) == Ok);
    assert(queue.Depth() == 0);
}

static void FragmentRoundtrip()
{
    Codec codec;
    Queue queue;
    std::vector<uint8_t> packet(4 + 24 + 100, 0);
    packet[0] = 0x80;
    packet[4] = 12;
    packet[5] = 4;
    packet[11] = 100;
    for (unsigned i = 28; i < packet.size(); ++i) packet[i] = static_cast<uint8_t>(i);
    assert(queue.Push(codec, packet.data(), packet.size(), 1));
    assert(queue.Drain([&](Item const& item) {
        assert(!item.reliable);
        std::vector<uint8_t> plain;
        assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain) && plain == packet);
        return Ok;
    }) == Ok);
    packet[4] = 0x88; // reliable fragmented definition
    packet[5] = 7;
    assert(queue.Push(codec, packet.data(), packet.size(), 2));
    assert(queue.Drain([&](Item const& item) {
        assert(item.reliable);
        std::vector<uint8_t> plain;
        assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain) && plain == packet);
        return Ok;
    }) == Ok);
}

static void PayloadSavings()
{
    Codec codec;
    g3t::net::NpcRecord record{};
    record.guid[0] = 1;
    record.incarnation = 1;
    record.position[0] = 88000.0f;
    record.hitPoints = record.hitPointsMax = 150;
    std::vector<uint8_t> payload(sizeof(record) + 80 * sizeof(g3t::pose::Bone));
    std::memcpy(payload.data(), &record, sizeof(record));
    for (unsigned i = 0; i < 80; ++i) {
        g3t::pose::Bone bone{};
        bone.name = 1000 + i;
        const double angle = i * 0.071;
        bone.values[0] = bone.values[4] = static_cast<int16_t>(std::cos(angle) * 4096);
        bone.values[1] = static_cast<int16_t>(std::sin(angle) * 4096);
        bone.values[3] = -bone.values[1];
        bone.values[8] = 4096;
        bone.values[9] = static_cast<int16_t>(i * 12);
        std::memcpy(payload.data() + sizeof(record) + i * sizeof(bone), &bone, sizeof(bone));
    }
    size_t original = 0, packed = 0;
    for (size_t at = 0; at < payload.size(); at += 1100) {
        const size_t end = (std::min)(at + 1100, payload.size());
        auto bytes = Command(4, 1, false, {payload.begin() + at, payload.begin() + end});
        auto expected = bytes;
        original += bytes.size();
        codec.Encode(bytes);
        packed += bytes.size();
        std::vector<uint8_t> decoded;
        assert(codec.Decode(bytes.data(), bytes.size(), decoded) && decoded == expected);
    }
    assert(packed * 100 < original * 60); // measurable lossless saving, not a lower update rate
    std::printf("structured 588-byte NPC record + 80 affine bones: %zu -> %zu bytes (%.1f%% saved)\n",
                original, packed, 100.0 * (original - packed) / original);
    std::mt19937 random(78);
    std::vector<uint8_t> noise(1100);
    for (auto& b : noise) b = static_cast<uint8_t>(random());
    auto bytes = Command(4, 2, false, noise);
    auto expected = bytes;
    codec.Encode(bytes);
    assert(bytes.size() <= expected.size());
    std::vector<uint8_t> decoded;
    assert(codec.Decode(bytes.data(), bytes.size(), decoded) && decoded == expected);
}

static void SaturatedNpcStream()
{
    Codec codec;
    Queue queue;
    constexpr unsigned Samples = 1000;
    for (unsigned i = 0; i < Samples; ++i) {
        auto pose = Command(4, i, false, {44, static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)});
        assert(queue.Push(codec, pose.data(), pose.size(), 0));
    }
    unsigned delivered = 0, players = 0;
    for (unsigned window = 0; delivered < Samples; ++window) {
        auto player = Command(0, window, true, {3, static_cast<uint8_t>(window)});
        assert(queue.Push(codec, player.data(), player.size(), window));
        unsigned available = 10; // finite-buffer stand-in, not a production send limit
        const unsigned before = delivered;
        const int result = queue.Drain([&](Item const& item) {
            if (Channel(codec, item) == 0) {
                assert(delivered == before); // newly queued player always bypasses old NPC debt
                ++players;
                return Ok;
            }
            if (!available) return window & 1u ? Ignored : LimitExceeded;
            --available;
            std::vector<uint8_t> plain;
            assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain));
            const unsigned sample = plain[13] | unsigned(plain[14]) << 8;
            assert(sample == delivered); // every due sample survives all failures, in stream order
            ++delivered;
            return Ok;
        });
        assert(result == Ok || Retryable(result));
    }
    assert(delivered == Samples && players == 100 && queue.Depth() == 0);
    assert(queue.counters.accepted[2] == Samples && queue.counters.playerStates == players);
}

static void LatestPoseReplacementAndAgeBound()
{
    Codec codec;
    Queue queue;
    // MsgControlPose (49): NpcsHeader(23) + controlBytes(2) + generation(4) + WireControl::id(2) + payload.
    auto controlPose = [&](uint16_t id, uint8_t seqByte, size_t totalLen = 600) {
        std::vector<uint8_t> p(totalLen, seqByte);
        p[0] = 49;
        p[25] = 1; p[26] = 0; p[27] = 0; p[28] = 0; // generation = 1
        p[29] = static_cast<uint8_t>(id & 0xffu);
        p[30] = static_cast<uint8_t>(id >> 8);
        return Command(4, seqByte, false, p);
    };
    // Multiple poses for the same NPC replace in-place rather than building multi-second FIFO debt.
    for (unsigned step = 1; step <= 20; ++step) {
        auto pkt = controlPose(7, static_cast<uint8_t>(step));
        assert(queue.Push(codec, pkt.data(), pkt.size(), step * 10));
        assert(!(pkt[0] & 0x40u));
    }
    assert(queue.Depth() == 1);
    assert(queue.counters.npcUpdatesDropped == 19);
    assert(queue.counters.plainBytes == queue.counters.wireBytes); // zero range-coder re-compression
    assert(queue.Drain([&](Item const& item) {
        assert(item.Lane() == static_cast<uint16_t>(Priority::Pose));
        assert(!(item.bytes[0] & 0x40u));
        return Ok;
    }) == Ok);
    assert(queue.Depth() == 0 && queue.Bytes() == 0);

    // Poses across many distinct NPCs never exceed MaxUnreliablePoseAgeMs or MaxUnreliablePoseBytes.
    for (unsigned npc = 1; npc <= 300; ++npc) {
        auto pkt = controlPose(static_cast<uint16_t>(npc), static_cast<uint8_t>(npc), 900);
        assert(queue.Push(codec, pkt.data(), pkt.size(), npc * 5));
    }
    queue.EvictStaleUnreliablePoses(300 * 5);
    assert(queue.Bytes() <= MaxUnreliablePoseBytes);
    assert(queue.OldestAge(300 * 5) <= MaxUnreliablePoseAgeMs);
    assert(queue.counters.npcUpdatesDropped > 100);

    // Multi-NPC interleaving: replacing NPC 7 after queuing NPC 8 must append at the tail so
    // ENet unreliable sequence numbers and queue timestamps remain strictly monotonic.
    Queue multi;
    auto a1 = controlPose(7, 1);
    auto b1 = controlPose(8, 2);
    auto a2 = controlPose(7, 3);
    assert(multi.Push(codec, a1.data(), a1.size(), 10));
    assert(multi.Push(codec, b1.data(), b1.size(), 20));
    assert(multi.Push(codec, a2.data(), a2.size(), 30));
    assert(multi.Depth() == 2);

    // Advancing past NPC 8's age deadline (280 - 20 = 260 > 250) must evict NPC 8 while keeping NPC 7 (age 250).
    auto c1 = controlPose(9, 4);
    assert(multi.Push(codec, c1.data(), c1.size(), 280));
    assert(multi.Depth() == 2);

    std::vector<uint16_t> drainedSeqs;
    std::vector<uint32_t> drainedTimes;
    assert(multi.Drain([&](Item const& item) {
        std::vector<uint8_t> plain;
        assert(codec.Decode(item.bytes.data(), item.bytes.size(), plain));
        uint16_t seq = 0;
        const bool ok = g3t::steam::ForEachEnetCommand(plain.data(), plain.size(), nullptr,
            [&](std::size_t, g3t::steam::EnetCommandView const& view) {
                assert(view.command == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE);
                seq = static_cast<uint16_t>((uint16_t(plain[view.offset + 4]) << 8) | plain[view.offset + 5]);
            });
        assert(ok);
        drainedSeqs.push_back(seq);
        drainedTimes.push_back(item.at);
        return Ok;
    }) == Ok);
    assert((drainedSeqs == std::vector<uint16_t>{3, 4}));
    assert((drainedTimes == std::vector<uint32_t>{30, 280}));
}

int ENET_CALLBACK main()
{
    RetentionAndPriority();
    CapacityConfiguration();
    SplittingAndRetransmissions();
    UnclassifiedRetention();
    RepackedRetransmissions();
    IndependentRefusals();
    FragmentRoundtrip();
    PayloadSavings();
    SaturatedNpcStream();
    LatestPoseReplacementAndAgeBound();
    std::puts("Steam queue: retained 25/41 (16=Timeout), independent priority, single reliability, exact compression, fragments and command retransmission dedup passed");
}
