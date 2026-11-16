# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

ADDRESS_NAVZONE_POLICY_TARGET = _testbuild.register_target(
    "address-navzone-policy",
    [_testbuild.unit(
        "tests/address_navzone_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_SRC_FILES = (
    "address.h",
    "address_policy.h",
    "address.cpp",
    "navzone_guard.h",
    "navzone_guard.cpp",
    "ai.h",
    "ai_internal.h",
    "ai.cpp",
    "ai_diag.cpp",
)


class AddressAndNavZonePolicyTests(unittest.TestCase):
    def test_engine_free_address_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            result = ADDRESS_NAVZONE_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_address_host_tick_and_navzone_guard_wiring(self):
        address = _testbuild.src_text("address.cpp")
        ai = _testbuild.src_text("ai.cpp")
        guard = _testbuild.src_text("navzone_guard.cpp")

        # address::InDialog must inspect all InfoManager partner/self/other slots, and HostTick must use
        # BuildHostNpcState + DecideHostApproach so host dialogs and non-zero AIMode cancel without RestoreRoutine or StartGoto.
        in_dialog = address[address.index("bool InDialog("):address.index("address_policy::HostNpcState BuildHostNpcState(")]
        self.assertIn("GetDialogPartnerNPC()", in_dialog)
        self.assertIn("GetDialogPartner1()", in_dialog)
        self.assertIn("GetCurrentSelf()", in_dialog)
        self.assertIn("GetCurrentOther()", in_dialog)

        build_state = address[address.index("BuildHostNpcState("):address.index("struct Asked")]
        self.assertIn("InDialog(inst)", build_state)
        self.assertIn("body::AIModeOf(inst)", build_state)

        host_tick = address[address.index("void HostTick("):address.index("void Tick()")]
        self.assertIn("BuildHostNpcState(", host_tick)
        self.assertIn("address_policy::DecideHostApproach(", host_tick)
        cancel_branch = host_tick[host_tick.index("CancelKeepState:"):host_tick.index("EraseOnly:")]
        self.assertNotIn("RestoreRoutine(", cancel_branch)
        self.assertNotIn("StartGoto(", cancel_branch)

        # navzone_guard.cpp must guard gCNavZone_PS::DetectCollisionCircleExit against null this/nav/out even during shutdown.
        self.assertIn("navzone_guard::Install()", ai)
        self.assertIn("?DetectCollisionCircleExit@gCNavZone_PS@@QAE_NPAVgCNavigation_PS@@AAVbCVector@@@Z", guard)
        guard_fn = guard[guard.index("SafeDetectCollisionCircleExit("):guard.index("void Install()")]
        self.assertLess(guard_fn.index("if (!self || !nav || !out)"), guard_fn.index("ShuttingDown()"))
        self.assertIn("return GEFalse;", guard_fn)

    def test_all_touched_source_files_stay_strictly_under_1000_lines(self):
        for name in TOUCHED_SRC_FILES:
            line_count = len(_testbuild.src_text(name).splitlines())
            self.assertLess(line_count, 1000, f"src/{name} has {line_count} lines (must be < 1000)")


if __name__ == "__main__":
    unittest.main()
