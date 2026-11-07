# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NpcControlTransportTests(unittest.TestCase):
    def test_live_handlers_and_packet_budget(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C++17 compiler is required for the transport fixture")
        prefix = ROOT / "tests" / f"npcwire_control_transport_{os.getpid()}"
        executable = prefix.with_suffix(".exe")
        sources = [ROOT / "tests" / "npcwire_control_transport_test.cpp",
                   ROOT / "src" / "npcwire_bones.cpp", ROOT / "src" / "npcwire_control.cpp",
                   ROOT / "src" / "npcwire_adapter.cpp"]
        objects = [Path(f"{prefix}_{i}.obj") for i in range(len(sources))]
        self.addCleanup(lambda: [path.unlink(missing_ok=True) for path in [executable, *objects]])
        include = ROOT / "thirdparty" / "enet" / "include"
        if Path(compiler).stem.lower() == "cl":
            for source, obj in zip(sources, objects):
                subprocess.run([compiler, "/nologo", "/std:c++17", "/EHsc", "/W4",
                                f"/I{include}", "/c", str(source), f"/Fo{obj}"],
                               cwd=ROOT, check=True, capture_output=True, text=True)
            command = [compiler, "/nologo", *map(str, objects), f"/Fe{executable}"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       f"-I{include}", *map(str, sources), "-o", str(executable)]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
        result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
