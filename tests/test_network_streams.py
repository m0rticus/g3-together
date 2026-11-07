# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NetworkStreamsTests(unittest.TestCase):
    def test_player_character_story_and_budget_integration(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="network-streams-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / "network_streams.exe"
            sources = [ROOT / "tests" / "network_streams_test.cpp",
                       ROOT / "src" / "npcwire_bones.cpp", ROOT / "src" / "npcwire_control.cpp",
                       ROOT / "src" / "npcwire_adapter.cpp"]
            include = ROOT / "thirdparty" / "enet" / "include"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           f"/I{include}", *map(str, sources), f"/Fe{executable}", f"/Fo{directory}\\"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           f"-I{include}", *map(str, sources), "-o", str(executable)]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
