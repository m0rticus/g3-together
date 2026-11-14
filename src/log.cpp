// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"
#include "symbols.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace g3t {
namespace {
char path[MAX_PATH] = {};
bool reportedFileError = false;
std::mutex logMutex;
std::atomic<DWORD> logOwner{0};

void EmergencyLog(char const* text)
{
    if (!*path) return;
    char emergency[320];
    const int length = _snprintf_s(emergency, sizeof(emergency), _TRUNCATE, "%s\r\n", text);
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, emergency, static_cast<DWORD>(length >= 0 ? length : std::strlen(emergency)), &written, nullptr);
    CloseHandle(file);
}
}

void OpenLog(const char* name)
{
    if (!symbols::GameDir(path)) {
        *path = '\0';
        return;
    }
    if (strcat_s(path, "\\") || strcat_s(path, name)) *path = '\0';
    if (*path) Logf("SESSION START pid=%lu build=%s %s", GetCurrentProcessId(), __DATE__, __TIME__);
}

void Log(const char* text)
{
    if (!text) text = "";
    const DWORD tid = GetCurrentThreadId();
    if (logOwner.load(std::memory_order_relaxed) == tid) {
        EmergencyLog(text);
        return;
    }
    std::unique_lock<std::mutex> lock(logMutex, std::try_to_lock);
    const DWORD start = GetTickCount();
    while (!lock.owns_lock() && GetTickCount() - start < 50) {
        SwitchToThread();
        lock.try_lock();
    }
    if (!lock.owns_lock()) {
        EmergencyLog(text);
        return;
    }
    logOwner.store(tid, std::memory_order_relaxed);
    struct OwnerGuard {
        ~OwnerGuard() { logOwner.store(0, std::memory_order_relaxed); }
    } ownerGuard;
    if (!*path) return;
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (!reportedFileError) {
            reportedFileError = true;
            char warning[512];
            Format(warning, "G3 Together: cannot append log %s (Windows error %lu)\n", path, GetLastError());
            OutputDebugStringA(warning);
        }
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    // A long text is cut off: sprintf_s and vsprintf_s end the game when the buffer is too small (stage E2 22.10.: a
    // trader's contents in one container line closed the client with 0xC0000409, no line written).
    thread_local char line[16448];
    int length = Format(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", now.wYear, now.wMonth, now.wDay,
                        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    if (length > 0) {
        _snprintf_s(line + length, sizeof(line) - length - 2, _TRUNCATE, "%s", text);
        length = static_cast<int>(std::strlen(line));
        line[length++] = '\r';
        line[length++] = '\n';
    }
    DWORD written = 0;
    if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

void Logf(const char* format, ...)
{
    thread_local char text[16384];
    va_list args;
    va_start(args, format);
    // Truncate: vsprintf_s ends the process when the text does not fit (a trader's goods, Fable review 22.10.).
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    Log(text);
}

int FormatV(char* out, size_t size, const char* format, va_list args)
{
    if (!out || !size) return 0;
    const int length = _vsnprintf_s(out, size, _TRUNCATE, format, args);
    return length >= 0 ? length : static_cast<int>(std::strlen(out));
}

int Format(char* out, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int length = FormatV(out, size, format, args);
    va_end(args);
    return length;
}

void FormatProbe(char const* argument, FormatLogFn log)
{
    (void)argument;
    if (log) log("FORMATPROBE unavailable");
}

void FormatStrict(FormatLogFn log)
{
    if (log) log("FORMATSTRICT unavailable");
}
}
