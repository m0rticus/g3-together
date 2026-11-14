import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SAVING_NATIVE_TARGET = _testbuild.register_target(
    "saving-native-test",
    [_testbuild.unit(
        "tests/saving_native_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class SavingNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = SAVING_NATIVE_TARGET.executable()

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
        digest = _testbuild.cached_sha256(fixture)
        result = subprocess.run([str(self.executable), "--pup", path], check=True,
                                capture_output=True, text=True)
        self.assertIn(result.stdout.strip(), {
            f"pup digest={digest} verified=structural",
            f"pup digest={digest} verified=pinned",
        })

    def test_pup_identity_is_informational(self):
        source = "\n".join(
            _testbuild.src_text(name)
            for name in ("persist.cpp", "persist_hooks.cpp", "persist_storage.cpp", "persist_client.cpp")
        )
        self.assertNotRegex(source, r'if\s*\(\s*!\s*(?:KnownPup|LogPupIdentity)\s*\(')
        self.assertNotRegex(source, r'PupRefused\s*\(\s*"companion-identity"')
        self.assertIn("persist: PUP identity digest=", source)
        self.assertIn("verified=", source)
        for offset in ("0x677a0", "0x97578", "0x99510"):
            self.assertNotIn(offset, source.lower())


if __name__ == "__main__":
    unittest.main()
