// SPDX-License-Identifier: GPL-3.0-or-later
#include "patch.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace g3t::patch {
namespace {
struct Record {
    void* address;
    std::vector<unsigned char> original;
    std::vector<unsigned char> installed;
    void* currentEntry = nullptr;
    bool bytePatch = false;
};

std::mutex& Mutex() { static std::mutex mutex; return mutex; }
std::vector<Record>& Registry() { static std::vector<Record> records; return records; }

void Track(void* address, void const* original, void const* installed, size_t size, bool bytePatch)
{
    if (!address || !original || !installed || !size) return;
    auto* before = static_cast<unsigned char const*>(original);
    auto* after = static_cast<unsigned char const*>(installed);
    Registry().push_back({address, {before, before + size}, {after, after + size}, nullptr, bytePatch});
}

bool WritePointer(void* slot, void const* replacement, void const** originalOut)
{
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    void const* previous = nullptr;
    std::memcpy(&previous, slot, sizeof(previous));
    std::memcpy(slot, &replacement, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    Track(slot, &previous, &replacement, sizeof(void*), false);
    if (originalOut) *originalOut = previous;
    return true;
}

void PutJump(unsigned char* at, void const* destination)
{
    at[0] = 0xe9;
    const int32_t displacement = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(destination) - reinterpret_cast<intptr_t>(at + 5));
    std::memcpy(at + 1, &displacement, sizeof(displacement));
}
}

LONG volatile* ShutdownFlagAddress()
{
    static LONG volatile* const flag = static_cast<LONG volatile*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    return flag;
}

void BeginShutdown()
{
    if (auto* flag = ShutdownFlagAddress()) InterlockedExchange(flag, 1);
}

bool ShuttingDown()
{
    auto* flag = ShutdownFlagAddress();
    return !flag || InterlockedCompareExchange(flag, 0, 0) != 0;
}

void TrackPatch(void* address, void const* original, void const* installed, size_t size)
{
    std::lock_guard<std::mutex> lock(Mutex());
    Track(address, original, installed, size, true);
}

void TrackPointer(void* slotAddress, void const* originalPtr, void const* installedPtr)
{
    std::lock_guard<std::mutex> lock(Mutex());
    Track(slotAddress, &originalPtr, &installedPtr, sizeof(void*), false);
}

bool PatchPointer(void* slotAddress, void const* replacement, void const** originalOut)
{
    std::lock_guard<std::mutex> lock(Mutex());
    return WritePointer(slotAddress, replacement, originalOut);
}

PatchRestoreResult RestorePatches()
{
    std::lock_guard<std::mutex> lock(Mutex());
    PatchRestoreResult result;
    for (auto it = Registry().rbegin(); it != Registry().rend(); ++it) {
        Record& record = *it;
        const size_t size = record.installed.size();
        if (std::memcmp(record.address, record.installed.data(), size)) {
            ++result.preserved;
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect(record.address, size, PAGE_EXECUTE_READWRITE, &old)) {
            ++result.failed;
            continue;
        }
        std::memcpy(record.address, record.original.data(), size);
        DWORD ignored = 0;
        if (!VirtualProtect(record.address, size, old, &ignored)) ++result.failed;
        else ++result.restored;
        FlushInstructionCache(GetCurrentProcess(), record.address, size);
    }
    Registry().clear();
    Logf("ai: patches removed restored=%u preserved=%u failed=%u",
         static_cast<unsigned>(result.restored), static_cast<unsigned>(result.preserved), static_cast<unsigned>(result.failed));
    return result;
}

bool IsPatchedByUs(void const* address, void const* expectedOriginalPrefix, size_t prefixSize)
{
    std::lock_guard<std::mutex> lock(Mutex());
    for (auto const& record : Registry()) {
        if (!record.bytePatch || record.address != address ||
            std::memcmp(address, record.installed.data(), record.installed.size())) continue;
        return !expectedOriginalPrefix || !prefixSize ||
            (record.original.size() >= prefixSize && !std::memcmp(record.original.data(), expectedOriginalPrefix, prefixSize));
    }
    return false;
}

void* JumpTo(unsigned char* target, void* detour, size_t stolen)
{
    std::lock_guard<std::mutex> lock(Mutex());
    if (!target || !detour || stolen < 5 || stolen > 27 || ShuttingDown()) return nullptr;
    Record* existing = nullptr;
    for (auto& record : Registry()) {
        if (record.address != target) continue;
        if (!record.currentEntry || record.installed.size() != stolen ||
            std::memcmp(target, record.installed.data(), stolen)) return nullptr;
        existing = &record;
        break;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    trampoline[0] = 0x83; trampoline[1] = 0x3d;
    const uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ShutdownFlagAddress()));
    std::memcpy(trampoline + 2, &flag, sizeof(flag));
    trampoline[6] = 0; trampoline[7] = 0x75; trampoline[8] = 5;
    PutJump(trampoline + 9, detour);
    if (existing) {
        PutJump(trampoline + 14, existing->currentEntry);
    } else {
        std::memcpy(trampoline + 14, target, stolen);
        PutJump(trampoline + 14 + stolen, target + stolen);
    }
    DWORD old = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    unsigned char original[27];
    std::memcpy(original, target, stolen);
    PutJump(target, trampoline);
    for (size_t i = 5; i < stolen; ++i) target[i] = 0x90;
    if (existing) {
        existing->installed.assign(target, target + stolen);
        existing->currentEntry = trampoline;
    } else {
        Track(target, original, target, stolen, true);
        Registry().back().currentEntry = trampoline;
    }
    DWORD ignored = 0;
    VirtualProtect(target, stolen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return trampoline + 14;
}

bool PatchImportFromDll(HMODULE module, char const* dll, char const* funcName, void const* replacement, void const** originalOut)
{
    std::lock_guard<std::mutex> lock(Mutex());
    if (!module || !funcName) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (dll && _stricmp(reinterpret_cast<char const*>(base + imp->Name), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<char const*>(byName->Name), funcName))
                return WritePointer(&slots->u1.Function, replacement, originalOut);
        }
    }
    return false;
}

bool PatchImport(HMODULE module, char const* funcName, void const* replacement, void const** originalOut)
{
    return PatchImportFromDll(module, nullptr, funcName, replacement, originalOut);
}
} // namespace g3t::patch
