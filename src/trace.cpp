// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace.h"

#include "ai.h"
#include "log.h"
#include "npcs.h"
#include "remote.h"
#include "symbols.h"

#include <Engine.h>
#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <cstring>

namespace g3t::trace {
namespace {
enum Mode { Off, Driven, All };
std::atomic<Mode> mode{Off};
bool installed = false;
std::mutex traceMutex;
FILE* out = nullptr;
unsigned lines = 0;
constexpr unsigned MaxLines = 50000;  // one session; the file is rewritten at every start

// Prologues of this build (position-independent, complete instructions; checked before patching):
// Engine.dll eCVisualAnimation_PS::PlayMotion RVA 0x30860: push ebp; mov ebp,esp; and esp,-8 (6 bytes)
// Game.dll gCScriptRoutine_PS::SetCurrentTask RVA 0x66100: push esi; mov esi,ecx; mov eax,[esi]; mov edx,[eax+58] (8)
// Game.dll gCScriptRoutine_PS::AISetTask RVA 0x164350: mov eax,[esp+4]; mov eax,[eax] (6)
struct Site { char const* module; uint32_t rva; unsigned char expected[8]; size_t stolen; };
const Site PlayMotionSite{"Engine.dll", 0x30860, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6};
const Site SetCurrentTaskSite{"Game.dll", 0x66100, {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0x8b, 0x50, 0x58}, 8};
const Site AISetTaskSite{"Game.dll", 0x164350, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00}, 6};

unsigned char* Locate(Site const& site)
{
    HMODULE module = GetModuleHandleA(site.module);
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + site.rva;
    return std::memcmp(code, site.expected, site.stolen) == 0 ? code : nullptr;
}

bool Wanted(eCEntity* entity)
{
    const Mode currentMode = mode.load(std::memory_order_acquire);
    if (currentMode == Off || !entity) return false;
    return currentMode == All || remote::IsPuppet(entity) || npcs::IsMirrored(entity);
}

// "Game.dll+0x16abb0" for a code address.
void Caller(void* address, char* text, size_t size)
{
    if (!symbols::DescribeImageAddress(reinterpret_cast<uintptr_t>(address), text, size))
        Format(text, size, "%p", address);
}

void Write(char const* kind, eCEntity* entity, char const* what, void* caller)
{
    char from[MAX_PATH + 16];
    Caller(caller, from, sizeof(from));
    SYSTEMTIME now;
    GetLocalTime(&now);
    Entity e(entity);
    std::lock_guard<std::mutex> lock(traceMutex);
    if (mode.load(std::memory_order_relaxed) == Off || !out || lines >= MaxLines) return;
    fprintf(out, "%02u:%02u:%02u.%03u %10lu telem=#%llu %s %s%s %s by %s\n",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetTickCount(),
            static_cast<unsigned long long>(LastTelemetrySeq()), kind, e.GetName().GetText(),
            remote::IsPuppet(entity) ? "(puppet)" : npcs::IsMirrored(entity) ? "(mirror)" : "", what, from);
    if (++lines % 50 == 0) fflush(out);
}

using MotionDesc = eCWrapper_emfx2Motion::eSMotionDesc;
using PlayMotionFn = void(__fastcall*)(eCVisualAnimation_PS*, void*, int, MotionDesc*);
PlayMotionFn playMotion = nullptr;
// The play options (eCWrapper_emfx2Motion::eSMotionDesc) are opaque in the SDK; the motion itself is the one set in
// the slot of this type, read after the call.
char const* MotionName(eCVisualAnimation_PS* self, int type)
{
    __try {
        return self->GetMotionDesc(static_cast<eCWrapper_emfx2Actor::eEMotionType>(type)).GetMotionFilename().GetText();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}
void __fastcall PlayMotionDetour(eCVisualAnimation_PS* self, void* edx, int type, MotionDesc* desc)
{
    void* caller = _ReturnAddress();
    playMotion(self, edx, type, desc);
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    if (!Wanted(entity)) return;
    char what[200];
    Format(what, "type=%d motion=%s", type, MotionName(self, type));
    Write("anim", entity, what, caller);
}

using SetTaskFn = void(__fastcall*)(gCScriptRoutine_PS*, void*, bCString const&);
SetTaskFn setCurrentTask = nullptr, aiSetTask = nullptr;
void TaskLine(char const* kind, gCScriptRoutine_PS* self, bCString const& task, void* caller)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    if (!Wanted(entity)) return;
    char what[1200];
    const int pos = Format(what, "task=%s", task.GetText());
    // AISetTask is reached through Script.dll's wrapper: the direct caller alone does not say who wants the task
    // (puppet turn, 20.10.). The frames above it, as far as the stack walk finds them, nearest first.
    if (!std::strcmp(kind, "aitask") && pos > 0 && static_cast<size_t>(pos) + 12 < sizeof(what)) {
        char chain[900] = {};
        CallerChain(2, 8, chain, sizeof(chain));
        if (chain[0] && chain[0] != '-')
            _snprintf_s(what + pos, sizeof(what) - static_cast<size_t>(pos), _TRUNCATE, " stack=%s", chain);
    }
    Write(kind, entity, what, caller);
}
void __fastcall SetCurrentTaskDetour(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
    TaskLine("task", self, task, _ReturnAddress());
    setCurrentTask(self, edx, task);
}
void __fastcall AISetTaskDetour(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
    TaskLine("aitask", self, task, _ReturnAddress());
    aiSetTask(self, edx, task);
}

void Install()
{
    if (installed) return;
    installed = true;
    if (unsigned char* code = Locate(PlayMotionSite)) playMotion = reinterpret_cast<PlayMotionFn>(ai::JumpTo(code, reinterpret_cast<void*>(&PlayMotionDetour), PlayMotionSite.stolen));
    if (unsigned char* code = Locate(SetCurrentTaskSite)) setCurrentTask = reinterpret_cast<SetTaskFn>(ai::JumpTo(code, reinterpret_cast<void*>(&SetCurrentTaskDetour), SetCurrentTaskSite.stolen));
    if (unsigned char* code = Locate(AISetTaskSite)) aiSetTask = reinterpret_cast<SetTaskFn>(ai::JumpTo(code, reinterpret_cast<void*>(&AISetTaskDetour), AISetTaskSite.stolen));
    Logf("trace: hooks play_motion=%d set_current_task=%d ai_set_task=%d", playMotion != nullptr, setCurrentTask != nullptr, aiSetTask != nullptr);
}
}

void Configure(char const* text)
{
    if (!text) text = "";
    const Mode wanted = !_stricmp(text, "all") ? All : (!_stricmp(text, "on") || !_stricmp(text, "driven")) ? Driven : Off;
    if (wanted != Off) Install();
    std::lock_guard<std::mutex> lock(traceMutex);
    if (wanted != Off) {
        if (!out) {
            char path[MAX_PATH];
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (char* slash = std::strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "g3trace.log");
            out = _fsopen(path, "w", _SH_DENYNO);  // readable while the game runs (logs, QA)
            lines = 0;
        }
    } else if (out) {
        fflush(out);
        fclose(out);
        out = nullptr;
    }
    mode.store(wanted, std::memory_order_release);
    if (out) fflush(out);
}

void Command(char const* text, LogFn log)
{
    if (text && *text) Configure(text);  // no argument: status only
    const Mode currentMode = mode.load(std::memory_order_acquire);
    unsigned currentLines = 0;
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        currentLines = lines;
    }
    char line[160];
    Format(line, "TRACE mode=%s lines=%u hooks=%d%d%d", currentMode == All ? "all" : currentMode == Driven ? "driven" : "off",
           currentLines, playMotion != nullptr, setCurrentTask != nullptr, aiSetTask != nullptr);
    log(line);
}

bool Active()
{
    if (mode.load(std::memory_order_acquire) == Off) return false;
    std::lock_guard<std::mutex> lock(traceMutex);
    return out != nullptr;
}

void Marker(uint64_t telemSeq, char const* domainEvent, char const* detail)
{
    if (mode.load(std::memory_order_acquire) == Off) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::lock_guard<std::mutex> lock(traceMutex);
    if (mode.load(std::memory_order_relaxed) == Off || !out || lines >= MaxLines) return;
    fprintf(out, "%02u:%02u:%02u.%03u %10lu telem=#%llu [%s] %s\n",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetTickCount(),
            static_cast<unsigned long long>(telemSeq), domainEvent ? domainEvent : "telem", detail ? detail : "");
    if (++lines % 25 == 0) fflush(out);
}

void CallerChain(unsigned skipFrames, unsigned maxFrames, char* outText, size_t size)
{
    if (!outText || !size) return;
    outText[0] = '\0';
    if (!maxFrames) return;
    if (maxFrames > 8) maxFrames = 8;
    void* frames[8] = {};
    const USHORT count = RtlCaptureStackBackTrace(static_cast<ULONG>(skipFrames + 1), static_cast<ULONG>(maxFrames), frames, nullptr);
    size_t pos = 0;
    for (USHORT i = 0; i < count && pos + 2 < size; ++i) {
        char frame[MAX_PATH + 16];
        Caller(frames[i], frame, sizeof(frame));
        pos += static_cast<size_t>(Format(outText + pos, size - pos, "%s%s", i ? "<" : "", frame));
    }
    if (!pos) Format(outText, size, "-");
}
}
