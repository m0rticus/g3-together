# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

IDENTITY_TARGET = _testbuild.register_target(
    "player_identity", ["tests/player_identity_test.cpp"],
    allow_msvc=False, respect_env=False,
)
TRANSPORT_TARGET = _testbuild.register_target(
    "identity_transport",
    [_testbuild.unit("tests/identity_transport_test.cpp", pedantic=True,
                     includes=(_testbuild.ENET_INCLUDE_DIR,)),
     *_testbuild.NPCWIRE_UNITS],
    allow_msvc=False, respect_env=False,
)


class PlayerIdentityTests(unittest.TestCase):
    def test_production_identity_migration_and_container_policy(self):
        result = IDENTITY_TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_network_identity_admission_and_capture(self):
        result = TRANSPORT_TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
