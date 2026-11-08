# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SteamSendTests(unittest.TestCase):
    def test_retention_priority_and_lossless_payload_savings(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".steam-send-build-{os.getpid()}"
        build.mkdir()
        try:
            obj = build / "compress.o"
            exe = build / "steam-send.exe"
            commands = [
                [compiler, "-x", "c", "-std=c11", "-O2",
                 "-I", str(ROOT / "thirdparty" / "enet" / "include"), "-c",
                 str(ROOT / "thirdparty" / "enet" / "compress.c"), "-o", str(obj)],
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                 "-I", str(ROOT / "thirdparty" / "enet" / "include"),
                 str(ROOT / "tests" / "steam_send_test.cpp"), str(obj), "-o", str(exe)],
                [str(exe)],
            ]
            for command in commands:
                result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if command == commands[-1]:
                    print(result.stdout, end="")
        finally:
            shutil.rmtree(build)


if __name__ == "__main__":
    unittest.main()
