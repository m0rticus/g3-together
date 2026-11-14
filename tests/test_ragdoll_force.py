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
    "ragdoll_force", ["tests/ragdoll_force_test.cpp"],
    respect_env=False, msvc_vcvars=False,
)


class RagdollForceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True)

    def test_force_is_independent_of_world_coordinates_and_contact_tracks_the_corpse(self):
        self.run_case("points")

    def test_native_forces_survive_and_larger_forces_keep_direction_within_the_bound(self):
        self.run_case("limit")

    def test_nonfinite_data_is_rejected_without_a_success_shaped_force(self):
        self.run_case("invalid")

    def test_host_gravity_late_duplicate_reload_and_new_life_decisions(self):
        self.run_case("once")


if __name__ == "__main__":
    unittest.main()
