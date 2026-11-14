# SPDX-License-Identifier: GPL-3.0-or-later
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


NPC_CONVERGENCE_TARGET = _testbuild.register_target(
    "npc-convergence",
    [
        _testbuild.unit(
            "tests/npc_convergence_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "thirdparty" / "enet" / "include", ROOT / "tests"),
        ),
        *_testbuild.NPCWIRE_UNITS,
    ],
)


class NpcConvergenceTests(unittest.TestCase):
    def test_cold_finite_buffer_delayed_feedback_loss_and_display_convergence(self):
        result = NPC_CONVERGENCE_TARGET.run(timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
