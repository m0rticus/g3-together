# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NetDirectPunchTests(unittest.TestCase):
    def test_direct_punch_security_fallback_and_goods_validation_separate_tus(self):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="net-direct-punch-", dir=build) as temporary:
            directory = Path(temporary)
            executable = directory / "net_direct_punch.exe"
            sources = [
                ROOT / "tests" / "net_direct_punch_test.cpp",
                ROOT / "src" / "net.cpp",
                ROOT / "src" / "net_session.cpp",
                ROOT / "src" / "net_npc.cpp",
                ROOT / "src" / "net_npc_pose.cpp",
                ROOT / "src" / "net_messages.cpp",
                ROOT / "src" / "npcwire_bones.cpp",
                ROOT / "src" / "npcwire_control.cpp",
                ROOT / "src" / "npcwire_adapter.cpp",
            ]
            include = ROOT / "thirdparty" / "enet" / "include"
            if Path(compiler).stem.lower() == "cl":
                command = [
                    compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                    "/DG3T_NET_SEPARATE_TUS=1",
                    f"/I{include}", *map(str, sources), f"/Fe{executable}", f"/Fo{directory}\\",
                ]
            else:
                command = [
                    compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-DG3T_NET_SEPARATE_TUS=1",
                    f"-I{include}", *map(str, sources), "-o", str(executable),
                ]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
