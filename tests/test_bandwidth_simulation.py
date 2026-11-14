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
    "bandwidth_simulation", [ROOT / "tests" / "bandwidth_simulation.cpp"],
    respect_env=False, skip_message="A C++17 compiler is required",
)


class BandwidthSimulationTests(unittest.TestCase):
    def test_measured_non_npc_stream_thresholds(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
