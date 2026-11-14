// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace g3t::npcplayback_hit_policy {

// Maximum bridge when a high-RTT host's Stumble_Hit is still in its early frames after the
// local predicted Stumble_Hit clip has already finished. Prevents rewinding the stagger
// without freezing the NPC across Stumble_Recover or multi-hundred-ms backlogs.
constexpr float MaxEchoBridgeSeconds = 0.15f;
constexpr float HostTailHandoffPhase = 0.75f;
constexpr uint32_t PendingHitTimeoutMs = 2000u;
constexpr int32_t HostHitStampMinHoldMs = 250;

// Matches net::HitReaction values without pulling net.h into policy unit tests.
constexpr uint8_t ReactionNone = 0;
constexpr uint8_t ReactionQuickStumble = 1;
constexpr uint8_t ReactionStumble = 2;
constexpr uint8_t ReactionPierceStumble = 3;
constexpr uint8_t ReactionSitKnockDown = 4;

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

// Matches CP 1.75 Script_Game.dll GetActionPlaySpeed (0x100042a0):
// QuickStumble runs at 2.0x (200 ms for a 0.40s clip), Stumble/StumbleL/StumbleR at 1.3x (~308 ms),
// PierceStumble and SitKnockDown at 1.0x, scaled by 0.75x when Diseased.
inline float NativeStumblePlaySpeed(uint8_t reaction, bool quickClipUsed, bool diseased = false)
{
    float speed = 1.3f;
    if (reaction == ReactionQuickStumble) speed = quickClipUsed ? 2.0f : 1.3f;
    else if (reaction == ReactionStumble) speed = 1.3f;
    else if (reaction == ReactionPierceStumble || reaction == ReactionSitKnockDown) speed = 1.0f;
    if (diseased) speed *= 0.75f;
    return speed;
}

// Matches CP 1.75 Game.dll gCScriptProcessingUnit::sAICombatMoveStart (0x1016abb0):
// front hits (victim facing attacker, dot > 0) select "_Back_" recoil clips; back hits select "_Fwd_".
inline bool FrontHitDirection(float victimForwardX, float victimForwardZ, float toAttackerX, float toAttackerZ)
{
    const float lenSq = toAttackerX * toAttackerX + toAttackerZ * toAttackerZ;
    if (!(lenSq > 1e-6f) || !std::isfinite(lenSq)) return true;
    const float invLen = 1.0f / std::sqrt(lenSq);
    const float dot = victimForwardX * (toAttackerX * invLen) + victimForwardZ * (toAttackerZ * invLen);
    return dot > 0.0f;
}

// Computes the horizontal knockback push unit vector away from the attacker (matching sAICombatMoveStart).
inline bool ComputeKnockbackStepDirection(float victimX, float victimZ, float attackerX, float attackerZ,
                                          float victimForwardX, float victimForwardZ, float outDirXZ[2])
{
    float dx = victimX - attackerX;
    float dz = victimZ - attackerZ;
    float lenSq = dx * dx + dz * dz;
    if (!(lenSq > 1e-4f) || !std::isfinite(lenSq)) {
        dx = -victimForwardX;
        dz = -victimForwardZ;
        lenSq = dx * dx + dz * dz;
    }
    if (!(lenSq > 1e-6f) || !std::isfinite(lenSq)) {
        outDirXZ[0] = 0.0f;
        outDirXZ[1] = 0.0f;
        return false;
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    outDirXZ[0] = dx * invLen;
    outDirXZ[1] = dz * invLen;
    return true;
}

// Parses the native root displacement encoded in token 13 (0-based) of a Gothic 3 .xmot filename
// (e.g. "Orc_Stand_None_2H_P0_StumbleL_Hit_N_Back_00_%_00_P1_90.xmot" -> 90.0f cm), matching
// Game.dll sAICombatMoveStart (0x1016abb0).
inline float ParseMotionDisplacementCm(char const* motionName)
{
    if (!motionName || !*motionName) return 0.0f;
    char const* base = motionName;
    for (char const* p = motionName; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    int token = 0;
    char const* tokenStart = base;
    for (char const* p = base;; ++p) {
        if (*p == '_' || *p == '.' || *p == '\0') {
            if (token == 13) {
                char buf[24] = {};
                const std::size_t len = static_cast<std::size_t>(p - tokenStart);
                if (len == 0 || len >= sizeof(buf)) return 0.0f;
                std::memcpy(buf, tokenStart, len);
                char* endPtr = nullptr;
                const float cm = std::strtof(buf, &endPtr);
                if (endPtr == buf || !std::isfinite(cm) || cm <= 0.0f) return 0.0f;
                return std::min(cm, 300.0f);
            }
            if (*p != '_') break;
            ++token;
            tokenStart = p + 1;
        }
    }
    return 0.0f;
}

inline float SmoothStep01(float t)
{
    const float x = std::clamp(t, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Phase-canceling knockback lead compensator:
// Advances the mirror immediately over [0, durationSec] by stepCm along dirXZ while subtracting
// the exact fraction already present in the buffered host snapshot (hostProgress), so the NPC
// steps back crisply on impact and holds steady without double-stepping or sliding away late.
struct HitKnockbackState {
    bool active = false;
    bool refused = false;
    bool hostImpactSeen = false;
    bool hostImpactDone = false;
    uint16_t hitSequence = 0;
    uint32_t startedAtMs = 0;
    uint32_t refusedAtMs = 0;
    float durationSec = 0.0f;
    float dirXZ[2] = {};
    float stepCm = 0.0f;
    float groundDeltaY = 0.0f;
    float carryOffset[3] = {};
    float currentOffset[3] = {};
};

inline void StartHitKnockback(HitKnockbackState& kb, uint16_t hitSequence, uint32_t nowMs, float durationSec,
                              float const dirXZ[2], float stepCm, float groundDeltaY = 0.0f)
{
    const float carry[3] = {
        kb.active ? kb.currentOffset[0] : 0.0f,
        kb.active ? kb.currentOffset[1] : 0.0f,
        kb.active ? kb.currentOffset[2] : 0.0f,
    };
    const bool hasCarry = (carry[0] * carry[0] + carry[1] * carry[1] + carry[2] * carry[2]) > 0.25f;
    kb = HitKnockbackState{};
    kb.carryOffset[0] = carry[0];
    kb.carryOffset[1] = carry[1];
    kb.carryOffset[2] = carry[2];
    kb.currentOffset[0] = carry[0];
    kb.currentOffset[1] = carry[1];
    kb.currentOffset[2] = carry[2];
    kb.hitSequence = hitSequence;
    kb.startedAtMs = nowMs;
    kb.durationSec = std::max(0.0f, durationSec);
    if (dirXZ && stepCm > 0.5f && kb.durationSec > 0.01f) {
        kb.dirXZ[0] = dirXZ[0];
        kb.dirXZ[1] = dirXZ[1];
        kb.stepCm = stepCm;
        kb.groundDeltaY = groundDeltaY;
    }
    kb.active = (kb.stepCm > 0.5f && kb.durationSec > 0.01f) || hasCarry;
}

inline void CancelHitKnockback(HitKnockbackState& kb, uint32_t nowMs)
{
    if (!kb.active) return;
    kb.carryOffset[0] = kb.currentOffset[0];
    kb.carryOffset[1] = kb.currentOffset[1];
    kb.carryOffset[2] = kb.currentOffset[2];
    kb.stepCm = 0.0f;
    kb.groundDeltaY = 0.0f;
    kb.refused = true;
    kb.refusedAtMs = nowMs;
}

inline bool AdvanceHitKnockback(HitKnockbackState& kb, uint32_t nowMs, uint32_t recordSentMs, uint32_t acknowledgedAtMs,
                                bool ownImpactEcho, float hostMotionPhase, bool incapacitated,
                                float const basePos[3], float outPos[3])
{
    outPos[0] = basePos[0];
    outPos[1] = basePos[1];
    outPos[2] = basePos[2];
    if (!kb.active) {
        kb.currentOffset[0] = kb.currentOffset[1] = kb.currentOffset[2] = 0.0f;
        return false;
    }
    if (incapacitated) {
        kb = HitKnockbackState{};
        return false;
    }
    constexpr float CarryFadeSec = 0.12f;
    const uint32_t carryOrigin = kb.refused ? kb.refusedAtMs : kb.startedAtMs;
    const float carryAge = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - carryOrigin)) / 1000.0f);
    const float carryScale = carryAge >= CarryFadeSec ? 0.0f : (1.0f - SmoothStep01(carryAge / CarryFadeSec));
    if (kb.refused) {
        kb.currentOffset[0] = kb.carryOffset[0] * carryScale;
        kb.currentOffset[1] = kb.carryOffset[1] * carryScale;
        kb.currentOffset[2] = kb.carryOffset[2] * carryScale;
        if (carryScale <= 0.001f) {
            kb = HitKnockbackState{};
            return false;
        }
        outPos[0] += kb.currentOffset[0];
        outPos[1] += kb.currentOffset[1];
        outPos[2] += kb.currentOffset[2];
        return true;
    }

    const float elapsedSec = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - kb.startedAtMs)) / 1000.0f);
    const float localProgress = kb.durationSec > 0.01f ? std::clamp(elapsedSec / kb.durationSec, 0.0f, 1.0f) : 0.0f;

    float hostProgress = 0.0f;
    if (ownImpactEcho) {
        kb.hostImpactSeen = true;
        hostProgress = std::clamp(hostMotionPhase, 0.0f, 1.0f);
    } else if (kb.hostImpactSeen) {
        kb.hostImpactDone = true;
        hostProgress = 1.0f;
    } else if (acknowledgedAtMs != 0) {
        // Native ZS_Stumble starts sAICombatMoveStart on the host frame following OnHitNpc.
        const int32_t hostAfterAckMs = static_cast<int32_t>(recordSentMs - acknowledgedAtMs) - 16;
        if (hostAfterAckMs > 0) {
            hostProgress = kb.durationSec > 0.01f
                ? std::clamp((static_cast<float>(hostAfterAckMs) / 1000.0f) / kb.durationSec, 0.0f, 1.0f)
                : 1.0f;
            if (hostProgress >= 1.0f) kb.hostImpactDone = true;
        }
    }

    float lead = std::max(0.0f, localProgress - hostProgress);
    if (!acknowledgedAtMs && !kb.hostImpactSeen) {
        const float holdLimit = kb.durationSec + 0.25f;
        if (elapsedSec > holdLimit) {
            const float fade = 1.0f - SmoothStep01((elapsedSec - holdLimit) / 0.18f);
            lead *= fade;
            if (fade <= 0.001f) kb.hostImpactDone = true;
        }
    } else if (elapsedSec > kb.durationSec + 0.80f) {
        const float fade = 1.0f - SmoothStep01((elapsedSec - (kb.durationSec + 0.80f)) / 0.15f);
        lead *= fade;
        if (fade <= 0.001f) kb.hostImpactDone = true;
    }

    kb.currentOffset[0] = kb.carryOffset[0] * carryScale + kb.dirXZ[0] * (kb.stepCm * lead);
    kb.currentOffset[1] = kb.carryOffset[1] * carryScale + kb.groundDeltaY * lead;
    kb.currentOffset[2] = kb.carryOffset[2] * carryScale + kb.dirXZ[1] * (kb.stepCm * lead);

    if ((kb.hostImpactDone || lead <= 0.0005f) && localProgress >= 1.0f && carryScale <= 0.0005f) {
        kb = HitKnockbackState{};
        return false;
    }
    outPos[0] += kb.currentOffset[0];
    outPos[1] += kb.currentOffset[1];
    outPos[2] += kb.currentOffset[2];
    return true;
}

} // namespace g3t::npcplayback_hit_policy
