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
    "npcwire_adapter",
    ["tests/npcwire_adapter_test.cpp", "src/npcwire_adapter.cpp", "src/npcwire_control.cpp"],
    allow_msvc=False, skip_message="C++17 compiler required for NPC wire adapter tests",
)


class NpcWireAdapterTests(unittest.TestCase):
    def test_exact_record_conversion_and_yaw_error(self):
        TARGET.run(check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
