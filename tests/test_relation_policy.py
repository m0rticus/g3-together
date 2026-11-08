import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RelationPolicyTests(unittest.TestCase):
    def test_production_relation_policy(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix=".relation-", dir=ROOT / "tests") as directory:
            build = Path(directory)
            executable = build / ("relation.exe" if os.name == "nt" else "relation")
            source = ROOT / "tests" / "relation_policy_test.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           str(source), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           str(source), "-o", str(executable)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=build, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
