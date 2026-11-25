# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild
from test_downed_policy import function_body

EQUIP_UNIT_CODE = r"""
#include "player_equip.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace g3t::player_equip;

// Models Gothic 3 gCInventory_PS stack merging and slot linking:
// - CreateItems merges into unlinked stacks OR linked gESlot_Ammo (14) stacks (MustSplitBeforeLinking(14) == false)
// - SplitStack splits N items off an existing stack into a new unlinked stack
// - LinkStackToSlot rejects linking into an already-occupied slot and moves the stack out of its old slot
struct FakeInventory {
    struct Entry {
        std::string name;
        uint16_t quality = 0;
        int amount = 1;
        uint8_t slot = 0;
        bool collisionDisabled = false;
    };

    std::vector<Entry> stacks;
    std::vector<std::string> invalidTemplates;
    int unlinkCalls = 0;
    int linkCalls = 0;

    bool IsInvalid(char const* name) const
    {
        for (auto const& bad : invalidTemplates)
            if (bad == name) return true;
        return false;
    }

    SlotItem ReadSlot(uint8_t nativeSlot) const
    {
        for (auto const& s : stacks) {
            if (s.slot == nativeSlot) {
                SlotItem out{};
                CopyItemName(out.name, s.name.c_str());
                out.quality = s.quality;
                return out;
            }
        }
        return SlotItem{};
    }

    void UnlinkSlot(uint8_t nativeSlot)
    {
        ++unlinkCalls;
        for (auto& s : stacks) {
            if (s.slot == nativeSlot) s.slot = 0;
        }
    }

    int AcquireUnlinkedStack(char const* name, uint16_t quality, uint8_t nativeSlot)
    {
        if (!name || !*name || IsInvalid(name)) return -1;
        for (size_t i = 0; i < stacks.size(); ++i) {
            if (stacks[i].slot != 0 || stacks[i].amount < 1) continue;
            if (stacks[i].name != name || stacks[i].quality != quality) continue;
            if (stacks[i].amount > 1 && nativeSlot != 14) {
                stacks[i].amount -= 1;
                stacks.push_back(Entry{name, quality, 1, 0, false});
                return static_cast<int>(stacks.size() - 1);
            }
            return static_cast<int>(i);
        }
        // Native CreateItems merges into unlinked or gESlot_Ammo (14) stacks of same template+quality.
        int created = -1;
        for (size_t i = 0; i < stacks.size(); ++i) {
            if (stacks[i].name == name && stacks[i].quality == quality &&
                (stacks[i].slot == 0 || stacks[i].slot == 14)) {
                stacks[i].amount += 1;
                created = static_cast<int>(i);
                break;
            }
        }
        if (created < 0) {
            stacks.push_back(Entry{name, quality, 1, 0, false});
            created = static_cast<int>(stacks.size() - 1);
        }
        if (stacks[static_cast<size_t>(created)].slot != 0 ||
            (stacks[static_cast<size_t>(created)].amount > 1 && nativeSlot != 14)) {
            stacks[static_cast<size_t>(created)].amount -= 1;
            stacks.push_back(Entry{name, quality, 1, 0, false});
            return static_cast<int>(stacks.size() - 1);
        }
        return stacks[static_cast<size_t>(created)].slot == 0 ? created : -1;
    }

    bool LinkToSlot(int stackIndex, uint8_t nativeSlot)
    {
        if (stackIndex < 0 || static_cast<size_t>(stackIndex) >= stacks.size() || nativeSlot == 0) return false;
        for (auto const& s : stacks)
            if (s.slot == nativeSlot) return false;
        ++linkCalls;
        stacks[static_cast<size_t>(stackIndex)].slot = nativeSlot;
        return true;
    }

    void DisableHandCollision(uint8_t nativeSlot)
    {
        for (auto& s : stacks)
            if (s.slot == nativeSlot) s.collisionDisabled = true;
    }
};

void AssertMatches(FakeInventory const& inv, PlayerEquipment const& eq, bool applyWeapons = true, bool applyWear = true)
{
    for (size_t i = 0; i < SlotCount; ++i) {
        const SlotItem want = DesiredSlotItem(eq, i, applyWeapons, applyWear);
        const SlotItem got = inv.ReadSlot(Slots[i].nativeSlot);
        assert(got == want);
    }
}

void TestWireCodecAndValidation()
{
    PlayerEquipment eq{};
    eq.id = 3;
    eq.sequence = 42;
    eq.sent = 123456;
    eq.world = 0x11223344u;
    eq.epoch = 9;
    eq.transformed = false;
    eq.SetSlot(SlotRightHand, "It_1H_MasterSword_01", 6);
    eq.SetSlot(SlotLeftHand, "It_Shield_Paladin_01", 0);
    eq.SetSlot(SlotBackRight, "It_2H_OrcSlayer_01", 2);
    eq.SetSlot(SlotBackLeft, "It_1H_ShortSword_01", 8);
    eq.SetSlot(SlotBow, "It_Crossbow_Heavy_01", 0);
    eq.SetSlot(SlotAmmo, "Bolt", 0);
    eq.SetSlot(SlotBody, "Body_Paladin_01", 0);
    eq.SetSlot(SlotHelmet, "It_Helmet_Horn_01", 256);

    std::vector<uint8_t> bytes;
    assert(Encode(eq, bytes));
    assert(bytes.size() > FixedWireBytes);
    uint8_t peeked = 0;
    assert(PeekPlayerId(bytes.data(), bytes.size(), peeked) && peeked == 3);

    PlayerEquipment decoded{};
    assert(Decode(bytes.data(), bytes.size(), decoded, 3));
    assert(decoded.id == 3 && decoded.sequence == 42 && decoded.sent == 123456);
    assert(decoded.world == 0x11223344u && decoded.epoch == 9 && !decoded.transformed);
    assert(decoded.SameSlots(eq));

    // Wrong authoritative ID, bad version, reserved flag, unknown mask bit, trailing byte, and quality > 1023 fail.
    assert(!Decode(bytes.data(), bytes.size(), decoded, 2));
    auto bad = bytes;
    bad[0] = 2;
    assert(!Decode(bad.data(), bad.size(), decoded));
    bad = bytes;
    bad[2] = 0x02;
    assert(!Decode(bad.data(), bad.size(), decoded));
    bad = bytes;
    bad[20] |= 0x80; // bit 15 beyond SlotCount (12)
    assert(!Decode(bad.data(), bad.size(), decoded));
    bad = bytes;
    bad.push_back(0);
    assert(!Decode(bad.data(), bad.size(), decoded));
    bad = bytes;
    bad[FixedWireBytes + 1] = 0x04; // quality 1024 > MaxQuality
    assert(!Decode(bad.data(), bad.size(), decoded));

    // Empty equipment encodes as exactly FixedWireBytes.
    PlayerEquipment empty{};
    empty.id = 2;
    empty.transformed = true;
    assert(Encode(empty, bytes) && bytes.size() == FixedWireBytes);
    assert(Decode(bytes.data(), bytes.size(), decoded, 2));
    assert(decoded.transformed && decoded.SameSlots(empty));
}

void TestEquipHistoryTimelineCoherence()
{
    EquipHistory hist;
    PlayerEquipment sheathed{};
    sheathed.id = 2;
    sheathed.sequence = 10;
    sheathed.sent = 1000;
    sheathed.SetSlot(SlotBackRight, "It_1H_Sword_01", 2);
    sheathed.SetSlot(SlotBackLeft, "It_Shield_01", 0);
    hist.Push(1, sheathed);

    // First application on newly spawned puppet gets initial sheathed loadout even if renderSent < first sent.
    PlayerEquipment out{};
    assert(hist.Sample(920, false, out));
    assert(out.SameSlots(sheathed));

    // Player draws weapon + shield at sent=1100.
    PlayerEquipment drawn{};
    drawn.id = 2;
    drawn.sequence = 11;
    drawn.sent = 1100;
    drawn.SetSlot(SlotRightHand, "It_1H_Sword_01", 2);
    drawn.SetSlot(SlotLeftHand, "It_Shield_01", 0);
    hist.Push(1, drawn);

    // While playback delay is at 1040 (< 1100), weapon stays cleanly on back (no premature vanish!).
    assert(hist.Sample(1040, true, out));
    assert(out.SameSlots(sheathed));
    // Once presentation reaches 1100, hand & back transition atomically on the same sample.
    assert(hist.Sample(1100, true, out));
    assert(out.SameSlots(drawn));

    // Out-of-order clock tick clamps without dropping newer sequence.
    PlayerEquipment swapped = drawn;
    swapped.sequence = 12;
    swapped.sent = 1095;
    swapped.SetSlot(SlotBackRight, "It_Staff_01", 0);
    hist.Push(1, swapped);
    assert(hist.Sample(1100, true, out));
    assert(std::strcmp(out.slots[SlotBackRight].name, "It_Staff_01") == 0);
}

void TestPuppetSlotTransitionsAndEdgeCases()
{
    FakeInventory inv;
    SlotItem failed[SlotCount]{};

    // Simulate a "look" template spawning with default starting items that the remote player doesn't carry.
    inv.stacks.push_back({"It_Look_DefaultSword", 0, 1, 6, false});
    inv.stacks.push_back({"It_Look_DefaultBow", 0, 1, 7, false});

    // 1. Remote player joins with sheathed 1H sword, shield, bow, ammo, armor, and helmet.
    PlayerEquipment eq{};
    eq.SetSlot(SlotBackRight, "It_1H_Sword_01", 2);
    eq.SetSlot(SlotBackLeft, "It_Shield_01", 0);
    eq.SetSlot(SlotBow, "It_Bow_Long_01", 0);
    eq.SetSlot(SlotAmmo, "Arrow", 0);
    eq.SetSlot(SlotBody, "Body_Merc_01", 0);
    eq.SetSlot(SlotHelmet, "It_Helmet_01", 0);

    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);

    // No-op frame performs zero unlinks and zero links!
    const int unlinksBefore = inv.unlinkCalls;
    const int linksBefore = inv.linkCalls;
    SyncSlots(inv, eq, true, true, failed);
    assert(inv.unlinkCalls == unlinksBefore && inv.linkCalls == linksBefore);

    // 2. Draw sword + shield (BackRight & BackLeft -> RightHand & LeftHand, keeping Bow/Ammo/Body/Helmet intact).
    eq.SetSlot(SlotBackRight, "", 0);
    eq.SetSlot(SlotBackLeft, "", 0);
    eq.SetSlot(SlotRightHand, "It_1H_Sword_01", 2);
    eq.SetSlot(SlotLeftHand, "It_Shield_01", 0);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);

    // 3. Swap directly from drawn melee to drawn Bow + Arrow (with Ammo already in slot 14!).
    eq.SetSlot(SlotBackRight, "It_1H_Sword_01", 2);
    eq.SetSlot(SlotBackLeft, "It_Shield_01", 0);
    eq.SetSlot(SlotBow, "", 0);
    eq.SetSlot(SlotLeftHand, "It_Bow_Long_01", 0);
    eq.SetSlot(SlotRightHand, "Arrow", 0);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);
    // Verify Arrow in Ammo (14) and Arrow in RightHand (1) are separate stacks and RightHand has collision off.
    int arrowStacksLinked = 0;
    for (auto const& s : inv.stacks) {
        if (s.name == "Arrow" && (s.slot == 14 || s.slot == 1)) ++arrowStacksLinked;
        if (s.slot == 1 || s.slot == 2) assert(s.collisionDisabled);
    }
    assert(arrowStacksLinked == 2);

    // 4. Dual-wield two IDENTICAL swords (both sheathed and drawn).
    eq = PlayerEquipment{};
    eq.SetSlot(SlotBackRight, "It_1H_MasterSword_01", 8);
    eq.SetSlot(SlotBackLeft, "It_1H_MasterSword_01", 8);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);

    eq.SetSlot(SlotBackRight, "", 0);
    eq.SetSlot(SlotBackLeft, "", 0);
    eq.SetSlot(SlotRightHand, "It_1H_MasterSword_01", 8);
    eq.SetSlot(SlotLeftHand, "It_1H_MasterSword_01", 8);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);

    // 5. Transform into creature -> clears all weapons & armor; return to human -> restores them.
    eq.transformed = true;
    SyncSlots(inv, eq, true, true, failed);
    for (auto const& spec : Slots) assert(inv.ReadSlot(spec.nativeSlot).Empty());
    eq.transformed = false;
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);

    // 6. QA flags gate weapon slots vs worn slots independently.
    eq.SetSlot(SlotBody, "Body_Paladin_01", 0);
    SyncSlots(inv, eq, false, true, failed);
    AssertMatches(inv, eq, false, true);
    SyncSlots(inv, eq, true, false, failed);
    AssertMatches(inv, eq, true, false);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq, true, true);

    // 7. Invalid template caches failure without infinite retry until desired item changes.
    inv.invalidTemplates.push_back("It_Mod_MissingWeapon");
    eq.SetSlot(SlotBackRight, "It_Mod_MissingWeapon", 0);
    SyncSlots(inv, eq, true, true, failed);
    assert(failed[SlotBackRight].name == std::string("It_Mod_MissingWeapon"));
    const int callsBeforeRetry = inv.linkCalls + inv.unlinkCalls;
    SyncSlots(inv, eq, true, true, failed);
    assert(inv.linkCalls + inv.unlinkCalls == callsBeforeRetry);
    eq.SetSlot(SlotBackRight, "It_2H_Staff_01", 0);
    SyncSlots(inv, eq, true, true, failed);
    AssertMatches(inv, eq);
}
} // namespace

int main()
{
    TestWireCodecAndValidation();
    TestEquipHistoryTimelineCoherence();
    TestPuppetSlotTransitionsAndEdgeCases();
    return 0;
}
"""

TARGET = _testbuild.register_target(
    "player_equip",
    [_testbuild.inline_unit("player_equip_unit.cpp", EQUIP_UNIT_CODE, pedantic=True)],
    respect_env=False, msvc_vcvars=False,
)


class PlayerEquipTests(unittest.TestCase):
    def test_codec_history_and_slot_sync_policy(self):
        executed = TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_event_driven_wiring_and_no_wear_polling(self):
        net_cpp = _testbuild.src_text("net.cpp")
        net_messages = _testbuild.src_text("net_messages.cpp")
        remote = _testbuild.src_text("remote.cpp")
        remote_qa = _testbuild.src_text("remote_qa.cpp")
        remote_equip = _testbuild.src_text("remote_equip.cpp")

        self.assertIn("FlushPlayerEquipment", net_cpp)
        self.assertIn("HandlePlayerEquipmentMessage", net_cpp)
        self.assertIn("equipEntryCache", net_cpp)
        self.assertIn("SetPlayerEquipmentHandler", net_messages)
        self.assertIn("UpdateLocalEquipment", net_messages)
        self.assertIn("CaptureLocalEquipment", remote)
        self.assertIn("FeedPlayerEquipment", remote)
        self.assertIn("UnlinkMismatchedEquipment", remote)
        self.assertIn("ApplyMissingEquipment", remote)
        self.assertIn("SyncAllEquipment", remote)
        self.assertIn("EquipStackToSlot", remote_equip)

        wear = function_body(remote_qa, r"\bvoid\s+Wear\s*\(")
        self.assertIsNotNone(wear)
        self.assertNotIn("250", wear)


if __name__ == "__main__":
    unittest.main()
