# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NpcWireControlPolicyTests(unittest.TestCase):
    def test_engine_free_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        executable = ROOT / "tests" / f"npcwire-control-policy-{os.getpid()}.exe"
        try:
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(ROOT / "tests" / "npcwire_control_policy_test.cpp"),
                 "-o", str(executable)],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(executable)], cwd=ROOT, check=True)
        finally:
            executable.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
