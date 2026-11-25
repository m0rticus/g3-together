# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

POLICY_CODE = r"""
#undef NDEBUG
#include "npc_equipment_policy.h"
#include <cassert>

int main()
{
    using namespace g3t::npc_equipment;

    // 1. Empty initial back slots need not be sent until populated, then changes (including clearing) do send.
    BackEquipment empty{1, {0, 0, 0, 0}};
    BackEquipment withBow{1, {0, 0x1111u, 0x2222u, 0}};
    assert(!ShouldSendBackEquipment(false, {}, empty));
    assert(ShouldSendBackEquipment(false, {}, withBow));
    assert(!ShouldSendBackEquipment(true, withBow, withBow));
    assert(ShouldSendBackEquipment(true, withBow, empty));
    BackEquipment respawned{2, {0, 0x1111u, 0x2222u, 0}};
    assert(ShouldSendBackEquipment(true, withBow, respawned));

    // 2. Incarnation ordering accepts equal/newer and rejects older lives (including wraparound).
    assert(ShouldAcceptBackEquipment(0, 1));
    assert(ShouldAcceptBackEquipment(3, 3));
    assert(ShouldAcceptBackEquipment(3, 4));
    assert(!ShouldAcceptBackEquipment(3, 2));
    assert(!ShouldAcceptBackEquipment(1, 0));

    // 3. Back-slot equipping never deletes on 0, never steals from active hand, and replaces merchandise stacks.
    assert(!ShouldEquipBackSlot(0, 0, 0, 0x2222u, false));
    assert(!ShouldEquipBackSlot(0x2222u, 0, 0x2222u, 0, false));
    assert(!ShouldEquipBackSlot(0x1111u, 0x1111u, 0, 0, false));
    assert(!ShouldEquipBackSlot(0x2222u, 0, 0, 0x2222u, false));
    assert(ShouldEquipBackSlot(0x2222u, 0, 0, 0, false));
    assert(ShouldEquipBackSlot(0x2222u, 0, 0, 0x2222u, true));
    assert(ShouldEquipBackSlot(0x3333u, 0, 0, 0x2222u, false));

    // 4. Dead hand-slot unlinking clears lingering hand grips when host cleared the hand slot.
    assert(ShouldUnlinkDeadHandSlot(true, 0, 0x1111u));
    assert(!ShouldUnlinkDeadHandSlot(false, 0, 0x1111u));
    assert(!ShouldUnlinkDeadHandSlot(true, 0x1111u, 0x1111u));
    assert(!ShouldUnlinkDeadHandSlot(true, 0, 0));

    // 5. Mirror StackOf skips Merchandise and non-positive stacks.
    assert(IsMirrorLootStackEligible(false, 1));
    assert(!IsMirrorLootStackEligible(true, 1));
    assert(!IsMirrorLootStackEligible(false, 0));
    assert(!IsMirrorLootStackEligible(false, -1));

    // 6. PersonalState ensures both chests and corpses on local hero loop.
    assert(ShouldEnsureCorpseOnState(true, true, false));
    assert(ShouldEnsureCorpseOnState(true, false, true));
    assert(!ShouldEnsureCorpseOnState(true, false, false));
    assert(!ShouldEnsureCorpseOnState(false, true, true));

    // 7. Host back-slot capture skips empty and Merchandise stacks so traders cannot replicate unlootable stock.
    assert(ShouldCaptureHostBackSlot(true, false));
    assert(!ShouldCaptureHostBackSlot(true, true));
    assert(!ShouldCaptureHostBackSlot(false, false));
    assert(!ShouldCaptureHostBackSlot(false, true));

    // 8. Mirror equipment creation stops once personal corpse state exists (or while unconscious for held items).
    assert(ShouldSyncMirrorBackEquipment(false));
    assert(!ShouldSyncMirrorBackEquipment(true));
    assert(ShouldCreateMirrorHeldItem(false, false));
    assert(!ShouldCreateMirrorHeldItem(true, false));
    assert(!ShouldCreateMirrorHeldItem(false, true));
    assert(!ShouldCreateMirrorHeldItem(true, true));

    // 9. Corpse equipped preservation keeps Trader->Corpse and awakened Corpse->Corpse re-armed gear,
    //    while keeping unawakened same-KO/dead-corpse restores strict.
    assert(ShouldPreserveCorpseEquipped(true, false, true, false));
    assert(ShouldPreserveCorpseEquipped(true, false, true, true));
    assert(!ShouldPreserveCorpseEquipped(false, true, true, false));
    assert(ShouldPreserveCorpseEquipped(false, true, true, true));
    assert(!ShouldPreserveCorpseEquipped(true, true, false, true));

    return 0;
}
"""

TARGET = _testbuild.register_target(
    "npc_equipment_policy",
    [_testbuild.inline_unit("npc_equipment_policy_test.cpp", POLICY_CODE)],
    respect_env=False,
)


class NpcEquipmentPolicyTests(unittest.TestCase):
    def test_back_equipment_and_loot_policy(self):
        result = TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_source_contracts_and_file_size_limits(self):
        for name in ("npc_equipment_policy.h", "npc_state.h", "npc_contact.cpp", "npc_send.cpp",
                     "npcs.cpp", "npc_identity.cpp", "containers.cpp", "personal_ledger.cpp",
                     "net.h", "net_internal.h", "net_messages.cpp", "net_npc.cpp"):
            self.assertLess(len(_testbuild.src_text(name).splitlines()), 1000, name)

        npc_state = _testbuild.src_text("npc_state.h")
        self.assertIn("IsMirrorLootStackEligible(", npc_state)
        self.assertIn("gEStackType_Merchandise", npc_state)

        npc_contact = _testbuild.src_text("npc_contact.cpp")
        self.assertIn("SyncMirrorBackEquipment(", npc_contact)
        self.assertIn("containers::HasPersonalInventoryState(entity)", npc_contact)
        self.assertIn("ShouldSyncMirrorBackEquipment(", npc_contact)
        self.assertIn("ShouldCreateMirrorHeldItem(", npc_contact)
        self.assertIn("ShouldEquipBackSlot(", npc_contact)
        self.assertIn("ShouldUnlinkDeadHandSlot(", npc_contact)

        npc_send = _testbuild.src_text("npc_send.cpp")
        self.assertIn("ShouldCaptureHostBackSlot(", npc_send)
        self.assertIn("gEStackType_Merchandise", npc_send)

        containers = _testbuild.src_text("containers.cpp")
        self.assertIn("ledger::HasCorpseState(key)", containers)

        npcs = _testbuild.src_text("npcs.cpp")
        self.assertIn("SetNpcEquipmentHandler(&OnNpcEquipment)", npcs)
        self.assertIn("SyncMirrorBackEquipment(m, m.latest, true);", npcs)
        self.assertIn("SyncMirrorBackEquipment(m, shown, true);", npcs)


if __name__ == "__main__":
    unittest.main()
