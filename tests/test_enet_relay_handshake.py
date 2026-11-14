# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild
from _g3copy import win32_tool_environment

_target = _testbuild.register_target if os.name == "nt" else _testbuild.NativeTarget
TARGET = _target(
    "enet_relay_handshake",
    [*_testbuild.enet_units(warnings="w3", stdcall_gz=True,
                            defines=("_WINSOCK_DEPRECATED_NO_WARNINGS",)),
     _testbuild.unit(ROOT / "tests" / "enet_relay_handshake_test.cpp",
                     opt="-O2", warnings="strict", stdcall_gz=True,
                     includes=(_testbuild.ENET_INCLUDE_DIR,))],
    toolchain="win32_cl", libs=("ws2_32", "winmm"),
)


class EnetRelayHandshakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "nt":
            raise unittest.SkipTest("production /Gz Win32 ENet ABI requires Windows")
        built = TARGET.build()
        print(f"Win32 /Gz compiler warnings: {len(built.warnings)}")
        for warning in built.warnings:
            print(warning)

    def run_case(self, *args):
        result = TARGET.run(args, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")
        return result.stdout

    def test_native_win32_compressed_handshake_through_both_queues(self):
        self.assertIn("native ENet two-relay queued handshake/data/reconnect passed", self.run_case())
        self.assertIn("all 13 native command sizes match", self.run_case("sizes"))
        self.assertIn("direct ENet UDP cookie hole-punch and uninvited packet drop passed", self.run_case("direct"))

    def test_native_win32_impaired_first_handshake_through_unreliable_queues(self):
        self.assertIn("impaired connected=2 exact_reliable_payloads=2",
                      self.run_case("impaired"))


if __name__ == "__main__":
    unittest.main()
