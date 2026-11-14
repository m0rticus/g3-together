# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "npcwire_pose",
    [_testbuild.unit("tests/npcwire_pose_test.cpp", opt="-O2"),
     _testbuild.NPCWIRE_UNITS[0]],
    respect_env=False, msvc_vcvars=False,
)


class NpcPoseTests(unittest.TestCase):
    def test_exact_bounds_recovery_and_75_npc_bandwidth(self):
        result = TARGET.run(timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
