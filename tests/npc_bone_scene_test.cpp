// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_pose.h"
#include "../src/npcwire_adapter.h"
#include "../src/npcsend_policy.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <cstring>
#include <chrono>

using namespace g3t;
using namespace g3t::npcwire::bones;

namespace {
constexpr int BoneCount = 76, NpcCount = 75;
constexpr uint32_t Seconds = 20, AckDelay = 25;

struct Matrix {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p[3] = {};
};

// Synthetic humanoid topology, not a recovered Gothic rig: spine/head, paired
// limbs, fingers and facial branches. Final affine transforms use Q12/Q2.
Matrix Compose(Matrix const& a, Matrix const& b)
{
    Matrix out;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row) {
            out.m[column * 3 + row] = 0;
            for (int k = 0; k < 3; ++k)
                out.m[column * 3 + row] += a.m[k * 3 + row] * b.m[column * 3 + k];
        }
    for (int row = 0; row < 3; ++row) {
        out.p[row] = a.p[row];
        for (int k = 0; k < 3; ++k) out.p[row] += a.m[k * 3 + row] * b.p[k];
    }
    return out;
}

void Skeleton(uint32_t now, int npc, int kind, std::vector<pose::Bone>& bones)
{
    const double t = now / 1000.0, phase = npc * .31;
    const double frequency = kind == 0 ? 1.2 : kind == 1 ? .8 : .2;
    const double amplitude = kind == 0 ? .24 : kind == 1 ? .12 : .004;
    Matrix world[BoneCount];
    for (int b = 0; b < BoneCount; ++b) {
        int parent = b ? b - 1 : -1;
        if (b == 6 || b == 9) parent = 0;
        if (b == 12 || b == 16) parent = 3;
        if (b >= 20 && b < 60) parent = (b - 20) % 4 ? b - 1 : (b < 40 ? 15 : 19);
        if (b >= 60) parent = 5;
        Matrix local;
        if (b) {
            local.p[1] = b < 6 ? 18 : b < 12 ? -34 : b < 20 ? 22 : b < 60 ? 2 : .8;
            if (b == 6 || b == 9) local.p[0] = b == 6 ? -10 : 10;
            if (b == 12 || b == 16) local.p[0] = b == 12 ? -19 : 19;
            if (b >= 60) local.p[0] = (b - 68) * .6;
        }
        const double scale = b < 20 ? 1 : b < 60 ? .18 : .08;
        const double x = amplitude * scale * std::sin(t * frequency * 6.283185307 + phase + b * .43);
        const double y = amplitude * scale * std::sin(t * frequency * 5.1 + phase + b * .27);
        const double z = amplitude * scale * std::sin(t * frequency * 4.3 + phase + b * .19);
        const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
        const double cz = std::cos(z), sz = std::sin(z);
        const double m[9] = {cz * cy, sz * cy, -sy, cz * sy * sx - sz * cx,
                            sz * sy * sx + cz * cx, cy * sx, cz * sy * cx + sz * sx,
                            sz * sy * cx - cz * sx, cy * cx};
        std::memcpy(local.m, m, sizeof(m));
        if (!b) {
            local.p[1] = 100 + amplitude * 2 * std::sin(t * frequency * 6.28);
            const double turn = .3 * std::sin(t * .7 + phase);
            local.m[0] = local.m[8] = std::cos(turn);
            local.m[2] = -std::sin(turn); local.m[6] = std::sin(turn);
            local.m[1] = local.m[3] = local.m[5] = local.m[7] = 0; local.m[4] = 1;
        }
        if (b >= 60) local.p[2] = .1 * std::sin(t * 2 + b); // nonrigid face
        world[b] = parent < 0 ? local : Compose(world[parent], local);
        pose::Transform transform;
        transform.name = static_cast<uint32_t>(b + 1);
        for (int k = 0; k < 9; ++k) transform.values[k] = static_cast<float>(world[b].m[k]);
        for (int k = 0; k < 3; ++k) transform.values[k + 9] = static_cast<float>(world[b].p[k]);
        assert(pose::Encode(transform, bones[b]));
    }
}

struct Legacy {
    struct Frame { uint32_t token; std::vector<pose::Bone> bones; };
    std::deque<Frame> frames;
    uint32_t ack = 0, keyAt = 0;
    bool haveKey = false;
    PoseStats stats;
    uint64_t boneBins[5] = {};
    void Ack(uint32_t token) { if (!ack || PoseNewer(token, ack)) ack = token; }
    size_t Encode(std::vector<pose::Bone> const& bones, uint32_t token, uint32_t now)
    {
        std::vector<pose::Bone> identity(BoneCount);
        for (auto& b : identity) b.values[0] = b.values[4] = b.values[8] = 4096;
        auto const* base = &identity;
        for (auto const& f : frames)
            if (f.token == ack && haveKey && now - keyAt < PoseKeyMs) base = &f.bones;
        unsigned widths[12] = {};
        size_t included = 0, payload = 0, rotationBits = 0, translationBits = 0;
        for (size_t i = 0; i < bones.size(); ++i) {
            bool changed = false;
            for (int k = 0; k < 12; ++k) {
                const int32_t d = int32_t(bones[i].values[k]) - (*base)[i].values[k];
                uint32_t v = d < 0 ? uint32_t(-d) * 2 - 1 : uint32_t(d) * 2;
                if (v) changed = true;
                unsigned width = 0;
                while (v) { ++width; v >>= 1; }
                widths[k] = (std::max)(widths[k], width);
            }
            if (changed) ++included;
        }
        for (size_t i = 0; i < bones.size(); ++i) {
            size_t bits = 0;
            for (int k = 0; k < 12; ++k) {
                if (bones[i].values[k] == (*base)[i].values[k]) continue;
                payload += widths[k]; bits += widths[k];
                if (k < 9) rotationBits += widths[k]; else translationBits += widths[k];
            }
            if (bits) {
                const size_t bytes = (bits + 12 + 7) / 8;
                ++boneBins[bytes < 4 ? 0 : bytes < 8 ? 1 : bytes < 12 ? 2 : bytes < 16 ? 3 : 4];
            }
        }
        const size_t sparse = 7 + 12 + (bones.size() + 7) / 8 + (included * 12 + payload + 7) / 8;
        const size_t raw = 7 + bones.size() * 24;
        stats.headerBytes = 19;
        stats.maskBits = ((bones.size() + 7) / 8) * 8 + included * 12;
        stats.rotationBits = rotationBits; stats.translationBits = translationBits;
        stats.includedBones = included;
        if (sparse >= raw) {
            stats.headerBytes = 7; stats.maskBits = 0;
            stats.rotationBits = bones.size() * 144; stats.translationBits = bones.size() * 48;
            stats.includedBones = bones.size();
        }
        if (base == &identity || sparse >= raw) { keyAt = now; haveKey = true; }
        if (frames.size() == PoseWindow) frames.pop_front();
        frames.push_back({token, bones});
        return (std::min)(sparse, raw);
    }
};

struct Message {
    size_t bytes;
    bool reliable;
    Message(size_t size, bool ordered = false) : bytes(size), reliable(ordered) {}
};

uint64_t Pack(std::vector<Message> const& messages, uint64_t& packets)
{
    uint64_t bytes = 0;
    size_t current = 4;
    auto flush = [&] {
        if (current > 4) { bytes += current + 16; ++packets; current = 4; }
    };
    for (auto const& message : messages) {
        const size_t size = message.bytes;
        for (size_t offset = 0; offset < size; offset += 1172) {
            const size_t part = (std::min)(size_t(1172), size - offset);
            const size_t header = size > 1172 ? 24 : message.reliable ? 6 : 8;
            if (current + header + part > 1200) flush();
            current += header + part;
        }
    }
    flush();
    return bytes;
}

void Scene(int fighters, uint32_t frameMs)
{
    Dictionary dictionary;
    assert(dictionary.Reset(1, 1) == Status::Ok);
    uint32_t names[BoneCount];
    for (int b = 0; b < BoneCount; ++b) names[b] = b + 1;
    assert(dictionary.Register(1, names, BoneCount) == Status::Ok);
    struct Npc {
        net::NpcRecord record{};
        npcsend::State policy;
        PoseCache sender, receiver;
        Legacy legacy;
        std::vector<pose::Bone> bones = std::vector<pose::Bone>(BoneCount);
        std::deque<std::pair<uint32_t, uint32_t>> acks;
        uint32_t token = 0, baselineAckAt = 0, oldBaselineAckAt = 0;
        uint32_t oldAction = UINT32_MAX, oldInstance = 0;
        uint16_t baseline = 0;
        uint32_t anchor = 0;
    };
    std::vector<Npc> npcs(NpcCount);
    npcwire::Context controls;
    assert(controls.Reset(1, 1));
    uint64_t before = 0, after = 0, oldBone = 0, newBone = 0, updates = 0, keys = 0;
    uint64_t oldPackets = 0, newPackets = 0, oldControlBytes = 0, newControlBytes = 0;
    uint64_t oldComponents[5] = {}, newComponents[5] = {};
    size_t worstBasis = 0;
    double encodeMicros = 0;
    for (uint32_t now = 0; now < Seconds * 1000; now += frameMs) {
        std::vector<Message> oldMessages, newMessages;
        for (int i = 0; i < NpcCount; ++i) {
            auto& n = npcs[i];
            while (!n.acks.empty() && n.acks.front().first <= now) {
                assert(n.sender.Ack(n.acks.front().second, 1));
                n.legacy.Ack(n.acks.front().second);
                n.acks.pop_front();
            }
            const int kind = i < fighters ? 0 : (fighters == 5 && i < 45 ? 1 : 2);
            n.record.guid[0] = static_cast<uint8_t>(i + 1);
            n.record.incarnation = 1; n.record.hitPointsMax = 1000;
            n.record.hitPoints = 1000 - static_cast<int>(now / 1000);
            n.record.rotation[3] = 1;
            n.record.sent = now;
            n.record.position[0] = kind < 2 ? now * .1f : 0;
            n.record.action = static_cast<uint8_t>(kind == 0 ? (now / 300) % 3 : 0);
            n.record.contact = kind == 0 && now % 1000 < 150;
            auto& motion = n.record.motions[0];
            motion.name = 1; motion.instance = kind == 0 ? 1 + now / 2000 : 1;
            motion.active = 1; motion.speed = 1; motion.weight = 1;
            motion.phase = static_cast<float>(now % 1000) / 1000;
            Skeleton(now, i, kind, n.bones);
            npcsend::Input input;
            input.record = &n.record; input.bones = n.bones.data(); input.boneCount = BoneCount;
            input.now = now; input.moving = kind < 2; input.combat = kind == 0;
            input.contact = n.record.contact != 0; input.distanceCm = kind < 2 ? 1500 : 4000;
            const auto decision = npcsend::Evaluate(n.policy, input);
            if (decision.decision == npcsend::Decision::Wait) continue;
            assert(npcsend::Commit(n.policy, input, decision.decision));
            const uint32_t token = ++n.token;
            if (!n.baseline || n.oldInstance != motion.instance) {
                ++n.baseline; ++n.anchor;
                npcwire::Control full;
                assert(npcwire::FromNpcRecord(n.record, n.anchor, n.baseline, true, full));
                if (n.baseline == 1) assert(controls.AddId({1, 1}, static_cast<uint16_t>(i + 1), full.guid));
                assert(controls.AdvanceLife({1, 1}, static_cast<uint16_t>(i + 1), 1));
                assert(controls.AddBaseline({1, 1}, static_cast<uint16_t>(i + 1), n.baseline, n.anchor, full));
                n.baselineAckAt = now + AckDelay;
                newMessages.insert(newMessages.end(), {{37, true}, {615, true}});
                newControlBytes += 652;
            }
            if (n.oldAction != n.record.action || n.oldInstance != motion.instance ||
                decision.essential || now % 1000 < frameMs) {
                oldMessages.insert(oldMessages.end(), {{37, true}, {615, true}});
                oldControlBytes += 652;
                n.oldBaselineAckAt = now + AckDelay;
            }
            n.oldAction = n.record.action; n.oldInstance = motion.instance;
            if (decision.essential) {
                npcwire::Control exact;
                std::vector<uint8_t> essential;
                assert(npcwire::FromNpcRecord(n.record, n.anchor, n.baseline, true, exact));
                assert(npcwire::EncodeBatch(controls, token, {exact}, essential, true));
                const size_t essentialBytes = now >= n.baselineAckAt ? 23 + essential.size() : 611;
                oldMessages.push_back({611, true}); newMessages.push_back({essentialBytes, true});
                oldControlBytes += 611; newControlBytes += essentialBytes;
            }
            npcwire::Control control;
            assert(npcwire::FromNpcRecord(n.record, n.anchor, n.baseline, false, control));
            std::vector<uint8_t> batch;
            assert(npcwire::EncodeBatch(controls, token, {control}, batch));
            const size_t newControl = now >= n.baselineAckAt ? 23 + 2 + batch.size() - 14 : 23 + 588;
            const size_t oldControl = now >= n.oldBaselineAckAt ? 23 + 2 + 18 + 24 + 16 + 3 : 23 + 588;
            std::vector<uint8_t> bytes;
            bool key = false;
            PoseStats stats;
            std::vector<pose::Bone> canonical;
            const auto encodeStart = std::chrono::steady_clock::now();
            assert(n.sender.EncodeCompact(dictionary, 1, 1, 0, n.bones.data(), BoneCount, now, bytes, key, canonical, &stats) == Status::Ok);
            encodeMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - encodeStart).count();
            assert(bytes.size() * 8 == stats.headerBytes * 8 + stats.maskBits +
                   stats.rotationBits + stats.translationBits + stats.PaddingBits());
            assert(canonical.size() == n.bones.size());
            assert(stats.maxBasisErrorUnits <= 2);
            worstBasis = (std::max)(worstBasis, stats.maxBasisErrorUnits);
            for (size_t b = 0; b < canonical.size(); ++b)
                for (int k = 0; k < 12; ++k) {
                    const int difference = std::abs(int(canonical[b].values[k]) - n.bones[b].values[k]);
                    assert(difference <= (k < 9 && !key ? 2 : 0));
                }
            assert(n.sender.Store(token, 1, 1, 0, canonical.data(), BoneCount, now, key));
            const size_t oldSize = n.legacy.Encode(n.bones, token, now);
            const auto accumulate = [](uint64_t (&totals)[5], PoseStats const& value) {
                totals[0] += value.headerBytes * 8; totals[1] += value.maskBits;
                totals[2] += value.rotationBits; totals[3] += value.translationBits; totals[4] += value.includedBones;
            };
            accumulate(oldComponents, n.legacy.stats); accumulate(newComponents, stats);
            // Three-percent pose and ACK loss; no retransmission or lower rate.
            if ((token + i * 7) % 31 != 0) {
                std::vector<pose::Bone> decoded;
                uint16_t layout = 0;
                PoseStats decodedStats;
                const auto status = n.receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout, &decodedStats);
                if (status != Status::Ok)
                    std::fprintf(stderr, "decode status=%u npc=%d now=%u token=%u mode=%u flags=%u bytes=%zu\n",
                        unsigned(status), i, now, token, unsigned(bytes[6]), unsigned(bytes[7]), bytes.size());
                assert(status == Status::Ok);
                assert(decoded.size() == canonical.size() &&
                       std::memcmp(decoded.data(), canonical.data(), decoded.size() * sizeof(pose::Bone)) == 0);
                assert(decodedStats.headerBytes == stats.headerBytes && decodedStats.maskBits == stats.maskBits &&
                       decodedStats.rotationBits == stats.rotationBits && decodedStats.translationBits == stats.translationBits &&
                       decodedStats.includedBones == stats.includedBones && decodedStats.predictive == stats.predictive);
                assert(n.receiver.Store(token, 1, 1, 0, decoded.data(), decoded.size(), now, false));
                if ((token + i * 3) % 29 != 0) n.acks.emplace_back(now + AckDelay, token);
            }
            oldMessages.push_back(oldControl + 2 + oldSize);
            newMessages.push_back(newControl + 2 + bytes.size());
            oldBone += oldSize; newBone += bytes.size(); ++updates; if (key) ++keys;
            oldControlBytes += oldControl; newControlBytes += newControl;
        }
        // Compact player state and heartbeat allowance, identical in both.
        oldMessages.push_back(100); newMessages.push_back(100);
        before += Pack(oldMessages, oldPackets); after += Pack(newMessages, newPackets);
    }
    // Full-hash bootstrap and layout/name dictionaries are identical.
    std::vector<Message> bootstrapMessages;
    for (int i = 0; i < NpcCount * 2; ++i) bootstrapMessages.emplace_back(23 + 588 + 4 + BoneCount * 28 + 100);
    for (int i = 0; i < BoneCount; ++i) bootstrapMessages.emplace_back(101, true);
    bootstrapMessages.emplace_back(20 + BoneCount * 4, true);
    uint64_t bootstrapPackets = 0;
    const uint64_t bootstrap = Pack(bootstrapMessages, bootstrapPackets);
    before += bootstrap; after += bootstrap;
    std::printf("BONE_SCENE fighters=%d frame_ms=%u bones=%d updates_per_s=%.1f "
                "bone_before_after_Bpu=%.1f/%.1f envelope_before_after_Bps=%.0f/%.0f "
                "control_before_after_Bps=%.0f/%.0f pose_updates_per_datagram_after=%.2f keys=%llu "
                "max_basis_error_q12=%zu origin_error_cm=0 skin_200cm_bound_cm=%.4f encode_us_per_update=%.1f\n",
                fighters, frameMs, BoneCount, double(updates) / Seconds, double(oldBone) / updates, double(newBone) / updates,
                double(before) / Seconds, double(after) / Seconds, double(oldControlBytes) / Seconds,
                double(newControlBytes) / Seconds, double(updates) / newPackets, static_cast<unsigned long long>(keys),
                worstBasis, double(worstBasis) * 600 / 4096, encodeMicros / updates);
    std::fflush(stdout);
    std::printf("BONE_COMPONENTS before_after_Bpu header=%.1f/%.1f mask=%.1f/%.1f rotation=%.1f/%.1f "
                "translation=%.1f/%.1f included=%.1f/%.1f\n",
                double(oldComponents[0]) / updates / 8, double(newComponents[0]) / updates / 8,
                double(oldComponents[1]) / updates / 8, double(newComponents[1]) / updates / 8,
                double(oldComponents[2]) / updates / 8, double(newComponents[2]) / updates / 8,
                double(oldComponents[3]) / updates / 8, double(newComponents[3]) / updates / 8,
                double(oldComponents[4]) / updates, double(newComponents[4]) / updates);
    uint64_t bins[5] = {};
    for (auto const& n : npcs) for (int b = 0; b < 5; ++b) bins[b] += n.legacy.boneBins[b];
    std::printf("LEGACY_BONE_BYTE_DISTRIBUTION included-bone samples bins_lt4_lt8_lt12_lt16_ge16=%llu/%llu/%llu/%llu/%llu\n",
                static_cast<unsigned long long>(bins[0]), static_cast<unsigned long long>(bins[1]),
                static_cast<unsigned long long>(bins[2]), static_cast<unsigned long long>(bins[3]),
                static_cast<unsigned long long>(bins[4]));
    std::fflush(stdout);
    assert(after < before);
    assert(newBone < oldBone);
    if (frameMs == 33) assert(after / Seconds < 1000000);
}
}

int main()
{
    Scene(5, 22);
    Scene(65, 33);
    Scene(70, 22);
}
