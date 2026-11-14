import os
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "enet_fragment_test",
    [*_testbuild.enet_units(exclude=("protocol.c",), opt=None, warnings="none",
                            extra_gnu_flags=("-std=c99",)),
     _testbuild.unit(ROOT / "thirdparty" / "enet" / "protocol.c", lang="c",
                     std="c99", warnings="none",
                     defines=("enet_socket_send=g3t_test_socket_send",),
                     includes=(_testbuild.ENET_INCLUDE_DIR,)),
     _testbuild.unit(ROOT / "tests" / "enet_fragment_test.cpp",
                     includes=(_testbuild.ENET_INCLUDE_DIR,))],
    libs=("ws2_32", "winmm") if os.name == "nt" else (),
    respect_env=False, msvc_vcvars=False,
    skip_message="An existing C/C++ toolchain is needed for ENet queue tests",
)


class EnetFragmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TARGET.build()

    def run_case(self, case):
        TARGET.run([case], check=True)

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
