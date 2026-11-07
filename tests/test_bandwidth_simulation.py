# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BandwidthSimulationTests(unittest.TestCase):
    def test_measured_non_npc_stream_thresholds(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bandwidth-simulation-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / "bandwidth_simulation.exe"
            source = ROOT / "tests" / "bandwidth_simulation.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           str(source), f"/Fe{executable}", f"/Fo{directory}\\"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           str(source), "-o", str(executable)]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
