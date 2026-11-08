# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ClientTemplateTests(unittest.TestCase):
    def compile_and_run(self, source, needs_folder=False):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "client-template.exe"
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / source), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            args = [str(executable)]
            if needs_folder:
                args.append(folder)
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_template_policy(self):
        self.compile_and_run("client_template_test.cpp")

    def test_template_store(self):
        self.compile_and_run("client_template_store_test.cpp", needs_folder=True)


if __name__ == "__main__":
    unittest.main()
