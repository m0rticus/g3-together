// SPDX-License-Identifier: GPL-3.0-or-later
#include "crashlog.h"

#include "log.h"
#include "ai.h"
#include "config.h"
#include "net.h"
#include <Game.h>
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace g3t::crashlog {
namespace {
bool installed = false, dumpInstalled = false;
volatile bool probing = false;
LONG logged = 0;
std::atomic<char const*> lastEndStep{nullptr};
worldentry::Progress entryOperation, entryGate;
std::atomic<uint32_t> entryEpoch{0}, entryWelcome{0};
worldentry::Reports entryReports;
struct CacheSnapshot {
    std::atomic<uint32_t> count{0};
    std::atomic<void const*> first{nullptr};
};
CacheSnapshot cacheSnapshots[static_cast<size_t>(CacheKind::Count)];
char const* const cacheNames[] = {"cameras", "prefetchers", "private_entities", "spheres", "anchors",
                                 "native_range", "puppets", "disabled", "summon_records"};
static_assert(std::size(cacheNames) == std::size(cacheSnapshots), "cache inventory names");

void LogEndStep()
{
    if (char const* step = lastEndStep.load())
        Logf("CRASH end-flow last_step=%s pid=%lu thread=%lu", step, GetCurrentProcessId(), GetCurrentThreadId());
}

using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                             PMINIDUMP_CALLBACK_INFORMATION);
DumpFn writeDump = nullptr;
HANDLE dumpRequest = nullptr, dumpDone = nullptr, dumpStop = nullptr, dumpThreadHandle = nullptr;
EXCEPTION_POINTERS* dumpInfo = nullptr;
DWORD dumpThreadId = 0, dumpWorkerThreadId = 0;
char dumpDir[MAX_PATH] = {};
LONG dumps = 0;
constexpr LONG MaxFirstChanceDumps = 2;
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
PVOID vectoredHandler = nullptr;
HANDLE watchdogCancel = nullptr, watchdogThreadHandle = nullptr;
bool stageWorld = false, stageForget = false, stageSteam = false, stageHooks = false;
bool stageThreads = false, stagePreShutdown = false, stageShutdownCalled = false;

void DeriveDumpDir()
{
    if (*dumpDir) return;
    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&DeriveDumpDir), &self) &&
        GetModuleFileNameA(self, path, MAX_PATH)) {
        for (int part = 0; part != 2; ++part) {
            char* slash = std::strrchr(path, '\\');
            if (!slash) { *path = '\0'; break; }
            *slash = '\0';
        }
    }
    if (!*path) {
        if (!GetCurrentDirectoryA(MAX_PATH, dumpDir)) dumpDir[0] = '\0';
    } else {
        strcpy_s(dumpDir, path);
    }
    if (*dumpDir) {
        size_t length = std::strlen(dumpDir);
        if (length && dumpDir[length - 1] != '\\') strcat_s(dumpDir, "\\");
    }
}

bool WriteDump(HANDLE file, PMINIDUMP_EXCEPTION_INFORMATION exception)
{
    if (!writeDump) { Log("crashlog: crash dump writer unavailable"); return false; }
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);
    if (writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception, nullptr, nullptr)) return true;
    DWORD error = GetLastError();
    // The game ships dbghelp 5.2, which rejects newer flags with HRESULT E_INVALIDARG and leaves an empty dump.
    if (error == ERROR_INVALID_PARAMETER || error == static_cast<DWORD>(E_INVALIDARG)) {
        Logf("crashlog: detailed minidump unsupported error=%lu; retrying MiniDumpNormal", error);
        const LARGE_INTEGER start{};
        if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
            Logf("crashlog: crash dump rewind failed error=%lu", GetLastError());
            return false;
        }
        if (writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, exception, nullptr, nullptr)) return true;
        error = GetLastError();
    }
    Logf("crashlog: crash dump write failed error=%lu", error);
    return false;
}

DWORD WINAPI DumpThread(LPVOID)
{
    for (;;) {
        HANDLE waits[2] = {dumpStop, dumpRequest};
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) return 0;
        if (result != WAIT_OBJECT_0 + 1) return 0;
        if (ai::ShuttingDown()) {
            SetEvent(dumpDone);
            return 0;
        }
        SYSTEMTIME now;
        GetLocalTime(&now);
        char path[MAX_PATH + 64];
        Format(path, "%sg3together-crash-%02u%02u%02u-%ld.dmp", dumpDir, now.wHour, now.wMinute, now.wSecond, dumps);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{dumpThreadId, dumpInfo, FALSE};
            WriteDump(file, dumpInfo ? &exception : nullptr);
            CloseHandle(file);
        } else {
            Logf("crashlog: crash dump create failed error=%lu", GetLastError());
        }
        SetEvent(dumpDone);
    }
}

void Dump(EXCEPTION_POINTERS* info)
{
    if (!writeDump || !dumpRequest || ai::ShuttingDown()) return;
    if (InterlockedCompareExchange(&dumps, 0, 0) < 0) return;
    dumpInfo = info;
    dumpThreadId = GetCurrentThreadId();
    SetEvent(dumpRequest);
    const DWORD wait = ai::ShuttingDown() ? 1000 : 30000;
    WaitForSingleObject(dumpDone, wait);
}

LONG WINAPI LastChance(EXCEPTION_POINTERS* info)
{
    if (ai::ShuttingDown() || GetCurrentThreadId() == dumpWorkerThreadId)
        return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    LogEndStep();
    if (InterlockedIncrement(&dumps) <= 4) Dump(info);
    return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallDumps()
{
    if (dumpInstalled) return;
    dumpInstalled = true;
    DeriveDumpDir();
    if (!*dumpDir) { Log("crashlog: no minidumps (game directory unavailable)"); return; }
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    writeDump = dbghelp ? reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    dumpRequest = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    dumpDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    dumpStop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    dumpThreadHandle = writeDump && dumpRequest && dumpDone && dumpStop
        ? CreateThread(nullptr, 256 * 1024, &DumpThread, nullptr, 0, &dumpWorkerThreadId) : nullptr;
    if (!dumpThreadHandle) { writeDump = nullptr; Log("crashlog: no minidumps (dbghelp or thread missing)"); return; }
    previousFilter = SetUnhandledExceptionFilter(&LastChance);
    Logf("crashlog: minidumps on a crash go to %sg3together-crash-*.dmp", dumpDir);
}

struct Module {
    char const* name;
    uintptr_t base = 0, size = 0;
    bool game = true;  // crash lines name its functions
};
Module modules[] = {{"Gothic3.exe"}, {"Engine.dll"}, {"Game.dll"}, {"Script.dll"}, {"Script_Game.dll"}, {"SharedBase.dll"},
                    {"Script_G3Together.dll", 0, 0, false}, {"Script_paru.dll", 0, 0, false}};

// Function names in crash lines (29.10.): the nearest export at or below the address, for Script_Game.dll also the scripts
// the game registered (most of them are not exported). Mangled names are undecorated by dbghelp, loaded at install time.
bool names = true;  // QA crashnamesoff: module+RVA only
unsigned named = 0;
using UndecorateFn = DWORD(WINAPI*)(PCSTR, PSTR, DWORD, DWORD);
UndecorateFn undecorate = nullptr;
constexpr DWORD UndnameNameOnly = 0x1000;  // UNDNAME_NAME_ONLY

struct Script {
    uintptr_t at;
    char name[48];
};
Script scripts[4096];
volatile LONG scriptCount = 0;
bool scriptsTaken = false;

IMAGE_NT_HEADERS const* Headers(Module const& m)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(m.base);
    return reinterpret_cast<IMAGE_NT_HEADERS const*>(m.base + dos->e_lfanew);
}

bool Executable(Module const& m, uintptr_t rva)
{
    IMAGE_NT_HEADERS const* nt = Headers(m);
    IMAGE_SECTION_HEADER const* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i != nt->FileHeader.NumberOfSections; ++i, ++section) {
        const uintptr_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (rva >= section->VirtualAddress && rva < section->VirtualAddress + size)
            return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    return false;
}

// The exported function that starts nearest at or below rva (forwarders and data skipped, a jmp thunk followed).
char const* NearestExport(Module const& m, uintptr_t rva, uintptr_t* start)
{
    auto const* base = reinterpret_cast<unsigned char const*>(m.base);
    IMAGE_DATA_DIRECTORY const& dir = Headers(m)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    auto const* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(base + dir.VirtualAddress);
    auto const* functions = reinterpret_cast<DWORD const*>(base + exports->AddressOfFunctions);
    auto const* nameRvas = reinterpret_cast<DWORD const*>(base + exports->AddressOfNames);
    auto const* ordinals = reinterpret_cast<WORD const*>(base + exports->AddressOfNameOrdinals);
    char const* best = nullptr;
    for (DWORD i = 0; i != exports->NumberOfNames; ++i) {
        if (ordinals[i] >= exports->NumberOfFunctions) continue;
        uintptr_t at = functions[ordinals[i]];
        if (at >= dir.VirtualAddress && at < dir.VirtualAddress + dir.Size) continue;  // forwarder
        if (!Executable(m, at)) continue;
        if (base[at] == 0xE9) {  // incremental-link thunk: jmp rel32
            int32_t relative;
            std::memcpy(&relative, base + at + 1, sizeof(relative));
            at += 5 + relative;
            if (at >= m.size || !Executable(m, at)) continue;
        }
        if (at <= rva && (!best || at > *start)) {
            best = reinterpret_cast<char const*>(base + nameRvas[i]);
            *start = at;
        }
    }
    return best;
}

char const* NearestScript(Module const& m, uintptr_t address, uintptr_t* start)
{
    char const* best = nullptr;
    const LONG count = scriptCount;
    for (LONG i = 0; i != count; ++i) {
        const uintptr_t at = scripts[i].at;
        if (at < m.base || at > address || (best && at - m.base <= *start)) continue;
        best = scripts[i].name;
        *start = at - m.base;
    }
    return best;
}

// Once a game runs, all scripts are registered: their entry points with their names, for crash lines.
void TakeScripts()
{
    if (scriptsTaken || !gCSession::GetInstance().IsGameRunning()) return;
    scriptsTaken = true;
    gCScriptAdminExt& admin = GetScriptAdminExt();
    LONG count = 0;
    auto take = [&](bCString const& name, void const* fn) {
        if (!fn || count == static_cast<LONG>(std::size(scripts))) return;
        scripts[count].at = reinterpret_cast<uintptr_t>(fn);
        strncpy_s(scripts[count].name, name.GetText(), _TRUNCATE);
        ++count;
    };
    GE_MAP_FOR_EACH (name, script, admin.GetScripts())
        if (script) take(name, reinterpret_cast<void const*>(script->m_funcScript));
    GE_MAP_FOR_EACH (name, state, admin.GetScriptAIStates())
        if (state) take(name, reinterpret_cast<void const*>(state->m_funcScriptAIState));
    GE_MAP_FOR_EACH (name, function, admin.GetScriptAIFunctions())
        if (function) take(name, reinterpret_cast<void const*>(function->m_funcScriptAIFunction));
    GE_MAP_FOR_EACH (name, callback, admin.GetScriptAICallbacks())
        if (callback) take(name, reinterpret_cast<void const*>(callback->m_funcScriptAICallback));
    InterlockedExchange(&scriptCount, count);
    Logf("crashlog: %d script names taken for crash lines", static_cast<int>(count));
}

void FindModules()
{
    for (Module& m : modules) {
        HMODULE h = GetModuleHandleA(m.name);
        if (!h) continue;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(h);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(reinterpret_cast<unsigned char const*>(h) + dos->e_lfanew);
        m.base = reinterpret_cast<uintptr_t>(h);
        m.size = nt->OptionalHeader.SizeOfImage;
    }
}

// "Module+rva" for an address in one of the modules, else nothing; for the game's modules with " (Function+offset)".
bool Where(uintptr_t address, char* out, size_t size)
{
    for (Module const& m : modules)
        if (m.base && address >= m.base && address < m.base + m.size) {
            const uintptr_t rva = address - m.base;
            int length = Format(out, size, "%s+%#x", m.name, static_cast<unsigned>(rva));
            if (!names || !m.game) return true;
            uintptr_t start = 0, scriptStart = 0;
            char const* name = NearestExport(m, rva, &start);
            if (char const* script = NearestScript(m, address, &scriptStart); script && (!name || scriptStart > start)) {
                name = script;
                start = scriptStart;
            }
            if (!name) return true;
            char plain[160];
            if (*name == '?' && undecorate && undecorate(name, plain, sizeof(plain), UndnameNameOnly)) name = plain;
            Format(out + length, size - length, " (%s+%#x)", name, static_cast<unsigned>(rva - start));
            ++named;
            return true;
        }
    return false;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    if (ai::ShuttingDown()) return EXCEPTION_CONTINUE_SEARCH;
    if (!info || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    bool gameFault = false;
    for (Module const& module : modules)
        if (module.base && fault >= module.base && fault - module.base < module.size) { gameFault = true; break; }
    // Drivers can handle their own first-chance exceptions. Dumping those may block graphics teardown.
    // Unhandled exceptions from any module still reach LastChance.
    if (!gameFault) return EXCEPTION_CONTINUE_SEARCH;
    LogEndStep();
    if (!probing && GetCurrentThreadId() != dumpWorkerThreadId && InterlockedIncrement(&dumps) <= MaxFirstChanceDumps)
        Dump(info);  // before anything
    // that could fault again
    if (InterlockedIncrement(&logged) > 12) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT const& c = *info->ContextRecord;
    char eip[240] = "?", line[1200];
    Where(c.Eip, eip, sizeof(eip));
    Format(line, "CRASH access violation at %s (%08lX) %s %08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX esp=%08lX ebp=%08lX thread=%lu",
              eip, c.Eip, info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
              static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[1]), c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Esp,
              c.Ebp, GetCurrentThreadId());
    Log(line);
    // Return addresses (and other code pointers) on the stack, nearest first. Log() keeps about 1000 characters of a line,
    // so a long stack goes on "CRASH stack+" lines.
    char stack[3000] = "CRASH stack";
    size_t lineStart = 0;
    int found = 0;
    auto const* top = reinterpret_cast<uintptr_t const*>(c.Esp);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(top, &mbi, sizeof(mbi))) {
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        for (int i = 0; i < 4096 && found < 32 && reinterpret_cast<uintptr_t>(top + i) + sizeof(uintptr_t) <= end; ++i) {
            char where[240];
            if (!Where(top[i], where, sizeof(where))) continue;
            const size_t need = std::strlen(where) + 2;
            size_t length = std::strlen(stack + lineStart);
            if (length + need >= 1000) {  // this line is full: log it, the next one starts behind it
                if (lineStart + length + 1 + 16 + need >= sizeof(stack)) break;
                Log(stack + lineStart);
                lineStart += length + 1;
                strcpy_s(stack + lineStart, sizeof(stack) - lineStart, "CRASH stack+");
                length = std::strlen(stack + lineStart);
            }
            if (lineStart + length + need >= sizeof(stack)) break;
            strcat_s(stack + lineStart, sizeof(stack) - lineStart, " ");
            strcat_s(stack + lineStart, sizeof(stack) - lineStart, where);
            ++found;
        }
    }
    Log(stack + lineStart);
    return EXCEPTION_CONTINUE_SEARCH;
}

// QA crashprobe: an access violation inside Engine.dll that is caught here (the vectored handler logs it first).
__declspec(noinline) void TouchBadEntity()
{
    bCVector position;
    reinterpret_cast<eCEntity const*>(0x10)->GetWorldPosition(position);
}

int ProbeFault()
{
    __try {
        TouchBadEntity();
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
}

struct WatchdogModule {
    DWORD_PTR base;
    DWORD_PTR size;
    char path[MAX_PATH];
};

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
            Format(out, size, "%08IX +%08IX", watchModules[i].base, address - watchModules[i].base);
            char const* slash = std::strrchr(watchModules[i].path, '\\');
            char suffix[MAX_PATH];
            Format(suffix, "%s", slash ? slash + 1 : watchModules[i].path);
            strcat_s(out, size, " ");
            strcat_s(out, size, suffix);
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
    Format(line, "G3 Together shutdown watchdog\nUTC/local %02u:%02u:%02u process=%lu watchdog_thread=%lu\n",
           now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), GetCurrentThreadId());
    WriteText(text, line);

    WatchdogModule watchModules[128] = {};
    unsigned moduleCount = 0;
    HANDLE moduleSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (moduleSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 module{};
        module.dwSize = sizeof(module);
        if (Module32First(moduleSnapshot, &module)) {
            do {
                if (moduleCount >= std::size(watchModules)) break;
                watchModules[moduleCount].base = reinterpret_cast<DWORD_PTR>(module.modBaseAddr);
                watchModules[moduleCount].size = module.modBaseSize;
                strncpy_s(watchModules[moduleCount].path, module.szExePath, _TRUNCATE);
                ++moduleCount;
                Format(line, "MODULE base=%08IX size=%08IX path=%s\n",
                       reinterpret_cast<DWORD_PTR>(module.modBaseAddr), static_cast<DWORD_PTR>(module.modBaseSize), module.szExePath);
                WriteText(text, line);
            } while (Module32Next(moduleSnapshot, &module));
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
    if (Thread32First(threadSnapshot, &thread)) {
        do {
            if (thread.th32OwnerProcessID != GetCurrentProcessId()) continue;
            const DWORD tid = thread.th32ThreadID;
            if (tid == GetCurrentThreadId()) continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!handle) {
                Format(line, "THREAD id=%lu CONTEXT failed=OpenThread error=%lu\n", tid, GetLastError());
                WriteText(text, line);
                continue;
            }
            DWORD suspended = SuspendThread(handle);
            if (suspended == static_cast<DWORD>(-1)) {
                Format(line, "THREAD id=%lu CONTEXT failed=SuspendThread error=%lu\n", tid, GetLastError());
                WriteText(text, line);
                CloseHandle(handle);
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(handle, &context)) {
                Format(line, "THREAD id=%lu CONTEXT failed=GetThreadContext error=%lu\n", tid, GetLastError());
                WriteText(text, line);
            } else {
                Format(line, "THREAD id=%lu EIP=%08lX ESP=%08lX EBP=%08lX\n",
                       tid, context.Eip, context.Esp, context.Ebp);
                WriteText(text, line);

                char where[300];
                WatchdogWhere(watchModules, moduleCount, context.Eip, where, sizeof(where));
                Format(line, "THREAD id=%lu frame=eip address=%08lX module+offset=%s\n", tid, context.Eip, where);
                WriteText(text, line);

                unsigned char stackBytes[4096];
                SIZE_T read = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Esp),
                                      stackBytes, sizeof(stackBytes), &read) && read >= sizeof(DWORD)) {
                    unsigned frame = 0;
                    for (SIZE_T i = 0; i + sizeof(DWORD) <= read && frame < 64; i += sizeof(DWORD)) {
                        DWORD_PTR address;
                        std::memcpy(&address, stackBytes + i, sizeof(DWORD));
                        WatchdogWhere(watchModules, moduleCount, address, where, sizeof(where));
                        if (!std::strcmp(where, "unknown")) continue;
                        Format(line, "THREAD id=%lu frame=%u address=%08IX module+offset=%s\n",
                               tid, frame++, address, where);
                        WriteText(text, line);
                    }
                }

                DWORD ebp = context.Ebp;
                unsigned ebpFrame = 0;
                for (unsigned i = 0; i < 32 && ebp; ++i) {
                    DWORD pair[2] = {};
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ebp),
                                            pair, sizeof(pair), &got) || got != sizeof(pair))
                        break;
                    if (!pair[1]) break;
                    WatchdogWhere(watchModules, moduleCount, pair[1], where, sizeof(where));
                    if (!std::strcmp(where, "unknown")) { ebp = pair[0]; continue; }
                    Format(line, "THREAD id=%lu ebp-frame=%u address=%08lX module+offset=%s\n",
                           tid, ebpFrame++, pair[1], where);
                    WriteText(text, line);
                    if (!pair[0] || pair[0] == ebp) break;
                    ebp = pair[0];
                }
            }
            const DWORD resumed = ResumeThread(handle);
            if (resumed == static_cast<DWORD>(-1)) {
                Format(line, "THREAD id=%lu RESUME failed=ResumeThread error=%lu\n", tid, GetLastError());
                WriteText(text, line);
            }
            CloseHandle(handle);
        } while (Thread32Next(threadSnapshot, &thread));
    }
    CloseHandle(threadSnapshot);
    CloseHandle(text);
}

void WriteWatchdogDump(char const* path)
{
    if (!writeDump) {
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        writeDump = dbghelp ? reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    }
    if (!writeDump) {
        Log("shutdown watchdog: dmp write failed (MiniDumpWriteDump unavailable)");
        return;
    }
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Logf("shutdown watchdog: dmp create failed error=%lu", GetLastError());
        return;
    }
    WriteDump(file, nullptr);
    CloseHandle(file);
}

DWORD WINAPI WatchdogThread(LPVOID)
{
    const DWORD result = WaitForSingleObject(watchdogCancel, 12000);
    if (result != WAIT_TIMEOUT) return 0;
    LogCacheInventory("shutdown watchdog");
    DeriveDumpDir();
    if (!*dumpDir) {
        Log("shutdown watchdog: write failed (game directory unavailable)");
        return 0;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char base[MAX_PATH + 64];
    Format(base, "%sg3together-hang-%02u%02u%02u", dumpDir, now.wHour, now.wMinute, now.wSecond);
    char dmp[MAX_PATH + 64], txt[MAX_PATH + 64];
    Format(dmp, "%s.dmp", base);
    Format(txt, "%s.txt", base);
    WriteWatchdogDump(dmp);
    WriteWatchdogText(txt);
    Logf("shutdown watchdog: files=%s ; %s", dmp, txt);
    return 0;
}
}

void WorldEntryWait(char const* step)
{
    if (GetConfig().host || ai::ShuttingDown()) return;
    entryEpoch.store(net::WorldEpoch());
    entryWelcome.store(net::WelcomeSerial());
    const auto previous = entryGate.Set(step, GetTickCount());
    if (!step && previous.name)
        Logf("world-entry: complete epoch=%08x welcome=%u", entryEpoch.load(), entryWelcome.load());
}

WorldEntryScope::WorldEntryScope(char const* operation)
{
    active = !GetConfig().host && !ai::ShuttingDown() && entryGate.Read().name;
    if (active) previous = entryOperation.Set(operation, GetTickCount());
}

WorldEntryScope::~WorldEntryScope()
{
    Complete();
}

void WorldEntryScope::Complete()
{
    if (!active) return;
    entryOperation.Restore(previous);
    active = false;
}

void PollWorldEntry()
{
    worldentry::Step gate, operation;
    if (!entryGate.Read(gate) || !gate.name || !entryOperation.Read(operation)) return;
    const uint32_t now = GetTickCount();
    if (!entryReports.Due(now, operation, gate)) return;
    Logf("world-entry: still waiting for %s for %u s; operation=%s operation_ms=%u epoch=%08x welcome=%u",
         gate.name, (now - gate.since) / 1000, operation.name ? operation.name : "engine outside mod",
         operation.name ? now - operation.since : 0, entryEpoch.load(), entryWelcome.load());
}

void EndStep(char const* step)
{
    lastEndStep.store(step);
}

void CacheObjects(CacheKind kind, uint32_t count, void const* first)
{
    auto& snapshot = cacheSnapshots[static_cast<size_t>(kind)];
    snapshot.first.store(first);
    snapshot.count.store(count);
}

void LogCacheInventory(char const* phase)
{
    char line[800];
    int length = Format(line, "world-cache: inventory %s", phase);
    for (size_t i = 0; i < std::size(cacheSnapshots) && length > 0; ++i) {
        const int written = Format(line + length, sizeof(line) - length, " %s=%u/%p", cacheNames[i],
                                   cacheSnapshots[i].count.load(), cacheSnapshots[i].first.load());
        if (written < 0 || static_cast<size_t>(length + written) >= sizeof(line)) {
            Log("FAIL world-cache: inventory diagnostic exceeded its buffer");
            return;
        }
        length += written;
    }
    Log(line);
}

void NativeStack(char const* reason)
{
    auto describe = [](void* frame, char* out, size_t size) {
        MEMORY_BASIC_INFORMATION memory{};
        char path[MAX_PATH] = {};
        if (!VirtualQuery(frame, &memory, sizeof(memory)) || memory.Type != MEM_IMAGE ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            !GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), path, MAX_PATH)) return false;
        char const* module = std::strrchr(path, '\\');
        Format(out, size, "%s+%#x", module ? module + 1 : path,
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(memory.AllocationBase)));
        return true;
    };
    void* frames[24] = {};
    const USHORT count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    Logf("CRASH native-stack reason=%s pid=%lu thread=%lu frames=%u", reason,
         GetCurrentProcessId(), GetCurrentThreadId(), static_cast<unsigned>(count));
    for (USHORT i = 0; i < count; ++i) {
        char where[MAX_PATH + 24];
        if (!describe(frames[i], where, sizeof(where))) strcpy_s(where, "(module unavailable)");
        Logf("CRASH native-stack frame=%u address=%p %s", static_cast<unsigned>(i), frames[i], where);
    }
    // x86 engine functions omit frame pointers. Keep bounded raw candidates separately, never label them unwound frames.
    auto* stack = static_cast<uintptr_t const*>(_AddressOfReturnAddress());
    auto const* tib = reinterpret_cast<NT_TIB const*>(NtCurrentTeb());
    const uintptr_t begin = reinterpret_cast<uintptr_t>(stack), top = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (begin < reinterpret_cast<uintptr_t>(tib->StackLimit) || begin >= top) {
        Log("FAIL CRASH native-stack: current-thread stack bounds unavailable");
        return;
    }
    const size_t words = std::min<size_t>((top - begin) / sizeof(uintptr_t), 1024);
    uintptr_t candidates[32] = {};
    unsigned retained = 0;
    for (size_t i = 0; i < words && retained < std::size(candidates); ++i) {
        if (!stack[i] || std::find(candidates, candidates + retained, stack[i]) != candidates + retained) continue;
        char where[MAX_PATH + 24];
        if (!describe(reinterpret_cast<void*>(stack[i]), where, sizeof(where))) continue;
        candidates[retained++] = stack[i];
        Logf("CRASH native-stack candidate_word=%u address=%p %s", static_cast<unsigned>(i),
             reinterpret_cast<void*>(stack[i]), where);
    }
}

void Tick()
{
    InstallDumps();  // every player: last-chance minidumps
    if (installed) { TakeScripts(); return; }
    installed = true;
    FindModules();
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    undecorate = dbghelp ? reinterpret_cast<UndecorateFn>(GetProcAddress(dbghelp, "UnDecorateSymbolName")) : nullptr;
    if (!vectoredHandler) vectoredHandler = AddVectoredExceptionHandler(1, &OnException);
    Log("crashlog: access violations are logged");
}

void NamesOff(LogFn log)
{
    names = false;
    log("CRASHNAMESOFF ok=1 (crash lines show module+RVA only)");
}

void Probe(LogFn log)
{
    const unsigned before = named;
    probing = true;  // no minidump for it
    const int fault = ProbeFault();
    probing = false;
    char line[120];
    Format(line, "CRASHPROBE fault=%d named=%u scripts=%d names=%d", fault, named - before, static_cast<int>(scriptCount),
           names ? 1 : 0);
    log(line);
}

void Stage(char const* name)
{
    if (!name) return;
    if (!std::strcmp(name, "world deactivated") && stageWorld) return;
    if (!std::strcmp(name, "forget done") && stageForget) return;
    if (!std::strcmp(name, "steam stopped") && stageSteam) return;
    if (!std::strcmp(name, "hooks removed") && stageHooks) return;
    if (!std::strcmp(name, "threads joined") && stageThreads) return;
    if (!std::strcmp(name, "pre-shutdown done") && stagePreShutdown) return;
    if (!std::strcmp(name, "shutdown called") && stageShutdownCalled) return;
    if (!std::strcmp(name, "world deactivated")) stageWorld = true;
    if (!std::strcmp(name, "forget done")) stageForget = true;
    if (!std::strcmp(name, "steam stopped")) stageSteam = true;
    if (!std::strcmp(name, "hooks removed")) stageHooks = true;
    if (!std::strcmp(name, "threads joined")) stageThreads = true;
    if (!std::strcmp(name, "pre-shutdown done")) stagePreShutdown = true;
    if (!std::strcmp(name, "shutdown called")) stageShutdownCalled = true;
    char line[160];
    Format(line, "shutdown-stage: %s", name);
    Log(line);
}

void CancelWatchdog()
{
    if (watchdogCancel) SetEvent(watchdogCancel);
    if (watchdogThreadHandle) {
        const DWORD waited = WaitForSingleObject(watchdogThreadHandle, 2000);
        if (waited == WAIT_TIMEOUT) {
            Log("crashlog: watchdog cancel timed out; leaving worker and event live");
            return;
        }
        CloseHandle(watchdogThreadHandle);
        watchdogThreadHandle = nullptr;
    }
    if (watchdogCancel) {
        CloseHandle(watchdogCancel);
        watchdogCancel = nullptr;
    }
}

void BeginShutdown()
{
    if (dumpStop) SetEvent(dumpStop);
    if (dumpDone) SetEvent(dumpDone);
    if (vectoredHandler) {
        RemoveVectoredExceptionHandler(vectoredHandler);
        vectoredHandler = nullptr;
    }
    if (previousFilter) {
        LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(&LastChance);
        if (current == &LastChance) {
            SetUnhandledExceptionFilter(previousFilter);
        } else {
            SetUnhandledExceptionFilter(current);
        }
        previousFilter = nullptr;
    }
}

bool Shutdown()
{
    CancelWatchdog();
    BeginShutdown();
    if (dumpStop) SetEvent(dumpStop);
    bool joined = true;
    if (dumpThreadHandle) {
        const DWORD waited = WaitForSingleObject(dumpThreadHandle, 2000);
        if (waited == WAIT_TIMEOUT) {
            Log("crashlog: dump worker stop timed out; continuing teardown");
            joined = false;
        }
        if (joined) {
            CloseHandle(dumpThreadHandle);
            dumpThreadHandle = nullptr;
        }
    }
    if (!joined) {
        return false;
    }
    if (dumpRequest) { CloseHandle(dumpRequest); dumpRequest = nullptr; }
    if (dumpDone) { CloseHandle(dumpDone); dumpDone = nullptr; }
    if (dumpStop) { CloseHandle(dumpStop); dumpStop = nullptr; }
    return true;
}

void StartWatchdog()
{
    if (watchdogThreadHandle) return;
    DeriveDumpDir();
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
        return;
    }
}
}
