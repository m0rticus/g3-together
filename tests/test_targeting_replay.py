import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGETING_REPLAY_TARGET = _testbuild.register_target(
    "targeting-replay",
    [
        _testbuild.unit(
            "tests/targeting_replay_test.cpp",
            std="c++17",
            warnings="strict",
        ),
        _testbuild.unit(
            "src/snapshot.cpp",
            std="c++17",
            warnings="strict",
            includes=("src",),
        ),
    ],
)


class TargetingReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGETING_REPLAY_TARGET.executable()

    def run_case(self, case):
        import subprocess
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True, timeout=10)

    def test_opponent_and_player_flags_follow_the_same_buffered_ai_state(self):
        self.run_case("timeline")

    def test_out_of_order_packets_cannot_restore_an_old_opponent_after_death(self):
        self.run_case("order")

    def test_same_timestamp_edge_burst_caps_pending_events(self):
        self.run_case("span_zero_cap")

    def test_native_target_uses_presented_not_latest_opponent(self):
        source = _testbuild.src_text("npcs.cpp")
        policy = _testbuild.src_text("npcsend_policy.h")
        self.assertIn("motion.combatTarget = records[i].combatTarget;", source)
        self.assertIn("shown.combatTarget = presentation.motion.combatTarget;", source)
        self.assertIn("a.aiMode != b.aiMode", policy)
        self.assertIn("a.combatTarget != b.combatTarget", policy)
        self.assertIn("sent.aiMode != current.aiMode", policy)
        self.assertIn("sent.combatTarget != current.combatTarget", policy)

    def test_production_diagnostic_observes_without_selecting_or_mutating(self):
        source = _testbuild.src_text("npcs.cpp")
        diagnostic = source.split("struct FocusInspection :", 1)[1].split("void PassiveMirrorTick()", 1)[0]
        for forbidden in ("FindFocusEntity(", "SetFocusEntity(", "SetCurrentTarget(", "SetTask(",
                          "EnableAutoAiming(", "CollectInteractionEntities("):
            self.assertNotIn(forbidden, diagnostic)
        for field in ("FOCUSSTATE", "FOCUSBIND", "FOCUSLOOP", "FOCUSVICTIM", "FOCUSCAND",
                      "FOCUSQUEUE", "FOCUSREL", "same_target=", "host_target=", "layout=unsupported",
                      "now - targetingWatch.killedAt <= 30000", "now - targetingWatch.loggedAt >= 1000"):
            self.assertIn(field, diagnostic)
        self.assertIn("targetingWatch = TargetingWatch{};", source)

    def test_diagnostic_records_fit_the_production_log_buffer(self):
        source = _testbuild.src_text("npcs.cpp")
        diagnostic = source.split("struct FocusInspection :", 1)[1].split("void PassiveMirrorTick()", 1)[0]
        formats = re.findall(r'Logf\("(npcs: FOCUS[^"\n]*)"', diagnostic)
        self.assertGreaterEqual(len(formats), 10)
        conversion = re.compile(r"%(?:\.(\d+))?0?\d*(?:l|z)?([sduXpf])")

        def worst_case(match):
            precision, kind = match.groups()
            if kind == "s":
                return "x" * (int(precision) if precision else 63)
            if kind in ("p", "X"):
                return "F" * 8
            if kind == "f":
                return "-" + "9" * 39 + ".9"  # finite float promoted to double, with one decimal
            return "-2147483648" if kind == "d" else "4294967295"

        for format_string in formats:
            rendered = conversion.sub(worst_case, format_string)
            self.assertNotIn("%", rendered)
            self.assertLessEqual(len(rendered), 899, format_string)


if __name__ == "__main__":
    unittest.main()
