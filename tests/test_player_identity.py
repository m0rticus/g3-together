# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PlayerIdentityTests(unittest.TestCase):
    def test_production_identity_migration_and_container_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "identity.exe"
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "player_identity_test.cpp"), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_network_identity_admission_and_capture(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "identity-transport.exe"
            sources = ["tests/identity_transport_test.cpp", "src/npcwire_bones.cpp",
                       "src/npcwire_control.cpp", "src/npcwire_adapter.cpp"]
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I" + str(ROOT / "thirdparty" / "enet" / "include"),
                 *[str(ROOT / source) for source in sources], "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
