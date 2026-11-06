# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NpcWireBoneCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("A C++17 compiler is required for the engine-free codec tests")
        prefix = ROOT / "tests" / f"npcwire_bones_codec_{os.getpid()}"
        cls.executable = prefix.with_suffix(".exe")
        objects = [Path(f"{prefix}_fixture.obj"), Path(f"{prefix}_codec.obj")]
        cls.addClassCleanup(lambda: [path.unlink(missing_ok=True) for path in [cls.executable, *objects]])
        sources = [ROOT / "tests" / "npcwire_bones_codec_test.cpp", ROOT / "src" / "npcwire_bones.cpp"]
        if Path(compiler).stem.lower() == "cl":
            for source, obj in zip(sources, objects):
                subprocess.run([compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                                "/c", str(source), f"/Fo{obj}"], cwd=ROOT, check=True,
                               capture_output=True, text=True)
            command = [compiler, "/nologo", *map(str, objects), f"/Fe{cls.executable}"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       *map(str, sources), "-o", str(cls.executable)]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], cwd=ROOT, check=True,
                       capture_output=True, text=True)

    def test_exact_golden_little_endian_bytes(self):
        self.run_case("golden")

    def test_loss_late_join_reordering_and_generation(self):
        self.run_case("recovery")

    def test_malformed_and_conflicting_messages_are_atomic(self):
        self.run_case("malformed")

    def test_full_pose_limit_and_every_int16(self):
        self.run_case("bounds")


class NpcWireBoneTransportTests(unittest.TestCase):
    def test_ack_fallback_reordering_recovery_and_rejoin(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C++17 compiler is required for the in-memory transport fixture")
        prefix = ROOT / "tests" / f"npcwire_bones_transport_{os.getpid()}"
        executable = prefix.with_suffix(".exe")
        objects = [Path(f"{prefix}_fixture.obj"), Path(f"{prefix}_codec.obj"),
                   Path(f"{prefix}_control.obj"), Path(f"{prefix}_adapter.obj")]
        self.addCleanup(lambda: [path.unlink(missing_ok=True) for path in [executable, *objects]])
        sources = [ROOT / "tests" / "npcwire_bones_transport_test.cpp", ROOT / "src" / "npcwire_bones.cpp",
                   ROOT / "src" / "npcwire_control.cpp", ROOT / "src" / "npcwire_adapter.cpp"]
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
        subprocess.run([str(executable)], cwd=ROOT, check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
