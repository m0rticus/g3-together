// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace g3t::npcplayback_hit_policy {

// Maximum bridge when a high-RTT host's Stumble_Hit/Recover is still in its early frames after the
// local predicted Stumble_Hit + Stumble_Recover sequence has finished. Prevents rewinding without
// freezing the NPC across multi-hundred-ms backlogs.
constexpr float MaxEchoBridgeSeconds = 0.15f;
constexpr float HostTailHandoffPhase = 0.75f;
constexpr float RecoverTailHandoffPhase = 0.55f;
constexpr float HitEntryBlendSeconds = 0.065f;
constexpr float HitRecoverBlendSeconds = 0.075f;
constexpr float HitExitBlendSeconds = 0.120f;
constexpr float KnockbackSettleSeconds = 0.160f;
constexpr float KnockbackRefusedFadeSeconds = 0.120f;
constexpr uint32_t PendingHitTimeoutMs = 2000u;
constexpr int32_t HostHitStampMinHoldMs = 250;

// Matches net::HitReaction values without pulling net.h into policy unit tests.
constexpr uint8_t ReactionNone = 0;
constexpr uint8_t ReactionQuickStumble = 1;
constexpr uint8_t ReactionStumble = 2;
constexpr uint8_t ReactionPierceStumble = 3;
constexpr uint8_t ReactionSitKnockDown = 4;

// Matches CP 1.75 GameEnum.h values (static_assert-verified in npcplayback_hit.cpp).
constexpr int SpeciesOrc = 5;
constexpr int UseNone = 0;
constexpr int Use1H = 2;
constexpr int UseShield = 9;
constexpr int UseTorch = 24;
constexpr int PoseNone = 0;
constexpr int PoseP0 = 1;
constexpr int PoseP1 = 2;
constexpr int PoseP2 = 3;
constexpr int PoseP3 = 4;

// Initial hit-impact stumble clip (excludes Stumble_Recover).
inline bool HitImpactMotion(char const* name)
{
    return name && std::strstr(name, "Stumble") && !std::strstr(name, "_Recover_");
}

// Recovery clip chained immediately after Stumble_Hit by Game.dll sAICombatMoveStartRecover (0x1016e360).
inline bool HitRecoverMotion(char const* name)
{
    return name && std::strstr(name, "Stumble") && std::strstr(name, "_Recover_") != nullptr;
}

// Matches any stumble sequence clip (both Hit and Recover phases) for echo and effect suppression.
inline bool HitStumbleSequenceMotion(char const* name)
{
    return name && std::strstr(name, "Stumble") != nullptr;
}

// Matches CP 1.75 Script_Game.dll _AI_Stumble (0x1004aaf0) and _AI_QuickStumble (0x100489c0):
// Contains(currentMovementAni, "_L.") selects gEAction_StumbleL (28); otherwise gEAction_StumbleR (27).
inline bool NativeStumbleLeftFoot(char const* currentMotion)
{
    return currentMotion && std::strstr(currentMotion, "_L.") != nullptr;
}

// Matches CP 1.75 Script_Game.dll _AI_QuickStumble (0x100489c0, 0x10048b08..0x10048b5d):
// QuickStumble (0x19) is used only for Orcs or 1H + {None, Shield, Torch}; all other stances
// (2H, Staff, Polearm, Bow, CrossBow, Fist, Dual) fall through to StumbleL/StumbleR at 1.3x.
inline bool NativeQuickStumbleAllowed(int species, int rightUse, int leftUse)
{
    if (species == SpeciesOrc) return true;
    return rightUse == Use1H && (leftUse == UseNone || leftUse == UseShield || leftUse == UseTorch);
}

// Extracts token 4 primary pose (P0..P3 -> 1..4) from a Gothic 3 .xmot filename so passive mirrors
// resolve the exact same P0/P1 stumble variant as the host.
inline int ParseMotionPrimaryPose(char const* motionName)
{
    if (!motionName || !*motionName) return PoseNone;
    char const* base = motionName;
    for (char const* p = motionName; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    int token = 0;
    char const* tokenStart = base;
    for (char const* p = base;; ++p) {
        if (*p == '_' || *p == '.' || *p == '\0') {
            if (token == 4 && (p - tokenStart) == 2 && tokenStart[0] == 'P' &&
                tokenStart[1] >= '0' && tokenStart[1] <= '3') {
                return PoseP0 + (tokenStart[1] - '0');
            }
            if (*p != '_') break;
            ++token;
            tokenStart = p + 1;
        }
    }
    return PoseNone;
}

// Builds the native 10-token gEPhase_Recover GetAniName stem (ending in '_00_') from a resolved Hit .xmot name,
// matching Game.dll sAICombatMoveStartRecover (0x1016e360) + GetAniName (0x1016f840): replaces token 6 ("Hit")
// with "Recover" and updates token 4 (entry PrimaryPose) to the Hit clip's exit pose token (hitIdx + 6, e.g. P0 -> P1).
inline bool BuildRecoverStemFromHitMotion(char const* hitMotion, char* outStem, std::size_t outCap,
                                          bool useExitPose = true)
{
    if (!outStem || outCap == 0) return false;
    outStem[0] = '\0';
    if (!hitMotion || !*hitMotion) return false;
    char const* base = hitMotion;
    for (char const* p = hitMotion; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    struct Span { char const* s; std::size_t n; };
    Span tokens[20] = {};
    int count = 0;
    char const* end = base;
    while (*end) ++end;
    if (end - base >= 5 && std::memcmp(end - 5, ".xmot", 5) == 0) end -= 5;
    char const* start = base;
    for (char const* p = base; p <= end && count < 20; ++p) {
        if (p == end || *p == '_') {
            tokens[count++] = {start, static_cast<std::size_t>(p - start)};
            start = p + 1;
        }
    }
    int hitIdx = -1;
    for (int i = 4; i < count; ++i) {
        if (tokens[i].n == 3 && std::memcmp(tokens[i].s, "Hit", 3) == 0) { hitIdx = i; break; }
    }
    if (hitIdx < 0) return false;
    // Native GetAniName stem has 4 tokens after Hit: <Action>_Recover_<N|O>_<Dir>_<00>_
    const int stemCount = (hitIdx + 4 <= count) ? (hitIdx + 4) : count;
    char dualExitBuf[2] = {'P', '0'};
    Span exitPose = {nullptr, 0};
    const int exitIdx = hitIdx + 6;
    if (useExitPose && exitIdx < count && tokens[exitIdx].s[0] == 'P') {
        if (tokens[exitIdx].n == 2 && tokens[exitIdx].s[1] >= '0' && tokens[exitIdx].s[1] <= '3') {
            exitPose = tokens[exitIdx];
        } else if (tokens[exitIdx].n == 3 && tokens[exitIdx].s[1] >= '0' && tokens[exitIdx].s[1] <= '3' &&
                   tokens[exitIdx].s[2] >= '0' && tokens[exitIdx].s[2] <= '3') {
            const char entryDigit = (4 < count && tokens[4].n == 2 && tokens[4].s[0] == 'P') ? tokens[4].s[1] : '\0';
            dualExitBuf[1] = (tokens[exitIdx].s[1] != entryDigit) ? tokens[exitIdx].s[1] : tokens[exitIdx].s[2];
            exitPose = {dualExitBuf, 2};
        }
    }
    std::size_t pos = 0;
    for (int i = 0; i < stemCount; ++i) {
        char const* src = tokens[i].s;
        std::size_t len = tokens[i].n;
        if (i == hitIdx) {
            src = "Recover";
            len = 7;
        } else if (i == 4 && exitPose.s && len >= 2 && src[0] == 'P') {
            src = exitPose.s;
            len = exitPose.n;
        }
        if (pos + len + 2 > outCap) { outStem[0] = '\0'; return false; }
        if (len) std::memcpy(outStem + pos, src, len);
        pos += len;
        outStem[pos++] = '_';
    }
    outStem[pos] = '\0';
    return pos > 0;
}

// Suppresses own-hit stumble echoes across back-to-back combo hits even after acknowledgedHit advances to hit #2.
inline bool OwnImpactSequenceSeen(uint16_t acknowledgedHit, uint16_t recordHitSequence)
{
    return acknowledgedHit != 0 && recordHitSequence != 0 &&
           static_cast<int16_t>(acknowledgedHit - recordHitSequence) >= 0;
}

// Local predicted stagger runs strictly for its own clip duration and never pins phase=1.0f
// while waiting on network acknowledgement or delayed buffered state.
inline bool LocalStaggerActive(bool hasLocalMotion, float localAgeSeconds, float localDurationSeconds)
{
    return hasLocalMotion && localDurationSeconds > 0.0f &&
           localAgeSeconds >= 0.0f && localAgeSeconds < localDurationSeconds;
}

enum class LocalStumbleStage : uint8_t {
    None = 0,
    Hit = 1,
    Recover = 2,
};

struct LocalStumbleStep {
    LocalStumbleStage stage = LocalStumbleStage::None;
    float phase = 0.0f;
    float totalDurationSec = 0.0f;
};

// Evaluates the chained local Hit -> Recover animation timeline matching Game.dll
// sAICombatMoveInstr (0x101696e0) -> sAICombatMoveStartRecover (0x1016e360).
inline LocalStumbleStep EvaluateLocalStumbleTimeline(bool hasHitMotion, bool hasRecoverMotion,
                                                     float localAgeSec, float hitDurationSec,
                                                     float recoverDurationSec,
                                                     bool hostInterruptingNonStumble = false)
{
    LocalStumbleStep out;
    if (!hasHitMotion || !(hitDurationSec > 0.0f) || localAgeSec < 0.0f) return out;
    const float recoverSec = (hasRecoverMotion && recoverDurationSec > 0.01f) ? recoverDurationSec : 0.0f;
    out.totalDurationSec = hitDurationSec + recoverSec;
    if (localAgeSec < hitDurationSec) {
        out.stage = LocalStumbleStage::Hit;
        out.phase = std::clamp(localAgeSec / hitDurationSec, 0.0f, 1.0f);
        return out;
    }
    if (recoverSec > 0.0f && localAgeSec < out.totalDurationSec) {
        if (hostInterruptingNonStumble) {
            out.phase = 1.0f;
            return out;
        }
        const float recoverAge = localAgeSec - hitDurationSec;
        out.stage = LocalStumbleStage::Recover;
        out.phase = std::clamp(recoverAge / recoverSec, 0.0f, 1.0f);
        return out;
    }
    out.phase = 1.0f;
    return out;
}

inline bool HoldSuppressedImpact(bool localActive, bool hasLocalMotion, bool suppressedSlot0,
                                 float localAgeSeconds, float localDurationSeconds,
                                 bool haveHostPose, float hostPhase, bool hostInRecover = false)
{
    if (localActive || !hasLocalMotion || !suppressedSlot0) return false;
    const float overtime = localAgeSeconds - localDurationSeconds;
    if (overtime >= MaxEchoBridgeSeconds) return false;
    const float tailPhase = hostInRecover ? RecoverTailHandoffPhase : HostTailHandoffPhase;
    if (haveHostPose && hostPhase >= tailPhase) return false;
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

inline float PoseBlendAlpha(bool active, uint32_t startedAtMs, uint32_t nowMs, float durationSec)
{
    if (!active || !(durationSec > 0.001f)) return 1.0f;
    const float elapsedSec = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - startedAtMs)) / 1000.0f);
    if (elapsedSec >= durationSec) return 1.0f;
    return SmoothStep01(elapsedSec / durationSec);
}

// World-anchored 1x knockback trajectory matching CP 1.75 Game.dll sAICombatMoveStart (0x1016abb0):
// Translates linearly from anchorPos to goalPos = anchorPos + dirXZ * stepCm over [0, hitDurationSec],
// holds planted at goalPos during Stumble_Recover (zero native velocity in sAICombatMoveStartRecover),
// and smoothly reconverges to authoritative basePos once the buffered host snapshot completes Hit.
// Because the trajectory is anchored in world space rather than added on top of basePos, buffered
// host position updates and back-to-back combo hits never stack beyond exact 1.0x native displacement.
struct HitKnockbackState {
    bool active = false;
    bool refused = false;
    bool settling = false;
    bool hasPresentedPos = false;
    bool hostImpactSeen = false;
    bool hostImpactDone = false;
    uint16_t hitSequence = 0;
    uint32_t startedAtMs = 0;
    uint32_t settleStartMs = 0;
    float hitDurationSec = 0.0f;
    float recoverDurationSec = 0.0f;
    float dirXZ[2] = {};
    float stepCm = 0.0f;
    float groundDeltaY = 0.0f;
    float anchorPos[3] = {};
    float goalPos[3] = {};
    float settleOriginPos[3] = {};
    float lastPresentedPos[3] = {};
    float currentOffset[3] = {};
};

inline bool KnockbackMatchesHostRecord(HitKnockbackState const& kb, uint16_t acknowledgedHit, uint16_t recordHitSequence)
{
    return kb.active && kb.hitSequence != 0 && acknowledgedHit == kb.hitSequence &&
           recordHitSequence == kb.hitSequence;
}

inline void StartHitKnockback(HitKnockbackState& kb, uint16_t hitSequence, uint32_t nowMs,
                              float hitDurationSec, float recoverDurationSec,
                              float const anchorPos[3], float const dirXZ[2],
                              float stepCm, float groundDeltaY = 0.0f)
{
    float startWorld[3] = {};
    if (anchorPos) {
        startWorld[0] = anchorPos[0];
        startWorld[1] = anchorPos[1];
        startWorld[2] = anchorPos[2];
    } else if (kb.hasPresentedPos) {
        startWorld[0] = kb.lastPresentedPos[0];
        startWorld[1] = kb.lastPresentedPos[1];
        startWorld[2] = kb.lastPresentedPos[2];
    }
    kb = HitKnockbackState{};
    kb.hitSequence = hitSequence;
    kb.startedAtMs = nowMs;
    kb.hitDurationSec = std::max(0.0f, hitDurationSec);
    kb.recoverDurationSec = std::max(0.0f, recoverDurationSec);
    kb.anchorPos[0] = kb.goalPos[0] = kb.lastPresentedPos[0] = startWorld[0];
    kb.anchorPos[1] = kb.goalPos[1] = kb.lastPresentedPos[1] = startWorld[1];
    kb.anchorPos[2] = kb.goalPos[2] = kb.lastPresentedPos[2] = startWorld[2];
    kb.hasPresentedPos = true;
    if (dirXZ && stepCm > 0.5f && kb.hitDurationSec > 0.01f) {
        kb.dirXZ[0] = dirXZ[0];
        kb.dirXZ[1] = dirXZ[1];
        kb.stepCm = stepCm;
        kb.groundDeltaY = groundDeltaY;
        kb.goalPos[0] = startWorld[0] + dirXZ[0] * stepCm;
        kb.goalPos[1] = startWorld[1] + groundDeltaY;
        kb.goalPos[2] = startWorld[2] + dirXZ[1] * stepCm;
        kb.active = true;
    }
}

inline void CancelHitKnockback(HitKnockbackState& kb, uint32_t nowMs)
{
    if (!kb.active) return;
    kb.settleOriginPos[0] = kb.hasPresentedPos ? kb.lastPresentedPos[0] : kb.anchorPos[0];
    kb.settleOriginPos[1] = kb.hasPresentedPos ? kb.lastPresentedPos[1] : kb.anchorPos[1];
    kb.settleOriginPos[2] = kb.hasPresentedPos ? kb.lastPresentedPos[2] : kb.anchorPos[2];
    kb.stepCm = 0.0f;
    kb.groundDeltaY = 0.0f;
    kb.refused = true;
    kb.settling = true;
    kb.settleStartMs = nowMs;
}

inline bool AdvanceHitKnockback(HitKnockbackState& kb, uint32_t nowMs, uint32_t recordSentMs, uint32_t acknowledgedAtMs,
                                bool ownImpactEcho, bool ownRecoverEcho, float hostMotionPhase, bool incapacitated,
                                float const basePos[3], float outPos[3])
{
    (void)hostMotionPhase;
    outPos[0] = basePos[0];
    outPos[1] = basePos[1];
    outPos[2] = basePos[2];
    if (!kb.active) {
        kb.lastPresentedPos[0] = basePos[0];
        kb.lastPresentedPos[1] = basePos[1];
        kb.lastPresentedPos[2] = basePos[2];
        kb.hasPresentedPos = true;
        kb.currentOffset[0] = kb.currentOffset[1] = kb.currentOffset[2] = 0.0f;
        return false;
    }
    if (incapacitated) {
        kb = HitKnockbackState{};
        return false;
    }
    float const* refPos = kb.settling ? kb.settleOriginPos : kb.goalPos;
    const float jumpDx = basePos[0] - refPos[0];
    const float jumpDz = basePos[2] - refPos[2];
    const float maxAllowedSpan = kb.stepCm + 260.0f;
    if ((jumpDx * jumpDx + jumpDz * jumpDz) > maxAllowedSpan * maxAllowedSpan) {
        kb = HitKnockbackState{};
        kb.lastPresentedPos[0] = basePos[0];
        kb.lastPresentedPos[1] = basePos[1];
        kb.lastPresentedPos[2] = basePos[2];
        kb.hasPresentedPos = true;
        return false;
    }

    if (kb.refused) {
        const float ageSec = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - kb.settleStartMs)) / 1000.0f);
        const float s = SmoothStep01(ageSec / KnockbackRefusedFadeSeconds);
        if (s >= 0.999f) {
            kb = HitKnockbackState{};
            kb.lastPresentedPos[0] = basePos[0];
            kb.lastPresentedPos[1] = basePos[1];
            kb.lastPresentedPos[2] = basePos[2];
            kb.hasPresentedPos = true;
            return false;
        }
        for (int k = 0; k < 3; ++k) {
            outPos[k] = kb.settleOriginPos[k] + (basePos[k] - kb.settleOriginPos[k]) * s;
            kb.lastPresentedPos[k] = outPos[k];
            kb.currentOffset[k] = outPos[k] - basePos[k];
        }
        kb.hasPresentedPos = true;
        return true;
    }

    if (ownImpactEcho) {
        kb.hostImpactSeen = true;
    } else if (ownRecoverEcho || kb.hostImpactSeen) {
        kb.hostImpactDone = true;
    } else if (acknowledgedAtMs != 0) {
        // Native ZS_Stumble starts sAICombatMoveStart on the host frame after OnHitNpc and finishes after hitDurationSec.
        const int32_t hostAfterAckMs = static_cast<int32_t>(recordSentMs - acknowledgedAtMs) - 16;
        const int32_t hitMs = static_cast<int32_t>(kb.hitDurationSec * 1000.0f) + 16;
        if (hostAfterAckMs >= hitMs) kb.hostImpactDone = true;
    }

    const float elapsedSec = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - kb.startedAtMs)) / 1000.0f);
    const float localProgress = kb.hitDurationSec > 0.01f ? std::clamp(elapsedSec / kb.hitDurationSec, 0.0f, 1.0f) : 1.0f;
    float targetPos[3] = {
        kb.anchorPos[0] + (kb.goalPos[0] - kb.anchorPos[0]) * localProgress,
        kb.anchorPos[1] + (kb.goalPos[1] - kb.anchorPos[1]) * localProgress,
        kb.anchorPos[2] + (kb.goalPos[2] - kb.anchorPos[2]) * localProgress,
    };

    if (localProgress >= 1.0f && !kb.settling) {
        const float unackHoldLimit = kb.hitDurationSec + 0.25f;
        const float ackHoldLimit = kb.hitDurationSec + std::max(kb.recoverDurationSec, 0.28f) + 0.20f;
        const bool unackExpired = !acknowledgedAtMs && !kb.hostImpactSeen && elapsedSec >= unackHoldLimit;
        if (kb.hostImpactDone || unackExpired || elapsedSec >= ackHoldLimit) {
            kb.settling = true;
            kb.settleStartMs = nowMs;
            kb.settleOriginPos[0] = targetPos[0];
            kb.settleOriginPos[1] = targetPos[1];
            kb.settleOriginPos[2] = targetPos[2];
        }
    }

    if (kb.settling) {
        const float settleAge = std::max(0.0f, static_cast<float>(static_cast<int32_t>(nowMs - kb.settleStartMs)) / 1000.0f);
        const float s = SmoothStep01(settleAge / KnockbackSettleSeconds);
        if (s >= 0.999f) {
            kb = HitKnockbackState{};
            kb.lastPresentedPos[0] = basePos[0];
            kb.lastPresentedPos[1] = basePos[1];
            kb.lastPresentedPos[2] = basePos[2];
            kb.hasPresentedPos = true;
            return false;
        }
        for (int k = 0; k < 3; ++k) outPos[k] = kb.settleOriginPos[k] + (basePos[k] - kb.settleOriginPos[k]) * s;
    } else {
        outPos[0] = targetPos[0];
        outPos[1] = targetPos[1];
        outPos[2] = targetPos[2];
    }

    for (int k = 0; k < 3; ++k) {
        kb.lastPresentedPos[k] = outPos[k];
        kb.currentOffset[k] = outPos[k] - basePos[k];
    }
    kb.hasPresentedPos = true;
    return true;
}

} // namespace g3t::npcplayback_hit_policy
