import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SAVING_NATIVE_TARGET = _testbuild.register_target(
    "saving-native-test",
    [_testbuild.unit(
        "tests/saving_native_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

JUMPTO_OWNED_HOOK_SOURCE = r"""
#define NOMINMAX
#include <Windows.h>
#include "log.h"
#include "patch.h"
#include "saving_native.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 4, "Win32 32-bit x86 target required");

namespace g3t {
void Log(char const*) {}
void Logf(char const*, ...) {}
} // namespace g3t

namespace {

void Require(bool ok, char const* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

bool Readable(void const* address, size_t size)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!start || size > UINTPTR_MAX - start) return false;
    for (uintptr_t at = start; at < start + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void const*>(at), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.Protect == PAGE_EXECUTE) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= at) return false;
        at = end;
    }
    return true;
}

bool Executable(void const* address)
{
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

bool OwnedHook(unsigned char* entry, void* original, void* detour, size_t stolen,
               char const* pattern, char const* prefix = nullptr)
{
    using namespace g3t::saving::native;
    uint32_t guard = 0, target = 0;
    const size_t length = Decode(pattern).size();
    return Readable(entry, length) &&
        PupEntryTarget(entry, stolen, reinterpret_cast<uintptr_t>(entry), guard) &&
        guard == reinterpret_cast<uintptr_t>(original) - 24 &&
        Readable(reinterpret_cast<void const*>(guard), 24 + stolen + 5) &&
        Executable(reinterpret_cast<void const*>(guard)) &&
        Matches(reinterpret_cast<uint8_t const*>(guard), 24,
            "9c833d????????0074069de9????????9de9????????") &&
        BranchTarget(reinterpret_cast<uint8_t const*>(guard + 11), 5, guard + 11, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(original) &&
        BranchTarget(reinterpret_cast<uint8_t const*>(guard + 17), 5, guard + 17, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(detour) &&
        BranchTarget(static_cast<uint8_t const*>(original) + stolen, 5,
            reinterpret_cast<uintptr_t>(original) + stolen, 0xe9, target) &&
        target == reinterpret_cast<uintptr_t>(entry + stolen) &&
        (stolen == Stolen || entry[6] == 0x90) &&
        Matches(static_cast<uint8_t const*>(original), stolen,
            prefix ? prefix : stolen == Stolen ? "558bec83e4f8" : "8b44240483ec68") &&
        Matches(entry + stolen, length - stolen, pattern + stolen * 2);
}

int __cdecl DummyDetour0(int x) { return x + 11; }
int __cdecl DummyDetour1(int x) { return x + 22; }
int __cdecl DummyDetour2(int x) { return x + 33; }
int __cdecl DummyDetour3(int x) { return x + 44; }
int __cdecl DummyDetour4(int x) { return x + 55; }

void MaterializePattern(unsigned char* dst, char const* pattern)
{
    const auto decoded = g3t::saving::native::Decode(pattern);
    Require(!decoded.empty(), "decoded pattern non-empty");
    for (size_t i = 0; i < decoded.size(); ++i)
        dst[i] = decoded[i] < 0 ? static_cast<unsigned char>(0x2a + (i & 7)) : static_cast<unsigned char>(decoded[i]);
}

std::string HexEncode(unsigned char const* bytes, size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0f]);
    }
    return out;
}

void TestPersistOwnedHookSites()
{
    using namespace g3t::saving::native;
    struct HookSpec {
        char const* bytes;
        size_t stolen;
        char const* prefix;
        void* detour;
    };
    const HookSpec specs[5] = {
        {Sites[0].bytes, Stolen, nullptr, reinterpret_cast<void*>(&DummyDetour0)},
        {Sites[1].bytes, Stolen, nullptr, reinterpret_cast<void*>(&DummyDetour1)},
        {Sites[2].bytes, Stolen, nullptr, reinterpret_cast<void*>(&DummyDetour2)},
        {DeleteSite.bytes, Stolen, "83ec10535657", reinterpret_cast<void*>(&DummyDetour3)},
        {UiSites[0].bytes, GuiStolen, nullptr, reinterpret_cast<void*>(&DummyDetour4)},
    };

    auto* page = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Require(page != nullptr, "VirtualAlloc persist fixture page");
    std::memset(page, 0xcc, 4096);
    for (size_t i = 0; i < 5; ++i)
        MaterializePattern(page + i * 256, specs[i].bytes);

    DWORD oldProtect = 0;
    Require(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &oldProtect) != 0, "protect RX");

    const uintptr_t expectedFlag = reinterpret_cast<uintptr_t>(g3t::patch::ShutdownFlagAddress());
    Require(expectedFlag != 0, "ShutdownFlagAddress non-null");

    for (size_t i = 0; i < 5; ++i) {
        unsigned char* entry = page + i * 256;
        void* original = g3t::patch::JumpTo(entry, specs[i].detour, specs[i].stolen);
        Require(original != nullptr, "JumpTo succeeded on persist site");

        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(entry, &info, sizeof(info)) != 0 && info.Protect == PAGE_EXECUTE_READ,
                "JumpTo restored target RX protection");

        Require(OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook accepted 24-byte JumpTo trampoline");

        auto* guard = static_cast<unsigned char*>(original) - 24;
        Require(Word(guard, 3) == expectedFlag, "guard cmp references ShutdownFlagAddress");
        Require(guard[0] == 0x9c && guard[7] == 0x00 && guard[8] == 0x74 && guard[9] == 0x06 &&
                guard[10] == 0x9d && guard[16] == 0x9d, "exact pushfd/je/popfd guard opcodes");

        // Regression check: shortened 14-byte header (original - 14) must fail OwnedHook.
        Require(!OwnedHook(entry, static_cast<unsigned char*>(original) - 10,
                           specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook rejects 14-byte header offset");

        guard[0] ^= 1;
        Require(!OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook rejects missing pushfd");
        guard[0] ^= 1;

        guard[12] ^= 1;
        Require(!OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook rejects wrong guard+11 jump target");
        guard[12] ^= 1;

        guard[18] ^= 1;
        Require(!OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook rejects wrong guard+17 detour target");
        guard[18] ^= 1;

        if (specs[i].stolen == GuiStolen) {
            Require(VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &oldProtect) != 0, "RWX for nop check");
            entry[6] = 0x91;
            Require(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &oldProtect) != 0, "RX for nop check");
            Require(!OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                    "OwnedHook rejects non-nop 7th byte on GuiStolen");
            Require(VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &oldProtect) != 0, "RWX restore nop");
            entry[6] = 0x90;
            Require(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &oldProtect) != 0, "RX restore nop");
        }
        Require(OwnedHook(entry, original, specs[i].detour, specs[i].stolen, specs[i].bytes, specs[i].prefix),
                "OwnedHook still accepts restored trampoline");
    }

    const auto restored = g3t::patch::RestorePatches();
    Require(restored.restored == 5 && restored.preserved == 0 && restored.failed == 0,
            "RestorePatches restored all 5 persist sites");
    for (size_t i = 0; i < 5; ++i) {
        const size_t len = Decode(specs[i].bytes).size();
        Require(Matches(page + i * 256, len, specs[i].bytes), "restored bytes match original signature");
    }
    VirtualFree(page, 0, MEM_RELEASE);
}

using TargetFn = int (__cdecl*)(int);
using InvokeFn = int (__cdecl*)(void* fn, uint32_t setMask, int arg);
constexpr uint32_t kArithFlagsMask = 0x08d5u; // CF | PF | AF | ZF | SF | OF

uint32_t g_bodyFlags = 0;
uint32_t g_bodyCalls = 0;
uint32_t g_detour1Flags = 0;
uint32_t g_detour2Flags = 0;
uint32_t g_expectedChainFlags = 0;
std::vector<int> g_order;
TargetFn g_orig1 = nullptr;
TargetFn g_orig2 = nullptr;
InvokeFn g_invoke = nullptr;

int __cdecl RealDetour1(int x)
{
    g_order.push_back(1);
    return g_invoke(reinterpret_cast<void*>(g_orig1), g_expectedChainFlags, x) + 10;
}

int __cdecl RealDetour2(int x)
{
    g_order.push_back(2);
    return g_invoke(reinterpret_cast<void*>(g_orig2), g_expectedChainFlags, x) * 2;
}

void EmitDetourProbe(unsigned char* at, uint32_t* flagsSlot, void* realDetour)
{
    at[0] = 0x9c; // pushfd
    at[1] = 0x58; // pop eax
    at[2] = 0xa3; // mov [flagsSlot], eax
    const uint32_t slotAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(flagsSlot));
    std::memcpy(at + 3, &slotAddr, 4);
    at[7] = 0xe9; // jmp realDetour
    const int32_t rel = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(realDetour) - reinterpret_cast<intptr_t>(at + 12));
    std::memcpy(at + 8, &rel, 4);
}

void EmitInvokeStub(unsigned char* at)
{
    const unsigned char code[] = {
        0x55,                               // push ebp
        0x8b, 0xec,                         // mov ebp, esp
        0xff, 0x75, 0x10,                   // push dword ptr [ebp + 16]
        0x9c,                               // pushfd
        0x58,                               // pop eax
        0x25, 0x2a, 0xf7, 0xff, 0xff,       // and eax, ~0x08d5u
        0x0b, 0x45, 0x0c,                   // or eax, dword ptr [ebp + 12]
        0x50,                               // push eax
        0x9d,                               // popfd
        0xff, 0x55, 0x08,                   // call dword ptr [ebp + 8]
        0x83, 0xc4, 0x04,                   // add esp, 4
        0x5d,                               // pop ebp
        0xc3                                // ret
    };
    std::memcpy(at, code, sizeof(code));
}

void EmitTargetFunction(unsigned char* at, uint32_t* bodyFlagsSlot, uint32_t* bodyCallsSlot)
{
    const unsigned char prologue[6] = {0x55, 0x8b, 0xec, 0x51, 0x51, 0x90};
    std::memcpy(at, prologue, 6);
    at[6] = 0x9c; // pushfd
    at[7] = 0x58; // pop eax
    at[8] = 0xa3; // mov [bodyFlagsSlot], eax
    const uint32_t flagsAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bodyFlagsSlot));
    std::memcpy(at + 9, &flagsAddr, 4);
    at[13] = 0xff;
    at[14] = 0x05; // inc dword ptr [bodyCallsSlot]
    const uint32_t callsAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bodyCallsSlot));
    std::memcpy(at + 15, &callsAddr, 4);
    const unsigned char tail[] = {
        0x8b, 0x45, 0x08, // mov eax, dword ptr [ebp + 8]
        0x83, 0xc0, 0x64, // add eax, 100
        0x8b, 0xe5,       // mov esp, ebp
        0x5d,             // pop ebp
        0xc3              // ret
    };
    std::memcpy(at + 19, tail, sizeof(tail));
}

void TestExecutionFlagsChainingAndShutdown()
{
    using namespace g3t::saving::native;
    auto* stubs = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    auto* targetPage = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Require(stubs != nullptr && targetPage != nullptr, "VirtualAlloc execution pages");
    std::memset(stubs, 0xcc, 4096);
    std::memset(targetPage, 0xcc, 4096);

    EmitInvokeStub(stubs);
    g_invoke = reinterpret_cast<InvokeFn>(stubs);
    unsigned char* detourProbe1 = stubs + 64;
    unsigned char* detourProbe2 = stubs + 128;
    EmitDetourProbe(detourProbe1, &g_detour1Flags, reinterpret_cast<void*>(&RealDetour1));
    EmitDetourProbe(detourProbe2, &g_detour2Flags, reinterpret_cast<void*>(&RealDetour2));
    FlushInstructionCache(GetCurrentProcess(), stubs, 256);

    unsigned char* target = targetPage;
    EmitTargetFunction(target, &g_bodyFlags, &g_bodyCalls);
    const unsigned char expectedPrologue[6] = {0x55, 0x8b, 0xec, 0x51, 0x51, 0x90};
    const std::string targetPattern = HexEncode(target, 29);
    DWORD oldProtect = 0;
    Require(VirtualProtect(targetPage, 4096, PAGE_EXECUTE_READ, &oldProtect) != 0, "protect target RX");
    FlushInstructionCache(GetCurrentProcess(), targetPage, 64);

    const uint32_t flagMasks[] = {0x08d5u, 0x0000u, 0x0841u, 0x0094u};
    for (uint32_t mask : flagMasks) {
        g_bodyFlags = 0xffffffffu;
        Require(g_invoke(target, mask, 7) == 107, "unhooked target result");
        Require((g_bodyFlags & kArithFlagsMask) == mask, "unhooked EFLAGS baseline");
    }

    // Install first hook and check OwnedHook + EFLAGS preservation.
    g_orig1 = reinterpret_cast<TargetFn>(g3t::patch::JumpTo(target, detourProbe1, 6));
    Require(g_orig1 != nullptr, "first JumpTo succeeded");
    Require(OwnedHook(target, reinterpret_cast<void*>(g_orig1), detourProbe1, 6,
                      targetPattern.c_str(), "558bec515190"),
            "OwnedHook accepts live hooked target");
    for (uint32_t mask : flagMasks) {
        g_expectedChainFlags = mask;
        g_detour1Flags = 0xffffffffu;
        g_bodyFlags = 0xffffffffu;
        g_order.clear();
        Require(g_invoke(target, mask, 7) == 117, "single-hook return value");
        Require(g_order.size() == 1 && g_order[0] == 1, "single-hook order");
        Require((g_detour1Flags & kArithFlagsMask) == mask, "single-hook preserved EFLAGS into detour");
        Require((g_bodyFlags & kArithFlagsMask) == mask, "single-hook preserved EFLAGS into original");
    }

    // Chain second hook on the same target (existing->currentEntry at trampoline + 24).
    Require(g3t::patch::JumpTo(target, detourProbe2, 7) == nullptr, "mismatched stolen size rejected");
    g_orig2 = reinterpret_cast<TargetFn>(g3t::patch::JumpTo(target, detourProbe2, 6));
    Require(g_orig2 != nullptr && g_orig2 != g_orig1, "chained JumpTo succeeded");

    auto const* guard1 = reinterpret_cast<uint8_t const*>(g_orig1) - 24;
    auto const* guard2 = reinterpret_cast<uint8_t const*>(g_orig2) - 24;
    uint32_t branchTarget = 0;
    Require(PupEntryTarget(target, 6, reinterpret_cast<uintptr_t>(target), branchTarget) &&
            branchTarget == reinterpret_cast<uintptr_t>(guard2),
            "target jumps to chained guard2");
    Require(Matches(guard2, 24, "9c833d????????0074069de9????????9de9????????"),
            "chained guard2 has exact 24-byte guard header");
    Require(BranchTarget(guard2 + 11, 5, reinterpret_cast<uintptr_t>(guard2 + 11), 0xe9, branchTarget) &&
            branchTarget == reinterpret_cast<uintptr_t>(g_orig2),
            "chained guard2 + 11 jumps to trampoline2 + 24");
    Require(BranchTarget(guard2 + 17, 5, reinterpret_cast<uintptr_t>(guard2 + 17), 0xe9, branchTarget) &&
            branchTarget == reinterpret_cast<uintptr_t>(detourProbe2),
            "chained guard2 + 17 jumps to detour2");
    Require(BranchTarget(reinterpret_cast<uint8_t const*>(g_orig2), 5,
                         reinterpret_cast<uintptr_t>(g_orig2), 0xe9, branchTarget) &&
            branchTarget == reinterpret_cast<uintptr_t>(guard1),
            "trampoline2 + 24 chains to existing->currentEntry (guard1)");
    Require(g3t::patch::IsPatchedByUs(target, expectedPrologue, 6),
            "IsPatchedByUs preserves original unpatched prologue across hook chain");

    for (uint32_t mask : flagMasks) {
        g_expectedChainFlags = mask;
        g_detour2Flags = 0xffffffffu;
        g_detour1Flags = 0xffffffffu;
        g_bodyFlags = 0xffffffffu;
        g_order.clear();
        Require(g_invoke(target, mask, 7) == 234, "chained-hook return value");
        Require(g_order.size() == 2 && g_order[0] == 2 && g_order[1] == 1, "chained-hook order 2 -> 1");
        Require((g_detour2Flags & kArithFlagsMask) == mask, "chained guard2 preserved EFLAGS");
        Require((g_detour1Flags & kArithFlagsMask) == mask, "chained guard1 preserved EFLAGS");
        Require((g_bodyFlags & kArithFlagsMask) == mask, "chained original preserved EFLAGS");
    }

    // RestorePatches unrolls chained target cleanly back to original prologue.
    auto restored = g3t::patch::RestorePatches();
    Require(restored.restored == 1 && restored.preserved == 0 && restored.failed == 0,
            "RestorePatches restored chained target");
    Require(std::memcmp(target, expectedPrologue, 6) == 0, "chained target bytes restored");
    g_order.clear();
    Require(g_invoke(target, 0x08d5u, 7) == 107 && g_order.empty(), "restored target runs unhooked");

    // Re-install chain, trigger BeginShutdown(), and verify guard+11 bypass + EFLAGS preservation.
    g_orig1 = reinterpret_cast<TargetFn>(g3t::patch::JumpTo(target, detourProbe1, 6));
    g_orig2 = reinterpret_cast<TargetFn>(g3t::patch::JumpTo(target, detourProbe2, 6));
    Require(g_orig1 != nullptr && g_orig2 != nullptr, "re-installed chain before shutdown");

    g3t::patch::BeginShutdown();
    Require(g3t::patch::ShuttingDown(), "ShuttingDown active after BeginShutdown");
    Require(g3t::patch::JumpTo(target, detourProbe1, 6) == nullptr, "JumpTo refused during shutdown");

    for (uint32_t mask : flagMasks) {
        g_order.clear();
        g_bodyFlags = 0xffffffffu;
        const uint32_t beforeCalls = g_bodyCalls;
        Require(g_invoke(target, mask, 7) == 107, "shutdown bypasses chained detours via guard+11");
        Require(g_order.empty(), "no detours entered during shutdown");
        Require(g_bodyCalls == beforeCalls + 1, "original body executed once during shutdown");
        Require((g_bodyFlags & kArithFlagsMask) == mask, "shutdown guard+10 popfd preserved EFLAGS");
    }

    restored = g3t::patch::RestorePatches();
    Require(restored.restored == 1 && restored.preserved == 0 && restored.failed == 0,
            "final RestorePatches after shutdown");

    VirtualFree(targetPage, 0, MEM_RELEASE);
    VirtualFree(stubs, 0, MEM_RELEASE);
}

} // namespace

int main()
{
    TestPersistOwnedHookSites();
    TestExecutionFlagsChainingAndShutdown();
    std::printf("jumpto-owned-hook ok\n");
    return 0;
}
"""

_WIN32_TARGET = _testbuild.register_target if os.name == "nt" else _testbuild.NativeTarget
JUMPTO_OWNED_HOOK_TARGET = _WIN32_TARGET(
    "saving-jumpto-owned-hook-test",
    [
        _testbuild.inline_unit(
            "saving_jumpto_owned_hook_test.cpp",
            JUMPTO_OWNED_HOOK_SOURCE,
            std="c++17",
            warnings="strict",
            includes=("src",),
        ),
        _testbuild.unit(
            "src/patch.cpp",
            std="c++17",
            warnings="strict",
            includes=("src",),
        ),
    ],
    toolchain="win32_cl",
)


class SavingNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = SAVING_NATIVE_TARGET.executable()

    def test_paired_save_recognizer_and_unique_native_patterns(self):
        subprocess.run([str(self.executable)], check=True, capture_output=True, text=True)

    def test_configured_real_dll_exports_patterns_and_aslr(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_GAME_DLL to the real CP 1.75 Game.dll for read-only native checks")
        args = [str(self.executable), "--game", path]
        pup = os.environ.get("G3T_PUP_DLL")
        if pup and Path(pup).is_file():
            args.extend(["--pup", pup])
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_configured_pup_dll_receiver_owner_operands_and_relocations(self):
        path = os.environ.get("G3T_PUP_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_PUP_DLL to the read-only PUP companion paru_s2.dll")
        fixture = Path(path)
        digest = _testbuild.cached_sha256(fixture)
        result = subprocess.run([str(self.executable), "--pup", path], check=True,
                                capture_output=True, text=True)
        self.assertIn(result.stdout.strip(), {
            f"pup digest={digest} verified=structural",
            f"pup digest={digest} verified=pinned",
        })

    def test_pup_identity_is_informational(self):
        source = "\n".join(
            _testbuild.src_text(name)
            for name in ("persist.cpp", "persist_hooks.cpp", "persist_storage.cpp", "persist_client.cpp")
        )
        self.assertNotRegex(source, r'if\s*\(\s*!\s*(?:KnownPup|LogPupIdentity)\s*\(')
        self.assertNotRegex(source, r'PupRefused\s*\(\s*"companion-identity"')
        self.assertIn("persist: PUP identity digest=", source)
        self.assertIn("verified=", source)
        for offset in ("0x677a0", "0x97578", "0x99510"):
            self.assertNotIn(offset, source.lower())

    def test_jumpto_trampoline_matches_owned_hook_24_byte_guard_and_chaining(self):
        hooks_src = _testbuild.src_text("persist_hooks.cpp")
        patch_src = _testbuild.src_text("patch.cpp")
        self.assertIn("guard == reinterpret_cast<uintptr_t>(original) - 24", hooks_src)
        self.assertIn('"9c833d????????0074069de9????????9de9????????"', hooks_src)
        self.assertIn("return trampoline + 24;", patch_src)
        if os.name != "nt":
            self.skipTest("Win32 x86 JumpTo trampoline execution test requires Windows")
        result = JUMPTO_OWNED_HOOK_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("jumpto-owned-hook ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
