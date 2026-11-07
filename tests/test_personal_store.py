# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import struct
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

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
        self.data = Path(path).read_bytes()
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
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required")
        cls.executable = ROOT / "tests" / "personal-store-test.exe"
        try:
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     str(ROOT / "tests" / "personal_store_test.cpp"), "-o", str(cls.executable)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        except BaseException:
            cls.executable.unlink(missing_ok=True)
            raise
        cls.addClassCleanup(cls.executable.unlink, missing_ok=True)
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
        source = (ROOT / "src" / "containers.cpp").read_text(encoding="utf-8")
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
        source = (ROOT / "src" / "trade.cpp").read_text(encoding="utf-8")
        native = source[source.index("GEBool __fastcall NativeTransfer("):source.index("bool __stdcall NativeCanExchange(")]
        self.assertIn("if (!fromPlayer && !toPlayer) return originalTransfer(from, to, type);", native)
        self.assertIn("BeginExchange(from, to, toPlayer)", native)
        install = source[source.index("void HookNative()"):source.index("bool OnResult(")]
        self.assertIn("originalTransfer = reinterpret_cast<TransferFn>(GetProcAddress", install)
        self.assertIn('"?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z"', install)
        self.assertIn("const uintptr_t calls[] = {0x8a123, 0x8a12d}", install)
        self.assertIn("std::memcpy(base + call + 1, &offset", install)

    def test_native_access_guards_and_deferred_capture(self):
        source = (ROOT / "src" / "containers.cpp").read_text(encoding="utf-8")

        def part(start, end):
            return source[source.index(start):source.index(end, source.index(start))]

        apply = part("bool ApplyPersonal(", "bool AwaitingDeath(")
        self.assertLess(apply.index("item->GetPropertySet"), apply.index("DeleteStack"))
        self.assertIn("!PersonalCounted(entity, i)", apply)
        self.assertIn("!entity.Inventory.DeleteStack(i)", apply)
        self.assertIn("inventory->CreateItems", apply)
        self.assertIn("< 0) return false", apply)
        ensure = part("bool EnsurePersonal(", "bool RememberPersonal(")
        self.assertIn("AwaitingDeath(entity", ensure)
        self.assertLess(ensure.index("!ApplyPersonal"), ensure.index("personalPending.erase"))
        capture = part("bool CapturePersonal(", "bool RestorePersonal(")
        self.assertIn("!InstallAccessGuards()", capture)
        self.assertIn("!FlushPersonalObservers()", capture)
        self.assertIn("!PersonalPass(true)", capture)
        self.assertIn("!EnsurePersonal(entity) || !RememberPersonal(entity)", capture)
        self.assertIn("items::AppendPersonal(ground)", capture)
        self.assertIn("!combined.emplace(entry).second", capture)
        self.assertIn("personal::Encode(combined, out)", capture)
        restore = part("bool RestorePersonal(", "void Tick()")
        self.assertLess(restore.index("personal::Decode"), restore.index("personalLedger.swap"))
        self.assertIn("!bytes.empty() && !personal::Decode", restore)
        self.assertIn("items::RestorePersonal(snapshot)", restore)
        self.assertIn("items::TickPersonal()", restore)
        self.assertIn("personal::Kind::GroundTaken", restore)
        self.assertIn("personalPending.insert", restore)
        self.assertIn("PersonalPass(false, &attempted)", restore)
        self.assertIn("guid::Resolve", restore)
        self.assertIn("AwaitingDeath", restore)
        self.assertIn("if (!EnsurePersonal(entity)) ok = false", restore)
        self.assertLess(restore.index("if (personalLedger.empty()) return true"),
                        restore.index("if (!InstallAccessGuards())"))
        reset = part("void OnWorldChange()", "bool InstallAccessGuards()")
        self.assertLess(reset.index("personalLedger.clear()"), reset.index("if (forgetOff)"))
        self.assertIn("items::OnWorldChange()", reset)
        quick = part("GEBool __fastcall QuickTake(", "void HookQuickLoot()")
        self.assertIn("if (wasPending)", quick)
        self.assertIn("EnsurePersonal(entity)", quick)
        self.assertIn("return GEFalse", quick)
        for marker in ("GeneratePersonal(", "BeginPersonalTrade(", "PersonalState(", "Opened("):
            self.assertIn(marker, source)
        observer = part("struct PersonalObserver final", "constexpr size_t MaxPersonalObservers")
        self.assertIn("changes.Changed()", observer)
        self.assertIn("changes.Destroyed()", observer)
        self.assertIn("personalUncertain.insert(key)", observer)
        self.assertNotIn("RememberPersonal", observer)
        self.assertNotIn("ReadPersonal", observer)
        completion = part("class PersonalMutation", "using PersonalGenerateFn")
        self.assertIn("~PersonalMutation() { Finish(); }", completion)
        self.assertIn("!RememberPersonal(entity)", completion)
        self.assertIn("personalUncertain.insert", completion)
        for marker in ("TransferPersonalItems", "TransferPersonalStack", "TransferPersonalAll",
                       "TransferPersonalTransaction", "PerformPersonalTransaction", "TransferPersonalScript"):
            self.assertIn(marker, source)
        transfers = part("GEBool __fastcall TransferPersonalItems(", "GEBool __fastcall BeginPersonalTrade(")
        self.assertEqual(transfers.count("!PersonalPlayerTransfer("), 6)
        self.assertEqual(transfers.count("PersonalMutation mutation;"), 6)
        ready = part("bool PersonalHooksReady()", "bool CapturePersonal(")
        for marker in ("nativeTransferItems", "nativeTransferStack", "nativeTransferAll",
                       "nativeTransferTransaction", "nativeTransaction", "nativeScriptTransfer"):
            self.assertIn(marker, ready)
        tick = part("void Tick()", "void Contents(")
        self.assertIn("items::TickPersonal()", tick)
        self.assertIn("PersonalPass(false)", tick)
        self.assertNotIn("HostTick();", tick)
        self.assertNotIn("ClientTick();", tick)


if __name__ == "__main__":
    unittest.main()
