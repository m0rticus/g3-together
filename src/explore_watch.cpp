// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore.h"

#include "ai.h"
#include "guards.h"
#include "log.h"
#include "npcs.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace g3t::explore {
namespace {
// QA scriptwatch: count the calls of up to eight scripts (by the script table), with the last call's self and other.
constexpr int WatchMax = 8;
struct Watched {
    std::string name, self, other;
    gFScript original = nullptr;
    unsigned calls = 0;
};
Watched watched[WatchMax];
int watchedCount = 0;

template <int I>
GEInt GE_STDCALL WatchWrap(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return watched[I].original ? watched[I].original(spu, self, other, args) : 0;
    Watched& w = watched[I];
    ++w.calls;
    Entity s = self ? *static_cast<Entity*>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
    Entity o = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    w.self = s.GetInstance() ? s.GetName().GetText() : "-";
    w.other = o.GetInstance() ? o.GetName().GetText() : "-";
    return w.original(spu, self, other, args);
}
const gFScript watchWrappers[WatchMax] = {&WatchWrap<0>, &WatchWrap<1>, &WatchWrap<2>, &WatchWrap<3>,
                                          &WatchWrap<4>, &WatchWrap<5>, &WatchWrap<6>, &WatchWrap<7>};

// QA nativewatch: count every call of up to eight scripts at their native code, a jump from the function's first bytes
// (ai::JumpTo). scriptwatch swaps the script table's entry and so sees only calls by name; the game calls many scripts
// directly (ContinueRoutine calls CanHealSelf and SelectRoutineDestination, the routine tasks EndGoto: re/out/hero2-
// scriptgame.c, hero5-scriptgame.c), which only a jump at the code counts. Per calling NPC (self): calls, nonzero answers,
// the last answer, and how often the game's player was a puppet then (guards::Acting).
constexpr int NativeMax = 8;
struct NativeSelf {
    unsigned calls = 0, nonzero = 0, puppetPlayer = 0;
    int last = 0;
};
struct NativeWatched {
    std::string name, lastSelf, lastOther;
    gFScript original = nullptr;
    unsigned calls = 0, heroSelf = 0, mirroredSelf = 0, nonzero = 0, puppetPlayer = 0;
    std::map<std::string, NativeSelf> selves;
};
NativeWatched natives[NativeMax];
int nativeCount = 0;

template <int I>
GEInt GE_STDCALL NativeWrap(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return natives[I].original ? natives[I].original(spu, self, other, args) : 0;
    NativeWatched& w = natives[I];
    eCEntity* s = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* o = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    const bool puppetPlayer = guards::CurrentActor() != nullptr;
    const GEInt result = w.original(spu, self, other, args);
    ++w.calls;
    if (result) ++w.nonzero;
    if (puppetPlayer) ++w.puppetPlayer;
    if (s && s == gCSession::GetInstance().GetHero()) ++w.heroSelf;
    if (s && npcs::IsMirrored(s)) ++w.mirroredSelf;
    w.lastSelf = s ? Entity(s).GetName().GetText() : "-";
    w.lastOther = o ? Entity(o).GetName().GetText() : "-";
    if (w.selves.size() < 64 || w.selves.count(w.lastSelf)) {
        NativeSelf& one = w.selves[w.lastSelf];
        ++one.calls;
        if (result) ++one.nonzero;
        if (puppetPlayer) ++one.puppetPlayer;
        one.last = result;
    }
    return result;
}
const gFScript nativeWrappers[NativeMax] = {&NativeWrap<0>, &NativeWrap<1>, &NativeWrap<2>, &NativeWrap<3>,
                                            &NativeWrap<4>, &NativeWrap<5>, &NativeWrap<6>, &NativeWrap<7>};
}

void ScriptWatch(char const* name, LogFn log)
{
    char line[300];
    if (!name || !*name) {
        for (int i = 0; i < watchedCount; ++i) {
            Format(line, "SCRIPTWATCH name=%s calls=%u self=%s other=%s", watched[i].name.c_str(), watched[i].calls,
                   watched[i].self.c_str(), watched[i].other.c_str());
            log(line);
        }
        log("SCRIPTWATCH end");
        return;
    }
    for (int i = 0; i < watchedCount; ++i)
        if (watched[i].name == name) { log("SCRIPTWATCH refused already watched"); return; }
    gSScript* script = nullptr;
    if (watchedCount >= WatchMax || !GetScriptAdminExt().GetScripts().GetAt(bCString(name), script) || !script || !script->m_funcScript) {
        Format(line, "SCRIPTWATCH refused name=%s (no such script or %d watched)", name, watchedCount);
        log(line);
        return;
    }
    Watched& w = watched[watchedCount];
    w.name = name;
    w.original = script->m_funcScript;
    script->m_funcScript = watchWrappers[watchedCount++];
    ai::TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                     reinterpret_cast<void*>(w.original),
                     reinterpret_cast<void*>(script->m_funcScript), "explore script watch");
    Format(line, "SCRIPTWATCH name=%s watched=1", name);
    log(line);
}

// "NAME": watch it from now on; "reset": all counts to 0; empty: NATIVEWATCH lines, then "NATIVEWATCH end". A line's
// selves=Name:calls:nonzero:last:puppet_player, the most frequent eight.
void NativeWatch(char const* name, LogFn log)
{
    char line[700];
    if (name && !std::strcmp(name, "reset")) {
        for (int i = 0; i < nativeCount; ++i) {
            NativeWatched& w = natives[i];
            w.calls = w.heroSelf = w.mirroredSelf = w.nonzero = w.puppetPlayer = 0;
            w.selves.clear();
            w.lastSelf = w.lastOther = "-";
        }
        log("NATIVEWATCH reset=1");
        return;
    }
    // "only_NAME": the report with that caller only in selves (the eight most frequent hide a rare one).
    if (!name || !*name || !std::strncmp(name, "only_", 5)) {
        const std::string only = name && *name ? name + 5 : "";
        for (int i = 0; i < nativeCount; ++i) {
            NativeWatched const& w = natives[i];
            std::vector<std::pair<std::string, NativeSelf>> rows;
            for (auto const& row : w.selves)
                if (only.empty() || row.first == only) rows.push_back(row);
            std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second.calls > b.second.calls; });
            std::string selves;
            for (size_t k = 0; k < rows.size() && k < 8; ++k) {
                char one[96];
                Format(one, "%s%.40s:%u:%u:%d:%u", k ? "," : "", rows[k].first.c_str(), rows[k].second.calls, rows[k].second.nonzero,
                       rows[k].second.last, rows[k].second.puppetPlayer);
                selves += one;
            }
            Format(line, "NATIVEWATCH name=%s calls=%u nonzero=%u hero_self=%u mirrored_self=%u puppet_player=%u self=%.40s other=%.40s selves=%s",
                   w.name.c_str(), w.calls, w.nonzero, w.heroSelf, w.mirroredSelf, w.puppetPlayer, w.lastSelf.c_str(), w.lastOther.c_str(),
                   selves.empty() ? "-" : selves.c_str());
            log(line);
        }
        log("NATIVEWATCH end");
        return;
    }
    for (int i = 0; i < nativeCount; ++i)
        if (natives[i].name == name) { log("NATIVEWATCH refused already watched"); return; }
    gSScript* script = nullptr;
    if (nativeCount >= NativeMax || !GetScriptAdminExt().GetScripts().GetAt(bCString(name), script) || !script || !script->m_funcScript) {
        Format(line, "NATIVEWATCH refused name=%s (no such script or %d watched)", name, nativeCount);
        log(line);
        return;
    }
    // The registered function itself, in Script_Game.dll and with the prologue JumpTo can move (push ebp; mov ebp,esp;
    // and esp,-8). A function this mod jumps from already (GetAttitude, HasAttackReasonToKill) starts with a jump.
    auto* code = reinterpret_cast<unsigned char*>(script->m_funcScript);
    HMODULE game = GetModuleHandleA("Script_Game.dll");
    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(code), &owner);
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (!game || owner != game || std::memcmp(code, prologue, sizeof(prologue))) {
        Format(line, "NATIVEWATCH refused name=%s (not Script_Game.dll's own code with the expected prologue)", name);
        log(line);
        return;
    }
    NativeWatched& w = natives[nativeCount];
    w.original = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(nativeWrappers[nativeCount])));
    if (!w.original) { log("NATIVEWATCH refused the jump failed"); return; }
    w.name = name;
    w.lastSelf = w.lastOther = "-";
    ++nativeCount;
    Format(line, "NATIVEWATCH name=%s watched=1 rva=%#lx", name,
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(game)));
    log(line);
}
}
