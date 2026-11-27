# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

HARNESS = r'''
#include "navzone_policy.h"
#include <cassert>

using namespace g3t::navzone_policy;

int main()
{
    int zA = 1, zB = 2, zC = 3;
    int d1 = 10, d2 = 11;

    // 1. Zone migration deduplicates zones/DCCs and registers navDcc in newZone.
    {
        const auto plan = PlanZoneMigration(&zA, &zA, &zB, &d1, &d1);
        assert(plan.deregisterZoneCount == 1);
        assert(plan.deregisterZones[0] == &zA);
        assert(plan.deregisterDccCount == 1);
        assert(plan.deregisterDccs[0] == &d1);
        assert(plan.registerInNewZone);
    }
    {
        const auto plan = PlanZoneMigration(&zA, &zC, &zB, &d1, &d2);
        assert(plan.deregisterZoneCount == 2);
        assert(plan.deregisterZones[0] == &zA);
        assert(plan.deregisterZones[1] == &zC);
        assert(plan.deregisterDccCount == 2);
        assert(plan.deregisterDccs[0] == &d1);
        assert(plan.deregisterDccs[1] == &d2);
        assert(plan.registerInNewZone);
    }
    // Missing navDcc does not force pre-registration before OnPreProcess, but still scrubs entityDcc.
    {
        const auto plan = PlanZoneMigration(&zA, &zB, &zB, nullptr, &d2);
        assert(plan.deregisterZoneCount == 1);
        assert(plan.deregisterZones[0] == &zA);
        assert(plan.deregisterDccCount == 1);
        assert(plan.deregisterDccs[0] == &d2);
        assert(!plan.registerInNewZone);
    }
    // Same zone never deregisters from newZone or re-registers.
    {
        const auto plan = PlanZoneMigration(&zB, &zA, &zB, &d1, &d1);
        assert(plan.deregisterZoneCount == 1);
        assert(plan.deregisterZones[0] == &zA);
        assert(!plan.registerInNewZone);
    }

    // 2. Entry classification handles null, retired, orphan, and live DCCs.
    assert(ClassifyDccEntry(nullptr, false, false) == DccEntryAction::RemoveNull);
    assert(ClassifyDccEntry(nullptr, true, true) == DccEntryAction::RemoveNull);
    assert(ClassifyDccEntry(&d1, true, true) == DccEntryAction::RemoveRetired);
    assert(ClassifyDccEntry(&d1, false, false) == DccEntryAction::RemoveOrphan);
    assert(ClassifyDccEntry(&d1, false, true) == DccEntryAction::Keep);

    // 3. DccRegistry tracks registrations across NavZone/NavPath, zone removal, retirement, and address reuse.
    DccRegistry reg;
    reg.OnRegister(&zA, false, &d1);
    reg.OnRegister(&zB, true, &d1);
    reg.OnRegister(&zA, false, &d1); // duplicate ignored
    reg.OnRegister(&zA, false, &d2);
    reg.OnZoneRemoved(&zB, true);    // destroyed NavPath must not be returned for d1

    const auto zonesForD1 = reg.TakeZonesForDcc(&d1, true);
    assert(zonesForD1.size() == 1);
    assert(zonesForD1[0].zone == &zA && !zonesForD1[0].isPath);
    assert(reg.IsRetired(&d1));
    assert(!reg.IsRetired(&d2));
    assert(reg.TakeZonesForDcc(&d1, true).empty());

    // Reusing d1's address clears retired status on new registration.
    reg.OnRegister(&zC, false, &d1);
    assert(!reg.IsRetired(&d1));
    reg.OnDeregister(&zC, false, &d1);
    assert(reg.TakeZonesForDcc(&d1, false).empty());
    assert(!reg.IsRetired(&d1));

    // Bounded FIFO eviction on retired entries.
    for (int i = 0; i < 8200; ++i) {
        auto* p = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + i * 4));
        reg.TakeZonesForDcc(p, true);
    }
    assert(!reg.IsRetired(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000))));
    assert(reg.IsRetired(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + 8199 * 4))));

    reg.Clear();
    assert(!reg.IsRetired(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + 8199 * 4))));
    assert(reg.TakeZonesForDcc(&d2, false).empty());
    return 0;
}
'''

TARGET = _testbuild.register_target(
    "navzone_policy",
    [_testbuild.inline_unit("navzone_policy.cpp", HARNESS, std="c++17", warnings="strict", includes=(ROOT / "src",))],
    msvc_vcvars=False,
)

TOUCHED_FILES = (
    "navzone_policy.h",
    "navzone_guard.h",
    "navzone_guard.cpp",
    "guards.cpp",
    "remote.cpp",
    "world_pvs.cpp",
    "Script_G3Together.cpp",
)


class NavZoneGuardTests(unittest.TestCase):
    def test_navzone_policy_header_is_engine_free(self):
        policy = _testbuild.src_text("navzone_policy.h")
        includes = re.findall(r"^\s*#\s*include\s+([^\r\n]+)", policy, re.M)
        for inc in includes:
            self.assertTrue(inc.startswith("<"), f"unexpected non-standard include in navzone_policy.h: {inc}")
        for forbidden in ("Game.h", "Script.h", "Engine.h", "Windows.h"):
            self.assertNotIn(forbidden, policy)

    def test_navzone_policy_executes_without_engine(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_navzone_guard_wiring_and_file_sizes(self):
        guard = _testbuild.src_text("navzone_guard.cpp")
        guards = _testbuild.src_text("guards.cpp")
        remote = _testbuild.src_text("remote.cpp")
        world_pvs = _testbuild.src_text("world_pvs.cpp")
        entry = _testbuild.src_text("Script_G3Together.cpp")

        # TestPointAgainstDCCs on both NavZone and NavPath must scrub null/retired/orphan DCCs before calling native.
        self.assertIn("?TestPointAgainstDCCs@gCNavZone_PS@@QAE_NABVbCVector@@ABVeCEntity@@@Z", guard)
        self.assertIn("?TestPointAgainstDCCs@gCNavPath_PS@@QAE_NABVbCVector@@ABVeCEntity@@@Z", guard)
        self.assertIn("?Destroy@gCDynamicCollisionCircle_PS@@UAEXXZ", guard)
        self.assertIn("ScrubDccArray(", guard)
        self.assertIn("navzone_policy::ClassifyDccEntry(", guard)

        # KeepZone must migrate DCC registrations before overwriting AccessCurrentZoneEntityProxy.
        self.assertIn("navzone_guard::MigrateZone(", guards)
        self.assertIn("navigation->AccessCurrentZoneEntityProxy() = here", guards)

        # Puppet despawn, entity destroy, and world change must scrub/reset DCC registrations.
        self.assertIn("navzone_guard::OnEntityDestroyed(entity)", remote)
        self.assertIn("navzone_guard::OnEntityDestroyed(entity)", world_pvs)
        self.assertIn("navzone_guard::OnWorldChange", entry)

        for name in TOUCHED_FILES:
            lines = _testbuild.src_text(name).splitlines()
            self.assertLess(len(lines), 1000, f"src/{name} has {len(lines)} lines (must stay < 1000)")


if __name__ == "__main__":
    unittest.main()
