# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
from _g3copy import win32_tool_environment


class BandwidthSimulationTests(unittest.TestCase):
    def test_measured_non_npc_stream_thresholds(self):
        with tempfile.TemporaryDirectory(prefix="bandwidth-simulation-") as temporary:
            directory = Path(temporary)
            executable = directory / "bandwidth_simulation.exe"
            source = ROOT / "tests" / "bandwidth_simulation.cpp"
            compiler = shutil.which("clang++") or shutil.which("g++")
            if compiler:
                env = os.environ.copy()
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           str(source), "-o", str(executable)]
            else:
                env = win32_tool_environment(directory)
                cl = shutil.which("cl", path=env.get("PATH"))
                if not cl:
                    self.skipTest("A C++17 compiler is required")
                command = [cl, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           str(source), f"/Fe:{executable}", f"/Fo:{directory}\\"]
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
