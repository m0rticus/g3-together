import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class EnetFragmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("An existing C/C++ toolchain is needed for ENet queue tests")
        compiler = Path(compiler)
        msvc = compiler.stem.lower() == "cl"
        c_name = "clang" if compiler.stem.lower() == "clang++" else "gcc"
        c_compiler = compiler if msvc else compiler.with_name(c_name + (".exe" if os.name == "nt" else ""))
        if not c_compiler.is_file():
            raise unittest.SkipTest("The C compiler accompanying the C++ toolchain is needed")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="enet-queue-tests-", dir=build)
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / "enet_fragment_test.exe"
        enet = ROOT / "thirdparty" / "enet"
        include = enet / "include"
        sources = list(enet.glob("*.c"))
        protocol = enet / "protocol.c"
        sources.remove(protocol)
        if msvc:
            command = [str(c_compiler), "/nologo", "/TC", "/c", f"/I{include}",
                       *(str(source) for source in sources), f"/Fo{directory}\\"]
            suffix = ".obj"
        else:
            command = [str(c_compiler), "-std=c99", "-c", "-I", str(include),
                       *(str(source) for source in sources)]
            suffix = ".o"
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"Vendored ENet compilation failed:\n{result.stdout}{result.stderr}")
        if msvc:
            command = [str(c_compiler), "/nologo", "/TC", "/c", f"/I{include}",
                       "/Denet_socket_send=g3t_test_socket_send", str(protocol), f"/Fo{directory}\\protocol.obj"]
        else:
            command = [str(c_compiler), "-std=c99", "-c", "-I", str(include),
                       "-Denet_socket_send=g3t_test_socket_send", str(protocol)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"ENet mock-socket compilation failed:\n{result.stdout}{result.stderr}")
        sources.append(protocol)
        source = ROOT / "tests" / "enet_fragment_test.cpp"
        objects = [str(directory / (source.stem + suffix)) for source in sources]
        if msvc:
            command = [str(compiler), "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", f"/I{include}",
                       str(source), *objects, f"/Fe{cls.executable}", f"/Fo{directory / 'test.obj'}",
                       "ws2_32.lib", "winmm.lib"]
        else:
            command = [str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                       str(source), *objects, "-o", str(cls.executable)]
            if os.name == "nt":
                command.extend(["-lws2_32", "-lwinmm"])
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"ENet queue test compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True)

    def test_old_1199_byte_promotion_is_reproduced_and_best_effort_boundaries_stay_unreliable(self):
        self.run_case("boundary")

    def test_explicit_reliable_life_hits_and_saves_retain_reliability(self):
        self.run_case("reliable")

    def test_fragmentation_uses_actual_negotiated_mtu_and_checksum_overhead(self):
        self.run_case("negotiated")

    def test_enet_reliable_epoch_rollover_remains_intact(self):
        self.run_case("epoch")

    def test_compact_poses_share_real_enet_datagrams(self):
        self.run_case("packing")

    def test_enet_recovers_reliable_actions_and_saves_over_unreliable_steam_relay(self):
        self.run_case("steam")

    def test_multi_npc_pose_replacement_preserves_enet_unreliable_sequence_order(self):
        self.run_case("pose-order")


if __name__ == "__main__":
    unittest.main()
