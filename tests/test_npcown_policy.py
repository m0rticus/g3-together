import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPCOWN_POLICY_TARGET = _testbuild.register_target(
    "npcown-policy",
    [
        _testbuild.unit(
            "tests/npcown_policy_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            defines=("_CRT_SECURE_NO_WARNINGS",),
            includes=(ROOT / "tests",),
        )
    ],
    pthread="posix",
)


class NpcOwnPolicyTests(unittest.TestCase):
    def test_policy_and_lease(self):
        executed = NPCOWN_POLICY_TARGET.run(timeout=60)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
        print(executed.stdout, end="")
