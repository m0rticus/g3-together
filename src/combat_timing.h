// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

namespace g3t::combat_timing {
// CP 1.75 Game.dll, sAICombatMoveItlLoop: OnHit at 60% of the Hit clip, not at its start.
constexpr float NativeHitPhase = 0.6f;
constexpr float PhaseTolerance = 0.5f / 4095.0f; // compact control's phase quantization

inline bool HitPhaseReached(float phase)
{
    return std::isfinite(phase) && phase >= NativeHitPhase - PhaseTolerance && phase <= 1.0f;
}

inline bool IncomingCombat(uint8_t flags, uint32_t npcTarget, uint8_t player)
{
    return player != 0 && !npcTarget && (flags & 4u) && !(flags & 3u) &&
           ((flags >> 3) & 7u) == player;
}

inline bool RemoteCombat(uint8_t flags, uint32_t npcTarget)
{
    return ((flags >> 3) & 7u) > 1 && IncomingCombat(flags, npcTarget, (flags >> 3) & 7u);
}
}
