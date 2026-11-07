// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_pose.h"
#include "../src/npcsend_policy.h"
#include <cassert>
#include <cstdio>
#include <random>

using namespace g3t;
using namespace g3t::npcwire::bones;

static bool Same(std::vector<pose::Bone> const& a, std::vector<pose::Bone> const& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(pose::Bone)) == 0;
}

static Dictionary Layout(size_t count)
{
    Dictionary d;
    assert(d.Reset(1, 1) == Status::Ok);
    std::vector<uint32_t> names;
    for (size_t i = 0; i < count; ++i) names.push_back(static_cast<uint32_t>(i + 1));
    assert(d.Register(1, names.data(), names.size()) == Status::Ok);
    return d;
}

static void Codec()
{
    auto d = Layout(80);
    std::vector<pose::Bone> bones(80), decoded;
    std::mt19937 random(74);
    for (size_t i = 0; i < bones.size(); ++i) {
        bones[i].name = static_cast<uint32_t>(i + 1);
        for (auto& v : bones[i].values) v = static_cast<int16_t>(random());
    }
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key = false;
    uint16_t id = 0;
    assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 0, bytes, key) == Status::Ok && key);
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
    assert(bytes[DeltaHeaderBytes] == 0); // arbitrary affine values select a raw key
    for (size_t size = 0; size < bytes.size(); ++size) {
        const auto previous = decoded;
        assert(receiver.Decode(d, bytes.data(), size, 1, 0, decoded, id) != Status::Ok);
        assert(Same(previous, decoded));
    }
    assert(sender.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, true));
    assert(receiver.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, false));
    assert(sender.Ack(1, 1));
    // Quantization error introduced by this codec is identically zero for
    // every basis/translation integer, including shear and signed extrema.
    for (int k = 0; k < 12; ++k) {
        const int16_t previous = bones[0].values[k];
        for (int value = -32768; value <= 32767; ++value) {
            bones[0].values[k] = static_cast<int16_t>(value);
            assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 1, bytes, key) == Status::Ok && !key);
            assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
            auto a = pose::Decode(bones[0]), b = pose::Decode(decoded[0]);
            for (int n = 0; n < 12; ++n) assert(a.values[n] == b.values[n]);
        }
        bones[0].values[k] = previous;
    }
    bones[0].values[0] ^= 0x7fff;
    assert(sender.Encode(d, 1, 1, 0, bones.data(), bones.size(), 2, bytes, key) == Status::Ok && !key);
    const auto valid = bytes;
    const auto unchanged = decoded;
    for (size_t size = 0; size < valid.size(); ++size) {
        decoded = unchanged;
        assert(receiver.Decode(d, valid.data(), size, 1, 0, decoded, id) != Status::Ok);
        assert(Same(decoded, unchanged));
    }
    bytes = valid; bytes.push_back(0);
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[DeltaHeaderBytes + 1] = 18;
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid; bytes[DeltaHeaderBytes + 1 + 12 + 10] |= 0x80;
    assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);

    // A single residual can span the complete int16 range in either direction.
    for (int k = 0; k < 12; ++k) {
        bones[0].values[k] = -32768;
        bones[1].values[k] = 32767;
    }
    PoseCache extrema;
    assert(extrema.Store(1, 1, 1, 0, bones.data(), bones.size(), 0, true));
    assert(extrema.Ack(1, 1));
    for (int k = 0; k < 12; ++k) {
        bones[0].values[k] = 32767;
        bones[1].values[k] = -32768;
    }
    assert(extrema.Encode(d, 1, 1, 0, bones.data(), bones.size(), 1, bytes, key) == Status::Ok && !key);
    assert(extrema.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(bones, decoded));
}

static void Recovery()
{
    auto d = Layout(1);
    std::vector<pose::Bone> bones(1), decoded;
    bones[0].name = 1;
    PoseCache sender, receiver;
    std::vector<uint8_t> bytes;
    bool key;
    uint16_t id = 0;
    auto receive = [&](uint32_t token) {
        assert(receiver.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
        assert(Same(decoded, bones));
        assert(receiver.Store(token, 1, 1, 0, decoded.data(), 1, token, false));
    };
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, 0, bytes, key) == Status::Ok && key);
    sender.Store(1, 1, 1, 0, bones.data(), 1, 0, key); receive(1);
    sender.Ack(1, 1);
    for (uint32_t token = 2; token < 60; ++token) {
        ++bones[0].values[9];
        assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, token, bytes, key) == Status::Ok);
        assert(sender.Store(token, 1, 1, 0, bones.data(), 1, token, key));
        // Loss of a delta and its ACK does not affect successors.
        if (token % 5 == 0) continue;
        receive(token);
        if (token % 7 != 0) sender.Ack(token, 1);
        assert(sender.Retained() <= PoseWindow && receiver.Retained() <= PoseWindow);
    }
    assert(!sender.Ack(1, 1) && !sender.Ack(59, 2));
    assert(!receiver.Store(1, 1, 1, 0, bones.data(), 1, 60, false)); // reorder/duplicate
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, PoseKeyMs, bytes, key) == Status::Ok && key);
    PoseCache rejoin;
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    sender.Store(60, 1, 1, 0, bones.data(), 1, PoseKeyMs, true); sender.Ack(60, 1);
    ++bones[0].values[9];
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, 1001, bytes, key) == Status::Ok && !key);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::UnknownPose);
    assert(sender.Ack(0, 1));
    assert(sender.Encode(d, 1, 1, 0, bones.data(), 1, 1002, bytes, key) == Status::Ok && key);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
    assert(sender.Encode(d, 1, 2, pose::Facial, bones.data(), 1, 1003, bytes, key) == Status::Ok && key);
    sender.Store(61, 2, 1, pose::Facial, bones.data(), 1, 1003, key);
    assert(sender.Retained() == 1 && !sender.Ack(60, 1));
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 2, pose::Facial, decoded, id) == Status::Ok);
    rejoin.Store(61, 2, 1, pose::Facial, decoded.data(), 1, 1003, false);
    assert(rejoin.Decode(d, bytes.data(), bytes.size(), 1, pose::Facial, decoded, id) == Status::WrongGeneration);
    auto other = Layout(1);
    other.Clear();
    assert(rejoin.Decode(other, bytes.data(), bytes.size(), 2, pose::Facial, decoded, id) == Status::UnknownLayout);

    for (size_t count : {1u, 7u, 8u, 9u, 255u, 256u}) {
        auto layout = Layout(count);
        std::vector<pose::Bone> rig;
        layout.IdentityPose(1, rig);
        PoseCache source, sink;
        assert(source.Encode(layout, 1, 1, 0, rig.data(), count, 0, bytes, key) == Status::Ok && key);
        assert(sink.Decode(layout, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(decoded, rig));
        assert(source.Store(UINT32_MAX - 1, 1, 1, 0, rig.data(), count, 0, true));
        assert(sink.Store(UINT32_MAX - 1, 1, 1, 0, rig.data(), count, 0, false));
        source.Ack(UINT32_MAX - 1, 1);
        rig.back().values[11] = 32767;
        assert(source.Encode(layout, 1, 1, 0, rig.data(), count, 1, bytes, key) == Status::Ok);
        assert(sink.Decode(layout, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok && Same(decoded, rig));
        source.Store(UINT32_MAX, 1, 1, 0, rig.data(), count, 1, key);
        assert(source.Store(1, 1, 1, 0, rig.data(), count, 2, false)); // zero token skipped on wrap
        assert(source.Ack(1, 1));
    }
}

static void Simulation()
{
    constexpr int count = 75, seconds = 20, hz = 60;
    auto dictionary = Layout(80);
    struct Npc {
        net::NpcRecord record{};
        npcsend::State policy;
        PoseCache sender, receiver;
        std::vector<pose::Bone> bones = std::vector<pose::Bone>(80);
        uint32_t token = 0;
        std::deque<std::pair<uint32_t, uint32_t>> acks;
    };
    std::vector<Npc> npcs(count);
    uint64_t before = 0, rateOnly = 0, unbatched = 0, after = 0, sends = 0, keys = 0, deltas = 0, byTier[4] = {};
    uint64_t boneBytes = 0, logical = 0, advertised[3] = {};
    auto envelope = [](size_t payload) {
        return payload + ((payload + 1171) / 1172) * 100; // conservative 4+24 ENet + 72 relay
    };
    for (int tick = 0; tick < seconds * hz; ++tick) {
        const uint32_t now = static_cast<uint32_t>(tick * 1000 / hz);
        size_t datagram = 4;
        auto command = [&](size_t payload, size_t header) {
            if (datagram + header + payload > 1200) {
                after += datagram + 72;
                datagram = 4;
            }
            datagram += header + payload;
        };
        for (int i = 0; i < count; ++i) {
            auto& n = npcs[i];
            while (!n.acks.empty() && n.acks.front().first <= now) {
                n.sender.Ack(n.acks.front().second, 1);
                n.acks.pop_front();
            }
            // 5 fighting, 35 walking, 15 breathing/idling, 15 static, 5 far walking.
            const bool fighting = i < 5, walking = (i >= 5 && i < 40) || i >= 70, idle = i >= 40 && i < 55;
            n.record.guid[0] = static_cast<uint8_t>(i + 1); n.record.rotation[3] = 1;
            n.record.sent = now;
            if (walking) n.record.position[0] = tick * 2.0f;
            if (walking || fighting || idle) {
                n.record.motions[0].name = n.record.motions[0].instance = 1;
                n.record.motions[0].active = 1; n.record.motions[0].weight = 1;
                n.record.motions[0].phase = float(tick % 120) / 120;
            }
            for (size_t b = 0; b < n.bones.size(); ++b) {
                const double t = tick / 60.0, offset = b * 0.17;
                const double amplitude = fighting ? .4 : walking ? .18 : idle ? .012 : 0;
                const double angle = amplitude * std::sin(t * (fighting ? 6 : 3) + offset);
                pose::Transform bone; bone.name = static_cast<uint32_t>(b + 1);
                bone.values[0] = bone.values[8] = static_cast<float>(std::cos(angle));
                bone.values[2] = static_cast<float>(std::sin(angle)); bone.values[6] = -bone.values[2];
                bone.values[4] = 1;
                for (int k = 9; k < 12; ++k)
                    bone.values[k] = static_cast<float>(b * .5 + amplitude * 10 * std::sin(t * 3 + offset + k));
                assert(pose::Encode(bone, n.bones[b]));
            }
            // Baseline calibrated to the observed 2,702 poses/s class; no live logs in tests.
            if (tick % 5 < 3) before += envelope(23 + 18 + 24 + 3 + 3 + 80 * 24);
            npcsend::Input in;
            in.record = &n.record; in.bones = n.bones.data(); in.boneCount = 80;
            in.now = now; in.moving = walking; in.combat = fighting; in.distanceCm = i >= 40 ? 4000 : 1500;
            const auto result = npcsend::Evaluate(n.policy, in);
            if (result.decision == npcsend::Decision::Wait) continue;
            assert(npcsend::Commit(n.policy, in, result.decision));
            ++sends; ++byTier[static_cast<unsigned>(result.tier)];
            const uint32_t hint = npcsend::PresentationInterval(n.policy, in);
            ++advertised[hint <= 16 ? 0 : hint < 100 ? 1 : 2];
            std::vector<uint8_t> bytes;
            bool key = false;
            assert(n.sender.Encode(dictionary, 1, 1, 0, n.bones.data(), 80, now, bytes, key) == Status::Ok);
            const uint32_t token = ++n.token;
            assert(n.sender.Store(token, 1, 1, 0, n.bones.data(), 80, now, key));
            std::vector<pose::Bone> decoded;
            uint16_t id;
            // 5% snapshot loss, then 5% ACK loss: no chained deltas.
            if (token % 20 != 0) {
                assert(n.receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Ok);
                assert(Same(decoded, n.bones));
                n.receiver.Store(token, 1, 1, 0, decoded.data(), 80, now, false);
                if (token % 19 != 0) n.acks.emplace_back(now + 17, token);
            }
            const size_t control = 23 + 2 + 18 + 24 + 3 + 2;
            rateOnly += envelope(control + 2 + 80 * 24);
            const size_t payload = control + bytes.size();
            unbatched += envelope(payload);
            if (payload <= 1172) command(payload, 8);
            else for (size_t offset = 0; offset < payload; offset += 1172)
                command(std::min<size_t>(1172, payload - offset), 24);
            boneBytes += bytes.size(); logical += control + bytes.size();
            if (key) ++keys; else ++deltas;
        }
        if (datagram > 4) after += datagram + 72;
    }
    // Initial world dictionaries/control baselines, plus ordinary player state
    // traffic. Stable descriptors need no repeated baseline offers in this scene.
    const uint64_t bootstrap = count * (37 + 615 + 611 + 2 * (2855 + 300)) + 341 + 81 * 101;
    // Two reliable contact/life/control transitions per fighter per second:
    // full essential record, baseline offer, ID and conservative ENet/relay envelope.
    const uint64_t criticalControls = seconds * 5 * 2 * (611 + 615 + 37 + 200);
    const uint64_t allowances = bootstrap + criticalControls + seconds * 60 * (402 + 8 + 4 + 72);
    after += allowances;
    std::printf("75-NPC synthetic 30 Hz ordinary, 20s, exact affine poses, 17 ms ACK delay, 5%% pose/ACK loss: "
                "baseline %.0f B/s, rate-only %.0f B/s, content/unbatched %.0f B/s, packed+bootstrap+controls+player %.0f B/s "
                "(pose application %.0f B/s, unbatched+same allowances %.0f B/s); "
                "updates/s %.1f, fast/ordinary/far/idle %.1f/%.1f/%.1f/%.1f; bone bytes/update %.1f keys=%llu deltas=%llu\n",
                double(before) / seconds, double(rateOnly) / seconds, double(unbatched) / seconds,
                double(after) / seconds, double(logical) / seconds, double(unbatched + allowances) / seconds,
                double(sends) / seconds, double(byTier[0]) / seconds, double(byTier[1]) / seconds,
                double(byTier[2]) / seconds, double(byTier[3]) / seconds, double(boneBytes) / sends,
                static_cast<unsigned long long>(keys), static_cast<unsigned long long>(deltas));
    std::fflush(stdout);
    std::printf("Advertised-tier fast/ordinary/slow updates/s %.1f/%.1f/%.1f (matches live pose log classification)\n",
                double(advertised[0]) / seconds, double(advertised[1]) / seconds, double(advertised[2]) / seconds);
    assert(after < before && after / seconds < 1000000);
}

int main()
{
    Codec();
    Recovery();
    Simulation();
}
