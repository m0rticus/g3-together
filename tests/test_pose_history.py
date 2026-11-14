# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "pose_history", ["tests/pose_history_test.cpp", "src/snapshot.cpp"],
    respect_env=False, msvc_vcvars=False,
)


class PoseHistoryTests(unittest.TestCase):
    def test_unpresented_bones_survive_bursts_then_prune_with_clock_wrap(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
