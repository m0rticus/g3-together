# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SteamDiagnosticsTests(unittest.TestCase):
    def test_flat_api_route_evidence_and_description_privacy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".steam-diagnostics-build-{os.getpid()}"
        build.mkdir()
        try:
            exe = build / "steam-diagnostics.exe"
            commands = [
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(ROOT / "tests" / "steam_diagnostics_test.cpp"), "-o", str(exe)],
                [str(exe)],
            ]
            for command in commands:
                result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
