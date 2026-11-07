import os
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class EnetRangeCoderExploreTests(unittest.TestCase):
    def test_range_coder_roundtrips_and_reports(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("A C++ compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        executable = build / f"enet-range-coder-{os.getpid()}.exe"
        compress_object = build / f"enet-range-coder-compress-{os.getpid()}.o"
        test_object = build / f"enet-range-coder-test-{os.getpid()}.o"
        try:
            c_command = [compiler, "-x", "c", "-std=c11", "-O2", "-Wall", "-Wextra",
                         "-I", str(ROOT / "thirdparty" / "enet" / "include"),
                         "-c", str(ROOT / "thirdparty" / "enet" / "compress.c"),
                         "-o", str(compress_object)]
            result = subprocess.run(c_command, cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Range coder compilation failed:\n{result.stdout}{result.stderr}")
            cpp_command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra",
                           "-I", str(ROOT / "thirdparty" / "enet" / "include"),
                           "-c", str(ROOT / "tests" / "enet_range_coder_explore.cpp"),
                           "-o", str(test_object)]
            result = subprocess.run(cpp_command, cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Range coder C++ compilation failed:\n{result.stdout}{result.stderr}")
            result = subprocess.run([compiler, str(compress_object), str(test_object),
                                     "-o", str(executable)], cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Range coder link failed:\n{result.stdout}{result.stderr}")
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"Range coder failed:\n{result.stdout}{result.stderr}")
            print(result.stdout.strip())
        finally:
            executable.unlink(missing_ok=True)
            compress_object.unlink(missing_ok=True)
            test_object.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
