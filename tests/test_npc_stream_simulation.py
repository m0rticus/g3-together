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


class NpcStreamSimulationTests(unittest.TestCase):
    def test_75_npc_stream_join_burst_valve_and_legacy_starvation(self):
        with tempfile.TemporaryDirectory(prefix="npcstream-build-") as temporary:
            build = Path(temporary)
            exe = build / "stream.exe"
            sources = [ROOT / "tests" / "npc_stream_simulation.cpp", ROOT / "src" / "npcwire_bones.cpp"]
            compiler = shutil.which("clang++") or shutil.which("g++")
            if compiler:
                env = os.environ.copy()
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           *map(str, sources), "-o", str(exe)]
            else:
                env = win32_tool_environment(build)
                cl = shutil.which("cl", path=env.get("PATH"))
                self.assertIsNotNone(cl, "C++17 compiler required")
                command = [cl, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           *map(str, sources), f"/Fe:{exe}", f"/Fo:{build}\\"]
            result = subprocess.run(command, cwd=build, env=env, capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=build, env=env, capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
