# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PeerTimeoutTests(unittest.TestCase):
    def test_vendored_enet_timeout_and_loading_margin(self):
        compiler = shutil.which("clang") or shutil.which("gcc")
        if not compiler:
            self.skipTest("A C compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="peer-timeout-", dir=build) as folder:
            executable = Path(folder) / "peer-timeout.exe"
            enet = ROOT / "thirdparty" / "enet"
            sources = [ROOT / "tests" / "peer_timeout_test.c"]
            sources.extend(path for path in enet.glob("*.c") if path.name != "protocol.c")
            command = [compiler, "-std=c99", "-I", str(enet / "include"),
                       *map(str, sources), "-o", str(executable)]
            if os.name == "nt":
                command.extend(["-lws2_32", "-lwinmm"])
            result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=folder, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
