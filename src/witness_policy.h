// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::witness::policy {

inline constexpr int AimRoutine = 0;
inline constexpr int AimReaction = 1;
inline constexpr int AimCombat = 3;
inline constexpr int AimFlee = 4;
inline constexpr int AimTalk = 6;
inline constexpr int AimSleep = 7;
inline constexpr int AimDown = 8;
inline constexpr int AimDead = 9;
inline constexpr int AimAbandoned = 10;

inline constexpr float MaxMirrorPerceiveRadiusCm = 2000.0f;
inline constexpr float MaxMirrorPerceiveRadiusSqCm = MaxMirrorPerceiveRadiusCm * MaxMirrorPerceiveRadiusCm;
inline constexpr float MaxChestPerceiveRadiusCm = 1000.0f;
inline constexpr float MaxChestPerceiveRadiusSqCm = MaxChestPerceiveRadiusCm * MaxChestPerceiveRadiusCm;

struct MirrorState {
    bool mirrored = false;
    bool isHeroOrPuppet = false;
    bool hasNpcAndRoutine = false;
    bool deadOrDown = false;
    float distanceSqCm = 0.0f;
    int aiMode = AimDead;
    bool taskEmpty = false;
    bool taskUnconscious = false;
};

struct MirrorPerceptionPlan {
    bool eligible = false;
    bool openMode = false;
    bool openTask = false;
};

inline bool IsMirrorPerceptible(MirrorState const& m, float maxRadiusSqCm) noexcept
{
    if (!m.mirrored || m.isHeroOrPuppet || !m.hasNpcAndRoutine || m.deadOrDown ||
        m.distanceSqCm > maxRadiusSqCm || m.taskUnconscious) {
        return false;
    }
    return m.aiMode != AimDead && m.aiMode != AimDown && m.aiMode != AimSleep &&
           m.aiMode != AimTalk && m.aiMode != AimFlee;
}

inline MirrorPerceptionPlan PlanMirrorPerception(MirrorState const& m) noexcept
{
    if (!IsMirrorPerceptible(m, MaxMirrorPerceiveRadiusSqCm)) return {};
    const bool openMode = (m.aiMode == AimAbandoned);
    const bool openTask = m.taskEmpty;
    if (!openMode && !openTask) return {};
    return {true, openMode, openTask};
}

inline MirrorPerceptionPlan PlanMirrorPerception(bool mirrored, bool isHeroOrPuppet, bool hasNpcAndRoutine,
                                                 bool deadOrDown, float distanceSqCm, int aiMode,
                                                 bool taskEmpty, bool taskUnconscious) noexcept
{
    MirrorState m{};
    m.mirrored = mirrored;
    m.isHeroOrPuppet = isHeroOrPuppet;
    m.hasNpcAndRoutine = hasNpcAndRoutine;
    m.deadOrDown = deadOrDown;
    m.distanceSqCm = distanceSqCm;
    m.aiMode = aiMode;
    m.taskEmpty = taskEmpty;
    m.taskUnconscious = taskUnconscious;
    return PlanMirrorPerception(m);
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

struct MirrorChestUseInput {
    bool enabled = false;
    bool mirrorsPerceive = false;
    MirrorState mirror{};
    float chestDistanceSqCm = 0.0f;
    bool npcHasMistEffect = false;
    bool heroHasGhostEffect = false;
    bool sameIndoorZone = false;
    bool freeLosToHero = false;
    bool freeLosToChest = false;
    bool inFovToHero = false;
    bool inFovToChest = false;
};

inline bool ShouldMirrorCatchChestUse(MirrorChestUseInput const& in) noexcept
{
    if (!in.enabled || !in.mirrorsPerceive || in.npcHasMistEffect || in.heroHasGhostEffect)
        return false;
    if (!IsMirrorPerceptible(in.mirror, MaxChestPerceiveRadiusSqCm))
        return false;
    if (in.chestDistanceSqCm > MaxChestPerceiveRadiusSqCm)
        return false;
    if (!in.sameIndoorZone && !in.freeLosToHero && !in.freeLosToChest)
        return false;
    return in.mirror.aiMode == AimReaction || in.inFovToHero || in.inFovToChest;
}

inline bool ShouldApplyWitnessReaction(int aiMode, bool sameTarget, bool inAttackTask) noexcept
{
    return !(aiMode == AimCombat && sameTarget && inAttackTask);
}

} // namespace g3t::witness::policy
