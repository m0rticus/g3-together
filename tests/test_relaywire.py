import os
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RelayWireTests(unittest.TestCase):
    def test_exact_relay_envelope(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("C++17 compiler required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        executable = build / f"relaywire-test-{os.getpid()}.exe"
        try:
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           str(ROOT / "tests" / "relaywire_test.cpp"), f"/Fe{executable}",
                           f"/Fo{build}\\relaywire-test-{os.getpid()}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           str(ROOT / "tests" / "relaywire_test.cpp"), "-o", str(executable)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Relay wire test compilation failed:\n{result.stdout}{result.stderr}")
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Relay wire test failed:\n{result.stdout}{result.stderr}")
        finally:
            executable.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
