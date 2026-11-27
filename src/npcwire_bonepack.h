// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_bones.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

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
    uint8_t const* data = nullptr;
    std::size_t size = 0;
    std::size_t bits = 0;
    bool ok = true;

    constexpr PoseRiceReader(uint8_t const* bytes, std::size_t length) noexcept
        : data(bytes), size(length)
    {
    }

    PoseRiceReader(std::vector<uint8_t> const& bytes) noexcept
        : data(bytes.data()), size(bytes.size())
    {
    }

    uint32_t Get(unsigned width)
    {
        if (width > 32 || (!data && width) || bits + width > size * 8) {
            ok = false;
            return 0;
        }
        uint32_t value = 0;
        for (unsigned read = 0; read < width;) {
            const unsigned take = (std::min)(width - read, unsigned(8 - (bits & 7)));
            value |= uint32_t((data[bits / 8] >> (bits & 7)) & ((1u << take) - 1)) << read;
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
            if (Get(1) || !ok) { ok = false; return false; }
            value = Get(rawWidth);
            if (!ok || (value >> k) < 15 || value >= (uint32_t(1) << rawWidth)) {
                ok = false;
                return false;
            }
            return true;
        }
        const uint32_t remainder = k ? Get(k) : 0;
        if (!ok) return false;
        value = (uint32_t(quotient) << k) | remainder;
        if (value >= (uint32_t(1) << rawWidth)) {
            ok = false;
            return false;
        }
        return true;
    }

    bool AlignAndFinish()
    {
        while (ok && (bits & 7)) if (Get(1)) ok = false;
        return ok && bits == size * 8;
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

inline int32_t PoseRoundQ8(int32_t product) noexcept
{
    return product < 0
        ? -((-product + 128) >> 8)
        : (product + 128) >> 8;
}

inline int32_t PoseRoundQ8(int64_t product) noexcept
{
    return static_cast<int32_t>(product < 0
        ? -((-product + 128) >> 8)
        : (product + 128) >> 8);
}

inline uint32_t PoseRiceCost(uint32_t value, unsigned k, unsigned rawWidth)
{
    const uint32_t quotient = value >> k;
    return quotient >= 15
        ? 16u + rawWidth
        : quotient + 1u + k;
}

inline unsigned PoseBitWidth(uint32_t value) noexcept
{
    if (!value) return 0;
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse(&index, value);
    return static_cast<unsigned>(index + 1);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(32 - __builtin_clz(value));
#else
    unsigned width = 0;
    for (uint32_t bits = value; bits; bits >>= 1) ++width;
    return width;
#endif
}

// Exact 21-bit-escape cost selection. Nonzero quotients below the escape
// threshold occupy at most four k values; larger k values share a zero count.
struct PoseRiceParameters {
    uint32_t savings[16]{};
    uint16_t widths[16]{};
    uint32_t count = 0;

    void Add(uint32_t value) noexcept
    {
        ++count;
        const unsigned width = PoseBitWidth(value);
        if (width < 16) ++widths[width];
        for (unsigned k = width > 4 ? width - 4 : 0; k < width && k < 16; ++k) {
            const uint32_t quotient = value >> k;
            if (quotient < 15) savings[k] += 37u - (quotient + 1u + k);
        }
    }

    unsigned Best(std::size_t& outBits) const noexcept
    {
        uint32_t zeros = 0;
        uint32_t bestSaving = 0;
        unsigned best = 0;
        for (unsigned k = 0; k < 16; ++k) {
            zeros += widths[k];
            const uint32_t saving = savings[k] + zeros * (36u - k);
            if (saving > bestSaving) { bestSaving = saving; best = k; }
        }
        outBits = static_cast<std::size_t>(uint64_t(count) * 37u - bestSaving);
        return best;
    }

    unsigned Best() const noexcept
    {
        std::size_t ignored = 0;
        return Best(ignored);
    }
};

inline uint64_t PoseIntegerSqrt(uint64_t value) noexcept
{
    if (value >= 65536ull * 65536ull) return 65536ull;
    uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<double>(value)));
    if (s * s > value) --s;
    if ((s + 1) * (s + 1) <= value) ++s;
    return s;
}

inline uint64_t PoseNearestSqrt(uint64_t value) noexcept
{
    const uint64_t floor = PoseIntegerSqrt(value);
    if (!floor) return 0;
    const uint64_t above = floor + 1;
    const uint64_t floorDistance = value - floor * floor;
    const uint64_t aboveDistance = above * above - value;
    return aboveDistance < floorDistance ? above : floor;
}

inline int32_t PoseRoundSignedDivide(int64_t numerator, int64_t denominator) noexcept
{
    if (numerator >= 0)
        return static_cast<int32_t>((numerator + denominator / 2) / denominator);
    return static_cast<int32_t>(-((-numerator + denominator / 2) / denominator));
}

inline int32_t PoseRoundSignedDivide32(int32_t numerator, int32_t denominator) noexcept
{
    if (numerator >= 0)
        return (numerator + (denominator >> 1)) / denominator;
    return -((-numerator + (denominator >> 1)) / denominator);
}

inline int32_t PoseRoundSignedDivide4(int32_t numerator) noexcept
{
    return numerator >= 0 ? (numerator + 2) >> 2 : -((-numerator + 2) >> 2);
}

inline int32_t PoseRoundSignedDivide65536(int32_t numerator) noexcept
{
    return numerator >= 0 ? (numerator + 32768) >> 16 : -((-numerator + 32768) >> 16);
}

struct PoseBitBufWriter {
    uint8_t* dst = nullptr;
    std::size_t bitPos = 0;

    inline void Put(uint32_t value, unsigned width) noexcept
    {
        if (!width) return;
        const std::size_t byteAt = bitPos >> 3;
        const unsigned shift = static_cast<unsigned>(bitPos & 7u);
        const uint64_t shifted = static_cast<uint64_t>(value) << shift;
        const unsigned totalBits = shift + width;
        for (unsigned b = 0; b * 8 < totalBits; ++b)
            dst[byteAt + b] |= static_cast<uint8_t>(shifted >> (b * 8));
        bitPos += width;
    }

    inline void Align() noexcept { bitPos = (bitPos + 7u) & ~std::size_t(7u); }

    inline void Rice(uint32_t value, unsigned k, unsigned rawWidth) noexcept
    {
        const unsigned quotient = value >> k;
        const unsigned unary = (std::min)(quotient, 15u);
        Put((1u << unary) - 1u, unary + 1u);
        if (quotient >= 15u) Put(value, rawWidth);
        else if (k) Put(value & ((1u << k) - 1u), k);
    }
};

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

inline bool PoseExtractQuat(pose::Bone const& bone, PoseQuat& result) noexcept
{
    const int32_t m0 = bone.values[0], m1 = bone.values[1], m2 = bone.values[2];
    const int32_t m3 = bone.values[3], m4 = bone.values[4], m5 = bone.values[5];
    const int32_t m6 = bone.values[6], m7 = bone.values[7], m8 = bone.values[8];
    const int32_t terms[4] = {
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
    const int32_t bigNumerator = terms[omitted] * 16384;
    if (bigNumerator <= 0) return false;
    const int32_t bigQ = static_cast<int32_t>(PoseNearestSqrt(static_cast<uint64_t>(bigNumerator)));
    if (!bigQ) return false;
    int32_t other[3] = {};
    if (omitted == 3) {
        other[0] = PoseRoundSignedDivide32((m5 - m7) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide32((m6 - m2) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide32((m1 - m3) * 16384, bigQ);
    } else if (omitted == 0) {
        other[0] = PoseRoundSignedDivide32((m5 - m7) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide32((m1 + m3) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide32((m2 + m6) * 16384, bigQ);
    } else if (omitted == 1) {
        other[0] = PoseRoundSignedDivide32((m6 - m2) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide32((m1 + m3) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide32((m5 + m7) * 16384, bigQ);
    } else {
        other[0] = PoseRoundSignedDivide32((m1 - m3) * 16384, bigQ);
        other[1] = PoseRoundSignedDivide32((m2 + m6) * 16384, bigQ);
        other[2] = PoseRoundSignedDivide32((m5 + m7) * 16384, bigQ);
    }
    const int64_t sum = int64_t(other[0]) * other[0] + int64_t(other[1]) * other[1] + int64_t(other[2]) * other[2];
    if (sum >= ScaleSquared) return false;
    const int32_t big = static_cast<int32_t>(PoseIntegerSqrt(static_cast<uint64_t>(ScaleSquared - sum)));
    if (!big) return false;

    result.omitted = static_cast<uint8_t>(omitted);
    if (omitted == 3) {
        result.values[0] = other[0];
        result.values[1] = other[1];
        result.values[2] = other[2];
        result.values[3] = big;
    } else if (omitted == 0) {
        result.values[0] = big;
        result.values[1] = other[1];
        result.values[2] = other[2];
        result.values[3] = other[0];
    } else if (omitted == 1) {
        result.values[0] = other[1];
        result.values[1] = big;
        result.values[2] = other[2];
        result.values[3] = other[0];
    } else {
        result.values[0] = other[1];
        result.values[1] = other[2];
        result.values[2] = big;
        result.values[3] = other[0];
    }
    return true;
}

inline bool PoseQuatToBone(PoseQuat const& quat, pose::Bone const& source, pose::Bone& target) noexcept
{
    constexpr int64_t scale = 16384;
    constexpr int64_t scaleSquared = scale * scale;
    const int64_t x64 = quat.values[0], y64 = quat.values[1], z64 = quat.values[2], w64 = quat.values[3];
    const int64_t sum = x64*x64 + y64*y64 + z64*z64 + w64*w64;
    if (sum == 0 || sum > scaleSquared) return false;

    const int32_t x = quat.values[0], y = quat.values[1], z = quat.values[2], w = quat.values[3];
    constexpr int32_t s2 = static_cast<int32_t>(scaleSquared);
    const int32_t numerator[9] = {
        s2 - 2 * (y * y + z * z),
        2 * (x * y + z * w),
        2 * (x * z - y * w),
        2 * (x * y - z * w),
        s2 - 2 * (x * x + z * z),
        2 * (y * z + x * w),
        2 * (x * z + y * w),
        2 * (y * z - x * w),
        s2 - 2 * (x * x + y * y)
    };
    int16_t basis[9];
    for (int i = 0; i < 9; ++i) {
        const int32_t rounded = PoseRoundSignedDivide65536(numerator[i]);
        if (rounded < -32768 || rounded > 32767) return false;
        basis[i] = static_cast<int16_t>(rounded);
    }
    target = source;
    for (int i = 0; i < 9; ++i) target.values[i] = basis[i];
    return true;
}

}
