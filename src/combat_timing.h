// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

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

// Protect the display clock during incoming attack windups and active hit clips, while allowing
// between-swing locomotion, idle, recovery, and stumble clips to absorb accumulated clock lag.
inline bool IncomingAttackMotion(char const* name, uint8_t contact)
{
    if (contact != 0) return true;
    if (!name || !*name) return true;
    if (std::strstr(name, "Stumble") || std::strstr(name, "_Recover_")) return false;
    if (std::strstr(name, "_Move_") || std::strstr(name, "_Walk_") || std::strstr(name, "_Run_") ||
        std::strstr(name, "_Sprint_") || std::strstr(name, "_Strafe") || std::strstr(name, "_Turn_") ||
        std::strstr(name, "_Ambient") || std::strstr(name, "_Threaten"))
        return false;
    if (std::strstr(name, "_Parade") && !std::strstr(name, "Attack")) return false;
    return true;
}
}
