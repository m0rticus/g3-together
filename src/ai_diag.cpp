// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai_internal.h"
#include "log.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::ai {
using namespace detail;

void AttitudeOff(void (*log)(char const*))
{
    attitudeOn = false;
    log("ATTITUDEOFF puppets are judged as ordinary NPCs again");
}
void FleeKeepOff(void (*log)(char const*))
{
    fleeKeep = false;
    log("FLEEKEEPOFF an NPC fleeing from a puppet judges the hero instead");
}

void ContinueRoutineReport(void (*log)(char const*))
{
    char line[200];
    Format(line, "CONTINUEROUTINE wrapped=%d block=%d blocked=%u run=%u player_spectator_refused=%u hero_perception_blocked=%u",
           originalContinue != nullptr, continueBlock ? 1 : 0, continueBlocked, continueRun, playerSpectatorRefused,
           heroPerceptionBlocked);
    log(line);
}
void ContinueRoutineOn(void (*log)(char const*))
{
    continueBlock = false;
    log("CONTINUEROUTINEON ok=1");
}
void MoveLengthReport(void (*log)(char const*))
{
    char line[160];
    Format(line, "MOVELENGTH wrapped=%d guard=%d calls=%u sanitized=%u unknown=%u", originalMoveLength != nullptr,
           moveLengthGuard ? 1 : 0, moveLengthCalls, moveLengthSanitized, moveLengthUnknown);
    log(line);
}
void MoveLengthOff(void (*log)(char const*))
{
    moveLengthGuard = false;
    log("MOVELENGTHOFF ok=1");
}

void Count(char const* names, void (*log)(char const*))
{
    // A name prefix: count those from now on (and start again); empty: show the counts.
    if (names && *names) {
        counted.clear();
        countPrefix = names;
    }
    char line[200];
    for (auto const& entry : counted) {
        Format(line, "AICOUNT name=%s hero=%u others=%u", entry.first.c_str(), entry.second.first, entry.second.second);
        log(line);
    }
    log("AICOUNT end");
}

void FindByRva(char const* rvaHex, void (*log)(char const*))
{
    gCScriptAdminExt& admin = GetScriptAdminExt();
    HMODULE game = GetModuleHandleA("Script_Game.dll");
    const uintptr_t want = reinterpret_cast<uintptr_t>(game) + std::strtoul(rvaHex, nullptr, 16);
    char line[200];
    unsigned found = 0;
    auto check = [&](char const* table, char const* name, void const* fn) {
        if (reinterpret_cast<uintptr_t>(fn) != want) return;
        Format(line, "SCRIPTFIND rva=%s table=%s name=%s", rvaHex, table, name);
        log(line);
        ++found;
    };
    GE_MAP_FOR_EACH (name, script, admin.GetScripts())
        if (script) check("script", name.GetText(), reinterpret_cast<void const*>(script->m_funcScript));
    GE_MAP_FOR_EACH (name, callback, admin.GetScriptAICallbacks())
        if (callback) check("callback", name.GetText(), reinterpret_cast<void const*>(callback->m_funcScriptAICallback));
    GE_MAP_FOR_EACH (name, routine, admin.GetScriptAIRoutines())
        if (routine) check("routine", name.GetText(), reinterpret_cast<void const*>(routine->m_funcScriptAIRoutine));
    for (auto const& entry : originalStates) check("state", entry.first.c_str(), reinterpret_cast<void const*>(entry.second));
    for (auto const& entry : originalFunctions) check("function", entry.first.c_str(), reinterpret_cast<void const*>(entry.second));
    Format(line, "SCRIPTFIND end rva=%s found=%u", rvaHex, found);
    log(line);
}

void Describe(char const* name, void (*log)(char const*))
{
    gCScriptAdminExt& admin = GetScriptAdminExt();
    char line[200];
    Format(line, "SCRIPTINFO name=%s script=%d callback=%d state=%d function=%d routine=%d", name,
           Find(admin.GetScripts(), name) != nullptr, Find(admin.GetScriptAICallbacks(), name) != nullptr,
           Find(admin.GetScriptAIStates(), name) != nullptr, Find(admin.GetScriptAIFunctions(), name) != nullptr,
           Find(admin.GetScriptAIRoutines(), name) != nullptr);
    log(line);
    // AI states and functions: the original entry (ours is wrapped around it).
    void* target = nullptr;
    if (auto it = originalStates.find(name); it != originalStates.end()) target = reinterpret_cast<void*>(it->second);
    else if (auto it2 = originalFunctions.find(name); it2 != originalFunctions.end()) target = reinterpret_cast<void*>(it2->second);
    if (target) {
        HMODULE owner = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(target), &owner);
        char file[MAX_PATH] = {};
        if (owner) GetModuleFileNameA(owner, file, MAX_PATH);
        char const* base = strrchr(file, '\\');
        Format(line, "SCRIPTINFO address name=%s module=%s rva=%#lx", name, base ? base + 1 : file,
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(owner)));
        log(line);
    }
    if (gSScript* script = Find(admin.GetScripts(), name)) {
        HMODULE owner = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(script->m_funcScript), &owner);
        char file[MAX_PATH] = {};
        if (owner) GetModuleFileNameA(owner, file, MAX_PATH);
        char const* base = strrchr(file, '\\');
        Format(line, "SCRIPTINFO address name=%s module=%s rva=%#lx", name, base ? base + 1 : file,
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(script->m_funcScript) - reinterpret_cast<uintptr_t>(owner)));
        log(line);
    }
}

// QA diagnostic: every registered script, AI callback, state, function and routine whose name contains text.
void ListNames(char const* text, void (*log)(char const*))
{
    gCScriptAdminExt& admin = GetScriptAdminExt();
    char line[200];
    auto scan = [&](char const* kind, auto& map) {
        GE_MAP_FOR_EACH (name, entry, map)
        {
            if (!entry || !strstr(name.GetText(), text)) continue;
            Format(line, "NAME kind=%s name=%s", kind, name.GetText());
            log(line);
        }
    };
    scan("script", admin.GetScripts());
    scan("callback", admin.GetScriptAICallbacks());
    scan("state", admin.GetScriptAIStates());
    scan("function", admin.GetScriptAIFunctions());
    scan("routine", admin.GetScriptAIRoutines());
    log("NAMES end");
}

// QA, for the inventory of what the game can do: every registered script, AI callback, state, function and routine
// with the module and RVA of its function and the source file the game registered it from (m_strSource).
void DumpNames(char const* path, void (*log)(char const*))
{
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) { log("SCRIPTDUMP refused cannot write the file"); return; }
    std::fputs("kind\tname\tmodule\trva\tsource\r\n", file);
    gCScriptAdminExt& admin = GetScriptAdminExt();
    int count = 0;
    auto dump = [&](char const* kind, auto& map, auto function) {
        GE_MAP_FOR_EACH (name, entry, map)
        {
            if (!entry) continue;
            void const* code = reinterpret_cast<void const*>(entry->*function);
            HMODULE module = nullptr;
            char moduleName[MAX_PATH] = "-";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(code), &module);
            if (module) {
                GetModuleFileNameA(module, moduleName, sizeof(moduleName));
                if (char const* slash = std::strrchr(moduleName, '\\')) std::memmove(moduleName, slash + 1, std::strlen(slash));
            }
            const uintptr_t rva = module ? reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(module) : 0;
            std::fprintf(file, "%s\t%s\t%s\t%#x\t%s\r\n", kind, name.GetText(), moduleName, static_cast<unsigned>(rva),
                         entry->m_strSource.GetText());
            ++count;
        }
    };
    dump("script", admin.GetScripts(), &gSScript::m_funcScript);
    dump("callback", admin.GetScriptAICallbacks(), &gSScriptAICallback::m_funcScriptAICallback);
    dump("state", admin.GetScriptAIStates(), &gSScriptAIState::m_funcScriptAIState);
    dump("function", admin.GetScriptAIFunctions(), &gSScriptAIFunction::m_funcScriptAIFunction);
    dump("routine", admin.GetScriptAIRoutines(), &gSScriptAIRoutine::m_funcScriptAIRoutine);
    std::fclose(file);
    char line[120];
    Format(line, "SCRIPTDUMP count=%d", count);
    log(line);
}
} // namespace g3t::ai
