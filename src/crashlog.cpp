// SPDX-License-Identifier: GPL-3.0-or-later
#include <enet/enet.h>
#include "crashlog.h"

#include "log.h"
#include "ai.h"
#include "symbols.h"
#include <Game.h>
#include <Windows.h>
#include <DbgHelp.h>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace g3t::crashlog {
namespace {
bool installed = false, dumpInstalled = false;
LONG logged = 0;
std::atomic<char const*> lastEndStep{nullptr};
std::atomic<uint32_t> sampledSince{0};
constexpr uint32_t MainSampleMs = 15000;

void LogEndStep()
{
    if (char const* step = lastEndStep.load())
        Logf("CRASH end-flow last_step=%s pid=%lu thread=%lu", step, GetCurrentProcessId(), GetCurrentThreadId());
}

using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                             PMINIDUMP_CALLBACK_INFORMATION);
std::atomic<DumpFn> writeDump{nullptr};
SRWLOCK dbghelpLock = SRWLOCK_INIT;
HANDLE dumpRequest = nullptr, dumpDone = nullptr, dumpStop = nullptr, dumpThreadHandle = nullptr;
EXCEPTION_RECORD dumpExceptionRecord{};
CONTEXT dumpContext{};
EXCEPTION_POINTERS dumpExceptionPointers{};
bool dumpExceptionCopied = false;
LONG dumpBusy = 0;
std::atomic<uint32_t> activeDumpCallers{0};
std::atomic<bool> shuttingDown{false};
DWORD dumpThreadId = 0, dumpWorkerThreadId = 0;
char dumpDir[MAX_PATH] = {};
LONG firstChanceDumps = 0, lastChanceDumps = 0, dumpSequence = 0;
constexpr LONG MaxFirstChanceDumps = 2;
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
bool filterInstalled = false;
PVOID vectoredHandler = nullptr;

HMODULE EnsureDbgHelp()
{
    static HMODULE module = LoadLibraryA("dbghelp.dll");
    return module;
}

struct DbgHelpGuard {
    bool locked = false;
    explicit DbgHelpGuard(bool wait)
    {
        if (wait) {
            AcquireSRWLockExclusive(&dbghelpLock);
            locked = true;
        } else {
            locked = TryAcquireSRWLockExclusive(&dbghelpLock) != FALSE;
        }
    }
    ~DbgHelpGuard()
    {
        if (locked) ReleaseSRWLockExclusive(&dbghelpLock);
    }
};

void EnsureDumpDir()
{
    symbols::EnsureGameDir(dumpDir);
}

bool WriteDump(HANDLE file, PMINIDUMP_EXCEPTION_INFORMATION exception, bool waitLock)
{
    DumpFn const fn = writeDump.load(std::memory_order_acquire);
    if (!fn) { Log("crashlog: crash dump writer unavailable"); return false; }
    DbgHelpGuard guard(waitLock);
    if (!guard.locked) {
        Log("crashlog: dbghelp busy; skipping concurrent dump");
        return false;
    }
    const LARGE_INTEGER start{};
    auto rewind = [&]() {
        if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
            Logf("crashlog: crash dump rewind failed error=%lu", GetLastError());
            return false;
        }
        return true;
    };
    const auto detailed = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo);
    const auto legacyRich = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);
    struct Profile {
        MINIDUMP_TYPE type;
        char const* name;
    };
    const Profile profiles[] = {{detailed, "detailed"}, {legacyRich, "legacy-rich"}, {MiniDumpNormal, "normal"}};
    struct DeferredRetry { char const* failed; char const* next; DWORD error; } retries[2]{};
    size_t retryCount = 0;
    auto logRetries = [&]() {
        for (size_t j = 0; j < retryCount; ++j)
            Logf("crashlog: crash dump profile=%s unsupported error=%lu; retrying %s",
                 retries[j].failed, retries[j].error, retries[j].next);
        retryCount = 0;
    };
    // The game ships dbghelp 5.2: only unsupported flags permit a more compatible profile.
    for (size_t i = 0; i != std::size(profiles); ++i) {
        if (i && !rewind()) { logRetries(); return false; }
        if (fn(GetCurrentProcess(), GetCurrentProcessId(), file, profiles[i].type, exception, nullptr, nullptr)) {
            logRetries();
            Logf("crashlog: crash dump success profile=%s type=%lu", profiles[i].name,
                 static_cast<unsigned long>(profiles[i].type));
            return true;
        }
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER || error == static_cast<DWORD>(E_INVALIDARG)) {
            if (i + 1 == std::size(profiles)) {
                logRetries();
                Logf("crashlog: crash dump profile=%s unsupported error=%lu", profiles[i].name, error);
                break;
            }
            retries[retryCount++] = {profiles[i].name, profiles[i + 1].name, error};
            continue;
        }
        logRetries();
        Logf("crashlog: crash dump write failed profile=%s error=%lu", profiles[i].name, error);
        return false;
    }
    Log("crashlog: crash dump write failed");
    return false;
}

DWORD WINAPI DumpThread(LPVOID)
{
    for (;;) {
        HANDLE waits[2] = {dumpStop, dumpRequest};
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            SetEvent(dumpDone);
            InterlockedExchange(&dumpBusy, 0);
            return 0;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            Logf("crashlog: crash dump worker wait failed result=%lu error=%lu",
                 result, result == WAIT_FAILED ? GetLastError() : 0);
            SetEvent(dumpDone);
            InterlockedExchange(&dumpBusy, 0);
            return 0;
        }
        if (ai::ShuttingDown()) {
            SetEvent(dumpDone);
            InterlockedExchange(&dumpBusy, 0);
            return 0;
        }
        SYSTEMTIME now;
        GetLocalTime(&now);
        char path[MAX_PATH + 64];
        Format(path, "%sg3together-crash-%02u%02u%02u-%ld.dmp", dumpDir, now.wHour, now.wMinute, now.wSecond, dumpSequence);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{dumpThreadId,
                                                     dumpExceptionCopied ? &dumpExceptionPointers : nullptr, FALSE};
            WriteDump(file, dumpExceptionCopied ? &exception : nullptr, true);
            CloseHandle(file);
        } else {
            Logf("crashlog: crash dump create failed error=%lu", GetLastError());
        }
        SetEvent(dumpDone);
        InterlockedExchange(&dumpBusy, 0);
    }
}

void Dump(EXCEPTION_POINTERS* info, LONG sequence = 0)
{
    struct CallerGuard {
        CallerGuard() { activeDumpCallers.fetch_add(1, std::memory_order_acq_rel); }
        ~CallerGuard() { activeDumpCallers.fetch_sub(1, std::memory_order_acq_rel); }
    } caller;
    if (shuttingDown.load(std::memory_order_acquire) || ai::ShuttingDown() ||
        !writeDump.load(std::memory_order_acquire) || !dumpRequest || !dumpDone || !dumpStop)
        return;
    if (InterlockedCompareExchange(&dumpBusy, 1, 0) != 0) {
        Log("guard: crashlog dump busy; skipping overlapping request");
        return;
    }
    if (!ResetEvent(dumpDone)) {
        Logf("guard: crashlog ResetEvent(dumpDone) failed error=%lu", GetLastError());
        InterlockedExchange(&dumpBusy, 0);
        return;
    }
    dumpExceptionCopied = false;
    dumpExceptionRecord = {};
    dumpContext = {};
    dumpExceptionPointers = {};
    if (info && info->ExceptionRecord && info->ContextRecord) {
        dumpExceptionRecord = *info->ExceptionRecord;
        dumpExceptionRecord.ExceptionRecord = nullptr;
        dumpContext = *info->ContextRecord;
        dumpExceptionPointers.ExceptionRecord = &dumpExceptionRecord;
        dumpExceptionPointers.ContextRecord = &dumpContext;
        dumpExceptionCopied = true;
    } else {
        Log("guard: crashlog missing exception record/context; dumping without exception");
    }
    dumpThreadId = GetCurrentThreadId();
    dumpSequence = sequence;
    if (!SetEvent(dumpRequest)) {
        Logf("guard: crashlog dump request failed error=%lu", GetLastError());
        InterlockedExchange(&dumpBusy, 0);
        return;
    }
    const DWORD wait = (shuttingDown.load(std::memory_order_acquire) || ai::ShuttingDown()) ? 1000 : 30000;
    HANDLE waits[2] = {dumpDone, dumpStop};
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, wait);
    if (waited == WAIT_TIMEOUT) {
        Logf("crashlog: crash dump wait timeout ms=%lu guard=busy", wait);
    } else if (waited == WAIT_FAILED) {
        Logf("crashlog: crash dump wait failed error=%lu guard=busy", GetLastError());
    }
}

LONG WINAPI LastChance(EXCEPTION_POINTERS* info)
{
    if (shuttingDown.load(std::memory_order_acquire) || ai::ShuttingDown() ||
        GetCurrentThreadId() == dumpWorkerThreadId)
        return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    if (const LONG seq = InterlockedIncrement(&lastChanceDumps); seq <= 4) Dump(info, 100 + seq);
    LogEndStep();
    return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallDumps()
{
    if (dumpInstalled) return;
    dumpInstalled = true;
    EnsureDumpDir();
    if (!*dumpDir) { Log("crashlog: no minidumps (game directory unavailable)"); return; }
    HMODULE dbghelp = EnsureDbgHelp();
    DumpFn const fn = dbghelp ? reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    writeDump.store(fn, std::memory_order_release);
    dumpRequest = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    dumpDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    dumpStop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    dumpThreadHandle = fn && dumpRequest && dumpDone && dumpStop
        ? CreateThread(nullptr, 256 * 1024, &DumpThread, nullptr, 0, &dumpWorkerThreadId) : nullptr;
    if (!dumpThreadHandle) { writeDump.store(nullptr, std::memory_order_release); Log("crashlog: no minidumps (dbghelp or thread missing)"); return; }
    previousFilter = SetUnhandledExceptionFilter(&LastChance);
    filterInstalled = true;
    Logf("crashlog: minidumps on a crash go to %sg3together-crash-*.dmp", dumpDir);
}

struct Module {
    char const* name;
    uintptr_t base = 0, size = 0;
};
struct ModuleSlot {
    char const* name;
    std::atomic<uint32_t> seq{0};
    std::atomic<uintptr_t> base{0}, size{0};
    Module Load() const
    {
        for (;;) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            const uintptr_t b = base.load(std::memory_order_relaxed);
            const uintptr_t s = size.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1)
                return {name, b, b ? s : 0};
        }
    }
    void Store(uintptr_t b, uintptr_t s)
    {
        const uint32_t cur = seq.load(std::memory_order_relaxed);
        seq.store(cur + 1, std::memory_order_release);
        base.store(b, std::memory_order_relaxed);
        size.store(s, std::memory_order_relaxed);
        seq.store(cur + 2, std::memory_order_release);
    }
};
ModuleSlot modules[] = {{"Gothic3.exe"}, {"Gothic3Final.exe"}, {"Engine.dll"}, {"Game.dll"}, {"Script.dll"},
                        {"Script_Game.dll"}, {"SharedBase.dll"}, {"Script_G3Together.dll"}, {"Script_paru.dll"},
                        {"steam_api.dll"}, {"steamclient.dll"}};

using UndecorateFn = DWORD(WINAPI*)(PCSTR, PSTR, DWORD, DWORD);
UndecorateFn undecorate = nullptr;
constexpr DWORD UndnameNameOnly = 0x1000;  // UNDNAME_NAME_ONLY

struct Script {
    uintptr_t at;
    char name[48];
};
Script scripts[4096];
std::atomic<LONG> scriptCount{0};
bool scriptsTaken = false;
bool tickModulesRefreshed = false;

bool RvaRange(Module const& m, uintptr_t rva, size_t size)
{
    return rva <= m.size && size <= m.size - rva;
}

struct KnownSymbol {
    uintptr_t at = 0;
    char const* name = nullptr;
};
KnownSymbol knownSymbols[4];
std::atomic<int> knownSymbolsState{0};  // 0=uninit, 1=initing, 2=ready

struct ExecRange {
    uintptr_t start = 0, end = 0;
};

void EnsureKnownSymbols()
{
    int expected = 0;
    if (!knownSymbolsState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    knownSymbols[0] = {reinterpret_cast<uintptr_t>(&enet_range_coder_compress), "enet_range_coder_compress"};
    knownSymbols[1] = {reinterpret_cast<uintptr_t>(&enet_range_coder_decompress), "enet_range_coder_decompress"};
    knownSymbols[2] = {reinterpret_cast<uintptr_t>(&enet_host_flush), "enet_host_flush"};
    knownSymbols[3] = {reinterpret_cast<uintptr_t>(&enet_host_service), "enet_host_service"};
    knownSymbolsState.store(2, std::memory_order_release);
}

bool ModuleMapped(Module const& m)
{
    if (!m.base || m.size < sizeof(IMAGE_DOS_HEADER)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(m.base), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
           reinterpret_cast<uintptr_t>(mbi.AllocationBase) == m.base;
}

IMAGE_NT_HEADERS const* Headers(Module const& m)
{
    if (!ModuleMapped(m)) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(m.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        !RvaRange(m, static_cast<uintptr_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS)))
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(m.base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

size_t CollectExecRanges(Module const& m, IMAGE_NT_HEADERS const* nt, ExecRange (&ranges)[16])
{
    if (!nt) return 0;
    if (!RvaRange(m, static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(IMAGE_FIRST_SECTION(nt)) - m.base),
                  nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)))
        return 0;
    size_t count = 0;
    IMAGE_SECTION_HEADER const* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i != nt->FileHeader.NumberOfSections && count < std::size(ranges); ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t secSize = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (!secSize || !RvaRange(m, section->VirtualAddress, 1)) continue;
        ranges[count++] = {section->VirtualAddress, section->VirtualAddress + secSize};
    }
    return count;
}

bool IsExecRva(Module const& m, uintptr_t rva, ExecRange const* ranges, size_t rangeCount)
{
    if (!RvaRange(m, rva, 1)) return false;
    for (size_t i = 0; i < rangeCount; ++i)
        if (rva >= ranges[i].start && rva < ranges[i].end) return true;
    return false;
}

char const* NearestKnownSymbol(Module const& m, uintptr_t rva, ExecRange const* ranges, size_t rangeCount, uintptr_t* start)
{
    if (knownSymbolsState.load(std::memory_order_acquire) != 2 || !IsExecRva(m, rva, ranges, rangeCount)) return nullptr;
    char const* best = nullptr;
    for (size_t i = 0; i != std::size(knownSymbols); ++i) {
        if (!knownSymbols[i].at || knownSymbols[i].at < m.base ||
            knownSymbols[i].at >= m.base + m.size)
            continue;
        const uintptr_t at = knownSymbols[i].at - m.base;
        if (!IsExecRva(m, at, ranges, rangeCount)) continue;
        if (at <= rva && (!best || at > *start)) {
            best = knownSymbols[i].name;
            *start = at;
        }
    }
    return best;
}

// The exported function that starts nearest at or below rva (forwarders and data skipped, a jmp thunk followed).
char const* NearestExport(Module const& m, IMAGE_NT_HEADERS const* nt, ExecRange const* ranges, size_t rangeCount,
                          uintptr_t rva, uintptr_t* start)
{
    if (!nt) return nullptr;
    auto const* base = reinterpret_cast<unsigned char const*>(m.base);
    IMAGE_DATA_DIRECTORY const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    if (!RvaRange(m, dir.VirtualAddress, dir.Size) ||
        !RvaRange(m, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return nullptr;
    auto const* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(base + dir.VirtualAddress);
    if (exports->NumberOfFunctions > m.size / sizeof(DWORD) ||
        exports->NumberOfNames > m.size / sizeof(DWORD) ||
        !RvaRange(m, exports->AddressOfFunctions, static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD)) ||
        !RvaRange(m, exports->AddressOfNames, static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD)) ||
        !RvaRange(m, exports->AddressOfNameOrdinals, static_cast<size_t>(exports->NumberOfNames) * sizeof(WORD)))
        return nullptr;
    auto const* functions = reinterpret_cast<DWORD const*>(base + exports->AddressOfFunctions);
    auto const* nameRvas = reinterpret_cast<DWORD const*>(base + exports->AddressOfNames);
    auto const* ordinals = reinterpret_cast<WORD const*>(base + exports->AddressOfNameOrdinals);
    char const* best = nullptr;
    for (DWORD i = 0; i != exports->NumberOfNames; ++i) {
        if (ordinals[i] >= exports->NumberOfFunctions) continue;
        uintptr_t at = functions[ordinals[i]];
        if (at >= dir.VirtualAddress && at < dir.VirtualAddress + dir.Size) continue;  // forwarder
        if (!IsExecRva(m, at, ranges, rangeCount)) continue;
        if (base[at] == 0xE9) {  // incremental-link thunk: jmp rel32
            if (!RvaRange(m, at, 5)) continue;
            int32_t relative;
            std::memcpy(&relative, base + at + 1, sizeof(relative));
            at += 5 + relative;
            if (at >= m.size || !IsExecRva(m, at, ranges, rangeCount)) continue;
        }
        if (at <= rva && (!best || at > *start)) {
            if (!RvaRange(m, nameRvas[i], 1)) continue;
            char const* name = reinterpret_cast<char const*>(base + nameRvas[i]);
            const size_t remaining = m.size - nameRvas[i];
            size_t length = 0;
            while (length != remaining && name[length]) ++length;
            if (length == remaining) continue;
            best = name;
            *start = at;
        }
    }
    return best;
}

char const* NearestScript(Module const& m, ExecRange const* ranges, size_t rangeCount, uintptr_t address, uintptr_t* start)
{
    char const* best = nullptr;
    const LONG count = scriptCount.load(std::memory_order_acquire);
    for (LONG i = 0; i != count; ++i) {
        const uintptr_t at = scripts[i].at;
        if (at < m.base || at > address || (best && at - m.base <= *start)) continue;
        if (at >= m.base + m.size || !IsExecRva(m, at - m.base, ranges, rangeCount)) continue;
        best = scripts[i].name;
        *start = at - m.base;
    }
    return best;
}

void FindModules()
{
    EnsureKnownSymbols();
    for (ModuleSlot& m : modules) {
        HMODULE h = GetModuleHandleA(m.name);
        if (!h) {
            m.Store(0, 0);
            continue;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(h);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS nt{};
        if (!symbols::ReadMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            !symbols::ReadMemory(base + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE) {
            m.Store(0, 0);
            continue;
        }
        m.Store(base, nt.OptionalHeader.SizeOfImage);
    }
}

// Once a game runs, all scripts are registered: their entry points with their names, for crash lines.
void TakeScripts()
{
    if (scriptsTaken || !gCSession::GetInstance().IsGameRunning()) return;
    scriptsTaken = true;
    FindModules();
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
    scriptCount.store(count, std::memory_order_release);
    Logf("crashlog: %d script names taken for crash lines", static_cast<int>(count));
}

}

// "Module+rva" for an address in one of the modules, else nothing; for the game's modules with " (Function+offset)".
bool Where(uintptr_t address, char* out, size_t size)
{
    for (ModuleSlot const& slot : modules) {
        const Module m = slot.Load();
        if (m.base && address >= m.base && address < m.base + m.size) {
            const uintptr_t rva = address - m.base;
            int length = Format(out, size, "%s+%#x", m.name, static_cast<unsigned>(rva));
            IMAGE_NT_HEADERS const* nt = Headers(m);
            ExecRange ranges[16]{};
            const size_t rangeCount = CollectExecRanges(m, nt, ranges);
            if (!IsExecRva(m, rva, ranges, rangeCount)) return true;
            uintptr_t start = 0, exportStart = 0, scriptStart = 0;
            char const* name = NearestKnownSymbol(m, rva, ranges, rangeCount, &start);
            char const* kind = name ? "nearest known symbol" : nullptr;
            if (char const* exportName = NearestExport(m, nt, ranges, rangeCount, rva, &exportStart);
                exportName && exportStart > start) {
                name = exportName;
                start = exportStart;
                kind = "nearest export symbol";
            }
            if (char const* script = NearestScript(m, ranges, rangeCount, address, &scriptStart); script && scriptStart > start) {
                name = script;
                start = scriptStart;
                kind = "nearest registered script";
            }
            if (!name) return true;
            char plain[160];
            if (*name == '?' && undecorate) {
                DbgHelpGuard guard(false);
                if (guard.locked && undecorate(name, plain, sizeof(plain), UndnameNameOnly)) name = plain;
            }
            Format(out + length, size - length, " (%s %s+%#x)", kind, name, static_cast<unsigned>(rva - start));
            return true;
        }
    }
    return false;
}

bool WriteProcessDump(char const* path)
{
    DumpFn fn = writeDump.load(std::memory_order_acquire);
    if (!fn) {
        HMODULE dbghelp = EnsureDbgHelp();
        fn = dbghelp ? reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        writeDump.store(fn, std::memory_order_release);
    }
    if (!fn) {
        Log("shutdown watchdog: dmp write failed (MiniDumpWriteDump unavailable)");
        return false;
    }
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Logf("shutdown watchdog: dmp create failed error=%lu", GetLastError());
        return false;
    }
    const bool ok = WriteDump(file, nullptr, false);
    CloseHandle(file);
    return ok;
}

namespace {
LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    if (shuttingDown.load(std::memory_order_acquire) || ai::ShuttingDown()) return EXCEPTION_CONTINUE_SEARCH;
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        Log("guard: crashlog missing exception record/context; cannot describe fault");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    bool gameFault = false;
    for (ModuleSlot const& slot : modules) {
        const Module module = slot.Load();
        if (module.base && fault >= module.base && fault - module.base < module.size) { gameFault = true; break; }
    }
    // Drivers can handle their own first-chance exceptions. Dumping those may block graphics teardown.
    // Unhandled exceptions from any module still reach LastChance.
    if (!gameFault) return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() != dumpWorkerThreadId) {
        if (const LONG seq = InterlockedIncrement(&firstChanceDumps); seq <= MaxFirstChanceDumps)
            Dump(info, seq);  // before anything
    }
    LogEndStep();
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
    // Return addresses (and other code pointers) on the stack, nearest first. Read via ReadProcessMemory so guard pages cannot fault.
    char stack[1024] = "CRASH stack";
    size_t length = std::strlen(stack);
    int found = 0;
    uintptr_t chunk[16]{};
    unsigned chunkCount = 0, chunkIndex = 0;
    for (int i = 0; i < 4096 && found < 32; ++i) {
        if (chunkIndex == chunkCount) {
            chunkIndex = chunkCount = 0;
            const uintptr_t addr = static_cast<uintptr_t>(c.Esp) + static_cast<size_t>(i) * sizeof(uintptr_t);
            if (i + 16 <= 4096 && symbols::ReadMemory(addr, chunk, sizeof(chunk))) chunkCount = 16;
            else if (symbols::ReadMemory(addr, chunk, sizeof(chunk[0]))) chunkCount = 1;
            else break;
        }
        const uintptr_t candidate = chunk[chunkIndex++];
        char where[240];
        if (!Where(candidate, where, sizeof(where))) continue;
        const size_t need = std::strlen(where) + 2;
        if (length + need >= sizeof(stack)) {
            Log(stack);
            strcpy_s(stack, "CRASH stack+");
            length = std::strlen(stack);
        }
        if (length + need >= sizeof(stack)) break;
        strcat_s(stack, " ");
        strcat_s(stack, where);
        length += need - 1;
        ++found;
    }
    Log(stack);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A native call that never returns leaves no dump (a hang is not a crash): say where the main thread is standing.
void LogMainThread(char const* operation)
{
    WorldEntryState state;
    const DWORD id = ReadWorldEntry(state) ? static_cast<DWORD>(state.threadId) : 0;
    symbols::ThreadSample sample;
    if (id) symbols::SampleThreadStack(id, sample);
    if (!sample.ok || !sample.resumed) {
        Logf("world-entry: main thread stack unavailable operation=%s thread=%lu", operation ? operation : "-", id);
        return;
    }
    char where[MAX_PATH + 200] = "(not in a known module)";
    Where(sample.eip, where, sizeof(where));
    Logf("world-entry: main thread stack operation=%s thread=%lu eip=%p %s esp=%p ebp=%p", operation ? operation : "-", id,
         reinterpret_cast<void*>(sample.eip), where, reinterpret_cast<void*>(sample.esp), reinterpret_cast<void*>(sample.ebp));
    // x86 engine code omits frame pointers: bounded return-address candidates, never labelled as unwound frames.
    unsigned shown = 0;
    for (unsigned i = 0; i < sample.count && shown < 32; ++i) {
        if (!Where(sample.words[i], where, sizeof(where))) continue;
        Logf("world-entry: main thread stack candidate_word=%u address=%p %s", i, reinterpret_cast<void*>(sample.words[i]), where);
        ++shown;
    }
}
}

void PollWorldEntry()
{
    WorldEntryState state;
    if (!ReadWorldEntry(state)) return;
    const auto& gate = state.gate;
    const auto& operation = state.operation;
    const uint32_t now = GetTickCount();
    if (operation.name && now - operation.since >= MainSampleMs && sampledSince.exchange(operation.since) != operation.since)
        LogMainThread(operation.name);
    if (!WorldEntryReportDue(now, operation, gate)) return;
    Logf("world-entry: still waiting for %s for %u s; operation=%s operation_ms=%u epoch=%08x welcome=%u",
         gate.name, (now - gate.since) / 1000, operation.name ? operation.name : "engine outside mod",
         operation.name ? now - operation.since : 0, state.epoch, state.welcome);
}

void EndStep(char const* step)
{
    lastEndStep.store(step);
}

void RefreshModules()
{
    FindModules();
}

void Install()
{
    InstallDumps();  // every player: last-chance minidumps
    if (installed) return;
    installed = true;
    FindModules();
    HMODULE dbghelp = EnsureDbgHelp();
    undecorate = dbghelp ? reinterpret_cast<UndecorateFn>(GetProcAddress(dbghelp, "UnDecorateSymbolName")) : nullptr;
    if (!vectoredHandler) vectoredHandler = AddVectoredExceptionHandler(1, &OnException);
    Log("crashlog: access violations are logged");
}

void Tick()
{
    Install();
    if (!tickModulesRefreshed) {
        tickModulesRefreshed = true;
        FindModules();
    }
    TakeScripts();
}

void BeginShutdown()
{
    shuttingDown.store(true, std::memory_order_release);
    if (dumpStop) SetEvent(dumpStop);
    if (vectoredHandler) {
        RemoveVectoredExceptionHandler(vectoredHandler);
        vectoredHandler = nullptr;
    }
    if (filterInstalled) {
        filterInstalled = false;
        LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(previousFilter);
        if (current != &LastChance) SetUnhandledExceptionFilter(current);
        previousFilter = nullptr;
    }
}

bool Shutdown()
{
    BeginShutdown();
    bool joined = true;
    if (dumpThreadHandle) {
        const DWORD waited = WaitForSingleObject(dumpThreadHandle, 2000);
        if (waited != WAIT_OBJECT_0) {
            Logf("crashlog: dump worker stop failed wait=%lu error=%lu; continuing teardown",
                 waited, waited == WAIT_FAILED ? GetLastError() : 0);
            joined = false;
        }
        if (joined) {
            CloseHandle(dumpThreadHandle);
            dumpThreadHandle = nullptr;
        }
    }
    if (!joined || activeDumpCallers.load(std::memory_order_acquire) != 0) return false;
    if (dumpRequest) { CloseHandle(dumpRequest); dumpRequest = nullptr; }
    if (dumpDone) { CloseHandle(dumpDone); dumpDone = nullptr; }
    if (dumpStop) { CloseHandle(dumpStop); dumpStop = nullptr; }
    return true;
}
}
