# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_WIN32 = os.name == "nt"
_ENET_OPTIONS = dict(
    opt="-O2", warnings="w3" if _WIN32 else "none",
    stdcall_gz=_WIN32, defines=("_WINSOCK_DEPRECATED_NO_WARNINGS",) if _WIN32 else (),
)
TARGET = _testbuild.register_target(
    "enet_compression_guard",
    [*_testbuild.enet_units(**_ENET_OPTIONS,
                            extra_gnu_flags=() if _WIN32 else ("-std=c99",)),
     _testbuild.unit(ROOT / "tests" / "enet_compression_guard_test.c",
                     lang="c", std=None if _WIN32 else "c99",
                     includes=(_testbuild.ENET_INCLUDE_DIR,), **_ENET_OPTIONS)],
    toolchain="win32_cl" if _WIN32 else "c",
    libs=("ws2_32", "winmm") if _WIN32 else (),
    respect_env=False,
)


class EnetCompressionGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        built = TARGET.build()
        print(f"ENet compression guard compiler warnings: {len(built.warnings)}")

    def run_case(self, case):
        result = TARGET.run([case], timeout=10)
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
        source = _testbuild.src_text("net.cpp")
        self.assertIn("host->compressor.compress = nullptr;", source)
        self.assertIn("host->compressor.decompress = &DecompressDatagram;", source)
        self.assertIn("guard: ENet compression empty_buffers=", source)
        self.assertIn("action=skip-empty-preserve-checkpoint", source)
        self.assertIn("guard: ENet compression null payload", source)
        self.assertIn("guard: ENet decompression missing context/input/output", source)


if __name__ == "__main__":
    unittest.main()
