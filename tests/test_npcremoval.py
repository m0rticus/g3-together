import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPCREMOVAL_TARGET = _testbuild.register_target(
    "npcremoval",
    [
        _testbuild.unit(
            "tests/npcremoval_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcRemovalTests(unittest.TestCase):
    def run_fixture(self, section):
        result = NPCREMOVAL_TARGET.run([section], timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_atomic_name_configuration(self):
        self.run_fixture("names")

    def test_exact_world_instances_and_ardea_box(self):
        self.run_fixture("scope")

    def test_terminal_removal_and_rejoin_publication(self):
        self.run_fixture("lifecycle")


if __name__ == "__main__":
    unittest.main()
