# SPDX-License-Identifier: GPL-3.0-or-later
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


NETWORK_STREAMS_TARGET = _testbuild.register_target(
    "network-streams",
    [
        _testbuild.unit(
            "tests/network_streams_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "thirdparty" / "enet" / "include", ROOT / "src"),
        ),
        *(
            _testbuild.unit(
                "src/npcwire_bones.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
            _testbuild.unit(
                "src/npcwire_control.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
            _testbuild.unit(
                "src/npcwire_adapter.cpp",
                std="c++17",
                warnings="strict",
                includes=(ROOT / "src",),
            ),
        ),
    ],
)


class NetworkStreamsTests(unittest.TestCase):
    def test_player_character_story_and_budget_integration(self):
        result = NETWORK_STREAMS_TARGET.run(timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
