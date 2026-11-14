# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

STEAM_DIAGNOSTICS_TARGET = _testbuild.register_target(
    "steam-diagnostics",
    [_testbuild.unit(
        "tests/steam_diagnostics_test.cpp",
        std="c++17",
        opt="-O2",
        warnings="strict",
    )],
)
STEAM_V012_RUNTIME_TARGET = _testbuild.register_target(
    "steam-v012-runtime",
    [
        _testbuild.ENET_COMPRESS_UNIT,
        _testbuild.unit(
            "tests/steam_v012_runtime_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(_testbuild.ENET_INCLUDE_DIR,),
        ),
    ],
    libs=("ws2_32", "bcrypt"),
)


class SteamDiagnosticsTests(unittest.TestCase):
    def test_flat_api_route_evidence_and_description_privacy(self):
        result = STEAM_DIAGNOSTICS_TARGET.run(cwd=ROOT, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_v012_runtime_lanes_fallback_pinning_and_rejoin_rotation(self):
        result = STEAM_V012_RUNTIME_TARGET.run(cwd=ROOT, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
