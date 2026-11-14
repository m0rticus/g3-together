# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPC_STREAM_SIMULATION_TARGET = _testbuild.register_target(
    "npc-stream-simulation",
    [
        _testbuild.unit(
            "tests/npc_stream_simulation.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "tests",),
        ),
        _testbuild.NPCWIRE_UNITS[0],
    ],
)


class NpcStreamSimulationTests(unittest.TestCase):
    def test_75_npc_stream_join_burst_valve_and_legacy_starvation(self):
        result = NPC_STREAM_SIMULATION_TARGET.run(timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
