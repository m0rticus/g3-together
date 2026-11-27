// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy rules for client and host arena, sparring, and dialog-duel interactions.
#pragma once

#include <cstdint>
#include <cstring>

namespace g3t::arena_policy {

constexpr int QuestTypeArena = 5;       // gEQuestType_Arena
constexpr int QuestTypeSparring = 12;   // gEQuestType_Sparring

constexpr int QuestStatusOpen = 0;      // gEQuestStatus_Open
constexpr int QuestStatusRunning = 1;   // gEQuestStatus_Running
constexpr int QuestStatusSuccess = 2;   // gEQuestStatus_Success
constexpr int QuestStatusFailed = 3;    // gEQuestStatus_Failed
constexpr int QuestStatusObsolete = 4;  // gEQuestStatus_Obsolete
constexpr int QuestStatusCancelled = 5; // gEQuestStatus_Cancelled
constexpr int QuestStatusLost = 6;      // gEQuestStatus_Lost
constexpr int QuestStatusWon = 7;       // gEQuestStatus_Won

constexpr int ArenaStatusNone = 0;      // gEArenaStatus_None
constexpr int ArenaStatusRunning = 1;   // gEArenaStatus_Running

constexpr int AttackReasonReactToWeapon = 6;   // gEAttackReason_ReactToWeapon
constexpr int AttackReasonAngry = 7;           // gEAttackReason_Angry
constexpr int AttackReasonStopFight = 8;       // gEAttackReason_StopFight
constexpr int AttackReasonReactToDamage = 9;   // gEAttackReason_ReactToDamage
constexpr int AttackReasonEnemy = 12;          // gEAttackReason_Enemy
constexpr int AttackReasonMurder = 13;         // gEAttackReason_Murder
constexpr int AttackReasonPlayerCommand = 16;  // gEAttackReason_PlayerCommand
constexpr int AttackReasonDuel = 17;           // gEAttackReason_Duel (0x11)
constexpr int AttackReasonArena = 18;          // gEAttackReason_Arena (0x12)
constexpr int AttackReasonRevolution = 19;     // gEAttackReason_Revolution
constexpr int AttackReasonKill = 20;           // gEAttackReason_Kill

constexpr bool IsArenaOrSparringQuestType(int questType) noexcept
{
    return questType == QuestTypeArena || questType == QuestTypeSparring;
}

constexpr bool IsTerminalBoutQuestStatus(int status) noexcept
{
    return status == QuestStatusSuccess || status == QuestStatusFailed ||
           status == QuestStatusObsolete || status == QuestStatusCancelled ||
           status == QuestStatusLost || status == QuestStatusWon;
}

// Initial activation (Open -> Running) and rematch reactivation (Lost/Cancelled -> Running via ConditionType 21)
// are valid pre-Running states when matching an NPC's candidate bout quest.
constexpr bool IsCandidateBoutQuestStatus(int status) noexcept
{
    return status == QuestStatusOpen || status == QuestStatusLost || status == QuestStatusCancelled;
}

// Both initial activation (Open -> Running) and rematch reactivation (Lost -> Running via ConditionType 21)
// start a fresh bout for the player whose dialog triggered the transition.
constexpr bool IsBoutStartTransition(int questType, int prevStatus, int newStatus) noexcept
{
    return IsArenaOrSparringQuestType(questType) &&
           prevStatus != QuestStatusRunning &&
           newStatus == QuestStatusRunning;
}

constexpr bool IsBoutForfeitTransition(int questType, int prevStatus, int newStatus) noexcept
{
    return IsArenaOrSparringQuestType(questType) &&
           prevStatus == QuestStatusRunning &&
           newStatus == QuestStatusLost;
}

// gCInfo_PS::AreConditionsFulfilled (Game.dll 0x1c3370) blocks all Open (5), Activator (6), and Reactivator (21)
// dialogs for gEQuestType_Arena quests whenever the destination entity's gCArena_PS::Status == Running (1).
// Keep every arena entity's gCArena_PS::Status strictly equal to whether any Arena quest targeting it is Running.
constexpr int DesiredArenaStatus(int arenaQuestRunningCount) noexcept
{
    return arenaQuestRunningCount > 0 ? ArenaStatusRunning : ArenaStatusNone;
}

// Client local quest detection: if an Arena/Sparring quest becomes Running locally on a client without having
// arrived from host story sync/baseline, that client started the bout (including via an Arena Master when the
// opponent or arena entity was unloaded on the client).
constexpr bool ShouldClaimLocalClientBout(bool isHost, bool connected, int questType,
                                          int prevStatus, int newStatus, bool syncedFromHost) noexcept
{
    return !isHost && connected && !syncedFromHost &&
           IsBoutStartTransition(questType, prevStatus, newStatus);
}

// Layer 1 ring-exit guard: OnLeaveArea (Script_Game 0xad9a0 -> gCQuest_PS::OnLeave 0x158e90) runs for each
// machine's own hero. Suppress it when a spectator (not the active challenger) walks out of an active arena,
// while letting the actual challenger's ring-exit and any NPC ring-outs pass through to native OnLeaveArea.
constexpr bool ShouldSuppressPlayerLeaveArea(bool isLocalPlayerLeaving, bool areaHasRunningBout,
                                             uint8_t activeChallengerPlayer, uint8_t localPlayerId) noexcept
{
    if (!isLocalPlayerLeaving || !areaHasRunningBout || localPlayerId == 0) return false;
    if (activeChallengerPlayer == 0) return localPlayerId >= 2;
    return activeChallengerPlayer != localPlayerId;
}

// Layer 2 host story guard: prevent a non-challenger spectator client from overwriting a Running bout on the host
// (e.g. Build 83 Joey_Arena false Lost=6 from spectator Player 2), and prevent stale client states from regressing
// an already-completed (Success/Won) arena or sparring quest.
constexpr bool ShouldAcceptClientQuestStatusOnHost(int questType, int hostCurrentStatus,
                                                   uint8_t activeChallengerPlayer, uint8_t fromPlayer,
                                                   int clientNewStatus) noexcept
{
    if (!IsArenaOrSparringQuestType(questType)) return true;
    if (fromPlayer < 2) return false;
    if (hostCurrentStatus != QuestStatusOpen && clientNewStatus == QuestStatusOpen)
        return false;
    if (hostCurrentStatus == QuestStatusSuccess || hostCurrentStatus == QuestStatusWon)
        return clientNewStatus == hostCurrentStatus;
    if (hostCurrentStatus == QuestStatusRunning) {
        if (activeChallengerPlayer != 0 && activeChallengerPlayer != fromPlayer)
            return false;
        // Only legitimate challenger ring-exit/knockout forfeit (Cancelled/Lost) or completion is accepted while Running.
        return clientNewStatus == QuestStatusRunning ||
               clientNewStatus == QuestStatusLost ||
               clientNewStatus == QuestStatusSuccess ||
               clientNewStatus == QuestStatusWon ||
               clientNewStatus == QuestStatusFailed ||
               clientNewStatus == QuestStatusCancelled;
    }
    return true;
}

// Keep npcs::SetContext bound to the challenger across arena walk, active combat, stumbles, arena-end bark
// ("YouLeftTheArena" in ZS_Attack_Arena_End), unconsciousness, ransack, and standing back up.
inline bool IsArenaFighterBusyTask(char const* taskName) noexcept
{
    if (!taskName || !*taskName) return false;
    static char const* const parts[] = {
        "ZS_Arena",
        "ZS_Attack",
        "ZS_Unconscious",
        "ZS_LieKnockDown",
        "ZS_SitKnockDown",
        "ZS_PiercedKO",
        "ZS_RansackBody",
        "ZS_Stumble",
        "ZS_QuickStumble",
        "ZS_ParadeStumble",
        "ZS_QuickParadeStumble",
        "ZS_HeavyParadeStumble",
        "ZS_PierceStumble",
        "ZS_StandUp",
    };
    for (char const* part : parts)
        if (std::strstr(taskName, part) != nullptr) return true;
    return false;
}

inline bool HasFighterEnteredBoutTask(char const* taskName) noexcept
{
    if (!taskName || !*taskName) return false;
    if (std::strstr(taskName, "ZS_Attack_End") != nullptr ||
        std::strstr(taskName, "ZS_Attack_Arena_End") != nullptr)
        return false;
    return std::strstr(taskName, "ZS_Arena") != nullptr ||
           std::strstr(taskName, "ZS_Attack") != nullptr;
}

// On a client, OnLeaveArea / OnEnterArea only belong to the local player or client-leased NPCs; passive mirrors
// and remote puppets are host-simulated and must not mutate local quest DeliveryCounters or trigger ring-outs.
constexpr bool ShouldSuppressNonLocalAreaActor(bool isHost, bool connected, bool isLocalPlayer,
                                               bool hasLocalAuthority) noexcept
{
    return !isHost && connected && !isLocalPlayer && !hasLocalAuthority;
}

inline bool ShouldDispatchSendToArenaOnHost(bool questRunning, bool heldInTalk, bool fencedByClientLease,
                                            char const* currentTask) noexcept
{
    if (!questRunning || heldInTalk || fencedByClientLease) return false;
    if (currentTask && std::strstr(currentTask, "ZS_Talk") != nullptr) return false;
    return !HasFighterEnteredBoutTask(currentTask);
}

// When a client challenger is knocked down (downed::CombatUnavailable) during an active bout on the host,
// fightmem's attackerless-hero guard on the client may skip gCQuest_PS::OnNPCDefeated. Transition the Running
// quest to Lost (6) on the host so gCArena_PS::Status resets and Lost (20) / Reactivator rematch (21) dialogs open,
// unless a co-op partner is still upright in the arena continuing the shared 2v1 fight.
constexpr bool ShouldMarkClientChallengerDefeatedOnHost(bool isHost, uint8_t challengerPlayer, bool boutStarted,
                                                        bool questRunning, bool challengerUnavailable,
                                                        bool fighterDead, bool fighterDown,
                                                        bool partnerFightingInArena = false) noexcept
{
    return isHost && challengerPlayer >= 2 && boutStarted && questRunning &&
           challengerUnavailable && !fighterDead && !fighterDown && !partnerFightingInArena;
}

// Shared 2v1 arena fight rules: any gEAttackReason_Arena fight or active Arena/Sparring quest fighter in combat
// (excluding ZS_Arena walk-in aiMode 5, Talk 6, Down 8, Dead 9) allows both host and client to engage together.
constexpr bool ShouldAllowSharedArenaCombat(int attackReason, bool runningArenaOrSparringQuest,
                                            int aiMode, uint32_t combatState) noexcept
{
    if (aiMode == 5 || aiMode == 6 || aiMode == 8 || aiMode == 9) return false;
    if (attackReason == AttackReasonArena) return true;
    return runningArenaOrSparringQuest && (aiMode == 3 || combatState != 0);
}

// Keep gEAttackReason_Arena on an active arena opponent when AssessAgressor / AssessTarget / downed retargeting
// fires for the second co-op player, so spectators never mistake the 2v1 arena bout for a street brawl.
constexpr bool ShouldUpgradeArenaAttackReason(bool isSharedArenaFighter, bool targetIsCoopParticipant,
                                              int incomingReason) noexcept
{
    return isSharedArenaFighter && targetIsCoopParticipant &&
           incomingReason > 0 && incomingReason != AttackReasonArena &&
           incomingReason != AttackReasonMurder && incomingReason != AttackReasonRevolution &&
           incomingReason != AttackReasonKill;
}

constexpr bool IsBystanderFightWitnessReason(int attackReason) noexcept
{
    return attackReason == AttackReasonReactToWeapon || attackReason == AttackReasonAngry ||
           attackReason == AttackReasonStopFight || attackReason == AttackReasonReactToDamage ||
           attackReason == AttackReasonEnemy || attackReason == AttackReasonPlayerCommand ||
           attackReason == AttackReasonDuel || attackReason == AttackReasonArena;
}

constexpr bool ShouldSuppressArenaBystanderReaction(bool nearActiveArena, bool isArenaFighter,
                                                    bool targetIsCoopParticipant, int attackReason,
                                                    bool bystanderAlreadyHostile) noexcept
{
    return nearActiveArena && !isArenaFighter && targetIsCoopParticipant &&
           !bystanderAlreadyHostile && IsBystanderFightWitnessReason(attackReason);
}

// If a client challenger disconnects or leaves the world mid-bout, forfeit the Running bout cleanly so the
// arena and fighter are never stranded in gEArenaStatus_Running.
constexpr bool ShouldForfeitAbsentChallengerOnHost(bool isHost, uint8_t challengerPlayer,
                                                   bool questRunning, bool challengerInWorld) noexcept
{
    return isHost && challengerPlayer >= 2 && questRunning && !challengerInWorld;
}

// Client dialog command "Attack" (e.g. Topork_Duell, Hogar, Trompok, Mirzo) must forward to host whether the
// target NPC is currently mirrored OR client-leased (>80m from host), checking IsLocalPlayer on Entity2.
constexpr bool ShouldForwardClientDialogAttack(bool isHost, bool connected, bool hasTarget,
                                               bool targetMirrored, bool targetClientLeased,
                                               bool entity2IsLocalPlayer) noexcept
{
    return !isHost && connected && hasTarget && (targetMirrored || targetClientLeased) && entity2IsLocalPlayer;
}

} // namespace g3t::arena_policy
