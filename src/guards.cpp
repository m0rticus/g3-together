// SPDX-License-Identifier: GPL-3.0-or-later
#include "guards.h"

#include "ai.h"
#include "fightmem.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"
#include "movement.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>

namespace g3t::guards {
namespace {
// AssessActivePerception (Script_Game.dll 0x295f0): an NPC's look around (re/out/p7-activeperc-b.c). It returns 0 when
// it found nothing to react to; for the player it perceived it calls AssessPlayerZone (0x2fca0) and AssessPlayerWeapon
// (0x2f780) directly, each giving up for anyone but the player.
constexpr uintptr_t PerceiveRva = 0x295f0;
gFScript originalPerceive = nullptr;
using GetPlayerFn = Entity*(GE_STDCALL*)(Entity* result);
using IsPlayerFn = GEBool(__fastcall*)(Entity const* self, void* edx);  // thiscall, called through fastcall
GetPlayerFn originalGetPlayer = nullptr;
IsPlayerFn originalIsPlayer = nullptr;
bool installed = false;
DWORD lastTry = 0;
thread_local eCEntity* actor = nullptr;  // the puppet that stands for the game's player now
thread_local int depth = 0;
std::unordered_map<eCEntity*, DWORD> lastCheck;  // per NPC: its look around runs every frame in some states
DWORD lastZoneLook = 0;

// Script_Game.dll's import of Entity::GetPlayer (static, __stdcall, the result returned through a hidden pointer).
Entity* GE_STDCALL ActingGetPlayer(Entity* result)
{
    if (ai::ShuttingDown()) return originalGetPlayer ? originalGetPlayer(result) : result;
    if (!actor) {
        Entity* selected = originalGetPlayer(result);
        eCEntity* hero = gCSession::GetInstance().GetHero();
        if (selected && selected == result && hero && remote::IsPuppet(selected->GetInstance())) {
            selected->~Entity();
            return new (result) Entity(hero);
        }
        return selected;
    }
    return new (result) Entity(actor);
}

GEBool __fastcall ActingIsPlayer(Entity const* self, void* edx)
{
    if (ai::ShuttingDown()) return originalIsPlayer ? originalIsPlayer(self, edx) : GEFalse;
    if (!actor) {
        eCEntity* controlled = remote::ControlledBody();
        if (controlled && self && self->GetInstance() == controlled) return GETrue;
        if (self && remote::IsPuppet(self->GetInstance())) return GEFalse;
        return originalIsPlayer(self, edx);
    }
    return self && self->GetInstance() == actor;
}

// This build: push ebp; mov ebp,esp; and esp,-8; sub esp,0x6a4.
unsigned char* NativePerceive()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0xa4, 0x06, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + PerceiveRva;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

float Distance(eCEntity* a, eCEntity* b)
{
    const bCVector& p = a->GetWorldPosition();
    const bCVector& q = b->GetWorldPosition();
    const float dx = p.GetX() - q.GetX(), dy = p.GetY() - q.GetY(), dz = p.GetZ() - q.GetZ();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The two rules the game checks for its own player, now for each other player near an NPC that saw nothing else.
GEInt GE_STDCALL Perceive(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    const GEInt result = originalPerceive(spu, self, other, args);
    if (result || depth || !net::Connected()) return result;
    eCEntity* npc = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    if (!npc || !npc->GetPropertySet(eEPropertySetType_NPC) || remote::IsPuppet(npc) || npcs::IsMirrored(npc) || IsLocalPlayer(npc)) return result;
    Entity guard(npc);
    if (guard.IsDead() || guard.IsDown()) return result;
    const DWORD now = GetTickCount();
    if (lastCheck.size() > 4096) lastCheck.clear();
    DWORD& last = lastCheck[npc];
    if (static_cast<int32_t>(last - now) > 0 || now - last < 500) return result;
    last = now;
    if (static_cast<gEAIMode>(guard.Routine.AIMode) != gEAIMode_Combat) {
        for (int id = 1; id <= net::MaxPlayers; ++id) {
            eCEntity* puppet = remote::PuppetOf(id);
            if (!puppet || Distance(npc, puppet) > 3000.0f || !fightmem::Upset(npc, puppet)) continue;
            remote::NoteContact(npc, puppet);
            return 1;
        }
    }
    if (remote::IsPuppet(guard.NPC.GetCurrentTarget().GetInstance())) return result;
    for (int id = 1; id <= net::MaxPlayers; ++id) {
        eCEntity* puppet = remote::PuppetOf(id);
        if (!puppet || Distance(npc, puppet) > 3000.0f) continue;  // the game's own checks decide nearer ones
        ++depth;
        GEInt reacted = 0;
        char const* rule = nullptr;
        {
            Acting acting(puppet);
            for (char const* name : {"AssessPlayerZone", "AssessPlayerWeapon"}) {
                reacted = GetScriptAdminExt().CallScript(bCString(name), npc, puppet, 0);
                if (reacted) { rule = name + 12; break; }
            }
        }
        --depth;
        if (reacted) {
            last = now + 8000;
            remote::NoteContact(npc, puppet);
            Logf("guards: %s reacts to player %d (%s; task %s, target %s, ani state %d)", guard.GetName().GetText(), id, rule,
                 guard.Routine.GetCurrentTask().GetText(),
                 guard.NPC.GetCurrentTarget().GetInstance() ? guard.NPC.GetCurrentTarget().GetName().GetText() : "-",
                 static_cast<int>(Entity(puppet).Routine.AniState));
            return reacted;
        }
    }
    return result;
}

gFScript originalKill = nullptr;
unsigned killAsked = 0;

void KeepZone(eCEntity* puppet)
{
    static gCNavigationAdmin* admin = nullptr;
    if (!puppet) return;
    if (!admin) admin = static_cast<gCNavigationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("gCNavigationAdmin")));
    auto* navigation = static_cast<gCNavigation_PS*>(puppet->GetPropertySet(eEPropertySetType_Navigation));
    if (!admin || !navigation || navigation->GetCurrentZoneEntityProxy().GetEntity()) return;
    eCEntityProxy here;
    GEBool path = GEFalse;
    if (admin->GetZone(*puppet, here, path, GEFalse) != bEResult_Ok || !here.GetEntity()) return;
    navigation->AccessCurrentZoneEntityProxy() = here;
}

GEInt GE_STDCALL KillIntent(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    eCEntity* asked = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    eCEntity* judging = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* puppet = remote::IsPuppet(asked) && !IsLocalPlayer(judging) ? asked : nullptr;
    KeepZone(puppet);
    Acting acting(puppet);
    const GEInt result = originalKill(spu, self, other, args);
    if (puppet) {
        ++killAsked;
        if (judging && judging->GetPropertySet(eEPropertySetType_NPC) && killAsked <= 40) {
            Entity npc(judging);
            Entity zone = Entity(puppet).GetZone();
            Logf("guards: %s means to kill a puppet: %d (attack reason %d, its target %s, puppet in %s, security %d)", npc.GetName().GetText(),
                 static_cast<int>(result), static_cast<int>(npc.NPC.AttackReason),
                 remote::IsPuppet(npc.NPC.GetCurrentTarget().GetInstance()) ? "the puppet" : "another",
                 zone.GetInstance() ? zone.GetName().GetText() : "-", zone.GetInstance() ? static_cast<int>(zone.GetAIZoneSecurityLevel()) : -1);
        }
    }
    return result;
}

gFScriptAICallback originalOnZone = nullptr, originalOnWeapon = nullptr;

eCEntity* TargetPuppet(gCScriptProcessingUnit* spu)
{
    eCEntity* self = spu ? spu->GetSelfEntity() : nullptr;
    if (!self || !self->GetPropertySet(eEPropertySetType_NPC) || IsLocalPlayer(self)) return nullptr;
    eCEntity* target = Entity(self).NPC.GetCurrentTarget().GetInstance();
    return remote::IsPuppet(target) ? target : nullptr;
}

GEBool GE_STDCALL OnZone(gCScriptProcessingUnit* spu)
{
    if (ai::ShuttingDown()) return originalOnZone ? originalOnZone(spu) : GEFalse;
    Acting acting(TargetPuppet(spu));
    return originalOnZone(spu);
}

GEBool GE_STDCALL OnWeapon(gCScriptProcessingUnit* spu)
{
    if (ai::ShuttingDown()) return originalOnWeapon ? originalOnWeapon(spu) : GEFalse;
    Acting acting(TargetPuppet(spu));
    return originalOnWeapon(spu);
}

gFScriptAICallback originalOnSleep = nullptr;

GEBool GE_STDCALL OnAnimalSleep(gCScriptProcessingUnit* spu)
{
    if (ai::ShuttingDown()) return originalOnSleep ? originalOnSleep(spu) : GEFalse;
    eCEntity* animal = spu ? spu->GetSelfEntity() : nullptr;
    const GEBool result = originalOnSleep(spu);
    if (depth || !animal || !net::Connected() || !animal->GetPropertySet(eEPropertySetType_NPC) ||
        remote::IsPuppet(animal) || npcs::IsMirrored(animal))
        return result;
    Entity self(animal);
    const bCString asleep = self.Routine.GetCurrentState();
    if (!std::strstr(asleep.GetText(), "Sleep")) return result;  // the game's own player woke it
    for (int id = 1; id <= net::MaxPlayers; ++id) {
        eCEntity* puppet = remote::PuppetOf(id);
        if (!puppet || Distance(animal, puppet) > 1100.0f) continue;
        ++depth;
        GEBool again;
        {
            Acting acting(puppet);
            again = originalOnSleep(spu);
        }
        --depth;
        if (std::strcmp(self.Routine.GetCurrentState().GetText(), asleep.GetText())) {
            remote::NoteContact(animal, puppet);
            Logf("guards: %s wakes up for player %d (%.0f m; task %s, target %s)", self.GetName().GetText(), id, Distance(animal, puppet) / 100.0f,
                 self.Routine.GetCurrentTask().GetText(),
                 self.NPC.GetCurrentTarget().GetInstance() ? self.NPC.GetCurrentTarget().GetName().GetText() : "-");
            return again;
        }
    }
    return result;
}

eCEntity* WaitActor(eCEntity* self)
{
    if (!net::Connected() || remote::IsPuppet(self) || npcs::IsMirrored(self) || IsLocalPlayer(self)) return nullptr;
    Entity dest = Entity(self).Navigation.GetCurrentDestinationPoint();
    eCEntity* point = dest.GetInstance();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!point || !hero) return nullptr;
    float best = Distance(hero, point);
    eCEntity* nearest = nullptr;
    for (int id = 1; id <= net::MaxPlayers; ++id) {
        eCEntity* puppet = remote::PuppetOf(id);
        if (!puppet) continue;
        const float d = Distance(puppet, point);
        if (d < best) { best = d; nearest = puppet; }
    }
    return nearest;
}

bool FollowsPlayer(char const* state)
{
    static char const* const names[] = {"ZS_ReactToZone", "ZS_GotoPlayerZone", "ZS_ObserveZonePlayer", "ZS_ObserveSuspect",
                                        "ZS_ObserveWeapon", "ZS_ReactToWeapon", "ZS_Threaten", "ZS_ReactToSneak",
                                        "ZS_ReactToDamage"};
    for (char const* name : names)
        if (!std::strncmp(state, name, std::strlen(name))) return true;
    return false;
}
}

static void KeepChapter(eCEntity* puppet)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !puppet->GetPropertySet(eEPropertySetType_PlayerMemory) || !hero->GetPropertySet(eEPropertySetType_PlayerMemory)) return;
    Entity body(puppet), self(hero);
    const GEI32 chapter = self.PlayerMemory.Chapter;
    if (static_cast<GEI32>(body.PlayerMemory.Chapter) == chapter) return;
    body.PlayerMemory.Chapter = chapter;
}

Acting::Acting(eCEntity* puppet, bool keepChapter) : before(actor)
{
    if (puppet) {
        if (keepChapter) KeepChapter(puppet);
        actor = puppet;
    }
}

Acting::~Acting() { actor = before; }

LocalPlayer::LocalPlayer(bool local) : before(actor)
{
    if (local) {
        if (actor) movement::Touch(remote::ControlledBody(), "local-context", "puppet-context-cleared");
        actor = nullptr;
    }
}

LocalPlayer::~LocalPlayer() { actor = before; }

eCEntity* CurrentActor() { return actor; }
bool PlayerContextReady() { return originalGetPlayer && originalIsPlayer; }

bool IsLocalPlayer(eCEntity const* entity)
{
    return entity && (entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody());
}

bool RejectRemoteWrite(eCEntity const* entity, char const* writer)
{
    if (!IsLocalPlayer(entity)) return false;
    static uint32_t last = 0;
    static unsigned rejected = 0;
    const uint32_t now = GetTickCount();
    if (++rejected == 1 || now - last >= 1000) {
        last = now;
        Logf("authority: local-body-write refused writer=%s count=%u entity=%p", writer, rejected, entity);
    }
    return true;
}

eCEntity* StateActor(eCEntity* self, char const* state)
{
    if (!installed || !self || !state || !self->GetPropertySet(eEPropertySetType_NPC) || IsLocalPlayer(self)) return nullptr;
    if (const uint8_t player = npcs::ContextPlayer(self); player > 1)
        if (eCEntity* leader = remote::PuppetOf(player)) return leader;
    if (!std::strcmp(state, "ZS_WaitForFreeDestination_Loop")) return WaitActor(self);
    if (!FollowsPlayer(state) && std::strncmp(state, "ZS_Talk", 7)) return nullptr;
    eCEntity* target = Entity(self).NPC.GetCurrentTarget().GetInstance();
    if (target && remote::IsPuppet(target)) return target;
    eCEntity* attacker = Entity(self).NPC.GetCurrentAttacker().GetInstance();
    return (attacker && remote::IsPuppet(attacker)) ? attacker : nullptr;
}

static void WatchZones()
{
    const DWORD now = GetTickCount();
    if (!originalPerceive || now - lastZoneLook < 500 || !net::Connected()) return;
    lastZoneLook = now;
    for (int id = 1; id <= net::MaxPlayers; ++id) KeepZone(remote::PuppetOf(id));
}

void Tick()
{
    if (installed) {
        if (gCSession::GetInstance().IsGameRunning()) WatchZones();
        return;
    }
    if (!gCSession::GetInstance().IsGameRunning()) return;
    const DWORD now = GetTickCount();
    if (now - lastTry < 1000) return;
    lastTry = now;
    HMODULE game = GetModuleHandleA("Script_Game.dll");
    unsigned char* code = NativePerceive();
    if (!game || !code) {
        installed = true;
        Logf("guards: not installed (other build)");
        return;
    }
    originalGetPlayer = reinterpret_cast<GetPlayerFn>(ai::PatchImport(game, "Script.dll", "?GetPlayer@Entity@@SG?AV1@XZ", reinterpret_cast<void*>(&ActingGetPlayer)));
    originalIsPlayer = reinterpret_cast<IsPlayerFn>(ai::PatchImport(game, "Script.dll", "?IsPlayer@Entity@@QBE_NXZ", reinterpret_cast<void*>(&ActingIsPlayer)));
    if (originalGetPlayer && originalIsPlayer) {
        originalPerceive = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Perceive)));
        // This build: the same prologue as AssessActivePerception's first six bytes.
        static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
        auto* kill = reinterpret_cast<unsigned char*>(game) + 0x103b0;
        if (!std::memcmp(kill, prologue, sizeof(prologue))) originalKill = reinterpret_cast<gFScript>(ai::JumpTo(kill, reinterpret_cast<void*>(&KillIntent)));
        struct CallbackHook {
            char const* name;
            gFScriptAICallback replacement;
            gFScriptAICallback* original;
            char const* label;
        };
        const CallbackHook hooks[] = {
            {"OnReactToZone", &OnZone, &originalOnZone, "guards OnReactToZone callback"},
            {"OnReactToWeapon", &OnWeapon, &originalOnWeapon, "guards OnReactToWeapon callback"},
            {"OnAnimalSleep", &OnAnimalSleep, &originalOnSleep, "guards OnAnimalSleep callback"},
        };
        GE_MAP_FOR_EACH (name, callback, GetScriptAdminExt().GetScriptAICallbacks())
        {
            if (!callback || !callback->m_funcScriptAICallback) continue;
            for (auto const& hook : hooks) {
                if (std::strcmp(name.GetText(), hook.name) != 0 ||
                    callback->m_funcScriptAICallback == hook.replacement)
                    continue;
                *hook.original = callback->m_funcScriptAICallback;
                callback->m_funcScriptAICallback = hook.replacement;
                ai::TrackPointer(reinterpret_cast<void*>(&callback->m_funcScriptAICallback),
                                 reinterpret_cast<void*>(*hook.original),
                                 reinterpret_cast<void*>(hook.replacement), hook.label);
                break;
            }
        }
    }
    installed = true;
    Logf("guards: GetPlayer %s, IsPlayer %s, AssessActivePerception %s, OnReactToZone %s, OnReactToWeapon %s, "
         "HasAttackReasonToKill %s, OnAnimalSleep %s",
         originalGetPlayer ? "redirectable" : "not found", originalIsPlayer ? "redirectable" : "not found",
         originalPerceive ? "wrapped" : "not wrapped", originalOnZone ? "wrapped" : "not found", originalOnWeapon ? "wrapped" : "not found",
         originalKill ? "wrapped" : "not wrapped", originalOnSleep ? "wrapped" : "not found");
}
}
