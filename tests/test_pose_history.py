# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PoseHistoryTests(unittest.TestCase):
    def test_unpresented_bones_survive_bursts_then_prune_with_clock_wrap(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("An existing C++17 compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pose-history-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / "pose_history_test.exe"
            sources = [ROOT / "tests" / "pose_history_test.cpp", ROOT / "src" / "snapshot.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           *(str(source) for source in sources), f"/Fe{executable}", f"/Fo{directory}\\"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           *(str(source) for source in sources), "-o", str(executable)]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
