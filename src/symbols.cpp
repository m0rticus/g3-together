// SPDX-License-Identifier: GPL-3.0-or-later
#include "symbols.h"
#include "log.h"
#include <cstring>

namespace g3t::symbols {
bool GameDir(char (&out)[MAX_PATH])
{
    out[0] = '\0';
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&GameDir), &self)) return false;
    const DWORD length = GetModuleFileNameA(self, out, MAX_PATH);
    if (!length || length >= MAX_PATH) { out[0] = '\0'; return false; }
    for (int part = 0; part != 2; ++part) {
        char* slash = std::strrchr(out, '\\');
        if (!slash) { out[0] = '\0'; return false; }
        *slash = '\0';
    }
    return true;
}

bool ReadMemory(uintptr_t address, void* bytes, size_t size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), bytes, size, &read) &&
           read == size;
}

bool DescribeImageAddress(uintptr_t address, char* out, size_t size)
{
    MEMORY_BASIC_INFORMATION memory{};
    char path[MAX_PATH]{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        !GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), path, MAX_PATH)) return false;
    char const* module = std::strrchr(path, '\\');
    Format(out, size, "%s+%#x", module ? module + 1 : path,
           static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(memory.AllocationBase)));
    return true;
}

void CopyRemoteStackWords(uintptr_t esp, uintptr_t (&words)[256], unsigned& count)
{
    count = 0;
    while (count < 256) {
        const uintptr_t address = esp + count * sizeof(uintptr_t);
        if (count + 16 <= 256 && ReadMemory(address, words + count, 16 * sizeof(uintptr_t))) {
            count += 16;
        } else if (ReadMemory(address, words + count, sizeof(uintptr_t))) {
            ++count;
        } else {
            break;
        }
    }
}

bool SampleThreadStack(DWORD id, ThreadSample& sample, bool captureEbp)
{
    sample = ThreadSample{};
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
    if (!thread) { sample.openError = GetLastError(); return false; }
    const DWORD previous = SuspendThread(thread);
    if (previous == static_cast<DWORD>(-1)) {
        sample.suspendError = GetLastError();
        CloseHandle(thread);
        return false;
    }
    sample.suspended = true;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    sample.contextOk = GetThreadContext(thread, &context) != FALSE;
    if (sample.contextOk) {
        sample.eip = context.Eip;
        sample.esp = context.Esp;
        sample.ebp = context.Ebp;
        CopyRemoteStackWords(sample.esp, sample.words, sample.count);
        if (captureEbp) {
            uintptr_t ebp = sample.ebp;
            for (unsigned i = 0; i < 32 && ebp; ++i) {
                uintptr_t pair[2] = {};
                if (!ReadMemory(ebp, pair, sizeof(pair)) || !pair[1]) break;
                sample.ebpFrames[sample.ebpCount++] = pair[1];
                if (!pair[0] || pair[0] <= ebp) break;
                ebp = pair[0];
            }
        }
        sample.ok = true;
    } else sample.contextError = GetLastError();
    sample.resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
    if (!sample.resumed) sample.resumeError = GetLastError();
    CloseHandle(thread);
    return sample.ok && sample.resumed;
}
}
