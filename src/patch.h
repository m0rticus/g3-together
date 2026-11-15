// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define NOMINMAX
#include <Windows.h>
#include <cstddef>

namespace g3t::patch {
struct PatchRestoreResult {
    size_t restored = 0;
    size_t preserved = 0;
    size_t failed = 0;
};
void BeginShutdown();
bool ShuttingDown();
LONG volatile* ShutdownFlagAddress();
void TrackPatch(void* address, void const* original, void const* installed, size_t size);
void TrackPointer(void* slotAddress, void const* originalPtr, void const* installedPtr);
bool PatchPointer(void* slotAddress, void const* replacement, void const** originalOut = nullptr);
PatchRestoreResult RestorePatches();
bool IsPatchedByUs(void const* address, void const* expectedOriginalPrefix = nullptr, size_t prefixSize = 0);
void* JumpTo(unsigned char* target, void* detour, size_t stolen = 6);
bool PatchImport(HMODULE module, char const* funcName, void const* replacement, void const** originalOut);
bool PatchImportFromDll(HMODULE module, char const* dll, char const* funcName,
                        void const* replacement, void const** originalOut);
} // namespace g3t::patch
