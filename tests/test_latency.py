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
        result = subprocess.run([str(self.executable), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, f"{case} failed:\n{result.stdout}{result.stderr}")

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

    def test_transit_baseline_and_five_second_stall_recover_within_eight_seconds(self):
        self.run_case("transit")

    def test_moderate_transit_jitter_and_time_based_decay(self):
        self.run_case("moderate-jitter")

    def test_burst_preserves_every_action_hit_and_life_boundary_across_wrap(self):
        self.run_case("events")

    def test_player_prediction_is_locomotion_only_speed_and_time_bounded(self):
        self.run_case("prediction")

    def test_incoming_combat_backlog_never_accelerates_or_runs_past_latest(self):
        self.run_case("protected-backlog")

    def test_precise_ping_rejects_bad_replies_and_expires_across_clock_wrap(self):
        self.run_case("ping")


if __name__ == "__main__":
    unittest.main()
