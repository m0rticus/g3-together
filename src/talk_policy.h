// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy decisions for stationary client-initiated NPC dialogue holds and gesture forwarding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::talk_policy {

constexpr int AiModeCombat = 3;      // gEAIMode_Combat
constexpr int AiModeTalk = 6;        // gEAIMode_Talk
constexpr int MoveModeSneak = 2;     // gECharMovementMode_Sneak
constexpr int MoveModeSprint = 5;    // gECharMovementMode_Sprint

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

// Re-enter ZS_Talk whenever a live, non-combat, non-downed held NPC drops out of gEAIMode_Talk or "ZS_Talk".
inline bool ShouldRepacifyHeldTalk(bool isDead, bool isDown, int aiMode, char const* currentTask) noexcept
{
    if (isDead || isDown || aiMode == AiModeCombat) return false;
    return aiMode != AiModeTalk || !currentTask || std::strcmp(currentTask, "ZS_Talk") != 0;
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
    return isHost && holdValid && holdPlayer != 0 && holdPlayer == fromPlayer && !isDead && !isDown && aiMode != AiModeCombat;
}

} // namespace g3t::talk_policy
