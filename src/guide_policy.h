// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy decisions for client-started FollowNpc quest guides, destination resolution, and routine transitions.
#pragma once

#include <cstdint>

namespace g3t::guide_policy {

constexpr int QuestStatusOpen = 0;     // gEQuestStatus_Open
constexpr int QuestStatusRunning = 1;  // gEQuestStatus_Running
constexpr int QuestTypeFollowNpc = 7;  // gEQuestType_FollowNpc

// Game.dll gCInfo_PS::OnEndInfo (0x101c1270) skips StartGuidePlayer when the FollowNpc quest's DestinationEntity
// (e.g. "Reddock" while talking to Gorn in Ardea) is not resolved in the client's local entity cache, leaving
// Hero.Party.PartyLeader null even though the quest just transitioned to Running locally. Claim the delivery NPC
// only on a local transition to Running (never for Running statuses received from the host's baseline or sync).
constexpr bool ShouldClaimLocalFollowQuest(bool isHost, bool connected, int questType,
                                           int previousStatus, int currentStatus, bool fromHostSync) noexcept
{
    return !isHost && connected && !fromHostSync && questType == QuestTypeFollowNpc &&
           currentStatus == QuestStatusRunning && previousStatus != QuestStatusRunning;
}

// Host fallback when a remote player's quest delta transitions a FollowNpc quest to Running before or without a
// companion claim packet: start guiding that player unless the delivery NPC is already claimed or guiding someone.
constexpr bool ShouldHostAutoGuideFromQuestSync(bool isHost, uint8_t fromPlayer, bool isSnapshot, int questType,
                                                int previousStatus, int currentStatus,
                                                uint8_t existingClaimOwner, uint8_t existingGuidePlayer) noexcept
{
    return isHost && fromPlayer >= 2 && !isSnapshot && questType == QuestTypeFollowNpc &&
           currentStatus == QuestStatusRunning && previousStatus != QuestStatusRunning &&
           existingClaimOwner == 0 && existingGuidePlayer == 0;
}

enum class GuideDestinationSource : uint8_t {
    QuestDestinationEntity = 0,
    UnresolvedLetRoutineSelect = 1,
};

// StartGuidePlayer's second entity argument is the quest destination point (e.g. Reddock), NEVER the guided player's
// puppet. Passing the puppet sets CurrentDestinationPoint to the puppet (~1.5 m away), making ZS_GuidePlayer mark
// IsOnDestination immediately without walking. When the destination entity is unloaded, pass nullptr so
// SelectRoutineDestination resolves Quest.DestinationEntity dynamically.
constexpr GuideDestinationSource SelectGuideDestination(bool hasDestinationName, bool destinationEntityResolved) noexcept
{
    return (hasDestinationName && destinationEntityResolved)
               ? GuideDestinationSource::QuestDestinationEntity
               : GuideDestinationSource::UnresolvedLetRoutineSelect;
}

// NpcRecord::waiting is constrained to {0, 1} by npcsend::detail::Valid and triggers reliable EssentialChanged updates.
// Party followers (partyPlayer != 0) carry Party.Waiting; pure quest guides (partyPlayer == 0, guidePlayer != 0) carry
// Navigation.IsOnDestination so arrival dialogues (e.g. Gorn's Reddock arrival info) unlock on the guided client.
constexpr uint8_t EncodeWaitingField(uint8_t partyPlayer, uint8_t guidePlayer,
                                     bool partyWaiting, bool navIsOnDestination) noexcept
{
    if (partyPlayer != 0) return partyWaiting ? 1 : 0;
    if (guidePlayer != 0) return navIsOnDestination ? 1 : 0;
    return 0;
}

struct DecodedWaitingField {
    bool partyWaiting = false;
    bool writeGuideDestination = false;
    bool guideIsOnDestination = false;
};

constexpr DecodedWaitingField DecodeWaitingField(uint8_t partyPlayer, uint8_t guidePlayer,
                                                 uint8_t rawWaiting, bool hadGuideDestinationApplied) noexcept
{
    if (partyPlayer != 0) return {rawWaiting != 0, hadGuideDestinationApplied, false};
    if (guidePlayer != 0) return {false, true, rawWaiting != 0};
    return {false, hadGuideDestinationApplied, false};
}

// When SetRoutine arrives on the host while the NPC is held in a client's active dialogue, store the new routine name
// immediately on Navigation, but defer FullStop/ContinueRoutine until talk::Release ends the dialogue hold.
constexpr bool ShouldImmediatelyRestartRoutineOnApply(bool mirrored, bool heldInTalk) noexcept
{
    return !mirrored && !heldInTalk;
}

// Active runtime claims and active guide PartyLeader links take precedence over saved SteamID fallback rows.
constexpr uint8_t ResolveCompanionOwner(uint8_t activeClaimOwner, uint8_t activeGuidePlayer,
                                        uint8_t partyLeaderPlayer, uint8_t savedIdentityOwner) noexcept
{
    if (activeClaimOwner != 0) return activeClaimOwner;
    if (activeGuidePlayer != 0) return activeGuidePlayer;
    if (partyLeaderPlayer != 0) return partyLeaderPlayer;
    return savedIdentityOwner;
}

} // namespace g3t::guide_policy
