// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace g3t::contact {
inline float SegmentDistanceSquared(float const a[3], float const b[3], float const c[3], float const d[3])
{
    float u[3], v[3], w[3];
    float aa = 0, bb = 0, cc = 0, dd = 0, ee = 0;
    for (int i = 0; i < 3; ++i) {
        u[i] = b[i] - a[i]; v[i] = d[i] - c[i]; w[i] = a[i] - c[i];
        aa += u[i] * u[i]; bb += u[i] * v[i]; cc += v[i] * v[i];
        dd += u[i] * w[i]; ee += v[i] * w[i];
    }
    float s = 0, t = 0;
    if (aa <= 0.0001f && cc <= 0.0001f) {
        s = t = 0;
    } else if (aa <= 0.0001f) {
        t = std::clamp(ee / cc, 0.0f, 1.0f);
    } else {
        if (cc <= 0.0001f) s = std::clamp(-dd / aa, 0.0f, 1.0f);
        else {
            const float denominator = aa * cc - bb * bb;
            if (denominator > 0.0001f) s = std::clamp((bb * ee - cc * dd) / denominator, 0.0f, 1.0f);
            t = (bb * s + ee) / cc;
            if (t < 0) { t = 0; s = std::clamp(-dd / aa, 0.0f, 1.0f); }
            else if (t > 1) { t = 1; s = std::clamp((bb - dd) / aa, 0.0f, 1.0f); }
        }
    }
    float distance = 0;
    for (int i = 0; i < 3; ++i) {
        const float delta = w[i] + s * u[i] - t * v[i];
        distance += delta * delta;
    }
    return distance;
}

inline void LerpPoint(float const a[3], float const b[3], float t, float out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

// Minimum distance squared between a swept blade/limb segment [(oldA, oldB) -> (a, b)] and a
// vertical character capsule axis [capsuleFirst, capsuleLast]. Sampling interior blade stations
// and intermediate sweep chords prevents long 2H weapons from straddling a torso in one tick.
inline float SweptSegmentDistanceSquared(float const oldA[3], float const oldB[3],
                                         float const a[3], float const b[3],
                                         float const capsuleFirst[3], float const capsuleLast[3],
                                         bool weapon)
{
    float best = std::min(SegmentDistanceSquared(oldA, a, capsuleFirst, capsuleLast),
                          SegmentDistanceSquared(oldB, b, capsuleFirst, capsuleLast));
    if (!weapon) return best;
    best = std::min(best, SegmentDistanceSquared(a, b, capsuleFirst, capsuleLast));
    best = std::min(best, SegmentDistanceSquared(oldA, oldB, capsuleFirst, capsuleLast));
    for (float station : {0.25f, 0.5f, 0.75f}) {
        float from[3], to[3];
        LerpPoint(oldA, oldB, station, from);
        LerpPoint(a, b, station, to);
        best = std::min(best, SegmentDistanceSquared(from, to, capsuleFirst, capsuleLast));
    }
    for (float step : {0.3333333f, 0.6666667f}) {
        float grip[3], tip[3];
        LerpPoint(oldA, a, step, grip);
        LerpPoint(oldB, b, step, tip);
        best = std::min(best, SegmentDistanceSquared(grip, tip, capsuleFirst, capsuleLast));
    }
    return best;
}
}
