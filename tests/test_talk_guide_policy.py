# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TALK_GUIDE_POLICY_TARGET = _testbuild.register_target(
    "talk-guide-policy",
    [_testbuild.unit(
        "tests/talk_guide_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_SRC_FILES = (
    "talk_policy.h",
    "guide_policy.h",
    "talk.h",
    "talk.cpp",
    "dialogaudio.h",
    "dialogaudio.cpp",
    "npcplayback.h",
    "npcplayback.cpp",
    "npcplayback_hooks.cpp",
    "remote.cpp",
    "npcs.h",
    "npc_state.h",
    "npc_companions.cpp",
    "npc_send.cpp",
    "infocmd.h",
    "infocmd_internal.h",
    "infocmd.cpp",
    "infocmd_progression.cpp",
    "infocmd_sector.cpp",
    "infocmd_qa.cpp",
    "guide.h",
    "guide.cpp",
    "story_internal.h",
    "story.cpp",
    "story_domains.cpp",
    "story_qa.cpp",
    "trade_policy.h",
    "trade_internal.h",
    "trade.cpp",
    "trade_qa.cpp",
)


class TalkAndGuidePolicyTests(unittest.TestCase):
    def test_engine_free_talk_and_guide_policies(self):
        with tempfile.TemporaryDirectory() as folder:
            result = TALK_GUIDE_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_guide_routine_context_and_follow_quest_destination_policies(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "guide_routine_context_policy_check.cpp"
            cpp.write_text(r'''
#include "guide_policy.h"
#include <cassert>

int main() {
    using namespace g3t::guide_policy;

    // 1. Open FollowNpc quests claimed before synced MsgStory arrives can be promoted to Running on host.
    assert(ShouldStartOpenFollowQuestOnGuide(QuestTypeFollowNpc, QuestStatusOpen));
    assert(!ShouldStartOpenFollowQuestOnGuide(QuestTypeFollowNpc, QuestStatusRunning));
    assert(!ShouldStartOpenFollowQuestOnGuide(QuestTypeFollowNpc, 2 /* Succeeded */));
    assert(!ShouldStartOpenFollowQuestOnGuide(QuestTypeFollowNpc, QuestStatusFailed));
    assert(!ShouldStartOpenFollowQuestOnGuide(0 /* non-FollowNpc */, QuestStatusOpen));

    // 2. Named FollowNpc quests (e.g., Gorn_ShowReddock) resolve destination across Open/Running/Succeeded,
    //    while rejecting Failed/Obsolete/Cancelled/Lost or non-FollowNpc quests.
    assert(ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusOpen));
    assert(ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusRunning));
    assert(ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, 2 /* Succeeded */));
    assert(!ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusFailed));
    assert(!ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusObsolete));
    assert(!ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusCancelled));
    assert(!ShouldResolveNamedFollowQuestDestination(QuestTypeFollowNpc, QuestStatusLost));
    assert(!ShouldResolveNamedFollowQuestDestination(1 /* non-FollowNpc */, QuestStatusOpen));

    // 3. ContinueRoutine on host must bind guards::Acting to the guided remote puppet (player >= 2) when not held
    //    in talk so SelectRoutineDestination (0xa7050) and SelectRoutineTask (0xa9440) preserve FP_Reddock and
    //    enter ZS_GuidePlayer instead of resetting CurrentDestinationPoint to the routine anchor.
    assert(GuideRoutineActingPlayer(true, false, 2) == 2);
    assert(GuideRoutineActingPlayer(true, true /* heldInTalk */, 2) == 0);
    assert(GuideRoutineActingPlayer(true, false, 1 /* host hero */) == 0);
    assert(GuideRoutineActingPlayer(true, false, 0) == 0);
    assert(GuideRoutineActingPlayer(false /* client */, false, 2) == 0);
    return 0;
}
''', encoding="utf-8")
            target = _testbuild.NativeTarget(
                "guide-routine-context-policy-check",
                [_testbuild.unit(cpp, std="c++17", warnings="strict", includes=(ROOT / "src",))],
            )
            executed = target.run(timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_held_talk_focus_and_seated_gesture_normalization_policies(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "talk_focus_and_gesture_policy_check.cpp"
            cpp.write_text(r'''
#include "talk_policy.h"
#include <cassert>

int main() {
    using namespace g3t::talk_policy;

    // 1. Seated / sleeping posture classification and body-alignment suppression (never spin seated NPCs off benches).
    assert(!IsSeatedTalkAniState(AniStateDummy0));
    assert(!IsSeatedTalkAniState(AniStateStand));
    assert(!IsSeatedTalkAniState(AniStateKneel));
    assert(!IsSeatedTalkAniState(AniStateTalkStand));
    assert(IsSeatedTalkAniState(AniStateSitGround));
    assert(IsSeatedTalkAniState(AniStateSitStool));
    assert(IsSeatedTalkAniState(AniStateSitBench));
    assert(IsSeatedTalkAniState(AniStateSitThrone));
    assert(IsSeatedTalkAniState(AniStateTalkSitBench));

    assert(ShouldAlignTalkBodyToSpeaker(AniStateStand));
    assert(ShouldAlignTalkBodyToSpeaker(AniStateTalkStand));
    assert(ShouldAlignTalkBodyToSpeaker(AniStateKneel));
    assert(!ShouldAlignTalkBodyToSpeaker(AniStateSitBench));
    assert(!ShouldAlignTalkBodyToSpeaker(AniStateSitGround));
    assert(!ShouldAlignTalkBodyToSpeaker(AniStateTalkSitBench));
    assert(!ShouldAlignTalkBodyToSpeaker(AniStateSleepBed));
    assert(!ShouldAlignTalkBodyToSpeaker(AniStateSleepGround));

    // 2. Passive mirror AniState inference from active slot-0 .xmot when Routine.AniState is Dummy0 (0).
    //    Fixes Gelford on the bench (Hero_SitBench_Smoke_None_P0_Ambient_Loop_N_Fwd_00_%.xmot) resolving Say clips.
    assert(InferDialogPartnerAniState("Hero_SitBench_Smoke_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateSitBench);
    assert(InferDialogPartnerAniState("Hero_SitStool_Cup_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateSitStool);
    assert(InferDialogPartnerAniState("Hero_SitGround_Meat_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateSitGround);
    assert(InferDialogPartnerAniState("Hero_SitThrone_None_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateSitThrone);
    assert(InferDialogPartnerAniState("Hero_Kneel_None_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateKneel);
    assert(InferDialogPartnerAniState("Hero_Stand_None_None_P0_Ambient_Loop_N_Fwd_00_%.xmot", AniStateDummy0) == AniStateStand);
    assert(InferDialogPartnerAniState(nullptr, AniStateDummy0) == AniStateStand);
    assert(InferDialogPartnerAniState(nullptr, AniStateSitBench) == AniStateSitBench);
    assert(InferDialogPartnerAniState(nullptr, AniStateTalkSitBench) == AniStateSitBench);
    assert(InferDialogPartnerAniState("Hero_SitGround_None_None_P0_StandUp_End_N_Fwd_00_%.xmot", AniStateSitGround) == AniStateStand);

    // 3. PrimaryPose normalization: all _Say_ clips in _compiledAnimation.pak require P0 (1), never gEPose_None (0).
    assert(NormalizeDialogPrimaryPose(PoseNone, true) == PoseP0);
    assert(NormalizeDialogPrimaryPose(PoseNone, false) == PoseP0);
    assert(NormalizeDialogPrimaryPose(-1, false) == PoseP0);
    assert(NormalizeDialogPrimaryPose(PoseCount, false) == PoseP0);
    assert(NormalizeDialogPrimaryPose(2, true /* empty/torch or seated */) == PoseP0);
    assert(NormalizeDialogPrimaryPose(2, false /* armed standing */) == 2);

    // 4. Held ambient prop clearing: seated Say clips exist only for None_None; standing ZS_Talk preserves right-hand torch.
    assert(!ShouldClearHeldItemsForTalk(AniStateSitBench, UseTypeNone, UseTypeNone));
    assert(ShouldClearHeldItemsForTalk(AniStateSitBench, 42 /* Smoke/Meat/Cup */, UseTypeNone));
    assert(ShouldClearHeldItemsForTalk(AniStateSitBench, UseTypeNone, UseTypeTorch));
    assert(!ShouldClearHeldItemsForTalk(AniStateStand, UseTypeNone, UseTypeNone));
    assert(!ShouldClearHeldItemsForTalk(AniStateStand, UseTypeNone, UseTypeTorch));
    assert(ShouldClearHeldItemsForTalk(AniStateStand, UseTypeTorch, UseTypeNone));
    assert(ShouldClearHeldItemsForTalk(AniStateStand, 42, UseTypeNone));

    // 5. StandUp gesture recognition (e.g., Gorn standing up during Gorn_ShowReddock).
    assert(IsStandUpMotion("Hero_SitGround_None_None_P0_StandUp_End_N_Fwd_00_%.xmot"));
    assert(!IsStandUpMotion("Hero_SitBench_None_None_P0_Say_Ambient_N_Fwd_01_%.xmot"));
    assert(!IsStandUpMotion(nullptr));

    // 6. Stationary host dialogue hold: block ZS_Talk_Loop -> ContinueRoutine and wandering task transitions
    //    while allowing ZS_Talk and combat/flee/death/stumble tasks through immediately.
    assert(ShouldBlockHeldTalkEvent(true, true, false, false, 0, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, true, false, false, AiModeCombat, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, true, false, false, AiModeFlee, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, true, true /* dead */, false, 0, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, true, false, true /* down */, 0, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, false /* not held */, false, false, 0, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(false /* client */, true, false, false, 0, "ContinueRoutine"));
    assert(!ShouldBlockHeldTalkEvent(true, true, false, false, 0, "OnFollowPlayer"));

    assert(ShouldBindActingPlayerForEvent(true, false, "ContinueRoutine"));
    assert(ShouldBindActingPlayerForEvent(true, false, "OnGuidePlayer"));
    assert(ShouldBindActingPlayerForEvent(true, false, "OnFollowPlayer"));
    assert(!ShouldBindActingPlayerForEvent(true, true /* heldInTalk */, "ContinueRoutine"));
    assert(!ShouldBindActingPlayerForEvent(false /* client */, false, "ContinueRoutine"));
    assert(!ShouldBindActingPlayerForEvent(true, false, "OnAssessPlayer"));

    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_Talk"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_Attack"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_RagDollDead"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_Unconscious"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_Flee"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_StumbleR"));
    assert(ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_HangAround"));
    assert(ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_HangAround_Bench"));
    assert(ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_MoveToPosition"));
    assert(ShouldBlockHeldTalkTask(true, true, false, false, 0, "ZS_GuidePlayer"));
    assert(!ShouldBlockHeldTalkTask(true, true, false, false, AiModeCombat, "ZS_HangAround"));

    // 7. Pending client talk partner window bridges AssessTalk -> PSInfoManager::Start so Silence() never wipes ZS_Talk.
    assert(ShouldKeepPendingClientTalk(true, false, true, 1500u, 2500u));
    assert(!ShouldKeepPendingClientTalk(true, false, true, 2500u, 2500u));
    assert(!ShouldKeepPendingClientTalk(true, true /* dialogRunning */, true, 1500u, 2500u));
    assert(!ShouldKeepPendingClientTalk(true, false, false /* heroDead */, 1500u, 2500u));
    assert(!ShouldKeepPendingClientTalk(false /* not active */, false, true, 1500u, 2500u));
    return 0;
}
''', encoding="utf-8")
            target = _testbuild.NativeTarget(
                "talk-focus-and-gesture-policy-check",
                [_testbuild.unit(cpp, std="c++17", warnings="strict", includes=(ROOT / "src",))],
            )
            executed = target.run(timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_stationary_client_dialogue_and_gesture_wiring(self):
        talk = _testbuild.src_text("talk.cpp")
        hooks = _testbuild.src_text("npcplayback_hooks.cpp")
        playback = _testbuild.src_text("npcplayback.cpp")
        remote = _testbuild.src_text("remote.cpp")
        infocmd = _testbuild.src_text("infocmd.cpp")

        # talk.cpp must never clobber npcs::SetContext (which belongs to companions/guides) or throttle maintenance/gestures.
        self.assertNotIn("SetContext(", talk)
        self.assertNotIn("nextTick", talk)
        self.assertNotIn("nextLook", talk)

        # Release on EndTalk erases the hold BEFORE ContinueRoutine and binds guards::Acting for newly started guides.
        erase_helper = talk[talk.index("EraseAndReleaseHold("):talk.index("bool PlayHeldGesture(")]
        self.assertLess(erase_helper.index("holds.erase(it)"), erase_helper.index("Release(ended)"))
        release_fn = talk[talk.index("void Release("):talk.index("EraseAndReleaseHold(")]
        self.assertIn("guide_policy::GuideRoutineActingPlayer(", release_fn)
        self.assertIn("guards::Acting context(", release_fn)
        on_talk = talk[talk.index("void OnTalk("):talk.index("void Start()")]
        end_branch = on_talk[on_talk.index("} else if (it != holds.end()"):]
        self.assertIn("EraseAndReleaseHold(it)", end_branch)

        # Stationary host hold stops navigation, respects seated vs standing facing, blocks ContinueRoutine/wandering tasks,
        # and normalizes passive client partner AniState/PrimaryPose/HoldStacks across the AssessTalk -> Start window.
        self.assertIn("StopTalkNavigation(npc)", talk)
        self.assertIn("ApplyTalkFacing(npc, speaker)", talk)
        self.assertIn("NormalizeTalkPartnerAnimationState(npc,", talk)
        self.assertIn("PrepareClientTalkPartner(", talk)
        self.assertIn("talk_policy::ShouldBlockHeldTalkEvent(", talk)
        self.assertIn("talk_policy::ShouldBlockHeldTalkTask(", talk)
        self.assertIn("talk_policy::IsStandUpMotion(motion)", talk)

        # Routine and Script hooks suppress Acting player spoofing while held in talk, EventScript blocks held ContinueRoutine
        # and binds Acting for guide/follow routine transitions, and Play forwards slot-0 partner motions AFTER originalPlay.
        self.assertIn("talk_policy::EffectiveActingPlayer(net::IsHost(), talk::IsHeld(entity), npcs::ContextPlayer(entity))", hooks)
        self.assertIn("talk::BlockHeldTalkEvent(receiver, event)", hooks)
        self.assertIn("talk_policy::ShouldBindActingPlayerForEvent(", hooks)
        play_fn = hooks[hooks.index("void __fastcall Play("):hooks.index("void __fastcall ActorPlay(")]
        self.assertLess(
            play_fn.index("originalPlay(self, edx, slot, options)"),
            play_fn.index("talk::OnPartnerMotion(entity, self->GetMotionDesc(slot).GetMotionFilename().GetText())"),
        )
        self.assertIn("talk_policy::ShouldAllowDialogPartnerProperty(", hooks)
        self.assertIn("talk::BlockHeldTalkTask(entity, task.GetText())", playback)
        self.assertIn("talk_policy::ShouldSilencePassiveRoutine(Passive(entity), talk::TalkingWithHero(entity))", playback)
        self.assertIn("talk::AllowPassiveTalkTask(entity, task.GetText())", playback)
        self.assertIn("YieldMirrorForClientTalk(", _testbuild.src_text("npcs.cpp"))
        self.assertIn("talk_policy::ShouldAllowPassiveTalkScript(talk::TalkingWithHero(self), name)", _testbuild.src_text("ai.cpp"))

        # Puppet exposes ZS_Talk after body::Drive while holding an NPC in talk without triggering Drive repacification.
        self.assertIn("talk_policy::PuppetRoutineTask(true, held, false, body::IdleTask())", remote)
        self.assertIn("talk_policy::PuppetRoutineTask(false, held, holdingTalk, body::IdleTask())", remote)

        # talk.cpp tracks active client dialog by GUID across frames, gates MsgTalk on net::Connected(),
        # re-syncs SendTalk across WelcomeSerial, and checks AssessTalk symmetrically.
        self.assertNotIn("talkingWith", talk)
        self.assertNotIn("net::Joined()", talk)
        self.assertIn("talk_policy::ShouldSendClientTalkHold(connected, talkHoldSent, talkingWelcome, net::WelcomeSerial())", talk)
        self.assertIn("HoldOf(addressedNpc)", talk)

        # Host routes WorldState kind 15 to talk::OnGesture without broadcasting it as an authoritative snapshot.
        world_state = infocmd[infocmd.index("void OnWorldState("):infocmd.index("void Send(")]
        self.assertIn("if (s.kind == 15)", world_state)
        self.assertIn("talk::OnGesture(", world_state)

    def test_client_follow_quest_guide_and_routine_transition_wiring(self):
        companions = _testbuild.src_text("npc_companions.cpp")
        npc_send = _testbuild.src_text("npc_send.cpp")
        infocmd = "\n".join(
            _testbuild.src_text(name)
            for name in ("infocmd.cpp", "infocmd_progression.cpp", "infocmd_sector.cpp")
        )
        guide = _testbuild.src_text("guide.cpp")
        story = "\n".join(
            _testbuild.src_text(name)
            for name in ("story_domains.cpp", "story.cpp", "story_qa.cpp")
        )

        # StartGuidePlayer on host resolves the quest's destination point (including explicitly named Open FollowNpc quests)
        # and keeps guards::Acting bound across FullStop + ContinueRoutine so FP_Reddock is preserved.
        self.assertIn("guide_policy::ShouldResolveNamedFollowQuestDestination(", companions)
        self.assertIn("guide_policy::ShouldStartOpenFollowQuestOnGuide(", companions)
        guide_fn = companions[companions.index("bool Guide("):companions.index("bool Follow(")]
        self.assertIn("ResolveQuestGuideDestination(npc, questName)", guide_fn)
        self.assertIn('CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), destination, 0)', guide_fn)
        self.assertLess(
            guide_fn.index("guards::Acting context("),
            guide_fn.index("npc.Routine.ContinueRoutine()"),
        )
        self.assertNotIn('CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), actor, 0)', companions)

        # StartGuiding in guide.cpp and ApplyNpcSnapshot in infocmd.cpp also bind guards::Acting around ContinueRoutine.
        self.assertIn("guide_policy::GuideRoutineActingPlayer(", guide)
        self.assertIn("guide_policy::GuideRoutineActingPlayer(", infocmd)

        # Navigation.IsOnDestination multiplexes through NpcRecord::waiting for pure quest guides.
        self.assertIn("guide_policy::EncodeWaitingField(", npc_send)
        self.assertIn("guide_policy::DecodeWaitingField(", companions)

        # guide.cpp runs frame-driven without a 500 ms poll cap and claims local FollowNpc transitions with questName.
        self.assertNotIn("CheckEvery", guide)
        self.assertNotIn("nextCheck", guide)
        self.assertIn("guide_policy::ShouldClaimLocalFollowQuest(", guide)
        self.assertIn("guide_policy::ShouldHostAutoGuideFromQuestSync(", guide)
        self.assertIn("guide_policy::ShouldAdoptHeroLeaderAsGuide(", guide)
        self.assertIn("awaitingHostRelease", guide)

        # story.cpp feeds baseline/synced quest states to NoteQuestStatusFromHost and client quest deltas to OnClientFollowQuestRunning.
        self.assertIn("guide::NoteQuestStatusFromHost(", story)
        self.assertIn("guide::OnClientFollowQuestRunning(", story)

        # SetRoutine captures ID1 immediately and defers FullStop/ContinueRoutine while held in talk.
        self.assertIn("guide_policy::ShouldImmediatelyRestartRoutineOnApply(", infocmd)
        self.assertIn('!std::strcmp(name, "SetRoutine") && *command.GetID1().GetText()', infocmd)

    def test_all_touched_source_files_stay_strictly_under_1000_lines(self):
        for name in TOUCHED_SRC_FILES:
            line_count = len(_testbuild.src_text(name).splitlines())
            self.assertLess(line_count, 1000, f"src/{name} has {line_count} lines (must be < 1000)")


if __name__ == "__main__":
    unittest.main()
