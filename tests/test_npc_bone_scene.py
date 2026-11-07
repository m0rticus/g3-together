# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BoneSceneTests(unittest.TestCase):
    def test_exact_hierarchical_motion_and_scene_bandwidth(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".bone-scene-build-{os.getpid()}"
        build.mkdir()
        try:
            executable = build / "scene.exe"
            sources = [ROOT / "tests" / "npc_bone_scene_test.cpp",
                       ROOT / "src" / "npcwire_bones.cpp", ROOT / "src" / "npcwire_control.cpp",
                       ROOT / "src" / "npcwire_adapter.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/O2",
                           *map(str, sources), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           *map(str, sources), "-o", str(executable)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
