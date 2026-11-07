import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcLeaseClientTests(unittest.TestCase):
    def test_client_activation_fence(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        build = ROOT / "tests" / f".npclease-client-build-{os.getpid()}"
        build.mkdir()
        try:
            executable = build / ("npclease_client_test.exe" if os.name == "nt" else "npclease_client_test")
            source = ROOT / "tests" / "npclease_client_test.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
                           "/D_CRT_SECURE_NO_WARNINGS",
                           str(source), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                           str(source), "-o", str(executable)]
                if os.name != "nt":
                    command.append("-pthread")
            compiled = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=build, capture_output=True, text=True, timeout=60)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            print(executed.stdout, end="")
        finally:
            shutil.rmtree(build)
