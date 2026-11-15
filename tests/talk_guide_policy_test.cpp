// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/guide_policy.h"
#include "../src/talk_policy.h"

#include <cassert>
#include <cstring>

namespace {

void TestTalkActingAndPuppetRoutine()
{
    using namespace g3t::talk_policy;

    // While held in talk on the host, Entity::GetPlayer() must NOT be spoofed to the remote puppet,
    // otherwise ZS_Talk / ZS_Talk_Loop sees !PSInfoManager::IsRunning() and exits immediately.
    assert(EffectiveActingPlayer(true, true, 2) == 0);
    assert(EffectiveActingPlayer(true, true, 0) == 0);
    // Once released from talk, guides and companions spoof Entity::GetPlayer() to their context player.
    assert(EffectiveActingPlayer(true, false, 2) == 2);
    assert(EffectiveActingPlayer(true, false, 1) == 1);
    assert(EffectiveActingPlayer(false, false, 2) == 0);
    assert(EffectiveActingPlayer(false, true, 2) == 0);

    constexpr char const* kIdle = "ZS_HangAround_DoNothing";
    // Restore idle before body::Drive so Drive does not repacify the puppet; expose ZS_Talk right after Drive.
    assert(std::strcmp(PuppetRoutineTask(true, false, true, kIdle), kIdle) == 0);
    assert(std::strcmp(PuppetRoutineTask(false, false, true, kIdle), "ZS_Talk") == 0);
    assert(std::strcmp(PuppetRoutineTask(false, true, true, kIdle), kIdle) == 0);
    assert(std::strcmp(PuppetRoutineTask(false, false, false, kIdle), kIdle) == 0);
}

void TestTalkHoldMaintenanceAndSilencing()
{
    using namespace g3t::talk_policy;

    assert(!ShouldStopHeldLocomotion(0));
    assert(!ShouldStopHeldLocomotion(1));
    for (int mode = MoveModeSneak; mode <= MoveModeSprint; ++mode)
        assert(ShouldStopHeldLocomotion(mode));
    assert(!ShouldStopHeldLocomotion(MoveModeSprint + 1));

    // Live non-combat NPC stays in gEAIMode_Talk + "ZS_Talk", and re-pacifies if moved out of either.
    assert(!ShouldRepacifyHeldTalk(false, false, AiModeTalk, "ZS_Talk"));
    assert(ShouldRepacifyHeldTalk(false, false, 1, "ZS_Talk"));
    assert(ShouldRepacifyHeldTalk(false, false, AiModeTalk, "ZS_GuidePlayer"));
    assert(ShouldRepacifyHeldTalk(false, false, AiModeTalk, "Reddock"));
    assert(ShouldRepacifyHeldTalk(false, false, AiModeTalk, nullptr));
    // Dead, downed, or combat NPCs are never forced back into talk.
    assert(!ShouldRepacifyHeldTalk(true, false, AiModeTalk, "ZS_Walk"));
    assert(!ShouldRepacifyHeldTalk(false, true, AiModeTalk, "ZS_Walk"));
    assert(!ShouldRepacifyHeldTalk(false, false, AiModeCombat, "ZS_Attack"));

    // Passive mirrors silence residual tasks except while acting as the hero's dialogue partner.
    assert(ShouldSilencePassiveRoutine(true, false));
    assert(!ShouldSilencePassiveRoutine(true, true));
    assert(!ShouldSilencePassiveRoutine(false, false));

    assert(ShouldTrackClientDialogPartner(true, true, false, false));
    assert(!ShouldTrackClientDialogPartner(false, true, false, false));
    assert(!ShouldTrackClientDialogPartner(true, false, false, false));
    assert(!ShouldTrackClientDialogPartner(true, true, true, false));
    assert(!ShouldTrackClientDialogPartner(true, true, false, true));
}

void TestTalkGestures()
{
    using namespace g3t::talk_policy;

    // Consecutive identical gesture clips must both forward (no same-clip suppression or rate cap).
    assert(ShouldForwardGesture(true, true, "G3_Male_Dialog_Explain_01.xmot"));
    assert(ShouldForwardGesture(true, true, "G3_Male_Dialog_Explain_01.xmot"));
    assert(!ShouldForwardGesture(false, true, "G3_Male_Dialog_Explain_01.xmot"));
    assert(!ShouldForwardGesture(true, false, "G3_Male_Dialog_Explain_01.xmot"));
    assert(!ShouldForwardGesture(true, true, nullptr));
    assert(!ShouldForwardGesture(true, true, ""));
    assert(!ShouldForwardGesture(true, true, "<fault>"));
    assert(!ShouldForwardGesture(true, true, "G3_Male_Move_Walk_01.xmot"));

    char stem[32]{};
    assert(MotionStem("G3_Male_Dialog_01.xmot", stem, sizeof(stem)) == 17);
    assert(std::strcmp(stem, "G3_Male_Dialog_01") == 0);
    assert(MotionStem("G3_Male_Dialog_02.XMOT", stem, sizeof(stem)) == 17);
    assert(std::strcmp(stem, "G3_Male_Dialog_02") == 0);
    assert(MotionStem("BareClipName", stem, sizeof(stem)) == 12);
    assert(std::strcmp(stem, "BareClipName") == 0);
    assert(MotionStem(".xmot", stem, sizeof(stem)) == 0 && stem[0] == '\0');
    assert(MotionStem("<fault>", stem, sizeof(stem)) == 0 && stem[0] == '\0');
    assert(MotionStem("", stem, sizeof(stem)) == 0 && stem[0] == '\0');
    assert(MotionStem("VeryLongAnimationClipName.xmot", stem, 8) == 7);
    assert(std::strcmp(stem, "VeryLon") == 0);

    assert(ShouldAcceptGestureOnHost(true, true, 2, 2, false, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(false, true, 2, 2, false, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, false, 2, 2, false, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, true, 0, 0, false, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, true, 2, 3, false, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, true, 2, 2, true, false, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, true, 2, 2, false, true, AiModeTalk));
    assert(!ShouldAcceptGestureOnHost(true, true, 2, 2, false, false, AiModeCombat));
}

void TestGuideClaimAndDestinationPolicy()
{
    using namespace g3t::guide_policy;

    // Client locally starts FollowNpc quest (e.g. Gorn_ShowReddock while Reddock is unloaded on client).
    assert(ShouldClaimLocalFollowQuest(false, true, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, false));
    // Never claim on host, when disconnected, when already Running, or when Running arrived from host sync/baseline.
    assert(!ShouldClaimLocalFollowQuest(true, true, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, false));
    assert(!ShouldClaimLocalFollowQuest(false, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, false));
    assert(!ShouldClaimLocalFollowQuest(false, true, QuestTypeFollowNpc, QuestStatusRunning, QuestStatusRunning, false));
    assert(!ShouldClaimLocalFollowQuest(false, true, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, true));
    assert(!ShouldClaimLocalFollowQuest(false, true, 1, QuestStatusOpen, QuestStatusRunning, false));

    // Host fallback from client quest delta when no claim/guide exists yet.
    assert(ShouldHostAutoGuideFromQuestSync(true, 2, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 0, 0));
    assert(!ShouldHostAutoGuideFromQuestSync(true, 2, true, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 0, 0));
    assert(!ShouldHostAutoGuideFromQuestSync(true, 1, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 0, 0));
    assert(!ShouldHostAutoGuideFromQuestSync(true, 2, false, QuestTypeFollowNpc, QuestStatusRunning, QuestStatusRunning, 0, 0));
    assert(!ShouldHostAutoGuideFromQuestSync(true, 2, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 2, 0));
    assert(!ShouldHostAutoGuideFromQuestSync(true, 2, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 0, 1));
    assert(!ShouldHostAutoGuideFromQuestSync(false, 2, false, QuestTypeFollowNpc, QuestStatusOpen, QuestStatusRunning, 0, 0));

    // Destination entity is either the quest's resolved destination point or nullptr (never the puppet).
    assert(SelectGuideDestination(true, true) == GuideDestinationSource::QuestDestinationEntity);
    assert(SelectGuideDestination(true, false) == GuideDestinationSource::UnresolvedLetRoutineSelect);
    assert(SelectGuideDestination(false, false) == GuideDestinationSource::UnresolvedLetRoutineSelect);
}

void TestGuideWireMultiplexRoutineDeferralAndOwnerPriority()
{
    using namespace g3t::guide_policy;

    // NpcRecord::waiting stays strictly in {0, 1}.
    assert(EncodeWaitingField(2, 0, true, false) == 1);
    assert(EncodeWaitingField(2, 0, false, true) == 0);
    assert(EncodeWaitingField(0, 2, false, true) == 1);
    assert(EncodeWaitingField(0, 2, true, false) == 0);
    assert(EncodeWaitingField(0, 0, true, true) == 0);

    // Pure guide: decodes Navigation.IsOnDestination without setting Party.Waiting.
    const auto guideArrived = DecodeWaitingField(0, 2, 1, false);
    assert(!guideArrived.partyWaiting && guideArrived.writeGuideDestination && guideArrived.guideIsOnDestination);
    const auto guideWalking = DecodeWaitingField(0, 2, 0, false);
    assert(!guideWalking.partyWaiting && guideWalking.writeGuideDestination && !guideWalking.guideIsOnDestination);

    // Guide ended: clears IsOnDestination once if previously applied, leaves unrelated NPCs untouched.
    const auto guideCleared = DecodeWaitingField(0, 0, 0, true);
    assert(!guideCleared.partyWaiting && guideCleared.writeGuideDestination && !guideCleared.guideIsOnDestination);
    const auto nonGuide = DecodeWaitingField(0, 0, 0, false);
    assert(!nonGuide.partyWaiting && !nonGuide.writeGuideDestination && !nonGuide.guideIsOnDestination);

    // Party follower: decodes Party.Waiting.
    const auto followerWaiting = DecodeWaitingField(2, 0, 1, false);
    assert(followerWaiting.partyWaiting && !followerWaiting.writeGuideDestination);

    // SetRoutine defers FullStop/ContinueRoutine while held in talk on the host; talk::Release triggers it on dialog close.
    assert(ShouldImmediatelyRestartRoutineOnApply(false, false));
    assert(!ShouldImmediatelyRestartRoutineOnApply(false, true));
    assert(!ShouldImmediatelyRestartRoutineOnApply(true, false));
    assert(!ShouldImmediatelyRestartRoutineOnApply(true, true));

    // Active runtime claims and active guide PartyLeader links override stale saved SteamID entries.
    assert(ResolveCompanionOwner(2, 1, 1, 1) == 2);
    assert(ResolveCompanionOwner(0, 2, 0, 1) == 2);
    assert(ResolveCompanionOwner(0, 0, 2, 1) == 2);
    assert(ResolveCompanionOwner(0, 0, 0, 1) == 1);
    assert(ResolveCompanionOwner(0, 0, 0, 0) == 0);
}

} // namespace

int main()
{
    TestTalkActingAndPuppetRoutine();
    TestTalkHoldMaintenanceAndSilencing();
    TestTalkGestures();
    TestGuideClaimAndDestinationPolicy();
    TestGuideWireMultiplexRoutineDeferralAndOwnerPriority();
    return 0;
}
