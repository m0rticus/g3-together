// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace g3t::movement {
inline bool PositionDiscontinuity(float const before[3], float const after[3],
                                  float const velocityBefore[3], float const velocityAfter[3],
                                  uint64_t elapsedUs, bool airborne)
{
    if (elapsedUs >= 250000) return false; // Loading/frame stalls cannot be attributed to one physics step.
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(before[i]) || !std::isfinite(after[i]) ||
            !std::isfinite(velocityBefore[i]) || !std::isfinite(velocityAfter[i]))
            return true;
    }
    float distanceSquared = 0.0f, speedBefore = 0.0f, speedAfter = 0.0f;
    for (unsigned i = 0; i < 3; ++i) {
        const float delta = after[i] - before[i];
        distanceSquared += delta * delta;
        speedBefore += velocityBefore[i] * velocityBefore[i];
        speedAfter += velocityAfter[i] * velocityAfter[i];
    }
    const float dt = static_cast<float>(elapsedUs) / 1000000.0f;
    // Local velocity may be rotated; its magnitude still bounds world-space travel.
    const float allowed = std::sqrt(std::max(speedBefore, speedAfter)) * dt + 490.5f * dt * dt;
    return std::sqrt(distanceSquared) > 30.0f + allowed ||
           (airborne && before[1] - after[1] > 15.0f + allowed);
}
}
