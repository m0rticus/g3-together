# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcStreamSimulationTests(unittest.TestCase):
    def test_75_npc_stream_join_burst_valve_and_legacy_starvation(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".npcstream-build-{os.getpid()}"
        build.mkdir()
        try:
            exe = build / "stream.exe"
            sources = [ROOT / "tests" / "npc_stream_simulation.cpp", ROOT / "src" / "npcwire_bones.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           *map(str, sources), f"/Fe:{exe}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           *map(str, sources), "-o", str(exe)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=build, capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
