# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CorePoliciesTests(unittest.TestCase):
    def test_damage_perception_processing_replication_and_guid_codec(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "A C++17 compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="core-policies-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / ("core_policies.exe" if os.name == "nt" else "core_policies")
            sources = [ROOT / "tests" / "core_policies_test.cpp", ROOT / "src" / "guid_text.cpp"]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           f"/I{ROOT / 'src'}", *map(str, sources), f"/Fe:{executable}", f"/Fo:{directory}\\"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           f"-I{ROOT / 'src'}", *map(str, sources), "-o", str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True, timeout=20)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
