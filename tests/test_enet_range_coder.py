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


class EnetRangeCoderExploreTests(unittest.TestCase):
    def test_range_coder_roundtrips_and_reports(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            executable = directory / "enet-range-coder.exe"
            include = ROOT / "thirdparty" / "enet" / "include"
            compress_c = ROOT / "thirdparty" / "enet" / "compress.c"
            explore_cpp = ROOT / "tests" / "enet_range_coder_explore.cpp"
            compiler = shutil.which("clang++") or shutil.which("g++")
            if compiler:
                env = os.environ.copy()
                compress_object = directory / "compress.o"
                test_object = directory / "explore.o"
                subprocess.run(
                    [compiler, "-x", "c", "-std=c11", "-O2", "-Wall", "-Wextra",
                     "-I", str(include), "-c", str(compress_c), "-o", str(compress_object)],
                    check=True, cwd=directory, env=env, capture_output=True, text=True)
                subprocess.run(
                    [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra",
                     "-I", str(include), "-c", str(explore_cpp), "-o", str(test_object)],
                    check=True, cwd=directory, env=env, capture_output=True, text=True)
                subprocess.run([compiler, str(compress_object), str(test_object), "-o", str(executable)],
                               check=True, cwd=directory, env=env, capture_output=True, text=True)
            else:
                env = win32_tool_environment(directory)
                cl = shutil.which("cl", path=env.get("PATH"))
                if not cl:
                    self.skipTest("A C++ compiler is required")
                compress_object = directory / "compress.obj"
                test_object = directory / "explore.obj"
                subprocess.run(
                    [cl, "/nologo", "/TC", "/O2", f"/I{include}", "/c", str(compress_c), f"/Fo:{compress_object}"],
                    check=True, cwd=directory, env=env, capture_output=True, text=True)
                subprocess.run(
                    [cl, "/nologo", "/TP", "/std:c++17", "/EHsc", "/O2", f"/I{include}",
                     "/c", str(explore_cpp), f"/Fo:{test_object}"],
                    check=True, cwd=directory, env=env, capture_output=True, text=True)
                subprocess.run(
                    [cl, "/nologo", str(compress_object), str(test_object), f"/Fe:{executable}"],
                    check=True, cwd=directory, env=env, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, cwd=directory, env=env,
                                    capture_output=True, text=True)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
