# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

STEAM_SEND_TARGET = _testbuild.register_target(
    "steam-send",
    [
        _testbuild.ENET_COMPRESS_UNIT,
        _testbuild.unit(
            "tests/steam_send_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(_testbuild.ENET_INCLUDE_DIR,),
        ),
    ],
)


class SteamSendTests(unittest.TestCase):
    def test_retention_priority_and_lossless_payload_savings(self):
        result = STEAM_SEND_TARGET.run(cwd=ROOT, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
