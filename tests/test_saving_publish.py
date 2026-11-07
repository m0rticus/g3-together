# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SavingPublishTests(unittest.TestCase):
    def test_production_publish_faults_cleanup_and_retry(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "saving-publish.exe"
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "saving_publish_test.cpp"), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable), str(Path(folder) / "slot")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
