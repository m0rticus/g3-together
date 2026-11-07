import os
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NpcWireControlExtensionTests(unittest.TestCase):
    def test_metadata_overrides(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        executable = ROOT / "tests" / f"npcwire-control-extension-{os.getpid()}.exe"
        env = os.environ.copy()
        env["TEMP"] = env["TMP"] = str(ROOT)
        try:
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(ROOT / "src" / "npcwire_control.cpp"),
                 str(ROOT / "tests" / "npcwire_control_extension_test.cpp"), "-o", str(executable)],
                check=True, env=env,
            )
            subprocess.run([str(executable)], check=True, env=env)
        finally:
            executable.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
