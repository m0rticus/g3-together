import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class TargetingReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise RuntimeError("The engine-free targeting test needs an existing C++ compiler")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="targeting-tests-", dir=build)
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / "targeting_replay_test.exe"
        sources = [ROOT / "tests" / "targeting_replay_test.cpp", ROOT / "src" / "snapshot.cpp"]
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc",
                       *(str(source) for source in sources), f"/Fe{cls.executable}"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       *(str(source) for source in sources), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f"Targeting test compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True, timeout=10)

    def test_opponent_and_player_flags_follow_the_same_buffered_ai_state(self):
        self.run_case("timeline")

    def test_out_of_order_packets_cannot_restore_an_old_opponent_after_death(self):
        self.run_case("order")

    def test_native_target_uses_presented_not_latest_opponent(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        self.assertIn("motion.combatTarget = records[i].combatTarget;", source)
        self.assertIn("shown.combatTarget = presentation.motion.combatTarget;", source)
        self.assertIn("a.aiMode != b.aiMode || a.combatTarget != b.combatTarget", source)

    def test_production_diagnostic_observes_without_selecting_or_mutating(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
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
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
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
