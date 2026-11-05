// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

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
}
