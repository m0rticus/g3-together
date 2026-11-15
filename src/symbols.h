// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::symbols {
bool GameDir(char (&out)[MAX_PATH]);
inline void EnsureGameDir(char (&out)[MAX_PATH])
{
    if (*out) return;
    if (!GameDir(out) && !GetCurrentDirectoryA(MAX_PATH, out)) out[0] = '\0';
    const size_t length = std::strlen(out);
    if (length && out[length - 1] != '\\') strcat_s(out, "\\");
}
bool ReadMemory(uintptr_t address, void* bytes, size_t size);
bool DescribeImageAddress(uintptr_t address, char* out, size_t size);
void CopyRemoteStackWords(uintptr_t esp, uintptr_t (&words)[256], unsigned& count);
struct ThreadSample {
    uintptr_t eip = 0, esp = 0, ebp = 0;
    uintptr_t words[256] = {};
    unsigned count = 0;
    bool ok = false, suspended = false, contextOk = false, resumed = true;
    DWORD openError = 0, suspendError = 0, contextError = 0, resumeError = 0;
};
bool SampleThreadStack(DWORD id, ThreadSample& sample);
}
