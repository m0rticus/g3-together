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

ARENA_POLICY_TARGET = _testbuild.register_target(
    "arena-policy",
    [_testbuild.unit(
        "tests/arena_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_ARENA_FILES = (
    "arena_policy.h",
    "arena.h",
    "arena.cpp",
    "story.h",
    "story.cpp",
    "infocmd.cpp",
    "infocmd_sector.cpp",
    "talk_policy.h",
    "talk.cpp",
)


class ArenaPolicyTests(unittest.TestCase):
    def test_engine_free_arena_policies(self):
        with tempfile.TemporaryDirectory() as folder:
            result = ARENA_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_arena_story_infocmd_and_talk_contracts(self):
        arena = _testbuild.src_text("arena.cpp")
        story = _testbuild.src_text("story.cpp")
        infocmd = _testbuild.src_text("infocmd.cpp")
        sector = _testbuild.src_text("infocmd_sector.cpp")
        talk = _testbuild.src_text("talk.cpp")

        # 1. Remote puppet CurrentZoneEntityProxy refresh so OnArena / ZS_Arena sees puppet inside the ring.
        self.assertIn("RefreshPuppetZone(eCEntity* puppet)", arena)
        self.assertIn("admin->GetZone(*puppet, here, path, GEFalse)", arena)
        self.assertIn("navigation->AccessCurrentZoneEntityProxy() = here", arena)

        # 2. Ring-exit protection hooks both OnLeaveArea and OnEnterArea and extracts other entity (not StringParameter).
        self.assertIn("OnLeaveAreaDetour", arena)
        self.assertIn("OnEnterAreaDetour", arena)
        self.assertIn("spu->GetOtherEntity()", arena)
        self.assertIn("arena_policy::ShouldSuppressPlayerLeaveArea(", arena)

        # 3. Arena.Status synchronization and immediate quest flush on dialog end / quest transitions.
        self.assertIn("SyncArenaStatuses()", arena)
        self.assertIn("arena::SyncArenaStatuses()", story)
        self.assertIn("arena::AllowClientQuestStatusOnHost(", story)
        self.assertIn("arena::OnClientArenaQuestStatus(", story)
        self.assertIn("arena::NoteQuestStatusFromHost(", story)
        self.assertIn("story::FlushQuests()", talk)

        # 4. Client dialog Attack forwarding works for both mirrored AND client-leased NPCs and relinquishes lease to host.
        self.assertIn("arena_policy::ShouldForwardClientDialogAttack(", infocmd)
        self.assertIn("npclease::RelinquishToHost(target", infocmd)
        self.assertIn("guards::IsLocalPlayer(e2.GetInstance())", infocmd)

        # 5. Host StartFights waits for lease fence and talk hold, binds guards::Acting(target), and tracks duels in arena.
        self.assertIn("npclease::Fenced(attacker.GetInstance())", sector)
        self.assertIn("talk::IsHeld(attacker.GetInstance())", sector)
        self.assertIn("guards::Acting context(target)", sector)
        self.assertIn("arena::TrackDuel(attacker.GetInstance(), it->player)", sector)

        # 6. Host OnWorldState handles kind=20 arena starts without re-broadcasting InfoCommand states.
        self.assertIn("case 20: if (net::IsHost()) arena::OnState(from, s); return;", infocmd)

        # 7. Talk release never clobbers AiModeArena / ZS_Arena with FullStop + ContinueRoutine.
        self.assertIn("talk_policy::ShouldRestartRoutineOnTalkRelease(", talk)

    def test_file_sizes_stay_under_one_thousand_lines(self):
        for rel in TOUCHED_ARENA_FILES:
            path = ROOT / "src" / rel
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertLess(
                len(lines),
                1000,
                f"src/{rel} has {len(lines)} lines (must stay strictly under 1,000 lines)",
            )


if __name__ == "__main__":
    unittest.main()
