# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcPoseTests(unittest.TestCase):
    def test_exact_bounds_recovery_and_75_npc_bandwidth(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".npcpose-build-{os.getpid()}"
        build.mkdir()
        try:
            exe = build / "pose.exe"
            sources = [ROOT / "tests" / "npcwire_pose_test.cpp", ROOT / "src" / "npcwire_bones.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           *map(str, sources), f"/Fe:{exe}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           *map(str, sources), "-o", str(exe)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
