// SPDX-License-Identifier: GPL-3.0-or-later
#include "respawn.h"

#include "ai.h"
#include "journal.h"
#include "log.h"
#include "net.h"
#include "npcs.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace g3t::respawn {
namespace {
// Respawn (Script_Game.dll 0xac3c0, self = the dead NPC; re/findings/hero-death.md 4, re/out/hero1-scriptgame.c): a success
// sets AIMode 0, refills it (RefreshHitPoints, RefreshWeaponry), moves it to its spawn point and returns 1; a failed roll
// sets species 40 and moves it out of the world (MoveTo -220000,100000,100000); too early or the player within 30 m: nothing.
constexpr uintptr_t RespawnRva = 0xac3c0;
gFScript original = nullptr;
bool installed = false, native = false;
DWORD lastTry = 0;
bool enabled = true;  // QA negative control: respawnoff
unsigned rolls = 0, stood = 0, gone = 0, kept = 0, mirrorsSkipped = 0;

unsigned char* NativeRespawn()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0xbc, 0x03, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + RespawnRva;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

bool Dead(Entity& npc) { return npc.Routine.AIMode == gEAIMode_Dead; }

// The routine's task stayed the death task it ended with (ZS_RagDollDead): the next lethal hit set that task again, which
// does not run it again, so the animal stood at 0 hit points and never died a second time (stageRespawn at Kent, 14:08).
void EndDeathTask(Entity& npc)
{
    const bCString task = npc.Routine.GetCurrentTask();
    if (!std::strstr(task.GetText(), "Dead")) return;
    npc.Routine.FullStop();
    npc.Routine.SetTask(bCString("ZS_Ambient"));
    npc.Routine.ContinueRoutine();
    Logf("respawn: %s still had its death task %s; now %s", npc.GetName().GetText(), task.GetText(),
         npc.Routine.GetCurrentTask().GetText());
}

GEInt GE_STDCALL Detour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    eCEntity* instance = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    if (npcs::IsMirrored(instance)) { ++mirrorsSkipped; return 0; }
    if (!enabled || !instance) return original(spu, self, other, args);
    Entity npc(instance);
    const bool deadBefore = Dead(npc);
    const bCVector before = instance->GetWorldPosition();
    const GEInt result = original(spu, self, other, args);
    ++rolls;
    if (!deadBefore) return result;
    if (!Dead(npc)) {
        ++stood;
        EndDeathTask(npc);
        journal::NewLife(instance);
        uint8_t guid[16];
        std::memcpy(guid, &eCEntityProxy(instance).GetEntityID().GetCore(), 16);
        npcs::NewLife(guid);
        Logf("respawn: %s stands up on the host; a new NPC incarnation will be published", npc.GetName().GetText());
    } else if ((instance->GetWorldPosition() - before).GetMagnitude() > 10000.0f) {
        ++gone;
        npcs::Despawn(instance);
        Logf("respawn: %s does not come back; host removal published", npc.GetName().GetText());
    } else {
        ++kept;
    }
    return result;
}

}

void Tick()
{
    const DWORD now = GetTickCount();
    if (installed || !gCSession::GetInstance().IsGameRunning()) return;
    if (now - lastTry < 1000) return;
    lastTry = now;
    gSScript* script = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("Respawn"), script) || !script || !script->m_funcScript) return;
    if (unsigned char* code = NativeRespawn(); code && reinterpret_cast<void*>(script->m_funcScript) == code) {
        original = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Detour)));
        native = original != nullptr;
    }
    installed = true;
    Logf("respawn: Respawn %s", native ? "wrapped (native jump)" : "native interception unavailable on this build");
}

void Report(LogFn log)
{
    char line[220];
    Format(line, "RESPAWNSYNC enabled=%d installed=%d native=%d rolls=%u stood=%u gone=%u kept=%u mirrors_skipped=%u host_authority=%d",
              enabled ? 1 : 0, installed ? 1 : 0, native ? 1 : 0, rolls, stood, gone, kept, mirrorsSkipped, native ? 1 : 0);
    log(line);
}

void Off(LogFn log)
{
    enabled = false;
    log("RESPAWNOFF ok=1");
}
}
