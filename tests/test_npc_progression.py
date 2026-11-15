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

NPC_PROGRESSION_POLICY_TARGET = _testbuild.register_target(
    "npc-progression-policy",
    [_testbuild.unit(
        "tests/npc_progression_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_PROGRESSION_FILES = (
    "npc_progression_policy.h",
    "infocmd.h",
    "infocmd.cpp",
    "journal.cpp",
    "story_policy.h",
    "story.cpp",
    "npc_send.cpp",
    "npcs.h",
    "npcs.cpp",
    "npc_companions.cpp",
)


class NpcProgressionSyncTests(unittest.TestCase):
    def test_engine_free_npc_progression_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            result = NPC_PROGRESSION_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cross_town_talked_defeated_and_friend_memory_wiring(self):
        infocmd = _testbuild.src_text("infocmd.cpp")
        npc_send = _testbuild.src_text("npc_send.cpp")
        npcs = _testbuild.src_text("npcs.cpp")
        companions = _testbuild.src_text("npc_companions.cpp")

        # WorldState carries TalkedToPlayer and explicit DefeatedByPlayer across arbitrary distance and merges monotonically on host.
        self.assertIn("npc_progression_policy::PackDialog(", infocmd)
        self.assertIn("npc_progression_policy::UnpackTalkedToPlayer(", infocmd)
        self.assertIn("npc_progression_policy::PackNpcFlags(", infocmd)
        self.assertIn("npc_progression_policy::UnpackDefeatedByPlayer(", infocmd)
        self.assertIn("npc_progression_policy::MergeHostDialog(", infocmd)
        self.assertIn("npc_progression_policy::MergeHostNpcFlags(", infocmd)
        self.assertIn("npc_progression_policy::FriendMemoryForName(", infocmd)

        # Active local dialogs and native OnDelivery/OnEndInfo/knockout mutations publish WorldState without rate caps.
        self.assertNotIn("ProgressionScanInterval", infocmd)
        self.assertIn("TrackLocalDialogAndProgression()", infocmd)
        self.assertIn("SeedHostWorldState()", infocmd)
        self.assertIn("FlushBufferedClientStates()", infocmd)
        self.assertIn("infocmd::SyncToClients()", npc_send)

        # Alive passive mirrors must not forge NPC.DefeatedByPlayer = GETrue locally (which broke DefeatedByPlayer conditions).
        self.assertNotIn("m.entity.NPC.DefeatedByPlayer = GETrue;", npcs)
        self.assertIn("m.defeatedBefore", npcs)
        self.assertIn("void SyncDefeatedByPlayer(", companions)

    def test_distant_npc_death_and_info_timestamp_sync_wiring(self):
        journal = _testbuild.src_text("journal.cpp")
        story_policy = _testbuild.src_text("story_policy.h")
        story = _testbuild.src_text("story.cpp")

        # Distant out-of-ROI NPC deaths apply immediately to unloaded/unstreamed gCNPC_PS / gCDamageReceiver_PS / gCScriptRoutine_PS.
        self.assertIn("SeedWorldDeaths()", journal)
        self.assertIn("npc.NPC.DefeatedByPlayer = GETrue;", journal)
        self.assertIn("npc.DamageReceiver.HitPoints = 0;", journal)
        self.assertIn("npc.Routine.AIMode = gEAIMode_Dead;", journal)
        self.assertIn("if (!Processed(npc.GetInstance())) {", journal)

        # Repeatable timed info cooldowns (gCInfo_PS::m_uEndTimestamp) replicate through StateQuestRuntime field "i".
        self.assertIn('part != "i"', story_policy)
        self.assertIn('field == "i"', story)
        self.assertIn("info->m_uEndTimestamp", story)

    def test_touched_files_stay_under_line_limits(self):
        for name in TOUCHED_PROGRESSION_FILES:
            lines = len(_testbuild.src_text(name).splitlines())
            self.assertLess(lines, 1000, f"src/{name} has {lines} lines (must be < 1000)")
        self.assertLessEqual(len(_testbuild.src_text("npcs.cpp").splitlines()), 975)
        self.assertLessEqual(len(_testbuild.src_text("npcplayback.cpp").splitlines()), 990)


if __name__ == "__main__":
    unittest.main()
