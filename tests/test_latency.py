import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class LatencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("An existing C++ compiler is needed for engine-free latency tests")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="latency-tests-", dir=build)
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / "latency_test.exe"
        sources = [ROOT / "tests" / "latency_test.cpp", ROOT / "src" / "snapshot.cpp"]
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                       *(str(source) for source in sources), f"/Fe{cls.executable}", f"/Fo{directory}\\"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       *(str(source) for source in sources), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"Latency test compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True)

    def test_clean_link_uses_34_ms_npc_floor_without_changing_player_delay(self):
        self.run_case("floor")

    def test_slow_capture_cadence_keeps_two_intervals(self):
        self.run_case("cadence")

    def test_jitter_grows_delay_then_recovers_without_rewinding(self):
        self.run_case("jitter")

    def test_single_loss_is_bridged_and_long_underruns_hold_with_bounded_recovery(self):
        self.run_case("loss")

    def test_life_transitions_wrap_and_discontinuity_keep_one_nonpredictive_timeline(self):
        self.run_case("timeline")

    def test_precise_ping_rejects_bad_replies_and_expires_across_clock_wrap(self):
        self.run_case("ping")


if __name__ == "__main__":
    unittest.main()
