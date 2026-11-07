# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NpcWireBonePackTests(unittest.TestCase):
    def test_pure_rice_quaternion_affine_and_atomic_roundtrips(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".npcwire-bonepack-build-{os.getpid()}"
        build.mkdir()
        try:
            exe = build / "bonepack.exe"
            sources = [ROOT / "tests" / "npcwire_bonepack_test.cpp", ROOT / "src" / "npcwire_bones.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           *map(str, sources), f"/Fe:{exe}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "/path-no", "-O2",
                           *map(str, sources), "-o", str(exe)]
                command = [part for part in command if part != "/path-no"]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
