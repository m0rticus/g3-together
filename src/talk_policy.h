// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy decisions for stationary client-initiated NPC dialogue holds and gesture forwarding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::talk_policy {

constexpr int AiModeCombat = 3;      // gEAIMode_Combat
constexpr int AiModeFlee = 4;        // gEAIMode_Flee
constexpr int AiModeArena = 5;       // gEAIMode_Arena
constexpr int AiModeTalk = 6;        // gEAIMode_Talk
constexpr int MoveModeSneak = 2;     // gECharMovementMode_Sneak
constexpr int MoveModeSprint = 5;    // gECharMovementMode_Sprint

constexpr int AniStateDummy0 = 0;         // gEAniState_Dummy0
constexpr int AniStateDummy1 = 1;         // gEAniState_Dummy1
constexpr int AniStateStand = 2;          // gEAniState_Stand
constexpr int AniStateKneel = 5;          // gEAniState_Kneel
constexpr int AniStateSitGround = 6;      // gEAniState_SitGround
constexpr int AniStateSitStool = 7;       // gEAniState_SitStool
constexpr int AniStateSitBench = 8;       // gEAniState_SitBench
constexpr int AniStateSitThrone = 9;      // gEAniState_SitThrone
constexpr int AniStateSleepBed = 10;      // gEAniState_SleepBed
constexpr int AniStateSleepGround = 11;   // gEAniState_SleepGround
constexpr int AniStateTalkStand = 22;     // gEAniState_TalkStand
constexpr int AniStateTalkSitGround = 23; // gEAniState_TalkSitGround
constexpr int AniStateTalkSitStool = 24;  // gEAniState_TalkSitStool
constexpr int AniStateTalkSitBench = 25;  // gEAniState_TalkSitBench
constexpr int AniStateTalkSitThrone = 26; // gEAniState_TalkSitThrone

constexpr int PoseNone = 0;   // gEPose_None
constexpr int PoseP0 = 1;     // gEPose_P0
constexpr int PoseCount = 25;

constexpr int UseTypeNone = 0;    // gEUseType_None
constexpr int UseTypeTorch = 24;  // gEUseType_Torch

constexpr uint32_t PendingClientTalkWindowMs = 4000u;

// Native Script_Game.dll ZS_Talk (0x10024ff0) and ZS_Talk_Loop (0x10025870) exit immediately via ContinueRoutine()
// when target == Entity::GetPlayer() and !PSInfoManager::IsRunning(). While the host holds an NPC in dialogue for a
// remote player, Routine and Script must NOT spoof Entity::GetPlayer() to the remote puppet so ZS_Talk_Loop takes its
// NPC-to-NPC conversation branch (looping cleanly while speaker.Routine.GetCurrentTask() == "ZS_Talk").
constexpr uint8_t EffectiveActingPlayer(bool isHost, bool heldInTalk, uint8_t contextPlayer) noexcept
{
    return (!isHost || heldInTalk) ? 0 : contextPlayer;
}

// body::Drive repacifies a puppet whenever CurrentTask != Idle ("ZS_HangAround_DoNothing"). Restore Idle right before
// body::Drive and expose "ZS_Talk" right after body::Drive while the puppet's player holds an NPC in dialogue.
constexpr char const* PuppetRoutineTask(bool beforeDrive, bool heldDown, bool holdingNpcInTalk, char const* idleTask) noexcept
{
    return (!beforeDrive && !heldDown && holdingNpcInTalk) ? "ZS_Talk" : idleTask;
}

// Stop ground locomotion (Sneak..Sprint) while held in dialogue without interfering with physics/fall/ragdoll states.
constexpr bool ShouldStopHeldLocomotion(int movementMode) noexcept
{
    return movementMode >= MoveModeSneak && movementMode <= MoveModeSprint;
}

// Re-enter ZS_Talk whenever a live, non-combat, non-arena, non-downed held NPC drops out of gEAIMode_Talk or "ZS_Talk".
inline bool ShouldRepacifyHeldTalk(bool isDead, bool isDown, int aiMode, char const* currentTask) noexcept
{
    if (isDead || isDown || aiMode == AiModeCombat || aiMode == AiModeFlee || aiMode == AiModeArena) return false;
    return aiMode != AiModeTalk || !currentTask || std::strcmp(currentTask, "ZS_Talk") != 0;
}

constexpr bool ShouldRestartRoutineOnTalkRelease(bool isDead, bool isDown, int aiMode) noexcept
{
    return !isDead && !isDown && aiMode != AiModeCombat && aiMode != AiModeFlee && aiMode != AiModeArena;
}

constexpr bool IsSeatedTalkAniState(int aniState) noexcept
{
    return (aniState >= AniStateSitGround && aniState <= AniStateSitThrone) ||
           (aniState >= AniStateTalkSitGround && aniState <= AniStateTalkSitThrone);
}

// Native ZS_Talk (0x1002562c) aligns standing NPCs to the speaker via SetAlignmentTarget, while seated/sleeping NPCs
// keep their bench/stool/throne root orientation and track the speaker via head LookAt only.
constexpr bool ShouldAlignTalkBodyToSpeaker(int aniState) noexcept
{
    return !IsSeatedTalkAniState(aniState) && aniState != AniStateSleepBed && aniState != AniStateSleepGround;
}

// Passive mirrors replay .xmot clips directly on emfx2Actor without updating Routine.AniState or NPC.PrimaryPose.
// Infer the partner's posture from its active slot-0 clip before ZS_Talk / _AI_Say_Ex calls Entity::GetAni.
inline int InferDialogPartnerAniState(char const* slot0Motion, int currentAniState) noexcept
{
    if (slot0Motion && *slot0Motion && std::strcmp(slot0Motion, "<fault>") != 0) {
        // StandUp / GetUp clips start from _Sit*_ or _Sleep*_ in their prefix (e.g.
        // Hero_SitGround_None_None_P0_StandUp_End_N_Fwd_00_%.xmot) and leave the NPC standing.
        if (std::strstr(slot0Motion, "_StandUp_") || std::strstr(slot0Motion, "_GetUp_"))
            return AniStateStand;
        if (std::strstr(slot0Motion, "_SitBench_")) return AniStateSitBench;
        if (std::strstr(slot0Motion, "_SitStool_")) return AniStateSitStool;
        if (std::strstr(slot0Motion, "_SitGround_")) return AniStateSitGround;
        if (std::strstr(slot0Motion, "_SitThrone_")) return AniStateSitThrone;
        if (std::strstr(slot0Motion, "_Kneel_")) return AniStateKneel;
        if (std::strstr(slot0Motion, "_SleepBed_")) return AniStateSleepBed;
        if (std::strstr(slot0Motion, "_SleepGround_")) return AniStateSleepGround;
        if (std::strstr(slot0Motion, "_Stand_") || std::strstr(slot0Motion, "_Move_") ||
            std::strstr(slot0Motion, "_Turn_"))
            return AniStateStand;
    }
    if (currentAniState == AniStateTalkSitGround) return AniStateSitGround;
    if (currentAniState == AniStateTalkSitStool) return AniStateSitStool;
    if (currentAniState == AniStateTalkSitBench) return AniStateSitBench;
    if (currentAniState == AniStateTalkSitThrone) return AniStateSitThrone;
    if (currentAniState == AniStateTalkStand) return AniStateStand;
    if (currentAniState >= AniStateKneel && currentAniState <= AniStateSleepGround) return currentAniState;
    return AniStateStand;
}

// Every *_Say_* clip in _compiledAnimation.pak uses P0 (gEPose_P0 = 1). Passive mirrors that never ran StartPlayAni
// still hold gEPose_None (0), causing Entity::GetAniEx to format "*_None_Say_*" (missing animation, frozen hands).
constexpr int NormalizeDialogPrimaryPose(int currentPose, bool emptyOrTorchHands) noexcept
{
    if (emptyOrTorchHands || currentPose <= PoseNone || currentPose >= PoseCount) return PoseP0;
    return currentPose;
}

// Seated *_Say_Ambient_* clips exist only for None_None; standing ZS_Talk (0x10002dd0) also clears all held items
// except a right-hand torch. Clear ambient bench props (meat, cup, bread, bowl, smoke) up front so frame-1 Say works.
constexpr bool ShouldClearHeldItemsForTalk(int aniState, int leftUseType, int rightUseType) noexcept
{
    if (IsSeatedTalkAniState(aniState))
        return leftUseType != UseTypeNone || rightUseType != UseTypeNone;
    return leftUseType != UseTypeNone || (rightUseType != UseTypeNone && rightUseType != UseTypeTorch);
}

inline bool IsStandUpMotion(char const* motion) noexcept
{
    return motion && std::strstr(motion, "_StandUp_") != nullptr;
}

// ai::FilterState binds guards::Acting(StateActor) inside ZS_Talk, causing host ZS_Talk to see !PSInfoManager::IsRunning()
// and call ContinueRoutine() every tick. Block ContinueRoutine and non-reaction task exits while the host holds the NPC.
inline bool ShouldBlockHeldTalkEvent(bool isHost, bool heldInTalk, bool isDead, bool isDown,
                                     int aiMode, char const* eventName) noexcept
{
    if (!isHost || !heldInTalk || isDead || isDown ||
        aiMode == AiModeCombat || aiMode == AiModeFlee || aiMode == AiModeArena)
        return false;
    return eventName && std::strcmp(eventName, "ContinueRoutine") == 0;
}

inline bool ShouldBindActingPlayerForEvent(bool isHost, bool heldInTalk, char const* eventName) noexcept
{
    if (!isHost || heldInTalk || !eventName || !*eventName) return false;
    return std::strcmp(eventName, "ContinueRoutine") == 0 ||
           std::strcmp(eventName, "OnGuidePlayer") == 0 ||
           std::strcmp(eventName, "OnFollowPlayer") == 0 ||
           std::strcmp(eventName, "OnArena") == 0 ||
           std::strcmp(eventName, "SendToArena") == 0;
}

inline bool IsAllowedHeldTalkTask(char const* taskName) noexcept
{
    if (!taskName || !*taskName) return true;
    return std::strcmp(taskName, "ZS_Talk") == 0 ||
           std::strcmp(taskName, "ZS_Arena") == 0 ||
           std::strcmp(taskName, "ZS_Attack") == 0 ||
           std::strcmp(taskName, "ZS_Flee") == 0 ||
           std::strcmp(taskName, "ZS_Dead") == 0 ||
           std::strcmp(taskName, "ZS_Unconscious") == 0 ||
           std::strcmp(taskName, "ZS_SitKnockDown") == 0 ||
           std::strncmp(taskName, "ZS_Assess", 9) == 0 ||
           std::strncmp(taskName, "ZS_Lie", 6) == 0 ||
           std::strncmp(taskName, "ZS_RagDoll", 10) == 0 ||
           std::strncmp(taskName, "ZS_Stumble", 10) == 0 ||
           std::strncmp(taskName, "ZS_QuickStumble", 15) == 0 ||
           std::strncmp(taskName, "ZS_ParadeStumble", 16) == 0 ||
           std::strncmp(taskName, "ZS_QuickParadeStumble", 21) == 0 ||
           std::strncmp(taskName, "ZS_HeavyParadeStumble", 21) == 0 ||
           std::strncmp(taskName, "ZS_PierceStumble", 16) == 0;
}

inline bool ShouldBlockHeldTalkTask(bool isHost, bool heldInTalk, bool isDead, bool isDown,
                                    int aiMode, char const* taskName) noexcept
{
    if (!isHost || !heldInTalk || isDead || isDown ||
        aiMode == AiModeCombat || aiMode == AiModeFlee || aiMode == AiModeArena)
        return false;
    return taskName && *taskName && !IsAllowedHeldTalkTask(taskName);
}

// AssessTalk enters ZS_Talk several frames before Hero's ZS_Talk calls PSInfoManager::Start. Keep the addressed
// partner recognized across that startup window so Silence() and Replay do not wipe ZS_Talk before the first Say line.
constexpr bool ShouldKeepPendingClientTalk(bool pendingActive, bool dialogRunning, bool heroValidAndAlive,
                                           uint32_t nowMs, uint32_t untilMs) noexcept
{
    return pendingActive && !dialogRunning && heroValidAndAlive && static_cast<int32_t>(untilMs - nowMs) > 0;
}

// Passive mirrors clear residual task/state names except for the client's active dialogue partner, whose local script
// routine executes Say/gesture commands for the client's info manager.
constexpr bool ShouldSilencePassiveRoutine(bool isPassive, bool talkingWithHero) noexcept
{
    return isPassive && !talkingWithHero;
}

// Track any valid NPC dialogue partner on the client (even before or between mirror stream transitions) so SendTalk
// always brackets the client's dialogue session on the host.
constexpr bool ShouldTrackClientDialogPartner(bool dialogRunning, bool hasNpcProperty, bool isHero, bool isPuppet) noexcept
{
    return dialogRunning && hasNpcProperty && !isHero && !isPuppet;
}

// Event-driven gesture forwarding from slot-0 PlayMotion on the client's dialogue partner: forward non-locomotion,
// non-fault motions immediately without rate caps or same-clip suppression across consecutive Say lines.
inline bool ShouldForwardGesture(bool gesturesEnabled, bool talkingWithPartner, char const* motion) noexcept
{
    if (!gesturesEnabled || !talkingWithPartner || !motion || !*motion) return false;
    if (std::strcmp(motion, "<fault>") == 0) return false;
    return std::strstr(motion, "_Move_") == nullptr;
}

// Strip optional ".xmot" suffix for Entity::StartPlayAni while rejecting empty/fault names.
inline size_t MotionStem(char const* motion, char* out, size_t capacity) noexcept
{
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!motion || !*motion || std::strcmp(motion, "<fault>") == 0) return 0;
    size_t len = std::strlen(motion);
    if (len >= 5 && (std::strcmp(motion + len - 5, ".xmot") == 0 || std::strcmp(motion + len - 5, ".XMOT") == 0))
        len -= 5;
    if (len == 0) return 0;
    if (len >= capacity) len = capacity - 1;
    std::memcpy(out, motion, len);
    out[len] = '\0';
    return len;
}

constexpr bool ShouldAcceptGestureOnHost(bool isHost, bool holdValid, uint8_t holdPlayer, uint8_t fromPlayer,
                                         bool isDead, bool isDown, int aiMode) noexcept
{
    return isHost && holdValid && holdPlayer != 0 && holdPlayer == fromPlayer &&
           !isDead && !isDown && aiMode != AiModeCombat && aiMode != AiModeArena;
}

// While a client's hero is actively talking to a live, non-downed, non-combat passive mirror, yield host pose/motion
// replay so local ZS_Talk, Say gestures, facial lip-sync, and hero-facing run smoothly without fighting Replay.
constexpr bool ShouldYieldMirrorToLocalTalk(bool talkingWithHero, bool killedOnHost, uint8_t hostFlags) noexcept
{
    return talkingWithHero && !killedOnHost && (hostFlags & 7u) == 0u;
}

// Allow local talk states, info scripts, and _AI_* helper functions on the client's active dialogue partner while keeping
// combat, death, flee, and ambient wandering states fenced.
inline bool ShouldAllowPassiveTalkScript(bool talkingWithHero, char const* scriptName) noexcept
{
    if (!talkingWithHero || !scriptName || !*scriptName) return false;
    return std::strcmp(scriptName, "ZS_Talk") == 0 ||
           std::strcmp(scriptName, "ZS_Talk_Loop") == 0 ||
           std::strncmp(scriptName, "IS_", 3) == 0 ||
           std::strncmp(scriptName, "_AI_", 4) == 0;
}

// Allow AssessTalk and client talk maintenance to enter "ZS_Talk" on the client's passive dialogue partner.
inline bool ShouldAllowPassiveTalkTask(bool talkingWithHero, bool assessingHeroTalk, char const* taskName) noexcept
{
    return (talkingWithHero || assessingHeroTalk) && taskName && std::strcmp(taskName, "ZS_Talk") == 0;
}

// Allow native ScriptRoutine, Animation, CharacterMovement, and Dialog property sets to tick on the client's active
// dialogue partner so gesture motions, facial lip-sync, LookAt constraints, and in-place facing advance smoothly.
constexpr bool ShouldAllowDialogPartnerProperty(bool dialogPartner, bool isScriptRoutine, bool isAnimation,
                                                bool isMovement, bool isDialog) noexcept
{
    return dialogPartner && (isScriptRoutine || isAnimation || isMovement || isDialog);
}

// Client MsgTalk is world-gated (Gated(MsgTalk) == true). Only mark a client talk hold as sent once net::Connected()
// (connected && inWorld) is true, and re-arm whenever admission resets or WelcomeSerial advances.
constexpr bool ShouldSendClientTalkHold(bool connected, bool holdSent, uint32_t sentWelcome, uint32_t currentWelcome) noexcept
{
    return connected && (!holdSent || sentWelcome != currentWelcome);
}

constexpr bool ShouldReleaseClientTalkHold(bool connected, bool holdSent) noexcept
{
    return connected && holdSent;
}

} // namespace g3t::talk_policy
