import hashlib
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SavingNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for native saving matcher fixture")
        cls.executable = ROOT / "tests" / (
            ".saving_native_test.exe" if os.name == "nt" else ".saving_native_test"
        )
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "saving_native_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )
        cls.addClassCleanup(cls.executable.unlink, missing_ok=True)

    def test_paired_save_recognizer_and_unique_native_patterns(self):
        subprocess.run([str(self.executable)], check=True, capture_output=True, text=True)

    def test_configured_real_dll_exports_patterns_and_aslr(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_GAME_DLL to the real CP 1.75 Game.dll for read-only native checks")
        args = [str(self.executable), "--game", path]
        pup = os.environ.get("G3T_PUP_DLL")
        if pup and Path(pup).is_file():
            args.extend(["--pup", pup])
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_configured_pup_dll_receiver_owner_operands_and_relocations(self):
        path = os.environ.get("G3T_PUP_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_PUP_DLL to the read-only PUP companion paru_s2.dll")
        fixture = Path(path)
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest().upper()
        self.assertEqual(
            digest,
            "60FCCBA5E27CA544FC1E72FE5BA4CBC33AAD107C1CBC90845B275F81B58C349C",
            "G3T_PUP_DLL identity differs from the captured paru_s2.dll",
        )
        subprocess.run([str(self.executable), "--pup", path], check=True,
                       capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
