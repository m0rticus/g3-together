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
from _g3copy import win32_tool_environment


class BoneSceneTests(unittest.TestCase):
    def test_exact_hierarchical_motion_and_scene_bandwidth(self):
        with tempfile.TemporaryDirectory(prefix="bone-scene-build-") as temporary:
            build = Path(temporary)
            executable = build / "scene.exe"
            sources = [ROOT / "tests" / "npc_bone_scene_test.cpp",
                       ROOT / "src" / "npcwire_bones.cpp", ROOT / "src" / "npcwire_control.cpp",
                       ROOT / "src" / "npcwire_adapter.cpp"]
            compiler = shutil.which("clang++") or shutil.which("g++")
            if compiler:
                env = os.environ.copy()
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           *map(str, sources), "-o", str(executable)]
            else:
                env = win32_tool_environment(build)
                cl = shutil.which("cl", path=env.get("PATH"))
                self.assertIsNotNone(cl, "C++17 compiler required")
                command = [cl, "/nologo", "/std:c++17", "/EHsc", "/W4", "/O2",
                           *map(str, sources), f"/Fe:{executable}", f"/Fo:{build}\\"]
            result = subprocess.run(command, cwd=build, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=build, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
