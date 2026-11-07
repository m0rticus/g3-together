// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_bones.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

namespace g3t::npcwire::bones {

struct PoseStats {
    std::size_t headerBytes = 0;
    std::size_t maskBits = 0;
    std::size_t rotationBits = 0;
    std::size_t translationBits = 0;
    std::size_t includedBones = 0;
    bool predictive = false;
    std::size_t maxBasisErrorUnits = 0;
    std::size_t rigidBones = 0;

    std::size_t PaddingBits() const
    {
        return (8 - (maskBits + rotationBits + translationBits) % 8) % 8;
    }
};

// For an originally rigid basis: existing float/Q12 rounding contributes
// <0.501 units. The nearest orthogonal matrix is no farther from the decoded
// matrix than the original rotation; the triangle/Frobenius bound is doubled.
inline double PoseRotationBoundDegrees(double basisError)
{
    return 2 * std::asin(6 * (basisError + 0.501) / (4096 * std::sqrt(8.0))) *
        180 / 3.14159265358979323846;
}

struct PoseRiceWriter {
    std::vector<uint8_t> bytes;
    std::size_t bits = 0;

    void Put(uint32_t value, unsigned width)
    {
        while (width) {
            if (!(bits & 7)) bytes.push_back(0);
            const unsigned take = (std::min)(width, unsigned(8 - (bits & 7)));
            bytes.back() |= static_cast<uint8_t>((value & ((1u << take) - 1)) << (bits & 7));
            value >>= take;
            bits += take;
            width -= take;
        }
    }

    void Align()
    {
        while (bits & 7) Put(0, 1);
    }

    // bounded unary quotient: 15 ones, zero terminator, then 15-or-greater
    // quotient escape in exactly rawWidth bits.
    void Rice(uint32_t value, unsigned k, unsigned rawWidth)
    {
        const unsigned quotient = value >> k;
        const unsigned unary = (std::min)(quotient, 15u);
        Put((1u << unary) - 1, unary + 1);
        if (quotient >= 15) {
            Put(value, rawWidth);
        } else {
            Put(value & ((1u << k) - 1), k);
        }
    }
};

struct PoseRiceReader {
    std::vector<uint8_t> const& bytes;
    std::size_t bits = 0;
    bool ok = true;

    uint32_t Get(unsigned width)
    {
        if (width > 32 || bits + width > bytes.size() * 8) {
            ok = false;
            return 0;
        }
        uint32_t value = 0;
        for (unsigned read = 0; read < width;) {
            const unsigned take = (std::min)(width - read, unsigned(8 - (bits & 7)));
            value |= uint32_t((bytes[bits / 8] >> (bits & 7)) & ((1u << take) - 1)) << read;
            bits += take;
            read += take;
        }
        return value;
    }

    bool Rice(uint32_t& value, unsigned k, unsigned rawWidth)
    {
        unsigned quotient = 0;
        while (quotient < 15 && ok && Get(1)) ++quotient;
        if (!ok) return false;
        if (quotient == 15) {
            if (Get(1)) { ok = false; return false; }
            value = Get(rawWidth);
            if ((value >> k) < 15 || value >= (uint32_t(1) << rawWidth)) {
                ok = false;
                return false;
            }
            return ok;
        }
        const uint32_t remainder = k ? Get(k) : 0;
        if (!ok) return false;
        value = (uint32_t(quotient) << k) | remainder;
        return value < (uint32_t(1) << rawWidth);
    }

    bool AlignAndFinish()
    {
        while (ok && (bits & 7)) if (Get(1)) ok = false;
        return ok && bits == bytes.size() * 8;
    }
};

inline uint32_t PoseZigZag(int32_t difference)
{
    return difference < 0
        ? uint32_t(-int64_t(difference) * 2 - 1)
        : uint32_t(int64_t(difference) * 2);
}

inline int32_t PoseUnzigZag(uint32_t value)
{
    return value & 1
        ? -int32_t((value + 1) / 2)
        : int32_t(value / 2);
}

inline int32_t PoseSaturateInt16(int64_t value)
{
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<int32_t>(value);
}

inline int32_t PoseRoundQ8(int64_t product)
{
    return static_cast<int32_t>(product < 0
        ? -((-product + 128) / 256)
        : (product + 128) / 256);
}

inline uint32_t PoseRiceCost(uint32_t value, unsigned k, unsigned rawWidth)
{
    const uint32_t quotient = value >> k;
    return quotient >= 15
        ? 16u + rawWidth
        : quotient + 1u + k;
}

// Exact 21-bit-escape cost selection. Nonzero quotients below the escape
// threshold occupy at most four k values; larger k values share a zero count.
struct PoseRiceParameters {
    uint64_t savings[16]{};
    uint32_t widths[33]{};

    void Add(uint32_t value)
    {
        unsigned width = 0;
        for (uint32_t bits = value; bits; bits >>= 1) ++width;
        ++widths[width];
        for (unsigned k = width > 4 ? width - 4 : 0; k < width && k < 16; ++k) {
            const uint32_t quotient = value >> k;
            if (quotient < 15) savings[k] += 37 - (quotient + 1 + k);
        }
    }

    unsigned Best() const
    {
        uint32_t zeros = 0;
        uint64_t bestSaving = 0;
        unsigned best = 0;
        for (unsigned k = 0; k < 16; ++k) {
            zeros += widths[k];
            const uint64_t saving = savings[k] + uint64_t(zeros) * (36 - k);
            if (saving > bestSaving) { bestSaving = saving; best = k; }
        }
        return best;
    }
};

inline uint64_t PoseIntegerSqrt(uint64_t value)
{
    uint64_t low = 0, high = 65536;
    while (low + 1 < high) {
        const uint64_t middle = (low + high) / 2;
        if (middle * middle <= value) low = middle;
        else high = middle;
    }
    if ((low + 1) * (low + 1) <= value) ++low;
    return low;
}

inline uint64_t PoseNearestSqrt(uint64_t value)
{
    const uint64_t floor = PoseIntegerSqrt(value);
    if (!floor) return 0;
    const uint64_t above = floor + 1;
    const uint64_t floorDistance = value - floor * floor;
    const uint64_t aboveDistance = above * above - value;
    return aboveDistance < floorDistance ? above : floor;
}

inline int32_t PoseRoundSignedDivide(int64_t numerator, int64_t denominator)
{
    if (numerator >= 0)
        return static_cast<int32_t>((numerator + denominator / 2) / denominator);
    return static_cast<int32_t>(-((-numerator + denominator / 2) / denominator));
}

struct PoseQuat {
    int32_t values[4] = {}; // x, y, z, w
    uint8_t omitted = 3;
};

inline bool PoseCompleteQuat(PoseQuat& quat)
{
    if (quat.omitted > 3) return false;
    int64_t sum = 0;
    for (int k = 0; k < 4; ++k)
        if (k != quat.omitted) sum += int64_t(quat.values[k]) * quat.values[k];
    if (sum >= 16384LL * 16384LL) return false;
    quat.values[quat.omitted] = static_cast<int32_t>(PoseIntegerSqrt(16384LL * 16384LL - sum));
    return quat.values[quat.omitted] != 0;
}

inline bool PoseExtractQuat(pose::Bone const& bone, PoseQuat& result)
{
    const int32_t m0 = bone.values[0], m1 = bone.values[1], m2 = bone.values[2];
    const int32_t m3 = bone.values[3], m4 = bone.values[4], m5 = bone.values[5];
    const int32_t m6 = bone.values[6], m7 = bone.values[7], m8 = bone.values[8];
    int64_t terms[4] = {
        4096 + m0 - m4 - m8,
        4096 - m0 + m4 - m8,
        4096 - m0 - m4 + m8,
        4096 + m0 + m4 + m8
    };
    unsigned omitted = 0;
    for (unsigned i = 1; i < 4; ++i)
        if (terms[i] > terms[omitted]) omitted = i;
    if (terms[omitted] <= 0) return false;

    constexpr int64_t Scale = 16384;
    constexpr int64_t ScaleSquared = Scale * Scale;
    const int64_t bigNumerator = terms[omitted] * 16384;
    if (bigNumerator <= 0) return false;
    const int64_t bigQ = static_cast<int64_t>(PoseNearestSqrt(static_cast<uint64_t>(bigNumerator)));
    if (!bigQ) return false;
    int64_t other[3] = {};
    if (omitted == 3) {
        other[0] = PoseRoundSignedDivide(int64_t(m5 - m7) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide(int64_t(m6 - m2) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide(int64_t(m1 - m3) * 16384, bigQ);
    } else if (omitted == 0) {
        other[0] = PoseRoundSignedDivide(int64_t(m5 - m7) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide(int64_t(m1 + m3) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide(int64_t(m2 + m6) * 16384, bigQ);
    } else if (omitted == 1) {
        other[0] = PoseRoundSignedDivide(int64_t(m6 - m2) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide(int64_t(m1 + m3) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide(int64_t(m5 + m7) * 16384, bigQ);
    } else {
        other[0] = PoseRoundSignedDivide(int64_t(m1 - m3) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide(int64_t(m2 + m6) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide(int64_t(m5 + m7) * 16384, bigQ);
    }
    int64_t sum = 0;
    for (int64_t value : other) sum += value * value;
    if (sum >= ScaleSquared) return false;
    int64_t big = PoseIntegerSqrt(ScaleSquared - sum);
    if (!big) return false;

    PoseQuat value;
    value.omitted = static_cast<uint8_t>(omitted);
    if (omitted == 3) {
        value.values[0] = static_cast<int32_t>(other[0]);
        value.values[1] = static_cast<int32_t>(other[1]);
        value.values[2] = static_cast<int32_t>(other[2]);
        value.values[3] = static_cast<int32_t>(big);
    } else if (omitted == 0) {
        value.values[0] = static_cast<int32_t>(big);
        value.values[1] = static_cast<int32_t>(other[1]);
        value.values[2] = static_cast<int32_t>(other[2]);
        value.values[3] = static_cast<int32_t>(other[0]);
    } else if (omitted == 1) {
        value.values[0] = static_cast<int32_t>(other[1]);
        value.values[1] = static_cast<int32_t>(big);
        value.values[2] = static_cast<int32_t>(other[2]);
        value.values[3] = static_cast<int32_t>(other[0]);
    } else {
        value.values[0] = static_cast<int32_t>(other[1]);
        value.values[1] = static_cast<int32_t>(other[2]);
        value.values[2] = static_cast<int32_t>(big);
        value.values[3] = static_cast<int32_t>(other[0]);
    }
    result = value;
    return true;
}

inline int32_t PoseQuatMatrix(int64_t numerator)
{
    return PoseRoundSignedDivide(numerator, 16384 * 16384);
}

inline bool PoseQuatToBone(PoseQuat const& quat, pose::Bone const& source, pose::Bone& target)
{
    constexpr int64_t scale = 16384;
    constexpr int64_t scaleSquared = scale * scale;
    const int64_t x = quat.values[0], y = quat.values[1], z = quat.values[2], w = quat.values[3];
    const int64_t sum = x*x + y*y + z*z + w*w;
    if (sum == 0) return false;
    if (sum > scaleSquared) return false;

    const int64_t numerator0 = scaleSquared - 2*(y*y + z*z);
    const int64_t numerator1 = 2*(x*y + z*w);
    const int64_t numerator2 = 2*(x*z - y*w);
    const int64_t numerator3 = 2*(x*y - z*w);
    const int64_t numerator4 = scaleSquared - 2*(x*x + z*z);
    const int64_t numerator5 = 2*(y*z + x*w);
    const int64_t numerator6 = 2*(x*z + y*w);
    const int64_t numerator7 = 2*(y*z - x*w);
    const int64_t numerator8 = scaleSquared - 2*(x*x + y*y);
    int64_t numerator[9] = {numerator0,numerator1,numerator2,numerator3,numerator4,numerator5,numerator6,numerator7,numerator8};
    int16_t basis[9];
    for (int i = 0; i < 9; ++i) {
        const int64_t rounded = PoseRoundSignedDivide(4096 * numerator[i], scaleSquared);
        if (rounded < -32768 || rounded > 32767) return false;
        basis[i] = static_cast<int16_t>(rounded);
    }
    target = source;
    for (int i = 0; i < 9; ++i) target.values[i] = basis[i];
    return true;
}

}
