# SPDX-License-Identifier: GPL-3.0-or-later
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

CODEC_TARGET = _testbuild.register_target(
    "npcwire_bones_codec",
    ["tests/npcwire_bones_codec_test.cpp", "src/npcwire_bones.cpp"],
    pedantic=True, respect_env=False, msvc_vcvars=False,
)
TRANSPORT_TARGET = _testbuild.register_target(
    "npcwire_bones_transport",
    [_testbuild.unit("tests/npcwire_bones_transport_test.cpp", pedantic=True,
                     includes=(_testbuild.ENET_INCLUDE_DIR,), warnings="warn",
                     extra_gnu_flags=("-Werror",)),
     *_testbuild.NPCWIRE_UNITS],
    respect_env=False, msvc_vcvars=False,
)


class NpcWireBoneCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = CODEC_TARGET.executable()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], cwd=ROOT, check=True,
                       capture_output=True, text=True)

    def test_exact_golden_little_endian_bytes(self):
        self.run_case("golden")

    def test_loss_late_join_reordering_and_generation(self):
        self.run_case("recovery")

    def test_malformed_and_conflicting_messages_are_atomic(self):
        self.run_case("malformed")

    def test_full_pose_limit_and_every_int16(self):
        self.run_case("bounds")


class NpcWireBoneTransportTests(unittest.TestCase):
    def test_ack_fallback_reordering_recovery_and_rejoin(self):
        TRANSPORT_TARGET.run(check=True)


if __name__ == "__main__":
    unittest.main()
