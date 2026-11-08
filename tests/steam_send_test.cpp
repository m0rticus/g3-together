// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/steam_send.h"
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
    assert(queue.Depth() == 3 && queue.counters.retries == 2);
    std::vector<unsigned> order;
    assert(queue.Drain([&](Item const& item) {
        order.push_back(Channel(codec, item));
        assert(item.Flags() & AutoRestart);
        assert(item.reliable ? !(item.Flags() & NoDelay) : (item.Flags() & NoDelay));
        return Ok;
    }) == Ok);
    assert((order == std::vector<unsigned>{0,7,4}));
    assert(queue.Depth() == 0 && queue.Bytes() == 0);
    assert(queue.counters.reliableAccepted == 1 && queue.counters.unreliableAccepted == 2);
    assert(queue.counters.playerStates == 1);
    assert(queue.counters.failed[25] == 1 && queue.counters.failed[16] == 1);

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
    assert(!queue.Push(codec, invalid.data(), invalid.size(), 40));
    assert(queue.Depth() == 1);
    assert(!queue.Push(codec, nullptr, 0, 40));
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

int ENET_CALLBACK main()
{
    RetentionAndPriority();
    SplittingAndRetransmissions();
    FragmentRoundtrip();
    PayloadSavings();
    SaturatedNpcStream();
    std::puts("Steam queue: retained 25/16, priority isolation, exact compression, fragments and retransmission dedup passed");
}
