import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SavingNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for native saving matcher fixture")
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "saving-native.exe"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "saving_native_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )

    def test_unique_patterns_call_edges_and_singleton_validation(self):
        subprocess.run([str(self.executable)], check=True, capture_output=True, text=True)

    def test_configured_real_dll_exports_patterns_and_aslr(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_GAME_DLL to the real CP 1.75 Game.dll for read-only native checks")
        subprocess.run([str(self.executable), path], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
