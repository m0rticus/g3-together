# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcWireAdapterTests(unittest.TestCase):
    def test_exact_record_conversion_and_yaw_error(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++17 compiler required for NPC wire adapter tests")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / ("npcwire_adapter.exe" if os.name == "nt" else "npcwire_adapter")
            sources = [ROOT / "tests" / "npcwire_adapter_test.cpp",
                       ROOT / "src" / "npcwire_adapter.cpp", ROOT / "src" / "npcwire_control.cpp"]
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            *map(str, sources), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
