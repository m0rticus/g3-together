// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define G3T_POSE_SSE2 1
#include <emmintrin.h>
#else
#define G3T_POSE_SSE2 0
#endif

namespace g3t::pose {
constexpr int MaxBones = 256;
constexpr int HistorySize = 128;
constexpr size_t MaxRetainedFrames = 512;
constexpr float BasisScale = 4096.0f;
constexpr float PositionScale = 4.0f;
constexpr uint8_t Facial = 1;
constexpr uint32_t GapResetMs = 1500;

#pragma pack(push, 1)
struct Bone {
    uint32_t name = 0;
    int16_t values[12] = {};
};
#pragma pack(pop)
static_assert(sizeof(Bone) == 28, "Bone poses retain affine storage and exact non-rigid fallback support");

struct Transform {
    uint32_t name = 0;
    float values[12] = {};
};

#if G3T_POSE_SSE2
namespace detail {
inline bool Encode4Sse2(__m128 v, __m128 scale, __m128i& outPacked4) noexcept
{
    const __m128 s = _mm_mul_ps(v, scale);
    const __m128 lo = _mm_set1_ps(-32768.5f);
    const __m128 hi = _mm_set1_ps(32767.5f);
    const __m128 ok = _mm_and_ps(_mm_cmpgt_ps(s, lo), _mm_cmplt_ps(s, hi));
    if (_mm_movemask_ps(ok) != 0xF) return false;
    const __m128d signMask = _mm_set1_pd(-0.0);
    const __m128d half = _mm_set1_pd(0.5);
    const __m128d d0 = _mm_cvtps_pd(s);
    const __m128d d1 = _mm_cvtps_pd(_mm_movehl_ps(s, s));
    const __m128d r0 = _mm_add_pd(d0, _mm_or_pd(half, _mm_and_pd(d0, signMask)));
    const __m128d r1 = _mm_add_pd(d1, _mm_or_pd(half, _mm_and_pd(d1, signMask)));
    const __m128i i0 = _mm_cvttpd_epi32(r0);
    const __m128i i1 = _mm_cvttpd_epi32(r1);
    const __m128i i01 = _mm_unpacklo_epi64(i0, i1);
    outPacked4 = _mm_packs_epi32(i01, i01);
    return true;
}
} // namespace detail
#endif

inline bool Encode(Transform const& source, Bone& target)
{
    if (!source.name) return false;
#if G3T_POSE_SSE2
    __m128i p0, p1, p2;
    const __m128 basis = _mm_set1_ps(BasisScale);
    const __m128 tail = _mm_set_ps(PositionScale, PositionScale, PositionScale, BasisScale);
    if (!detail::Encode4Sse2(_mm_loadu_ps(source.values), basis, p0) ||
        !detail::Encode4Sse2(_mm_loadu_ps(source.values + 4), basis, p1) ||
        !detail::Encode4Sse2(_mm_loadu_ps(source.values + 8), tail, p2))
        return false;
    target.name = source.name;
    _mm_storel_epi64(reinterpret_cast<__m128i*>(target.values), p0);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(target.values + 4), p1);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(target.values + 8), p2);
    return true;
#else
    Bone result;
    result.name = source.name;
    for (int i = 0; i < 12; ++i) {
        const float scaled = source.values[i] * (i < 9 ? BasisScale : PositionScale);
        // Rejects NaN, +-Inf, and values whose half-away-from-zero rounding exceeds int16_t.
        if (!(scaled > -32768.5f && scaled < 32767.5f)) return false;
        const int rounded = static_cast<int>(static_cast<double>(scaled) + (scaled >= 0.0f ? 0.5 : -0.5));
        result.values[i] = static_cast<int16_t>(rounded);
    }
    target = result;
    return true;
#endif
}

// Encodes a 4x4 row-major engine bCMatrix ([x0,x1,x2,w, y0,y1,y2,w, z0,z1,z2,w, t0,t1,t2,w]) directly.
inline bool EncodeMatrix4x4(uint32_t name, float const* matrix16, Bone& target)
{
    if (!name || !matrix16) return false;
#if G3T_POSE_SSE2
    const __m128 xyzMask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const __m128 basis = _mm_set1_ps(BasisScale);
    const __m128 pos = _mm_set1_ps(PositionScale);
    __m128i p0, p1, p2, p3;
    if (!detail::Encode4Sse2(_mm_and_ps(_mm_loadu_ps(matrix16), xyzMask), basis, p0) ||
        !detail::Encode4Sse2(_mm_and_ps(_mm_loadu_ps(matrix16 + 4), xyzMask), basis, p1) ||
        !detail::Encode4Sse2(_mm_and_ps(_mm_loadu_ps(matrix16 + 8), xyzMask), basis, p2) ||
        !detail::Encode4Sse2(_mm_and_ps(_mm_loadu_ps(matrix16 + 12), xyzMask), pos, p3))
        return false;
    int16_t packed[16];
    _mm_storel_epi64(reinterpret_cast<__m128i*>(packed), p0);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(packed + 3), p1);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(packed + 6), p2);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(packed + 9), p3);
    target.name = name;
    std::memcpy(target.values, packed, sizeof(target.values));
    return true;
#else
    Transform t;
    t.name = name;
    t.values[0] = matrix16[0]; t.values[1] = matrix16[1]; t.values[2] = matrix16[2];
    t.values[3] = matrix16[4]; t.values[4] = matrix16[5]; t.values[5] = matrix16[6];
    t.values[6] = matrix16[8]; t.values[7] = matrix16[9]; t.values[8] = matrix16[10];
    t.values[9] = matrix16[12]; t.values[10] = matrix16[13]; t.values[11] = matrix16[14];
    return Encode(t, target);
#endif
}

inline Transform Decode(Bone const& bone)
{
    Transform result;
    result.name = bone.name;
    constexpr float InvBasis = 1.0f / BasisScale;
    constexpr float InvPos = 1.0f / PositionScale;
    for (int i = 0; i < 9; ++i) result.values[i] = bone.values[i] * InvBasis;
    for (int i = 9; i < 12; ++i) result.values[i] = bone.values[i] * InvPos;
    return result;
}

namespace detail {
inline bool RigidBasis(float const (&v)[12])
{
    for (int axis = 0; axis < 3; ++axis) {
        const float* a = v + axis * 3;
        const float normSq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(normSq >= 0.94f && normSq <= 1.06f)) return false;
    }
    const float xy = v[0] * v[3] + v[1] * v[4] + v[2] * v[5];
    const float xz = v[0] * v[6] + v[1] * v[7] + v[2] * v[8];
    const float yz = v[3] * v[6] + v[4] * v[7] + v[5] * v[8];
    if (std::fabs(xy) > 0.04f || std::fabs(xz) > 0.04f || std::fabs(yz) > 0.04f) return false;
    const float det = v[0] * (v[4] * v[8] - v[5] * v[7]) -
                      v[3] * (v[1] * v[8] - v[2] * v[7]) +
                      v[6] * (v[1] * v[5] - v[2] * v[4]);
    return det >= 0.90f && det <= 1.10f;
}

inline void BasisToQuat(float const (&v)[12], float (&q)[4])
{
    const float m00 = v[0], m10 = v[1], m20 = v[2];
    const float m01 = v[3], m11 = v[4], m21 = v[5];
    const float m02 = v[6], m12 = v[7], m22 = v[8];
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt((std::max)(1.0f + m00 - m11 - m22, 1e-12f)) * 2.0f;
        q[0] = 0.25f * s;
        q[3] = (m21 - m12) / s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt((std::max)(1.0f + m11 - m00 - m22, 1e-12f)) * 2.0f;
        q[1] = 0.25f * s;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt((std::max)(1.0f + m22 - m00 - m11, 1e-12f)) * 2.0f;
        q[2] = 0.25f * s;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
    }
    const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (norm > 1e-6f) for (float& c : q) c /= norm;
}

inline void SlerpRigidBasis(float (&out)[12], float const (&end)[12], float fraction)
{
    float a[4], b[4], r[4];
    BasisToQuat(out, a);
    BasisToQuat(end, b);
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) { dot = -dot; for (float& c : b) c = -c; }
    if (dot > 0.9995f) {
        for (int i = 0; i < 4; ++i) r[i] = a[i] + (b[i] - a[i]) * fraction;
    } else {
        const float theta = std::acos((std::min)(dot, 1.0f));
        const float invSin = 1.0f / std::sin(theta);
        const float wa = std::sin((1.0f - fraction) * theta) * invSin;
        const float wb = std::sin(fraction * theta) * invSin;
        for (int i = 0; i < 4; ++i) r[i] = a[i] * wa + b[i] * wb;
    }
    const float norm = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (norm > 1e-6f) for (float& c : r) c /= norm;
    const float x = r[0], y = r[1], z = r[2], w = r[3];
    out[0] = 1.0f - 2.0f * (y * y + z * z); out[1] = 2.0f * (x * y + w * z); out[2] = 2.0f * (x * z - w * y);
    out[3] = 2.0f * (x * y - w * z); out[4] = 1.0f - 2.0f * (x * x + z * z); out[5] = 2.0f * (y * z + w * x);
    out[6] = 2.0f * (x * z + w * y); out[7] = 2.0f * (y * z - w * x); out[8] = 1.0f - 2.0f * (x * x + y * y);
    for (int k = 9; k < 12; ++k) out[k] += (end[k] - out[k]) * fraction;
}
} // namespace detail

class History {
    struct Frame {
        uint32_t incarnation = 0, sent = 0;
        uint8_t flags = 0;
        std::vector<Bone> bones;
    };
    std::deque<Frame> frames;
    std::vector<std::vector<Bone>> spareBones;
    uint32_t sampledSent = 0;
    bool haveSampled = false;

    void Recycle(Frame& frame)
    {
        if (spareBones.size() < 64) {
            frame.bones.clear();
            spareBones.push_back(std::move(frame.bones));
        }
    }

    void Prune()
    {
        size_t consumed = 0;
        while (frames.size() - consumed > HistorySize && haveSampled &&
               static_cast<int32_t>(sampledSent - frames[consumed + 1].sent) >= 0)
            ++consumed;
        if (frames.size() - consumed > MaxRetainedFrames)
            consumed = frames.size() - MaxRetainedFrames;
        while (consumed--) {
            Recycle(frames.front());
            frames.pop_front();
        }
    }

public:
    size_t Size() const { return frames.size(); }
    uint32_t LatestSent() const { return frames.empty() ? 0 : frames.back().sent; }
    uint32_t Incarnation() const { return frames.empty() ? 0 : frames.back().incarnation; }

    void Clear()
    {
        for (auto& frame : frames) Recycle(frame);
        frames.clear();
        sampledSent = 0;
        haveSampled = false;
    }

    void DiscardBefore(uint32_t incarnation, uint32_t sent)
    {
        frames.erase(std::remove_if(frames.begin(), frames.end(), [&](Frame const& frame) {
            return frame.incarnation != incarnation || static_cast<int32_t>(frame.sent - sent) < 0;
        }), frames.end());
        if (frames.empty()) {
            sampledSent = 0;
            haveSampled = false;
        } else if (haveSampled && static_cast<int32_t>(sampledSent - frames.front().sent) < 0) {
            sampledSent = frames.front().sent;
        }
    }

    bool Push(uint32_t incarnation, uint32_t sent, Bone const* bones, int count, uint8_t flags = 0)
    {
        if (!incarnation || !bones || count <= 0 || count > MaxBones || (flags & ~Facial)) return false;
        if (!frames.empty()) {
            const auto& last = frames.back();
            if (static_cast<int32_t>(incarnation - last.incarnation) < 0 ||
                (incarnation == last.incarnation && static_cast<int32_t>(sent - last.sent) <= 0)) return false;
            if (incarnation != last.incarnation || static_cast<uint32_t>(sent - last.sent) > GapResetMs) {
                for (auto& frame : frames) Recycle(frame);
                frames.clear();
                haveSampled = false;
            }
        }
        Frame next;
        next.incarnation = incarnation;
        next.sent = sent;
        next.flags = flags;
        if (!spareBones.empty()) {
            next.bones = std::move(spareBones.back());
            spareBones.pop_back();
        }
        next.bones.assign(bones, bones + count);
        frames.push_back(std::move(next));
        // Unpresented bones must outlive the matching queued action/root samples.
        Prune();
        return true;
    }

    bool Sample(uint32_t incarnation, uint32_t sent, std::vector<Transform>& result, uint8_t* flags = nullptr)
    {
        if (frames.empty() || frames.front().incarnation != incarnation ||
            static_cast<int32_t>(sent - frames.front().sent) < 0 ||
            static_cast<int32_t>(sent - frames.back().sent) > 0) return false;
        const auto it = std::lower_bound(
            frames.begin() + 1, frames.end(), sent,
            [](Frame const& frame, uint32_t target) { return static_cast<int32_t>(target - frame.sent) > 0; });
        const Frame* a = &frames.back();
        const Frame* b = a;
        if (it != frames.end()) {
            a = &*(it - 1);
            b = &*it;
        }
        const uint32_t span = b->sent - a->sent;
        const float fraction = span ? static_cast<float>(sent - a->sent) / span : 0.0f;
        if (fraction >= 1.0f) a = b;
        if (flags) *flags = fraction < 1.0f ? a->flags : b->flags;
        bool compatible = fraction > 0.0f && fraction < 1.0f &&
                          a->flags == b->flags && a->bones.size() == b->bones.size();
        for (size_t i = 0; compatible && i < a->bones.size(); ++i)
            compatible = a->bones[i].name == b->bones[i].name;
        result.resize(a->bones.size());
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] = Decode(a->bones[i]);
            if (!compatible) continue;
            const auto end = Decode(b->bones[i]);
            if (detail::RigidBasis(result[i].values) && detail::RigidBasis(end.values))
                detail::SlerpRigidBasis(result[i].values, end.values, fraction);
            else
                for (int k = 0; k < 12; ++k)
                    result[i].values[k] += (end.values[k] - result[i].values[k]) * fraction;
        }
        if (!haveSampled || static_cast<int32_t>(sent - sampledSent) > 0) sampledSent = sent;
        haveSampled = true;
        Prune();
        return true;
    }

    bool SampleClamped(uint32_t incarnation, uint32_t sent, uint32_t maxLagMs, uint32_t maxLeadMs,
                       std::vector<Transform>& result, uint8_t* flags = nullptr)
    {
        if (frames.empty() || frames.front().incarnation != incarnation) return false;
        const int32_t beforeFirst = static_cast<int32_t>(frames.front().sent - sent);
        if (beforeFirst > 0) {
            if (static_cast<uint32_t>(beforeFirst) > maxLagMs) return false;
            sent = frames.front().sent;
        } else {
            const int32_t afterLast = static_cast<int32_t>(sent - frames.back().sent);
            if (afterLast > 0) {
                if (static_cast<uint32_t>(afterLast) > maxLeadMs) return false;
                sent = frames.back().sent;
            }
        }
        return Sample(incarnation, sent, result, flags);
    }
};
}
