// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>

namespace g3t::npcplayback_hit_policy {

// Maximum bridge when a high-RTT host's Stumble_Hit is still in its early frames after the
// local predicted Stumble_Hit clip has already finished. Prevents rewinding the stagger
// without freezing the NPC across Stumble_Recover or multi-hundred-ms backlogs.
constexpr float MaxEchoBridgeSeconds = 0.15f;
constexpr float HostTailHandoffPhase = 0.75f;
constexpr uint32_t PendingHitTimeoutMs = 2000u;
constexpr int32_t HostHitStampMinHoldMs = 250;

// Only the initial hit-impact stumble duplicates LocalHit's predicted Stumble_Hit clip.
// Stumble_Recover is the recovery animation out of recoil and must play normally.
inline bool HitImpactMotion(char const* name)
{
    return name && std::strstr(name, "Stumble") && !std::strstr(name, "_Recover_");
}

// Local predicted stagger runs strictly for its own clip duration and never pins phase=1.0f
// while waiting on network acknowledgement or delayed buffered state.
inline bool LocalStaggerActive(bool hasLocalMotion, float localAgeSeconds, float localDurationSeconds)
{
    return hasLocalMotion && localDurationSeconds > 0.0f &&
           localAgeSeconds >= 0.0f && localAgeSeconds < localDurationSeconds;
}

inline bool HoldSuppressedImpact(bool localActive, bool hasLocalMotion, bool suppressedSlot0,
                                 float localAgeSeconds, float localDurationSeconds,
                                 bool haveHostPose, float hostPhase)
{
    if (localActive || !hasLocalMotion || !suppressedSlot0) return false;
    const float overtime = localAgeSeconds - localDurationSeconds;
    if (overtime >= MaxEchoBridgeSeconds) return false;
    if (haveHostPose && hostPhase >= HostTailHandoffPhase) return false;
    return true;
}

inline bool PendingHitExpired(uint16_t pendingHit, uint32_t localStartMs, uint32_t nowMs)
{
    return pendingHit != 0 && static_cast<uint32_t>(nowMs - localStartMs) >= PendingHitTimeoutMs;
}

inline bool HostHitStampExpired(uint32_t stampedAtMs, uint32_t nowMs, bool staggerTask, char const* motionName)
{
    if (static_cast<int32_t>(nowMs - stampedAtMs) <= HostHitStampMinHoldMs) return false;
    if (staggerTask) return false;
    return !(motionName && std::strstr(motionName, "Stumble"));
}

} // namespace g3t::npcplayback_hit_policy
