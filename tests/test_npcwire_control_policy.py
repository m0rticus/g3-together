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
    "npcwire_control_policy", ["tests/npcwire_control_policy_test.cpp"],
    pedantic=True, allow_msvc=False, respect_env=False,
)


class NpcWireControlPolicyTests(unittest.TestCase):
    def test_engine_free_policy(self):
        TARGET.run(check=True)


if __name__ == "__main__":
    unittest.main()
