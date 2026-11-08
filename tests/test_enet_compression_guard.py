# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class EnetCompressionGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("A C compiler is required")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="enet-compression-guard-", dir=build)
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / "enet_compression_guard.exe"
        include = ROOT / "thirdparty" / "enet" / "include"
        sources = [ROOT / "tests" / "enet_compression_guard_test.c",
                   *(ROOT / "thirdparty" / "enet").glob("*.c")]
        cls.environment = os.environ.copy()
        if os.name == "nt":
            from test_enet_relay_handshake import win32_tool_environment
            cls.environment = win32_tool_environment(directory)
            compiler = shutil.which("cl.exe", path=cls.environment.get("PATH") or cls.environment.get("Path"))
            if not compiler:
                raise RuntimeError("Win32 compiler not found after vcvars32")
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/TC", "/Gz", "/W3", "/O2", "/D_WINSOCK_DEPRECATED_NO_WARNINGS",
                       f"/I{include}", *map(str, sources), f"/Fe{cls.executable}", f"/Fo{directory}\\",
                       "/link", "ws2_32.lib", "winmm.lib"]
        else:
            command = [compiler, "-std=c99", "-O2", f"-I{include}",
                       *map(str, sources), "-o", str(cls.executable)]
            if os.name == "nt":
                command.extend(["-lws2_32", "-lwinmm"])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        warnings = re.findall(r"\bwarning [A-Z]+\d+:", result.stdout + result.stderr, re.IGNORECASE)
        print(f"ENet compression guard compiler warnings: {len(warnings)}")

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], cwd=ROOT,
                                env=self.environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_segments_preserve_exact_compression(self):
        self.run_case("buffers")

    def test_null_inputs_return_codec_failure(self):
        self.run_case("invalid")

    def test_input_limit_bounds_segmented_reads(self):
        self.run_case("limit")

    def test_reliable_pose_checkpoint_survives_compressed_flush(self):
        self.run_case("checkpoint")

    def test_live_transport_installs_logging_guards(self):
        source = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        self.assertIn("host->compressor.compress = &CompressDatagram;", source)
        self.assertIn("host->compressor.decompress = &DecompressDatagram;", source)
        self.assertIn("guard: ENet compression empty_buffers=", source)
        self.assertIn("action=skip-empty-preserve-checkpoint", source)
        self.assertIn("guard: ENet compression null payload", source)
        self.assertIn("guard: ENet decompression missing context/input/output", source)


if __name__ == "__main__":
    unittest.main()
