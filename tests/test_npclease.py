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


NPCLEASE_TARGET = _testbuild.register_target(
    "npclease",
    [
        _testbuild.unit(
            "tests/npclease_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcLeaseTests(unittest.TestCase):
    def test_engine_free_npclease_policy_and_legacy_discard(self):
        executed = NPCLEASE_TARGET.run(timeout=60)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
        print(executed.stdout, end="")


if __name__ == "__main__":
    unittest.main()
