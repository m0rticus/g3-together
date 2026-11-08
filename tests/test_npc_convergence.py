# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcConvergenceTests(unittest.TestCase):
    def test_cold_finite_buffer_delayed_feedback_loss_and_display_convergence(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".npc-convergence-build-{os.getpid()}"
        build.mkdir()
        try:
            exe = build / "convergence.exe"
            sources = [ROOT / "tests" / "npc_convergence_test.cpp",
                       ROOT / "src" / "npcwire_bones.cpp",
                       ROOT / "src" / "npcwire_control.cpp",
                       ROOT / "src" / "npcwire_adapter.cpp"]
            include = ROOT / "thirdparty" / "enet" / "include"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/O2",
                           f"/I{include}", *map(str, sources), f"/Fe:{exe}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           f"-I{include}", *map(str, sources), "-o", str(exe)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
