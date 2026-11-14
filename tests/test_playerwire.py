import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "playerwire", ["tests/playerwire_test.cpp"],
    pedantic=True, respect_env=False, msvc_vcvars=False,
)


class PlayerWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], cwd=ROOT, check=True,
                       capture_output=True, text=True)

    def test_roundtrip_masks_commit_preview_and_reliable_changes(self):
        self.run_case("roundtrip")

    def test_loss_recovery_stale_and_keyframe_reset(self):
        self.run_case("recovery")

    def test_full_keyframe_cadence_and_sequence_wrap(self):
        self.run_case("wrap")

    def test_malformed_trailing_bounds_and_authoritative_id(self):
        self.run_case("malformed")


if __name__ == "__main__":
    unittest.main()
