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


class EnetCoalesceTests(unittest.TestCase):
    def test_vendored_enet_packs_small_cross_channel_messages(self):
        with tempfile.TemporaryDirectory(prefix="enet-coalesce-") as temporary:
            directory = Path(temporary)
            executable = directory / "enet_coalesce.exe"
            include = ROOT / "thirdparty" / "enet" / "include"
            sources = [ROOT / "tests" / "enet_coalesce_test.c",
                       *(ROOT / "thirdparty" / "enet").glob("*.c")]
            compiler = shutil.which("clang") or shutil.which("gcc")
            if compiler:
                env = os.environ.copy()
                command = [compiler, "-std=c99", f"-I{include}", *map(str, sources), "-o", str(executable)]
                if os.name == "nt":
                    command.extend(["-lws2_32", "-lwinmm"])
            else:
                env = win32_tool_environment(directory)
                cl = shutil.which("cl", path=env.get("PATH"))
                if not cl:
                    self.skipTest("A C compiler is required")
                command = [cl, "/nologo", "/W4", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
                           f"/I{include}", *map(str, sources), f"/Fe:{executable}", f"/Fo:{directory}\\",
                           "/link", "ws2_32.lib", "winmm.lib"]
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
