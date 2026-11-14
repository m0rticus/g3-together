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


BONE_SCENE_TARGET = _testbuild.register_target(
    "npc-bone-scene",
    [
        _testbuild.unit(
            "tests/npc_bone_scene_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "tests",),
        ),
        *_testbuild.NPCWIRE_UNITS,
    ],
)


class BoneSceneTests(unittest.TestCase):
    def test_exact_hierarchical_motion_and_scene_bandwidth(self):
        result = BONE_SCENE_TARGET.run(timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
