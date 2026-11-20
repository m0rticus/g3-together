// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_bonepack.h"
#include "../src/npcwire_pose.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <array>
#include <vector>

using namespace g3t;
using namespace g3t::npcwire::bones;

static Dictionary Layout(std::size_t count)
{
    Dictionary dictionary;
    assert(dictionary.Reset(1, 1) == Status::Ok);
    std::vector<uint32_t> names(count);
    for (std::size_t i = 0; i < count; ++i) names[i] = static_cast<uint32_t>(i + 1);
    assert(dictionary.Register(1, names.data(), count) == Status::Ok);
    return dictionary;
}

static void SetUnitQuaternion(pose::Bone& bone, double x, double y, double z, double w)
{
    const double length = std::sqrt(x*x + y*y + z*z + w*w);
    x /= length; y /= length; z /= length; w /= length;
    bone.values[0] = static_cast<int16_t>(std::lround(4096 * (1 - 2*y*y - 2*z*z)));
    bone.values[1] = static_cast<int16_t>(std::lround(4096 * (2*x*y + 2*z*w)));
    bone.values[2] = static_cast<int16_t>(std::lround(4096 * (2*x*z - 2*y*w)));
    bone.values[3] = static_cast<int16_t>(std::lround(4096 * (2*x*y - 2*z*w)));
    bone.values[4] = static_cast<int16_t>(std::lround(4096 * (1 - 2*x*x - 2*z*z)));
    bone.values[5] = static_cast<int16_t>(std::lround(4096 * (2*y*z + 2*x*w)));
    bone.values[6] = static_cast<int16_t>(std::lround(4096 * (2*x*z + 2*y*w)));
    bone.values[7] = static_cast<int16_t>(std::lround(4096 * (2*y*z - 2*x*w)));
    bone.values[8] = static_cast<int16_t>(std::lround(4096 * (1 - 2*x*x - 2*y*y)));
    bone.values[9] = bone.values[10] = bone.values[11] = 0;
}

static void RiceAndIntegerBounds()
{
    std::mt19937 rng(7416);
    for (unsigned trial = 0; trial < 4000; ++trial) {
        PoseRiceParameters parameters;
        uint64_t costs[16]{};
        for (unsigned i = 0; i < 76; ++i) {
            const uint32_t value = trial % 3 ? rng() % 262141 : rng() % 16;
            parameters.Add(value);
            for (unsigned k = 0; k < 16; ++k) costs[k] += PoseRiceCost(value, k, 21);
        }
        unsigned best = 0;
        for (unsigned k = 1; k < 16; ++k) if (costs[k] < costs[best]) best = k;
        std::size_t bestBits = 0;
        assert(parameters.Best(bestBits) == best && bestBits == costs[best]);
    }
    for (uint32_t value = 0; value <= 262140; ++value) {
        PoseRiceParameters parameters; parameters.Add(value);
        unsigned best = 0;
        for (unsigned k = 1; k < 16; ++k)
            if (PoseRiceCost(value, k, 21) < PoseRiceCost(value, best, 21)) best = k;
        std::size_t bestBits = 0;
        assert(parameters.Best(bestBits) == best && bestBits == PoseRiceCost(value, best, 21));
        PoseRiceWriter writer;
        writer.Put(value & 7, 3); writer.Rice(value, best, 21); writer.Align();
        PoseRiceReader reader{writer.bytes};
        assert(reader.Get(3) == (value & 7));
        uint32_t decoded = 0;
        assert(reader.Rice(decoded, best, 21) && decoded == value && reader.AlignAndFinish());
    }
    for (int64_t difference = -65535; difference <= 65535; ++difference) {
        const uint32_t zig = PoseZigZag(static_cast<int32_t>(difference));
        assert(PoseUnzigZag(zig) == static_cast<int32_t>(difference));
    }
    assert(PoseSaturateInt16(-100000) == -32768);
    assert(PoseSaturateInt16(100000) == 32767);
    for (int value = -32768; value <= 32767; ++value) {
        auto value16 = static_cast<int16_t>(value);
        assert(PoseUnzigZag(PoseZigZag(value16)) == value16);
    }
    {
        pose::Transform t{};
        t.name = 42;
        t.values[0] = 0.5f / pose::BasisScale;
        t.values[1] = -0.5f / pose::BasisScale;
        t.values[2] = 32767.49f / pose::BasisScale;
        t.values[3] = -32768.49f / pose::BasisScale;
        t.values[9] = 0.5f / pose::PositionScale;
        t.values[10] = -0.5f / pose::PositionScale;
        pose::Bone encoded{};
        assert(pose::Encode(t, encoded));
        assert(encoded.values[0] == 1 && encoded.values[1] == -1);
        assert(encoded.values[2] == 32767 && encoded.values[3] == -32768);
        assert(encoded.values[9] == 1 && encoded.values[10] == -1);
        t.values[0] = NAN;
        assert(!pose::Encode(t, encoded));
        t.values[0] = 32768.0f / pose::BasisScale;
        assert(!pose::Encode(t, encoded));
    }
    // PoseRiceReader::Rice sets ok = false on out-of-range value and truncated escape.
    {
        PoseRiceWriter w;
        w.Rice(1u << 21, 0, 21);
        w.Align();
        PoseRiceReader r{w.bytes};
        uint32_t out = 0;
        assert(!r.Rice(out, 0, 21) && !r.ok);

        PoseRiceWriter trunc;
        trunc.Put(0x7fff, 15);
        trunc.Align();
        PoseRiceReader rTrunc(trunc.bytes.data(), 1);
        assert(!rTrunc.Rice(out, 0, 21) && !rTrunc.ok);
    }
}

static void QuaternionFidelity()
{
    std::mt19937 random(76);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    constexpr std::size_t count = 80;
    auto dictionary = Layout(count);
    std::vector<pose::Bone> frameA(count), frameB(count), target(count), decoded, canonical;
    for (std::size_t i = 0; i < count; ++i) {
        frameA[i].name = frameB[i].name = target[i].name = static_cast<uint32_t>(i + 1);
        double x = unit(random), y = unit(random), z = unit(random), w = unit(random);
        if (i & 1) w = -w;
        SetUnitQuaternion(frameA[i], x, y, z, w);
        x = unit(random); y = unit(random); z = unit(random); w = unit(random);
        SetUnitQuaternion(frameB[i], x, y, z, w);
        x = unit(random); y = unit(random); z = unit(random); w = unit(random);
        SetUnitQuaternion(target[i], x, y, z, w);
    }
    PoseCache sender, receiver;
    bool key = false;
    std::vector<uint8_t> bytes;
    uint16_t id = 0;
    PoseStats stats{};
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, frameA.data(), count, 0, bytes, key, canonical, &stats) == Status::Ok);
    assert(key);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(sender.Store(1, 1, 1, 0, frameA.data(), count, 0, true));
    assert(receiver.Store(1, 1, 1, 0, decoded.data(), count, 0, false));
    sender.Ack(1, 1);

    assert(sender.EncodeCompact(dictionary, 1, 1, 0, frameB.data(), count, 16, bytes, key, canonical, &stats) == Status::Ok);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(sender.Store(2, 1, 1, 0, canonical.data(), count, 16, key));
    assert(receiver.Store(2, 1, 1, 0, decoded.data(), count, 16, false));
    sender.Ack(2, 1);

    assert(sender.EncodeCompact(dictionary, 1, 1, 0, target.data(), count, 32, bytes, key, canonical, &stats) == Status::Ok);
    assert(bytes[6] == 4);
    assert(stats.predictive && stats.rigidBones > 0);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(decoded.size() == count);
    std::size_t maxError = 0;
    for (std::size_t i = 0; i < count; ++i) {
        for (int k = 0; k < 9; ++k)
            maxError = (std::max)(maxError, std::size_t(std::abs(int32_t(decoded[i].values[k]) -
                                                                  int32_t(target[i].values[k]))));
        for (int k = 9; k < 12; ++k)
            assert(decoded[i].values[k] == target[i].values[k]);
    }
    assert(maxError <= 2);
    assert(std::memcmp(decoded.data(), canonical.data(), decoded.size() * sizeof(pose::Bone)) == 0);
    assert(stats.maxBasisErrorUnits <= 2);
    assert(stats.rigidBones > 0);

    const auto valid = bytes;
    for (std::size_t n = 0; n < valid.size(); ++n) {
        decoded = canonical;
        assert(receiver.Decode(dictionary, valid.data(), n, 1, 0, decoded, id) != Status::Ok);
        assert(std::memcmp(decoded.data(), canonical.data(), decoded.size() * sizeof(pose::Bone)) == 0);
    }
    decoded = canonical;
    bytes = valid;
    bytes.push_back(0);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
    bytes = valid;
    bytes[7] = 8;
    decoded = canonical;
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id) == Status::Malformed);
}

static void AffineFallbackAndAtomic()
{
    auto dictionary = Layout(80);
    std::vector<pose::Bone> target(80), decoded, canonical;
    for (std::size_t i = 0; i < target.size(); ++i) {
        target[i].name = static_cast<uint32_t>(i + 1);
        for (int k = 0; k < 12; ++k)
            target[i].values[k] = static_cast<int16_t>((int(i) * 7919 + k * 3571) & 0xffff);
    }
    PoseCache sender, receiver;
    bool key = false;
    std::vector<uint8_t> bytes;
    uint16_t id = 0;
    PoseStats stats{};
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, target.data(), 80, 0, bytes, key, canonical, &stats) == Status::Ok);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(sender.Store(1, 1, 1, 0, target.data(), 80, 0, true));
    assert(receiver.Store(1, 1, 1, 0, decoded.data(), 80, 0, false));
    sender.Ack(1, 1);

    auto frame2 = target;
    for (auto& bone : frame2) bone.values[11] ^= 0x3210;
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, frame2.data(), 80, 16, bytes, key, canonical, &stats) == Status::Ok);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(sender.Store(2, 1, 1, 0, canonical.data(), 80, 16, key));
    assert(receiver.Store(2, 1, 1, 0, decoded.data(), 80, 16, false));
    sender.Ack(2, 1);

    auto scaled = target;
    for (auto& bone : scaled) {
        bone.values[0] = 8000; bone.values[4] = -9000; bone.values[8] = 12000;
        bone.values[9] = static_cast<int16_t>(static_cast<int>(bone.values[9]) + 7);
    }
    assert(sender.EncodeCompact(dictionary, 1, 1, 0, scaled.data(), 80, 32, bytes, key, canonical, &stats) == Status::Ok);
    assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
    assert(!std::memcmp(decoded.data(), scaled.data(), decoded.size() * sizeof(pose::Bone)));
    assert(stats.rigidBones == 0);
    assert(stats.maxBasisErrorUnits == 0);
}

static void ExhaustEveryInt16()
{
    auto dictionary = Layout(1);
    std::vector<pose::Bone> source(1), decoded, canonical;
    source[0].name = 1;
    PoseCache sender, receiver;
    bool key = false;
    std::vector<uint8_t> bytes;
    uint16_t id = 0;
    for (int value = -32768; value <= 32767; ++value) {
        source[0].values[11] = static_cast<int16_t>(value);
        PoseStats stats{};
        assert(sender.Encode(dictionary, 1, 1, 0, source.data(), 1, static_cast<uint32_t>(value & 65535),
                             bytes, key, &stats) == Status::Ok);
        assert(receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, id, &stats) == Status::Ok);
        assert(decoded[0].values[11] == static_cast<int16_t>(value));
        assert(stats.rotationBits == 0 || stats.rotationBits <= 144);
    }
}

static void BoundedStreamAndRecovery()
{
    constexpr size_t count = 80;
    auto dictionary = Layout(count);
    uint32_t names[count];
    for (size_t i = 0; i < count; ++i) names[i] = static_cast<uint32_t>(i + 201);
    assert(dictionary.Register(2, names, count) == Status::Ok);
    PoseCache sender, receiver;
    std::mt19937 rng(76074);
    std::uniform_real_distribution<double> unit(-1, 1);
    std::vector<pose::Bone> source(count), canonical, decoded;
    std::vector<uint8_t> bytes;
    size_t compact = 0, coarse = 0;
    double maxOriginErrorCm = 0, maxAxisErrorDegrees = 0;
    for (unsigned step = 0; step < 180; ++step) {
        const uint16_t layout = step >= 140 ? 2 : 1;
        const uint8_t flags = step >= 155 ? pose::Facial : 0;
        const uint32_t life = step >= 165 ? 2 : 1;
        for (size_t i = 0; i < count; ++i) {
            source[i].name = static_cast<uint32_t>(i + (layout == 2 ? 201 : 1));
            SetUnitQuaternion(source[i], unit(rng), unit(rng), unit(rng), unit(rng));
            if (i % 11 == 0) source[i].values[0] = 7000; // exact scale/shear escape
            for (int k = 9; k < 12; ++k)
                source[i].values[k] = static_cast<int16_t>(int(rng() % 65536) - 32768);
        }
        bool key = false;
        PoseStats stats;
        assert(sender.EncodeCompact(dictionary, layout, life, flags, source.data(), count, step * 33,
                                    bytes, key, canonical, &stats) == Status::Ok);
        assert(bytes.size() * 8 == stats.headerBytes * 8 + stats.maskBits + stats.rotationBits +
               stats.translationBits + stats.PaddingBits());
        if (step == 0 || step == 81 || step == 140 || step == 155 || step == 165) assert(key);
        if (bytes[6] == 4) { ++compact; if (bytes[7] & 2) ++coarse; }
        for (size_t i = 0; i < count; ++i)
            for (int k = 0; k < 12; ++k) {
                const int error = std::abs(int(canonical[i].values[k]) - source[i].values[k]);
                assert(error <= (k < 9 && !key && i % 11 ? 2 : 0));
                if (k >= 9) maxOriginErrorCm = (std::max)(maxOriginErrorCm, error / 4.0);
            }
        for (size_t i = 0; i < count; ++i) if (i % 11)
            for (int axis = 0; axis < 3; ++axis) {
                double dot = 0, sourceLength = 0, decodedLength = 0;
                for (int row = 0; row < 3; ++row) {
                    const double a = source[i].values[axis * 3 + row];
                    const double b = canonical[i].values[axis * 3 + row];
                    dot += a * b; sourceLength += a * a; decodedLength += b * b;
                }
                const double cosine = (std::max)(-1.0, (std::min)(1.0, dot / std::sqrt(sourceLength * decodedLength)));
                maxAxisErrorDegrees = (std::max)(maxAxisErrorDegrees, std::acos(cosine) * 180 / 3.141592653589793);
            }
        assert(sender.Store(step + 1, life, layout, flags, canonical.data(), count, step * 33, key));
        if (step % 17 == 16) continue; // packet loss cannot advance the ACK reference
        if (step == 80) receiver = PoseCache{};
        uint16_t decodedLayout = 0;
        const auto status = receiver.Decode(dictionary, bytes.data(), bytes.size(), life, flags, decoded, decodedLayout);
        if (step == 80) {
            assert(status == Status::UnknownPose && sender.Ack(0, life));
            continue;
        }
        assert(status == Status::Ok && decodedLayout == layout);
        assert(std::memcmp(decoded.data(), canonical.data(), count * sizeof(pose::Bone)) == 0);
        assert(receiver.Store(step + 1, life, layout, flags, decoded.data(), count, step * 33, false));
        if (step % 19 != 18) assert(sender.Ack(step + 1, life)); // independent ACK loss
        if (step > 2) sender.Ack(step - 1, life); // old/reordered ACK never advances
    }
    assert(compact && coarse);

    bool key = true;
    PoseStats stats; stats.headerBytes = 999;
    const auto previousBytes = bytes;
    const auto previousCanonical = canonical;
    assert(sender.EncodeCompact(dictionary, 2, 2, pose::Facial, source.data(), pose::MaxBones + 1,
                                6000, bytes, key, canonical, &stats) == Status::InvalidArgument);
    assert(bytes == previousBytes && key && stats.headerBytes == 999);
    assert(std::memcmp(canonical.data(), previousCanonical.data(), count * sizeof(pose::Bone)) == 0);

    // Entrywise guard -> Frobenius -> nearest-rotation triangle inequality.
    assert(PoseRotationBoundDegrees(2) < 0.15);
    const double axisBoundDegrees = std::asin(std::sqrt(3.0) * 2.501 / 4096) * 180 / 3.141592653589793;
    assert(axisBoundDegrees < 0.061);
    const double vertexBoundCm = 3.0 * 2 / 4096 * 200;
    assert(vertexBoundCm < 0.293);
    assert(maxOriginErrorCm == 0 && maxAxisErrorDegrees < 0.061);
}

int main()
{
    RiceAndIntegerBounds();
    QuaternionFidelity();
    AffineFallbackAndAtomic();
    ExhaustEveryInt16();
    BoundedStreamAndRecovery();
    return 0;
}
