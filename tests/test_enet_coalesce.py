# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class EnetCoalesceTests(unittest.TestCase):
    def test_vendored_enet_packs_small_cross_channel_messages(self):
        compiler = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="enet-coalesce-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / "enet_coalesce.exe"
            include = ROOT / "thirdparty" / "enet" / "include"
            sources = [ROOT / "tests" / "enet_coalesce_test.c",
                       *(ROOT / "thirdparty" / "enet").glob("*.c")]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/W4", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
                           f"/I{include}", *map(str, sources), f"/Fe{executable}", f"/Fo{directory}\\",
                           "/link", "ws2_32.lib", "winmm.lib"]
            else:
                command = [compiler, "-std=c99", f"-I{include}", *map(str, sources), "-o", str(executable)]
                if os.name == "nt":
                    command.extend(["-lws2_32", "-lwinmm"])
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
