// SPDX-License-Identifier: GPL-3.0-or-later
#include "shutdown_watchdog.h"

#include "crashlog.h"
#include "log.h"
#include "shutdown_policy.h"
#include "symbols.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace g3t::crashlog {
namespace {
HANDLE watchdogCancel = nullptr, watchdogThreadHandle = nullptr;
std::atomic<bool> preShutdownFinished{false}, fastExitReady{false};
std::atomic<uint32_t> preShutdownAt{0};
std::atomic<uint32_t> stagesSeen{0};
char watchdogDir[MAX_PATH] = {};

struct GraphicsSites {
    DWORD thread = 0;
    uintptr_t nativeWait = 0;
    uint32_t conditionReturn = 0, driverReturn = 0, deviceReturn = 0;
};
struct GraphicsSnapshot {
    shutdown::GraphicsWait wait{};
    bool matched = false;
    char const* status = "not sampled";
    DWORD error = 0;
};
GraphicsSites graphicsSites;
GraphicsSnapshot lastGraphics;

struct WatchdogModule {
    DWORD_PTR base;
    DWORD_PTR size;
    char path[MAX_PATH];
};

constexpr char const* StageNames[] = {
    "world deactivated",
    "forget done",
    "steam stopped",
    "hooks removed",
    "threads joined",
    "pre-shutdown done",
    "shutdown called",
};

void EnsureWatchdogDir()
{
    symbols::EnsureGameDir(watchdogDir);
}

GraphicsSites FindGraphicsSites()
{
    GraphicsSites sites;
    sites.thread = GetCurrentThreadId();
    HMODULE engine = GetModuleHandleA("Engine.dll"), driver = GetModuleHandleA("AMDXN32.DLL");
    HMODULE kernel = GetModuleHandleA("KERNELBASE.dll"), native = GetModuleHandleA("ntdll.dll");
    const auto device = reinterpret_cast<uintptr_t>(GetProcAddress(engine, "??1eCDX9Device@@UAE@XZ"));
    const auto condition = reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "SleepConditionVariableCS"));
    const auto wait = reinterpret_cast<uintptr_t>(GetProcAddress(native, "NtWaitForAlertByThreadId"));
    uint8_t deviceCode[15]{}, driverCode[18]{}, conditionCode[34]{}, waitCode[15]{};
    const uintptr_t driverSite = reinterpret_cast<uintptr_t>(driver) + 0x2cad4;
    uintptr_t conditionImport = 0;
    if (!engine || !driver || !kernel || !native || !device || !condition || !wait ||
        !symbols::ReadMemory(device + 0x317, deviceCode, sizeof(deviceCode)) ||
        !shutdown::DeviceReleaseSite(deviceCode, sizeof(deviceCode)) ||
        !symbols::ReadMemory(driverSite, driverCode, sizeof(driverCode)) ||
        !shutdown::DriverWaitSite(driverCode, sizeof(driverCode)) ||
        !symbols::ReadMemory(condition, conditionCode, sizeof(conditionCode)) ||
        conditionCode[0x1a] != 0xff || conditionCode[0x1b] != 0x15 ||
        conditionCode[0x20] != 0x8b || conditionCode[0x21] != 0xf0 ||
        !symbols::ReadMemory(wait, waitCode, sizeof(waitCode)) ||
        waitCode[0] != 0xb8 || waitCode[0xc] != 0xc2 || waitCode[0xd] != 8 || waitCode[0xe] != 0) {
        Log("shutdown watchdog: native graphics signatures unsupported; diagnostic-only");
        return sites;
    }
    uint32_t importSlot = 0;
    std::memcpy(&importSlot, driverCode + 9, sizeof(importSlot));
    if (!symbols::ReadMemory(importSlot, &conditionImport, sizeof(conditionImport)) || conditionImport != condition) {
        Log("shutdown watchdog: AMD condition import differs; diagnostic-only");
        return sites;
    }
    sites.nativeWait = wait;
    sites.conditionReturn = static_cast<uint32_t>(condition + 0x20);
    sites.driverReturn = static_cast<uint32_t>(driverSite + 13);
    sites.deviceReturn = static_cast<uint32_t>(device + 0x322);
    Logf("shutdown watchdog: native graphics wait verified thread=%lu device_return=%08x driver_return=%08x",
         sites.thread, sites.deviceReturn, sites.driverReturn);
    return sites;
}

bool SampleGraphicsWait(shutdown::GraphicsWait& wait)
{
    lastGraphics.error = 0;
    lastGraphics.status = "unsupported signatures";
    if (!graphicsSites.nativeWait) return false;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, graphicsSites.thread);
    if (!thread) {
        lastGraphics.status = "OpenThread failed";
        lastGraphics.error = GetLastError();
        return false;
    }
    const DWORD suspended = SuspendThread(thread);
    if (suspended == static_cast<DWORD>(-1)) {
        lastGraphics.status = "SuspendThread failed";
        lastGraphics.error = GetLastError();
        CloseHandle(thread);
        return false;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    uint32_t stack[1024]{}, objectWords[22]{};
    SIZE_T read = 0;
    const bool captured = GetThreadContext(thread, &context) != FALSE;
    lastGraphics.status = captured ? "outside verified native wait" : "GetThreadContext failed";
    if (!captured) lastGraphics.error = GetLastError();
    bool matched = !suspended && captured && context.Eip == graphicsSites.nativeWait + 0xc;
    if (suspended) lastGraphics.status = "thread was already suspended";
    if (matched) {
        const BOOL stackRead = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Esp),
                                                 stack, sizeof(stack), &read);
        if (!stackRead) lastGraphics.error = GetLastError();
        const uint32_t object = shutdown::WaitingObject(stack, read / sizeof(stack[0]),
            graphicsSites.conditionReturn, graphicsSites.driverReturn, graphicsSites.deviceReturn);
        lastGraphics.status = object ? "driver counters unreadable" : "nested wait stack not matched";
        matched = object && symbols::ReadMemory(object, objectWords, sizeof(objectWords));
        if (matched) {
            lastGraphics.status = "verified native graphics wait";
            wait = {object, objectWords[0x48 / 4], objectWords[0x50 / 4], objectWords[0x54 / 4]};
        } else if (object) {
            lastGraphics.error = GetLastError();
        }
    }
    // No logging, allocation, engine access or termination while the main thread is suspended.
    const DWORD resumed = ResumeThread(thread);
    const DWORD error = resumed == static_cast<DWORD>(-1) ? GetLastError() : 0;
    CloseHandle(thread);
    if (resumed == static_cast<DWORD>(-1)) {
        Logf("shutdown watchdog: main thread resume failed error=%lu; fast exit disabled", error);
        lastGraphics.status = "ResumeThread failed";
        lastGraphics.error = error;
        fastExitReady.store(false);
        return false;
    }
    return matched;
}

bool WriteText(HANDLE file, char const* text)
{
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const size_t length = std::strlen(text);
    return length == 0 || (WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr) && written == length);
}

void WatchdogWhere(WatchdogModule const* watchModules, unsigned count, DWORD_PTR address, char* out, size_t size)
{
    for (unsigned i = 0; i < count; ++i)
        if (address >= watchModules[i].base && address < watchModules[i].base + watchModules[i].size) {
            char const* slash = std::strrchr(watchModules[i].path, '\\');
            Format(out, size, "%08IX +%08IX %s", watchModules[i].base, address - watchModules[i].base,
                   slash ? slash + 1 : watchModules[i].path);
            return;
        }
    Format(out, size, "unknown");
}

void WriteWatchdogText(char const* path)
{
    HANDLE text = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text == INVALID_HANDLE_VALUE) {
        Logf("shutdown watchdog: text create failed error=%lu", GetLastError());
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1024];
    Format(line, "G3 Together shutdown watchdog\nUTC/local %02u:%02u:%02u process=%lu watchdog_thread=%lu\n"
                 "SHUTDOWN_THREAD id=%lu cleanup_complete=%d fast_exit_eligible=%d graphics_supported=%d\n"
                 "GRAPHICS_WAIT matched=%d object=%08x completed=%u produced=%u retired=%u\n"
                 "GRAPHICS_SAMPLE status=%s error=%lu\n"
                 "STACK_NOTE candidate_word entries are raw stack values, not unwound frames\n",
           now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), GetCurrentThreadId(),
           graphicsSites.thread, preShutdownFinished.load() ? 1 : 0, fastExitReady.load() ? 1 : 0,
           graphicsSites.nativeWait ? 1 : 0, lastGraphics.matched ? 1 : 0, lastGraphics.wait.object,
           lastGraphics.wait.completed, lastGraphics.wait.produced, lastGraphics.wait.retired,
           lastGraphics.status, lastGraphics.error);
    WriteText(text, line);

    WatchdogModule watchModules[128] = {};
    unsigned moduleCount = 0;
    HANDLE moduleSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (moduleSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 module{};
        module.dwSize = sizeof(module);
        for (BOOL ok = Module32First(moduleSnapshot, &module); ok && moduleCount < std::size(watchModules);
             ok = Module32Next(moduleSnapshot, &module)) {
            watchModules[moduleCount].base = reinterpret_cast<DWORD_PTR>(module.modBaseAddr);
            watchModules[moduleCount].size = module.modBaseSize;
            strncpy_s(watchModules[moduleCount].path, module.szExePath, _TRUNCATE);
            ++moduleCount;
            Format(line, "MODULE base=%08IX size=%08IX path=%s\n",
                   reinterpret_cast<DWORD_PTR>(module.modBaseAddr), static_cast<DWORD_PTR>(module.modBaseSize), module.szExePath);
            WriteText(text, line);
        }
        CloseHandle(moduleSnapshot);
    } else {
        WriteText(text, "MODULE_INVENTORY failed=CreateToolhelp32Snapshot\n");
    }

    CONTEXT selfContext{};
    selfContext.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&selfContext);
    Format(line, "WATCHDOG_THREAD id=%lu EIP=%08lX ESP=%08lX EBP=%08lX (captured without suspension)\n",
           GetCurrentThreadId(), selfContext.Eip, selfContext.Esp, selfContext.Ebp);
    WriteText(text, line);
    if (selfContext.Eip) {
        char where[300];
        WatchdogWhere(watchModules, moduleCount, selfContext.Eip, where, sizeof(where));
        Format(line, "WATCHDOG_THREAD id=%lu frame=eip address=%08lX module+offset=%s\n",
               GetCurrentThreadId(), selfContext.Eip, where);
        WriteText(text, line);
    }

    HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnapshot == INVALID_HANDLE_VALUE) {
        WriteText(text, "THREAD_INVENTORY failed=CreateToolhelp32Snapshot\n");
        CloseHandle(text);
        return;
    }
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    for (BOOL ok = Thread32First(threadSnapshot, &thread); ok; ok = Thread32Next(threadSnapshot, &thread)) {
        if (thread.th32OwnerProcessID != GetCurrentProcessId() || thread.th32ThreadID == GetCurrentThreadId()) continue;
        const DWORD tid = thread.th32ThreadID;
        symbols::ThreadSample sample{};
        symbols::SampleThreadStack(tid, sample, true);
        if (!sample.suspended) {
            Format(line, "THREAD id=%lu CONTEXT failed=%s error=%lu\n", tid,
                   sample.openError ? "OpenThread" : "SuspendThread",
                   sample.openError ? sample.openError : sample.suspendError);
            WriteText(text, line);
            continue;
        }
        if (!sample.contextOk) {
            Format(line, "THREAD id=%lu CONTEXT failed=GetThreadContext error=%lu\n", tid, sample.contextError);
            WriteText(text, line);
        } else {
            char where[300];
            WatchdogWhere(watchModules, moduleCount, sample.eip, where, sizeof(where));
            Format(line, "THREAD id=%lu EIP=%08IX ESP=%08IX EBP=%08IX\n"
                         "THREAD id=%lu frame=eip address=%08IX module+offset=%s\n",
                   tid, sample.eip, sample.esp, sample.ebp, tid, sample.eip, where);
            WriteText(text, line);

            unsigned frame = 0;
            for (unsigned i = 0; i < sample.count && frame < 64; ++i) {
                WatchdogWhere(watchModules, moduleCount, sample.words[i], where, sizeof(where));
                if (!std::strcmp(where, "unknown")) continue;
                Format(line, "THREAD id=%lu candidate_word=%u address=%08IX module+offset=%s\n",
                       tid, i, sample.words[i], where);
                ++frame;
                WriteText(text, line);
            }

            unsigned ebpFrame = 0;
            for (unsigned i = 0; i < sample.ebpCount; ++i) {
                WatchdogWhere(watchModules, moduleCount, sample.ebpFrames[i], where, sizeof(where));
                if (std::strcmp(where, "unknown")) {
                    Format(line, "THREAD id=%lu ebp-frame=%u address=%08IX module+offset=%s\n",
                           tid, ebpFrame++, sample.ebpFrames[i], where);
                    WriteText(text, line);
                }
            }
        }
        if (!sample.resumed) {
            Format(line, "THREAD id=%lu RESUME failed=ResumeThread error=%lu\n", tid, sample.resumeError);
            WriteText(text, line);
        }
    }
    CloseHandle(threadSnapshot);
    CloseHandle(text);
}

void WriteWatchdogDump(char const* path)
{
    WriteProcessDump(path);
}

DWORD WINAPI WatchdogThread(LPVOID)
{
    const uint32_t began = GetTickCount();
    shutdown::ExitPolicy exit;
    for (;;) {
        const DWORD result = WaitForSingleObject(watchdogCancel, shutdown::PollMs);
        if (result == WAIT_OBJECT_0) return 0;
        if (result != WAIT_TIMEOUT) {
            Logf("shutdown watchdog: wait failed error=%lu", GetLastError());
            return 0;
        }
        const uint32_t now = GetTickCount();
        const bool finished = preShutdownFinished.load();
        const uint32_t finishedAt = preShutdownAt.load();
        lastGraphics.matched = finished && fastExitReady.load() && SampleGraphicsWait(lastGraphics.wait);
        if (exit.Observe(now, finished && fastExitReady.load(), finishedAt, lastGraphics.matched, lastGraphics.wait)) {
            Logf("shutdown: native DX9/AMD Release stalled after cleanup thread=%lu object=%08x "
                 "completed=%u target=%u; exiting process without DLL detach",
                 graphicsSites.thread, lastGraphics.wait.object, lastGraphics.wait.completed,
                 lastGraphics.wait.produced > lastGraphics.wait.retired
                     ? lastGraphics.wait.produced - lastGraphics.wait.retired
                     : 0u);
            // ExitProcess runs DLL detach, which can wait on a driver lock held by
            // a terminated thread. Our world/save/transport cleanup already returned.
            if (!TerminateProcess(GetCurrentProcess(), 0)) {
                Logf("shutdown: native graphics fast exit failed error=%lu; retaining hang diagnostics", GetLastError());
                fastExitReady.store(false);
            }
        }
        if (now - (finished ? finishedAt : began) >= shutdown::DiagnosticMs) break;
    }
    LogCacheInventory("shutdown watchdog");
    EnsureWatchdogDir();
    if (!*watchdogDir) {
        Log("shutdown watchdog: write failed (game directory unavailable)");
        return 0;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char base[MAX_PATH + 64], dmp[MAX_PATH + 64], txt[MAX_PATH + 64];
    Format(base, "%sg3together-hang-%02u%02u%02u", watchdogDir, now.wHour, now.wMinute, now.wSecond);
    Format(dmp, "%s.dmp", base);
    Format(txt, "%s.txt", base);
    WriteWatchdogDump(dmp);
    WriteWatchdogText(txt);
    Logf("shutdown watchdog: files=%s ; %s", dmp, txt);
    return 0;
}
}

void Stage(char const* name)
{
    if (!name) return;
    for (size_t i = 0; i < std::size(StageNames); ++i) {
        if (std::strcmp(name, StageNames[i]) != 0) continue;
        const uint32_t bit = 1u << i;
        if (stagesSeen.fetch_or(bit) & bit) return;
        break;
    }
    Logf("shutdown-stage: %s", name);
}

void StartWatchdog()
{
    if (watchdogThreadHandle) return;
    EnsureWatchdogDir();
    graphicsSites = FindGraphicsSites();
    watchdogCancel = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!watchdogCancel) {
        Log("shutdown watchdog: start failed (cancel event unavailable)");
        return;
    }
    watchdogThreadHandle = CreateThread(nullptr, 256 * 1024, &WatchdogThread, nullptr, 0, nullptr);
    if (!watchdogThreadHandle) {
        CloseHandle(watchdogCancel);
        watchdogCancel = nullptr;
        Log("shutdown watchdog: start failed (thread unavailable)");
    }
}

void CompletePreShutdown(bool safeToExit)
{
    preShutdownAt.store(GetTickCount());
    fastExitReady.store(safeToExit);
    preShutdownFinished.store(true);
    Logf("shutdown watchdog: cleanup complete thread=%lu fast_exit_eligible=%d",
         GetCurrentThreadId(), safeToExit ? 1 : 0);
}
}
