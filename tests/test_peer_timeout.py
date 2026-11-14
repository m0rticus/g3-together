# SPDX-License-Identifier: GPL-3.0-or-later
from dataclasses import replace
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "peer_timeout",
    [_testbuild.unit("tests/peer_timeout_test.c", std="c99", warnings="none",
                     includes=(_testbuild.ENET_INCLUDE_DIR,)),
     *(replace(source, std="c99") for source in
       _testbuild.enet_units(exclude=("protocol.c",), opt=None, warnings="none"))],
    toolchain="c", allow_msvc=False, respect_env=False, libs=("ws2_32", "winmm"),
)


class PeerTimeoutTests(unittest.TestCase):
    def test_vendored_enet_timeout_and_loading_margin(self):
        result = TARGET.run(timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
