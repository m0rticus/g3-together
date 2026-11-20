// SPDX-License-Identifier: GPL-3.0-or-later
#include "patch.h"
#include "log.h"

#include <intrin.h>
#include <algorithm>
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

bool ReadableBytes(void const* address, size_t size)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!start || size > UINTPTR_MAX - start) return false;
    const uintptr_t end = start + size;
    for (uintptr_t at = start; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void const*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        const DWORD protect = info.Protect & 0xff;
        if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
             protect != PAGE_EXECUTE_READ && protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY))
            return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}

void AtomicWriteCodeBytes(unsigned char* target, unsigned char const* bytes, size_t size)
{
    if (size <= 8) {
        long long desired = 0;
        long long expected = 0;
        std::memcpy(&expected, target, 8);
        std::memcpy(&desired, &expected, 8);
        std::memcpy(&desired, bytes, size);
        _InterlockedCompareExchange64(reinterpret_cast<long long volatile*>(target), desired, expected);
        return;
    }
    std::memcpy(target, bytes, size);
}

void Track(void* address, void const* original, void const* installed, size_t size, bool bytePatch)
{
    if (!address || !original || !installed || !size) return;
    auto* before = static_cast<unsigned char const*>(original);
    auto* after = static_cast<unsigned char const*>(installed);
    Registry().push_back({address, {before, before + size}, {after, after + size}, nullptr, bytePatch});
}

bool WritePointer(void* slot, void const* replacement, void const** originalOut)
{
    if (!slot || ShuttingDown() || !ReadableBytes(slot, sizeof(void*))) return false;
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

void PutJump(unsigned char* at, void const* destination, void const* from = nullptr)
{
    at[0] = 0xe9;
    const intptr_t origin = reinterpret_cast<intptr_t>(from ? from : at) + 5;
    const int32_t displacement = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(destination) - origin);
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
        const size_t writeSize = record.bytePatch && size <= 8 ? 8 : size;
        if (!ReadableBytes(record.address, writeSize) || std::memcmp(record.address, record.installed.data(), size)) {
            ++result.preserved;
            continue;
        }
        DWORD old = 0;
        const DWORD prot = record.bytePatch ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        if (!VirtualProtect(record.address, writeSize, prot, &old)) {
            ++result.failed;
            continue;
        }
        if (record.bytePatch)
            AtomicWriteCodeBytes(static_cast<unsigned char*>(record.address), record.original.data(), size);
        else
            std::memcpy(record.address, record.original.data(), size);
        DWORD ignored = 0;
        if (!VirtualProtect(record.address, writeSize, old, &ignored)) ++result.failed;
        else ++result.restored;
        FlushInstructionCache(GetCurrentProcess(), record.address, writeSize);
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
            !ReadableBytes(address, record.installed.size()) ||
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
    const size_t writeSize = (std::max<size_t>)(stolen, 8);
    if (!ReadableBytes(target, writeSize)) return nullptr;
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
    std::memset(trampoline, 0xcc, 64);
    trampoline[0] = 0x9c;
    trampoline[1] = 0x83;
    trampoline[2] = 0x3d;
    const uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ShutdownFlagAddress()));
    std::memcpy(trampoline + 3, &flag, sizeof(flag));
    trampoline[7] = 0x00;
    trampoline[8] = 0x74;
    trampoline[9] = 0x06;
    trampoline[10] = 0x9d;
    PutJump(trampoline + 11, trampoline + 24);
    trampoline[16] = 0x9d;
    PutJump(trampoline + 17, detour);
    if (existing) {
        PutJump(trampoline + 24, existing->currentEntry);
    } else {
        std::memcpy(trampoline + 24, target, stolen);
        PutJump(trampoline + 24 + stolen, target + stolen);
    }
    DWORD old = 0;
    if (!VirtualProtect(target, writeSize, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    unsigned char original[27];
    std::memcpy(original, target, stolen);
    unsigned char patched[27];
    std::memcpy(patched, target, stolen);
    PutJump(patched, trampoline, target);
    for (size_t i = 5; i < stolen; ++i) patched[i] = 0x90;
    AtomicWriteCodeBytes(target, patched, stolen);
    if (existing) {
        existing->installed.assign(target, target + stolen);
        existing->currentEntry = trampoline;
    } else {
        Track(target, original, target, stolen, true);
        Registry().back().currentEntry = trampoline;
    }
    DWORD ignored = 0;
    VirtualProtect(target, writeSize, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    FlushInstructionCache(GetCurrentProcess(), target, writeSize);
    return trampoline + 24;
}

bool PatchImportFromDll(HMODULE module, char const* dll, char const* funcName, void const* replacement, void const** originalOut)
{
    std::lock_guard<std::mutex> lock(Mutex());
    if (!module || !funcName || ShuttingDown()) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!ReadableBytes(base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096 ||
        !ReadableBytes(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize <= sizeof(WORD)) return false;
    auto const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress >= imageSize ||
        sizeof(IMAGE_IMPORT_DESCRIPTOR) > imageSize - dir.VirtualAddress)
        return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         reinterpret_cast<unsigned char*>(imp + 1) <= base + imageSize && imp->Name; ++imp) {
        if (imp->Name >= imageSize) continue;
        if (dll && _stricmp(reinterpret_cast<char const*>(base + imp->Name), dll)) continue;
        if (imp->OriginalFirstThunk == 0 && imp->TimeDateStamp != 0) continue;
        const DWORD nameRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        if (!nameRva || !imp->FirstThunk || nameRva >= imageSize || imp->FirstThunk >= imageSize) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + nameRva);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; reinterpret_cast<unsigned char*>(names + 1) <= base + imageSize &&
               reinterpret_cast<unsigned char*>(slots + 1) <= base + imageSize &&
               names->u1.AddressOfData;
             ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const DWORD byNameRva = static_cast<DWORD>(names->u1.AddressOfData);
            if (byNameRva == 0 || byNameRva >= imageSize - 2) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + byNameRva);
            char const* importName = reinterpret_cast<char const*>(byName->Name);
            const size_t maxNameBytes = imageSize - (static_cast<size_t>(byNameRva) + 2);
            if (::strnlen(importName, maxNameBytes) == maxNameBytes) continue;
            if (!std::strcmp(importName, funcName))
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
