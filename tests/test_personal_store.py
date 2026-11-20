# SPDX-License-Identifier: GPL-3.0-or-later
import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "personal_store", ["tests/personal_store_test.cpp"],
    allow_msvc=False, respect_env=False,
)

NORMALIZE_TARGET = _testbuild.register_target(
    "personal_store_normalize",
    [_testbuild.inline_unit(
        "personal_store_normalize",
        r'''
#include "personal_store.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::string MakeGuid(uint16_t index)
{
    std::string id(16, '\0');
    id[0] = static_cast<char>((index & 0xffu) ? (index & 0xffu) : 1u);
    id[1] = static_cast<char>((index >> 8) & 0xffu);
    id[15] = 0x42;
    return id;
}
} // namespace

int main()
{
    using namespace g3t::saving::personal;

    Inventory gelford;
    gelford.kind = Kind::Trader;
    gelford.flags = GeneratedTrade;
    const std::string zeroId(16, '\0');
    const std::string rebelArmor = MakeGuid(1);
    const std::string goldCoin = MakeGuid(2);
    const std::string healingPotion = MakeGuid(3);

    // Quirky engine stacks: zero-GUID dummy, 0-amount placeholder, high quality flag bits (>= 1024),
    // duplicate (item, maskedQuality, stock) groups, and an oversized stack above MaxAmount.
    gelford.stacks.push_back({zeroId, 5, 0, Stock::Merchandise});
    gelford.stacks.push_back({rebelArmor, 0, 0, Stock::Merchandise});
    gelford.stacks.push_back({rebelArmor, 1, 2048u | 7u, Stock::Merchandise});
    gelford.stacks.push_back({rebelArmor, 2, 4096u | 7u, Stock::Merchandise});
    gelford.stacks.push_back({goldCoin, MaxAmount + 55u, 0, Stock::Merchandise});
    gelford.stacks.push_back({healingPotion, 4, 1024u, Stock::Normal});

    bool truncated = true;
    NormalizeStacks(gelford.stacks, &truncated);
    assert(!truncated);
    assert(Valid(gelford));
    assert(gelford.stacks.size() == 3);

    Snapshot snap;
    snap[MakeGuid(100)] = gelford;
    std::vector<uint8_t> encoded;
    assert(Encode(snap, encoded));
    Snapshot decoded;
    assert(Decode(encoded, decoded));
    assert(decoded.size() == 1);
    auto const& restored = decoded.begin()->second;
    assert(restored.kind == Kind::Trader);
    assert(restored.stacks.size() == 3);
    assert(restored.stacks[0].item == rebelArmor && restored.stacks[0].amount == 3 &&
           restored.stacks[0].quality == 7 && restored.stacks[0].type == Stock::Merchandise);
    assert(restored.stacks[1].item == goldCoin && restored.stacks[1].amount == MaxAmount);
    assert(restored.stacks[2].item == healingPotion && restored.stacks[2].amount == 4 &&
           restored.stacks[2].quality == 0 && restored.stacks[2].type == Stock::Normal);

    // Exceeding MaxStacks keeps player Stock::Normal items ahead of excess Stock::Merchandise rows.
    Inventory crowded;
    crowded.kind = Kind::Trader;
    crowded.flags = GeneratedTrade;
    for (uint16_t i = 1; i <= MaxStacks + 16; ++i)
        crowded.stacks.push_back({MakeGuid(static_cast<uint16_t>(200 + i)), 1, 0, Stock::Merchandise});
    const std::string playerSword = MakeGuid(9999);
    crowded.stacks.push_back({playerSword, 1, 15, Stock::Normal});
    NormalizeStacks(crowded.stacks, &truncated);
    assert(truncated);
    assert(crowded.stacks.size() == MaxStacks);
    assert(Valid(crowded));
    bool foundPlayerSword = false;
    for (auto const& stack : crowded.stacks)
        if (stack.item == playerSword && stack.type == Stock::Normal && stack.quality == 15) foundPlayerSword = true;
    assert(foundPlayerSword);
    return 0;
}
''',
        includes=[ROOT / "src"],
    )],
    allow_msvc=False,
    respect_env=False,
)

EXPECTED_SITES = {
    "?GeneratePlunderInvFromTreasureSets@gCInventory_PS@@QAE_NXZ": (0xB71F0, 8),
    "?GenerateTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ": (0xB7340, 8),
    "?RefreshTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ": (0xB7400, 5),
    "?BeginTrade@gCGUIManager@@QAE_NPAVeCEntity@@@Z": (0x7BFD0, 6),
    "?TransferItemsTo@gCInventory_PS@@QAE_NPAV1@HHW4gEStackType@@@Z": (0xB6E20, 8),
    "?TransferStackTo@gCInventory_PS@@QAE_NPAV1@HW4gEStackType@@@Z": (0xB6DE0, 7),
    "?TransferAllTo@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@@Z": (0xB6D70, 6),
    "?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z": (0xB7170, 5),
    "?PerformTransaction@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@1@Z": (0xB7130, 6),
    "?TransferAmountTo@PSInventory@@QAE_NHHAAVEntity@@@Z": (0x64D0, 6),
}


class PeFile:
    """Read only the configured on-disk PE; never load or execute it."""
    def __init__(self, path):
        self.data = _testbuild.cached_bytes(path)
        pe = self.u32(0x3C)
        if self.u16(0) != 0x5A4D or self.u32(pe) != 0x4550 or self.u16(pe + 4) != 0x14C:
            raise ValueError("not a 32-bit x86 PE")
        optional = pe + 24
        if self.u16(optional) != 0x10B:
            raise ValueError("not PE32")
        self.image_base = self.u32(optional + 28)
        self.export_rva, self.export_size = self.u32(optional + 96), self.u32(optional + 100)
        self.import_rva = self.u32(optional + 104)
        self.reloc_rva, self.reloc_size = self.u32(optional + 136), self.u32(optional + 140)
        section_table = optional + self.u16(pe + 20)
        self.sections = []
        for i in range(self.u16(pe + 6)):
            section = section_table + i * 40
            va, raw_size, raw_at = (self.u32(section + 12), self.u32(section + 16),
                                    self.u32(section + 20))
            self.sections.append((va, raw_size, raw_at))

    def u16(self, offset):
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    def offset(self, rva, size=1):
        for va, raw_size, raw_at in self.sections:
            if va <= rva and rva - va + size <= raw_size and raw_at + rva - va + size <= len(self.data):
                return raw_at + rva - va
        raise ValueError(f"unmapped RVA {rva:#x}")

    def read(self, rva, size):
        offset = self.offset(rva, size)
        return self.data[offset:offset + size]

    def exports(self):
        directory = self.offset(self.export_rva, 40)
        function_count, name_count = self.u32(directory + 20), self.u32(directory + 24)
        functions, names, ordinals = (self.u32(directory + 28), self.u32(directory + 32),
                                      self.u32(directory + 36))
        found = {}
        for i in range(name_count):
            name_rva = self.u32(self.offset(names + 4 * i, 4))
            start = self.offset(name_rva)
            end = self.data.find(b"\0", start)
            if end < 0:
                raise ValueError("unterminated export name")
            name = self.data[start:end].decode("ascii")
            ordinal = self.u16(self.offset(ordinals + 2 * i, 2))
            if ordinal >= function_count or name in found:
                raise ValueError("invalid or duplicate export")
            found[name] = self.u32(self.offset(functions + 4 * ordinal, 4))
        return found

    def imported_at(self, iat_rva):
        for i in range(4096):
            descriptor = self.offset(self.import_rva + i * 20, 20)
            original, _, _, name, first = struct.unpack_from("<IIIII", self.data, descriptor)
            if not original and not name and not first:
                break
            start = self.offset(name)
            dll = self.data[start:self.data.index(b"\0", start)].decode("ascii")
            for n in range(65536):
                thunk = self.u32(self.offset((original or first) + n * 4, 4))
                if not thunk:
                    break
                if first + n * 4 == iat_rva:
                    if thunk & 0x80000000:
                        raise ValueError("ordinal import")
                    start = self.offset(thunk, 3) + 2
                    symbol = self.data[start:self.data.index(b"\0", start)].decode("ascii")
                    return dll, symbol
        raise ValueError("missing IAT slot")

    def has_highlow_relocation(self, target):
        pos = 0
        while pos + 8 <= self.reloc_size:
            block = self.offset(self.reloc_rva + pos, 8)
            page, size = struct.unpack_from("<II", self.data, block)
            if size < 8 or size % 2 or pos + size > self.reloc_size:
                raise ValueError("invalid relocation block")
            for index in range(8, size, 2):
                entry = self.u16(self.offset(self.reloc_rva + pos + index, 2))
                if entry >> 12 == 3 and page + (entry & 0xFFF) == target:
                    return True
            pos += size
        return False


class PersonalStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()
        result = subprocess.run([str(cls.executable), "--hook-sites"], capture_output=True, text=True, check=True)
        cls.sites = []
        for line in result.stdout.splitlines():
            module, symbol, rva, stolen, head = line.split("\t")
            cls.sites.append((module, symbol, int(rva, 16), int(stolen), bytes.fromhex(head)))

    def test_format(self):
        result = subprocess.run([str(self.executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_verified_guard_registration(self):
        self.assertEqual(len(self.sites), len(EXPECTED_SITES))
        self.assertEqual({symbol: (rva, stolen) for _, symbol, rva, stolen, _ in self.sites}, EXPECTED_SITES)
        source = _testbuild.src_text("containers.cpp")
        install = source[source.index("void* InstallPersonalNative("):source.index("void HookQuickLoot()")]
        self.assertIn("personal::native::Find(module, symbol)", install)
        self.assertIn("personal::native::Match(*site, site->rva, entry, site->headSize)", install)
        self.assertIn("reinterpret_cast<uintptr_t>(game) + site->rva", install)
        self.assertIn("ai::JumpTo(entry, detour, stolen)", install)
        for _, symbol, _, _, _ in self.sites:
            self.assertIn('"' + symbol + '"', install)

    def check_configured_dll(self, env, module):
        path = os.environ.get(env)
        if not path:
            self.skipTest(f"Set {env} for read-only native PE checks")
        image = PeFile(path)
        exports = image.exports()
        sites = [site for site in self.sites if site[0] == module]
        self.assertEqual(len(sites), 9 if module == "Game.dll" else 1)
        for _, name, rva, stolen, head in sites:
            with self.subTest(export=name):
                self.assertIn(name, exports)
                self.assertEqual(exports[name], rva)
                self.assertFalse(image.export_rva <= rva < image.export_rva + image.export_size,
                                 "forwarded export cannot be patched")
                self.assertGreaterEqual(stolen, 5)
                self.assertLessEqual(stolen, len(head))
                self.assertEqual(image.read(rva, len(head)), head)
        return image, sites

    def test_configured_game_exports_and_trade_coexistence(self):
        image, sites = self.check_configured_dll("G3T_GAME_DLL", "Game.dll")
        transaction = EXPECTED_SITES["?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z"][0]
        gate = bytes.fromhex("3b df 7c 0c 85 db 7e 08 85 ff 7e 04 b0 01 eb 02 32 c0")
        self.assertEqual(image.read(0x8144A, len(gate)), gate)
        for call in (0x8A123, 0x8A12D):
            instruction = image.read(call, 5)
            self.assertEqual(instruction[0], 0xE8)
            self.assertEqual(call + 5 + struct.unpack("<i", instruction[1:])[0], transaction)
        trade_patches = [(0x8144A, len(gate)), (0x8A124, 4), (0x8A12E, 4), (0xA1570, 6)]
        for _, name, rva, _, head in sites:
            for other, size in trade_patches:
                self.assertTrue(rva + len(head) <= other or other + size <= rva,
                                f"{name} signature overlaps a trade hook")

    def test_configured_script_export(self):
        self.check_configured_dll("G3T_SCRIPT_DLL", "Script.dll")

    def test_configured_script_game_loot_access(self):
        path = os.environ.get("G3T_SCRIPT_GAME_DLL")
        if not path:
            self.skipTest("Set G3T_SCRIPT_GAME_DLL for read-only loot access checks")
        image = PeFile(path)
        self.assertEqual(image.read(0x6BFD0, 12), bytes.fromhex("55 8b ec 83 e4 f8 81 ec 54 01 00 00"))
        quick_call = image.read(0x81CED, 6)
        self.assertEqual(quick_call[:2], b"\xff\x15")
        self.assertEqual(struct.unpack("<I", quick_call[2:])[0], image.image_base + 0xE4500)
        self.assertTrue(image.has_highlow_relocation(0x81CEF))
        self.assertEqual(image.imported_at(0xE4500),
                         ("Script.dll", "?TransferAmountTo@PSInventory@@QAE_NHHAAVEntity@@@Z"))

    def test_trade_chains_through_the_export(self):
        source = _testbuild.src_text("trade.cpp")
        native = source[source.index("GEBool __fastcall NativeTransfer("):source.index("bool __stdcall NativeCanExchange(")]
        self.assertIn("if (!fromPlayer && !toPlayer) return originalTransfer(from, to, type);", native)
        self.assertIn("BeginExchange(from, to, toPlayer)", native)
        install = source[source.index("void HookNative()"):source.index("bool OnResult(")]
        self.assertIn("originalTransfer = reinterpret_cast<TransferFn>(GetProcAddress", install)
        self.assertIn('"?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z"', install)
        self.assertIn("const uintptr_t calls[] = {0x8a123, 0x8a12d}", install)
        self.assertIn("std::memcpy(base + call + 1, &offset", install)

    def test_native_access_guards_and_deferred_capture(self):
        containers_source = _testbuild.src_text("containers.cpp")
        ledger_source = _testbuild.src_text("personal_ledger.cpp")
        ledger_header = _testbuild.src_text("personal_ledger.h")

        def part(text, start, end):
            return text[text.index(start):text.index(end, text.index(start))]

        resolve = part(ledger_source, "bool ResolveStackTemplates(", "void ApplyPersonalMetadata(")
        self.assertIn("item->GetPropertySet", resolve)
        self.assertIn("!PersonalCounted(entity, i)", resolve)
        apply = part(ledger_source, "bool ApplyPersonal(", "bool AwaitingDeath(")
        self.assertLess(apply.index("ResolveStackTemplates(entity, instance, row.stacks"), apply.index("DeleteStack"))
        self.assertIn("PersonalCounted(entity, i)", apply)
        self.assertIn("!entity.Inventory.DeleteStack(i)", apply)
        self.assertIn("inventory->CreateItems", apply)
        self.assertIn("< 0) return rollbackAndFail()", apply)
        self.assertIn("RollbackPersonal(entity, *inventory, before, beforeTemplates, &beforeSlots)", apply)
        ensure = part(ledger_source, "bool EnsurePersonal(", "bool RememberPersonal(")
        self.assertIn("AwaitingDeath(entity", ensure)
        self.assertLess(ensure.index("!ApplyPersonal"), ensure.index("personalPending.erase"))
        capture = part(ledger_source, "bool Capture(", "bool Restore(")
        self.assertIn("!installAccessGuards()", capture)
        self.assertIn("FlushPersonalObservers()", capture)
        self.assertIn("PersonalPass(true)", capture)
        self.assertIn("EnsurePersonal(entity)", capture)
        self.assertIn("RememberPersonal(entity)", capture)
        self.assertIn("items::AppendPersonal(ground)", capture)
        self.assertIn("combined.emplace(entry).second", capture)
        self.assertIn("personal::Encode(combined, out)", capture)
        self.assertNotIn("personalErrors", capture)
        self.assertNotIn("personalErrorOverflow", capture)
        restore = part(ledger_source, "bool Restore(", "bool PersonalMutation::Enter(")
        self.assertLess(restore.index("personal::Decode"), restore.index("personalLedger.swap"))
        self.assertIn("!bytes.empty() && !personal::Decode", restore)
        self.assertIn("items::RestorePersonal(snapshot)", restore)
        self.assertIn("items::TickPersonal()", restore)
        self.assertIn("personal::Kind::GroundTaken", restore)
        self.assertIn("personalPending.insert", restore)
        self.assertIn("PersonalPass(false, &attempted)", restore)
        self.assertIn("guid::Resolve", restore)
        self.assertIn("AwaitingDeath", restore)
        self.assertIn("EnsurePersonal(entity);", restore)
        self.assertLess(restore.index("if (personalLedger.empty()) return true"),
                        restore.index("if (!installAccessGuards || !installAccessGuards())"))
        reset_ledger = part(ledger_source, "void Reset()", "bool Capture(")
        self.assertIn("personalLedger.clear()", reset_ledger)
        self.assertIn("items::OnWorldChange()", reset_ledger)
        reset = part(containers_source, "void OnWorldChange()", "bool InstallAccessGuards()")
        self.assertIn("ledger::Reset()", reset)
        quick = part(containers_source, "GEBool __fastcall QuickTake(", "void HookQuickLoot()")
        self.assertIn("if (wasPending)", quick)
        self.assertIn("EnsurePersonal(entity)", quick)
        self.assertIn("return GEFalse", quick)
        for marker in ("GeneratePersonal(", "BeginPersonalTrade(", "PersonalState(", "Opened("):
            self.assertIn(marker, containers_source)
        observer = part(ledger_source, "struct ObserverState final", "namespace {")
        observer_destroy = part(ledger_source, "void ObserverState::OnDestroyInventory()", "bool HasInventory(")
        self.assertIn("changes.Changed()", observer)
        self.assertIn("changes.Destroyed()", observer_destroy)
        self.assertIn("personalUncertain.insert(key)", observer_destroy)
        self.assertNotIn("RememberPersonal", observer + observer_destroy)
        self.assertNotIn("ReadPersonal", observer + observer_destroy)
        self.assertIn("~PersonalMutation() { Finish(); }", ledger_header)
        completion = part(ledger_source, "void PersonalMutation::Finish()", "} // namespace g3t::containers::ledger")
        self.assertIn("!RememberPersonal(entity)", completion)
        self.assertIn("personalUncertain.insert", completion)
        for marker in ("TransferPersonalItems", "TransferPersonalStack", "TransferPersonalAll",
                       "TransferPersonalTransaction", "PerformPersonalTransaction", "TransferPersonalScript"):
            self.assertIn(marker, containers_source)
        guard_helper = part(containers_source, "GEBool GuardedInventoryTransfer(", "GEBool __fastcall TransferPersonalItems(")
        self.assertIn("!PersonalPlayerTransfer(", guard_helper)
        self.assertIn("PersonalMutation mutation;", guard_helper)
        transfers = part(containers_source, "GEBool __fastcall TransferPersonalItems(", "GEBool __fastcall BeginPersonalTrade(")
        self.assertEqual(transfers.count("return GuardedInventoryTransfer("), 6)
        ready_helper = part(containers_source, "bool AreAllPersonalHooksReady()", "bool InstallAccessGuards()")
        for marker in ("nativeTransferItems", "nativeTransferStack", "nativeTransferAll",
                       "nativeTransferTransaction", "nativeTransaction", "nativeScriptTransfer"):
            self.assertIn(marker, ready_helper)
        ready = part(containers_source, "bool PersonalHooksReady()", "bool CapturePersonal(")
        self.assertIn("AreAllPersonalHooksReady()", ready)
        tick = containers_source[containers_source.index("void Tick()"):]
        self.assertIn("items::TickPersonal()", tick)
        self.assertIn("PersonalPass(false)", tick)
        self.assertNotIn("HostTick();", tick)
        self.assertNotIn("ClientTick();", tick)
        reclaim = part(ledger_source, "void DetachAndRecycleObserver(", "void ReclaimCompletedObservers(")
        self.assertIn("observer->inventory->RemoveListener(observer)", reclaim)
        self.assertIn("reusablePersonalObservers.push_back(observer)", reclaim)
        self.assertIn("ReclaimCompletedObservers()", ledger_source)

    def test_merchant_quirks_normalize_and_never_poison_saves(self):
        result = NORMALIZE_TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        ledger_source = _testbuild.src_text("personal_ledger.cpp")
        containers_source = _testbuild.src_text("containers.cpp")

        def part(text, start, end):
            return text[text.index(start):text.index(end, text.index(start))]

        read_fn = part(ledger_source, "bool ReadPersonal(", "bool CompatiblePersonalKind(")
        self.assertIn("StackMerchandise(entity, *inventory, i, kind)", read_fn)
        self.assertIn("personal::MaskQuality(entity.Inventory.GetQuality(i))", read_fn)
        self.assertIn("personal::NormalizeStacks(result.stacks", read_fn)

        resolve_fn = part(ledger_source, "bool ResolveStackTemplates(", "void ApplyPersonalMetadata(")
        self.assertIn("entity.Inventory.GetSlotFromStack(i) == gESlot_None", resolve_fn)

        apply_fn = part(ledger_source, "bool ApplyPersonal(", "bool AwaitingDeath(")
        self.assertIn("CompatiblePersonalKind(row.kind, kind)", apply_fn)

        remember_fn = part(ledger_source, "bool RememberPersonal(", "bool TrackPersonalFocus(")
        self.assertIn("observer->changes.Refreshed()", remember_fn)

        enter_fn = part(ledger_source, "bool PersonalMutation::Enter(", "bool PersonalMutation::StillAvailable(")
        self.assertNotIn("if (!RememberPersonal(entity)) return false", enter_fn)

        prepare_trade = part(containers_source, "bool PrepareLocalTrade(", "bool Opened(")
        self.assertNotIn("cannot refresh personal stock", prepare_trade)
        self.assertIn("AccessGeneratedTrade() = GETrue", prepare_trade)

    def test_corpse_equipped_loot_ko_state_and_observer_reclaim(self):
        ledger_source = _testbuild.src_text("personal_ledger.cpp")
        containers_source = _testbuild.src_text("containers.cpp")
        containers_header = _testbuild.src_text("containers.h")

        for name in ("containers.cpp", "containers.h", "personal_ledger.cpp", "personal_ledger.h", "personal_store.h"):
            self.assertLess(len(_testbuild.src_text(name).splitlines()), 1000, name)

        def part(text, start, end):
            return text[text.index(start):text.index(end, text.index(start))]

        counted_fn = part(ledger_source, "bool Counted(", "bool StackMerchandise(")
        self.assertIn("bool allowEquipped", counted_fn)
        self.assertIn("entity.Inventory.GetCategory(stack) == gEItemCategory_None", counted_fn)

        personal_counted = part(ledger_source, "bool PersonalCounted(", "void DetachAndRecycleObserver(")
        self.assertIn("kind == personal::Kind::Corpse", personal_counted)
        self.assertIn("Counted(entity, index, allowEquipped)", personal_counted)

        for name in ("bool IsContainer(", "bool IsTrader(", "bool Corpse(", "bool AwaitingDeath("):
            end_marker = {
                "bool IsContainer(": "bool IsChest(",
                "bool IsTrader(": "bool Shared(",
                "bool Corpse(": "bool PersonalKind(",
                "bool AwaitingDeath(": "} // namespace",
            }[name]
            fn = part(ledger_source, name, end_marker)
            self.assertIn("IsDown()", fn, name)

        apply_fn = part(ledger_source, "bool ApplyPersonal(", "bool AwaitingDeath(")
        self.assertIn("CaptureEquippedSlots(", apply_fn)
        self.assertIn("UnEquipStack(i)", apply_fn)
        self.assertIn("RestoreMatchingSlot(", apply_fn)

        rollback_fn = part(ledger_source, "void RollbackPersonal(", "bool ReadPersonal(")
        self.assertIn("UnEquipStack(i)", rollback_fn)
        self.assertIn("RestoreMatchingSlot(", rollback_fn)

        resolve_fn = part(ledger_source, "bool ResolveStackTemplates(", "void CaptureEquippedSlots(")
        self.assertIn("gESlot_Ammo", resolve_fn)

        reclaim_fn = part(ledger_source, "void ReclaimCompletedObservers(", "void RetirePersonalObservers(")
        self.assertNotIn("activeTeardown", reclaim_fn)
        self.assertIn("destroyedPersonalObservers", reclaim_fn)

        destroy_fn = part(ledger_source, "void ObserverState::OnDestroyInventory()", "bool HasInventory(")
        self.assertIn("active = false;", destroy_fn)
        self.assertIn("++destroyedPersonalObservers;", destroy_fn)

        ensure_fn = part(ledger_source, "bool EnsurePersonal(", "bool RememberPersonal(")
        self.assertIn("if (destroyedPersonalObservers > 0) ReclaimCompletedObservers();", ensure_fn)

        pass_fn = part(ledger_source, "bool PersonalPass(", "bool FlushPersonalObservers(")
        self.assertIn("if (destroyedPersonalObservers > 0) ReclaimCompletedObservers();", pass_fn)

        enter_fn = part(ledger_source, "bool PersonalMutation::Enter(", "bool PersonalMutation::StillAvailable(")
        self.assertNotIn("if (wasPending) return false;", enter_fn)

        self.assertIn("void PrepareCorpseLoot(eCEntity* entity);", containers_header)
        local_tick = part(containers_source, "void LocalInventoryTick()", "gFScript originalLoot")
        self.assertIn("Corpse(focus)", local_tick)
        self.assertIn("PrepareLocalLoot(focus)", local_tick)
        quick = part(containers_source, "GEBool __fastcall QuickTake(", "void HookQuickLoot()")
        self.assertIn("kind == personal::Kind::Corpse && !entity.Inventory.IsGeneratedPlunder()", quick)
        self.assertIn("PrepareLocalLoot(entity)", quick)


if __name__ == "__main__":
    unittest.main()
