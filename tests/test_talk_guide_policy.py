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
    "npcplayback.cpp",
    "npcplayback_hooks.cpp",
    "remote.cpp",
    "npcs.h",
    "npc_state.h",
    "npc_companions.cpp",
    "npc_send.cpp",
    "infocmd.h",
    "infocmd.cpp",
    "guide.h",
    "guide.cpp",
    "story.cpp",
)


class TalkAndGuidePolicyTests(unittest.TestCase):
    def test_engine_free_talk_and_guide_policies(self):
        with tempfile.TemporaryDirectory() as folder:
            result = TALK_GUIDE_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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

        # Release on EndTalk erases the hold BEFORE ContinueRoutine so newly started guides see !talk::IsHeld.
        on_talk = talk[talk.index("void OnTalk("):talk.index("void Start()")]
        end_branch = on_talk[on_talk.index("} else if (it != holds.end()"):]
        self.assertLess(end_branch.index("holds.erase(it)"), end_branch.index("Release(ended)"))

        # Routine and Script hooks suppress Acting player spoofing while held in talk, and Play forwards slot-0 partner motions.
        self.assertIn("talk_policy::EffectiveActingPlayer(net::IsHost(), talk::IsHeld(entity), npcs::ContextPlayer(entity))", hooks)
        self.assertIn("talk::OnPartnerMotion(entity, self->GetMotionDesc(slot).GetMotionFilename().GetText())", hooks)
        self.assertIn("talk_policy::ShouldSilencePassiveRoutine(Passive(entity), talk::TalkingWithHero(entity))", playback)

        # Puppet exposes ZS_Talk after body::Drive while holding an NPC in talk without triggering Drive repacification.
        self.assertIn("talk_policy::PuppetRoutineTask(true, held, false, body::IdleTask())", remote)
        self.assertIn("talk_policy::PuppetRoutineTask(false, held, holdingTalk, body::IdleTask())", remote)

        # Host routes WorldState kind 15 to talk::OnGesture without broadcasting it as an authoritative snapshot.
        world_state = infocmd[infocmd.index("void OnWorldState("):infocmd.index("void Send(")]
        self.assertIn("if (s.kind == 15)", world_state)
        self.assertIn("talk::OnGesture(", world_state)

    def test_client_follow_quest_guide_and_routine_transition_wiring(self):
        companions = _testbuild.src_text("npc_companions.cpp")
        npc_send = _testbuild.src_text("npc_send.cpp")
        infocmd = _testbuild.src_text("infocmd.cpp")
        guide = _testbuild.src_text("guide.cpp")
        story = _testbuild.src_text("story.cpp")

        # StartGuidePlayer on host resolves the quest's destination point, never passing actor as the destination.
        guide_fn = companions[companions.index("bool Guide("):companions.index("uint8_t ClaimedBy(")]
        self.assertIn("ResolveQuestGuideDestination(npc, questName)", guide_fn)
        self.assertIn('CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), destination, 0)', guide_fn)
        self.assertNotIn('CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), actor, 0)', companions)

        # Navigation.IsOnDestination multiplexes through NpcRecord::waiting for pure quest guides.
        self.assertIn("guide_policy::EncodeWaitingField(", npc_send)
        self.assertIn("guide_policy::DecodeWaitingField(", companions)

        # guide.cpp runs frame-driven without a 500 ms poll cap and claims local FollowNpc transitions with questName.
        self.assertNotIn("CheckEvery", guide)
        self.assertNotIn("nextCheck", guide)
        self.assertIn("guide_policy::ShouldClaimLocalFollowQuest(", guide)
        self.assertIn("guide_policy::ShouldHostAutoGuideFromQuestSync(", guide)

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
