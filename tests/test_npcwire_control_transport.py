# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "npcwire_control_transport",
    [_testbuild.unit("tests/npcwire_control_transport_test.cpp", pedantic=True,
                     includes=(_testbuild.ENET_INCLUDE_DIR,), warnings="warn",
                     extra_gnu_flags=("-Werror",)),
     *_testbuild.NPCWIRE_UNITS],
    respect_env=False, msvc_vcvars=False,
)


class NpcControlTransportTests(unittest.TestCase):
    def test_live_handlers_and_packet_budget(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
