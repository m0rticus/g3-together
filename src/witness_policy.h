// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::witness::policy {

inline constexpr int AimRoutine = 0;
inline constexpr int AimFlee = 4;
inline constexpr int AimTalk = 6;
inline constexpr int AimSleep = 7;
inline constexpr int AimDown = 8;
inline constexpr int AimDead = 9;
inline constexpr int AimAbandoned = 10;

inline constexpr float MaxMirrorPerceiveRadiusCm = 2000.0f;
inline constexpr float MaxMirrorPerceiveRadiusSqCm = MaxMirrorPerceiveRadiusCm * MaxMirrorPerceiveRadiusCm;

struct MirrorPerceptionPlan {
    bool eligible = false;
    bool openMode = false;
    bool openTask = false;
};

inline MirrorPerceptionPlan PlanMirrorPerception(bool mirrored, bool isHeroOrPuppet, bool hasNpcAndRoutine,
                                                 bool deadOrDown, float distanceSqCm, int aiMode,
                                                 bool taskEmpty, bool taskUnconscious) noexcept
{
    if (!mirrored || isHeroOrPuppet || !hasNpcAndRoutine || deadOrDown ||
        distanceSqCm > MaxMirrorPerceiveRadiusSqCm || taskUnconscious) {
        return {};
    }
    if (aiMode == AimDead || aiMode == AimDown || aiMode == AimSleep ||
        aiMode == AimTalk || aiMode == AimFlee) {
        return {};
    }
    const bool openMode = (aiMode == AimAbandoned);
    const bool openTask = taskEmpty;
    if (!openMode && !openTask) return {};
    return {true, openMode, openTask};
}

inline uint8_t ResolveWitnessOwner(uint8_t reportedOwner, bool isHost, bool mirrored) noexcept
{
    if (reportedOwner != 0) return reportedOwner;
    if (!isHost && mirrored) return 1;
    return 0;
}

inline bool ShouldSendWitnessHolds(bool enabled, bool together, int beforeCrime, int afterCrime) noexcept
{
    return enabled && together && afterCrime > beforeCrime;
}

inline bool ShouldApplyWitnessHolds(bool enabled, bool together, bool witnessDead,
                                    int incomingCrime, int currentCrime, int crimeCountLimit) noexcept
{
    return enabled && together && !witnessDead &&
           incomingCrime > currentCrime && incomingCrime < crimeCountLimit;
}

} // namespace g3t::witness::policy
