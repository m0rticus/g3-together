import os
from pathlib import Path
import shutil
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild
from test_downed_policy import function_body


_compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
_warnings = "none" if _compiler and Path(_compiler).stem.lower() == "cl" else "strict"
TARGET = _testbuild.register_target(
    "puppet_animation_sync",
    [
        _testbuild.unit(ROOT / "tests" / "puppet_animation_sync_test.cpp", warnings=_warnings, includes=(ROOT / "src",)),
        _testbuild.unit(ROOT / "src" / "snapshot.cpp", warnings=_warnings, includes=(ROOT / "src",)),
    ],
    msvc_vcvars=False,
)


class PuppetAnimationSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.body = _testbuild.src_text("body.cpp")
        cls.remote = _testbuild.src_text("remote.cpp")
        cls.remote_internal = _testbuild.src_text("remote_internal.h")
        cls.movement_policy = _testbuild.src_text("movement_policy.h")
        cls.saving_policy = _testbuild.src_text("saving_policy.h")
        cls.playerwire = _testbuild.src_text("playerwire.h")

    def test_end_to_end_timeline_locomotion_wrap_and_phase_sync(self):
        executed = TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_protocol_79_and_line_limits_preserved(self):
        self.assertIn("constexpr uint16_t Protocol = 79;", self.saving_policy)
        self.assertIn("constexpr std::size_t FixedBytes = 65;", self.playerwire)
        for rel in (
            "src/net.cpp",
            "src/net.h",
            "src/net_direct.cpp",
            "src/net_internal.h",
            "src/net_messages.cpp",
            "src/net_metrics.cpp",
            "src/net_metrics.h",
            "src/net_npc.cpp",
            "src/net_npc_pose.cpp",
            "src/net_session.cpp",
            "src/netbudget.h",
            "src/latency.h",
            "src/remote.cpp",
            "src/remote.h",
            "src/remote_internal.h",
            "src/remote_qa.cpp",
            "src/body.cpp",
            "src/body.h",
            "src/body_internal.h",
            "src/body_qa.cpp",
            "src/movement.cpp",
            "src/movement.h",
            "src/movement_policy.h",
            "src/player_state.h",
            "src/playerwire.h",
            "src/pose.h",
        ):
            line_count = len((ROOT / rel).read_text(encoding="utf-8").splitlines())
            self.assertLess(line_count, 1000, f"{rel} has {line_count} lines (must be < 1000)")

    def test_puppet_timeline_drives_slot0_directly_without_movement_controller_fight(self):
        drive_puppet = function_body(self.body, r"\bvoid\s+DrivePuppetTimeline\s*\(")
        self.assertIsNotNone(drive_puppet)
        self.assertLess(drive_puppet.index("guards::RejectRemoteWrite"), drive_puppet.index("entity.CharacterMovement.StopMovement"))
        self.assertNotIn("SetMovementGoal", drive_puppet)
        self.assertIn("routine->AIStopPlayAnimation()", drive_puppet)
        self.assertIn("routine->AIStopCombatMove()", drive_puppet)
        self.assertIn("actor->SwitchFadeOut(slot0, GEFalse, blendSec)", drive_puppet)
        self.assertIn("static_assert(sizeof(MotionPlayDesc) == 28)", drive_puppet)
        self.assertIn("animation->SetMotionOwner(slot0, static_cast<Owner>(4))", drive_puppet)
        self.assertIn("animation->SetMotionOwner(slot0, static_cast<Owner>(2))", drive_puppet)

        drive = function_body(self.body, r"\bvoid\s+Drive\s*\(")
        self.assertIn("if (state.puppet && target.timeline)", drive)
        self.assertIn("DrivePuppetTimeline(", drive)

    def test_feed_player_and_hold_keep_continuous_motions_stall_free_and_weapons_instant(self):
        feed = function_body(self.remote, r"\bvoid\s+FeedPlayer\s*\(")
        self.assertIn("puppet.body.playing[0] = '\\0';", feed)
        self.assertIn("puppet.downedSequence = 0;", feed)
        self.assertIn("movement::IsContinuousPuppetMotion(state.animation)", feed)
        self.assertIn("motion.motion = continuous ? 0u : animHash", feed)
        self.assertIn("puppet.motions.Push(puppet.body.track.serial, sample)", feed)

        update = function_body(self.remote, r"\bvoid\s+UpdatePuppet\s*\(")
        self.assertIn("puppet.motions.Sample(presentation.sent, sampledMotion, sampledAgeMs)", update)
        self.assertIn("target.motionFrac = *target.motion ? remote.latest.motionFrac : movement::NoMotionFrac;", update)

        hold = function_body(self.remote, r"\bvoid\s+Hold\s*\(")
        self.assertIn("puppet.heldRight != right || puppet.heldLeft != left", hold)


if __name__ == "__main__":
    unittest.main()
