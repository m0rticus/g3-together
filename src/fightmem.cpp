// SPDX-License-Identifier: GPL-3.0-or-later
#include "fightmem.h"

#include "ai.h"
#include "downed_policy.h"
#include "guards.h"
#include "guards_policy.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace g3t::fightmem {
namespace {
static_assert(downed::IsHumanoidSpecies(gESpecies_Human) && downed::IsHumanoidSpecies(gESpecies_Orc));
static_assert(!downed::IsHumanoidSpecies(gESpecies_Skeleton));
static_assert(downed::SafeQuestDefeatNeedsAttackerGuard(true, gEQuestType_Arena, gEQuestStatus_Running, false));
static_assert(downed::SafeQuestDefeatNeedsAttackerGuard(true, gEQuestType_Sparring, gEQuestStatus_Running, false));

constexpr uint8_t Kind = 18;  // net::WorldState kind (17: the epilogue, ending.cpp): guard = the player it is about, npc = LastFightAgainstPlayer
bool on = true;               // QA negative control: fightmemoff
bool apart = true;            // QA negative control (grudgeoff): the NPC keeps what it remembers of another player, no grudge
bool forget = true;           // QA negative control (grudgeloadoff): the grudges outlive a load
bool installed = false;
DWORD lastTry = 0;
unsigned sounds = 0, defeats = 0, watched = 0, ends = 0, stateRuns = 0, sent = 0, applied = 0;
unsigned defeatsSeen = 0, defeatsYes = 0;  // QA: every AssessDefeat here and those that reacted (helped, attacked, fled)
unsigned upsetAsked = 0, upsets = 0, forgotten = 0;

using Script = GEInt(GE_STDCALL*)(gCScriptProcessingUnit*, GELPVoid, GELPVoid, GEInt);
using Callback = GEBool(GE_STDCALL*)(gCScriptProcessingUnit*);
using QuestDefeatedFn = void(__fastcall*)(gCQuest_PS*, void*, eCEntity const*);
Script originalSound = nullptr, originalDefeated = nullptr, originalDefeat = nullptr, originalCancel = nullptr;
Callback originalWatch = nullptr;
QuestDefeatedFn originalQuestDefeated = nullptr;
bool questDefeatAttempted = false;
unsigned questDefeatSkipped = 0;

eCEntity* Of(gCScriptProcessingUnit* spu, GELPVoid entity, bool self)
{
    if (entity) return static_cast<Entity*>(entity)->GetInstance();
    return spu ? (self ? spu->GetSelfEntity() : spu->GetOtherEntity()) : nullptr;
}

bool IsNpc(eCEntity* entity) { return entity && entity->GetPropertySet(eEPropertySetType_NPC); }
eCEntity* TargetOf(eCEntity* npc) { return IsNpc(npc) ? Entity(npc).NPC.GetCurrentTarget().GetInstance() : nullptr; }
eCEntity* AlternativeOf(eCEntity* npc) { return IsNpc(npc) ? Entity(npc).NPC.GetAlternativeTarget().GetInstance() : nullptr; }
eCEntity* AttackerOf(eCEntity* npc) { return IsNpc(npc) ? Entity(npc).NPC.GetCurrentAttacker().GetInstance() : nullptr; }

eCEntity* PuppetAmong(std::initializer_list<eCEntity*> fighters)
{
    if (!on) return nullptr;
    for (eCEntity* e : fighters)
        if (remote::IsPuppet(e)) return e;
    return nullptr;
}

// The scripts below run as the NPC `self` does; under Acting the game's player is the puppet and IsPlayer is false for this
// machine's own hero, which then judged the fight as an onlooker (OnWatchFight: ZS_WatchFight, ZS_Applaud). Never for the hero.
eCEntity* PuppetFor(eCEntity* self, std::initializer_list<eCEntity*> fighters)
{
    return guards::IsLocalPlayer(self) ? nullptr : PuppetAmong(fighters);
}

struct Memory {
    bool valid = false;
    int value = 0;
    float at = 0.0f;
    int reason = 0;  // LastPlayerAR
};

Memory Read(eCEntity* npc)
{
    if (!IsNpc(npc) || remote::IsPuppet(npc) || guards::IsLocalPlayer(npc)) return {};
    Entity e(npc);
    return {true, static_cast<int>(e.NPC.LastFightAgainstPlayer), static_cast<float>(e.NPC.LastFightTimestamp),
            static_cast<int>(e.NPC.LastPlayerAR)};
}

void Write(eCEntity* npc, Memory const& m)
{
    Entity e(npc);
    e.NPC.LastFightAgainstPlayer = static_cast<gELastFightAgainstPlayer>(m.value);
    e.NPC.LastFightTimestamp = m.at;
    e.NPC.LastPlayerAR = static_cast<gEAttackReason>(m.reason);
}

int ReasonOf(eCEntity* npc) { return IsNpc(npc) ? static_cast<int>(Entity(npc).NPC.AttackReason) : 0; }

// The game has one memory of "the player" per NPC. On the machine that simulates the NPC, what it remembers of a fight
// with another player is kept apart (a fight given up against the puppet, CancelAttack with the puppet as the player,
// stayed in the host's NPC, where IsUpset would hold it against the host's hero); the NPC keeps this machine's player's.
std::unordered_map<std::string, Memory> grudges;  // key: the NPC's GUID and the player id

std::string Key(eCEntity* npc, int player)
{
    const eCEntityProxy proxy(npc);
    std::string key(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
    key += static_cast<char>(player);
    return key;
}

// After a script ran with `puppet` as the game's player: what the NPC now remembers is about the puppet's player. It goes
// to that player's machine (its copy of the NPC remembers the same); here the NPC gets its own player's memory back, and
// this machine keeps the other player's for the grudge (Upset). `reason`: the NPC's attack reason before the script; the
// game sets LastPlayerAR for its own player only, so the reason it fought the puppet for stands in.
void Pass(eCEntity* npc, Memory const& before, eCEntity* puppet, char const* where, int reason = 0)
{
    if (!npc || !before.valid || !puppet) return;
    Memory now = Read(npc);
    if (!now.valid || (now.value == before.value && now.at == before.at && now.reason == before.reason)) return;
    if (apart) Write(npc, before);
    if (npcs::IsMirrored(npc)) return;  // its owner keeps it
    // Only what the script changed is about the other player (an onlooker's earlier memory is this machine's player's).
    const int player = remote::PuppetOwner(puppet);
    Memory& kept = grudges[Key(npc, player)];
    const Memory was = kept.valid ? kept : Memory{true, 0, 0.0f, 0};
    kept = was;
    kept.valid = true;
    if (now.value != before.value) kept.value = now.value;
    if (now.at != before.at) kept.at = now.at;
    if (now.reason != before.reason) kept.reason = now.reason;
    else if (reason && now.value != before.value) kept.reason = reason;
    now = kept;
    if ((now.value == was.value && now.at == was.at) || !net::Connected()) return;
    net::WorldState s{};
    s.kind = Kind;
    const eCEntityProxy proxy(npc);
    std::memcpy(s.guid, &proxy.GetEntityID().GetCore(), 16);
    s.guard = static_cast<uint8_t>(remote::PuppetOwner(puppet));
    s.npc = static_cast<uint8_t>(now.value);
    net::SendWorldState(s, 0);
    ++sent;
    Logf("fightmem: %s remembers a fight with player %u: %d (%s, reason %d)", Entity(npc).GetName().GetText(), s.guard, now.value,
         where, now.reason);
}

// The scripts as the game calls them (self, other); the fight they are about is other's (AssessFightSound, AssessDefeat:
// the one hit or knocked out, and its attacker) or self's and other's (Defeat, CancelAttack).
GEInt GE_STDCALL Sound(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalSound ? originalSound(spu, self, other, args) : 0;
    eCEntity* seer = Of(spu, self, true);
    eCEntity* victim = Of(spu, other, false);
    eCEntity* puppet = PuppetFor(seer, {victim, AttackerOf(victim)});
    const Memory before = Read(seer);
    guards::Acting acting(puppet);
    const GEInt result = originalSound(spu, self, other, args);
    if (puppet) {
        ++sounds;
        Pass(seer, before, puppet, "fight sound");
    }
    return result;
}

GEInt GE_STDCALL Defeated(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalDefeated ? originalDefeated(spu, self, other, args) : 0;
    eCEntity* seer = Of(spu, self, true);
    eCEntity* down = Of(spu, other, false);
    eCEntity* attacker = AttackerOf(down);
    eCEntity* puppet = PuppetFor(seer, {down, attacker});
    const Memory before = Read(seer);
    guards::Acting acting(puppet);
    const GEInt result = originalDefeated(spu, self, other, args);
    ++defeatsSeen;
    if (result && ++defeatsYes <= 40 && seer)
        Logf("fightmem: %s saw %s knocked out by %s%s: task %s, target %s", Entity(seer).GetName().GetText(),
             down ? Entity(down).GetName().GetText() : "-", attacker ? Entity(attacker).GetName().GetText() : "-",
             puppet ? " (a puppet)" : "", Entity(seer).Routine.GetCurrentTask().GetText(),
             TargetOf(seer) ? Entity(TargetOf(seer)).GetName().GetText() : "-");
    if (puppet) {
        ++defeats;
        Pass(seer, before, puppet, "knock-out seen");
    }
    return result;
}

void __fastcall QuestDefeated(gCQuest_PS* self, void* edx, eCEntity const* entity)
{
    if (ai::ShuttingDown()) {
        if (originalQuestDefeated) originalQuestDefeated(self, edx, entity);
        return;
    }
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (self && entity && hero && entity == hero) {
        const int questType = static_cast<int>(self->GetType().GetValue());
        const int questStatus = static_cast<int>(self->GetStatus().GetValue());
        const bool hasAttacker = AttackerOf(hero) != nullptr;
        if (downed::SafeQuestDefeatNeedsAttackerGuard(true, questType, questStatus, hasAttacker)) {
            if (++questDefeatSkipped <= 40)
                Logf("fightmem: skipped gCQuest_PS::OnNPCDefeated for attackerless hero (quest=%s type=%d status=%d)",
                     self->GetName().GetText(), questType, questStatus);
            return;
        }
    }
    originalQuestDefeated(self, edx, entity);
}

void InstallQuestDefeatGuard()
{
    if (originalQuestDefeated || questDefeatAttempted || ai::ShuttingDown()) return;
    HMODULE engineGame = GetModuleHandleA("Game.dll");
    if (!engineGame) return;
    questDefeatAttempted = true;
    auto* entry = reinterpret_cast<unsigned char*>(
        GetProcAddress(engineGame, "?OnNPCDefeated@gCQuest_PS@@QAEXABVeCEntity@@@Z"));
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (!entry || std::memcmp(entry, prologue, sizeof(prologue)) != 0) {
        Log("fightmem: ANOMALY gCQuest_PS::OnNPCDefeated prologue mismatch; fallback Defeat guard active");
        return;
    }
    originalQuestDefeated = reinterpret_cast<QuestDefeatedFn>(
        ai::JumpTo(entry, reinterpret_cast<void*>(&QuestDefeated), sizeof(prologue)));
}

GEInt GE_STDCALL Defeat(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalDefeat ? originalDefeat(spu, self, other, args) : 0;
    InstallQuestDefeatGuard();
    eCEntity* a = Of(spu, self, true);
    eCEntity* b = Of(spu, other, false);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bool targetIsHero = b && hero && b == hero;
    if (targetIsHero) {
        const bool hasAttacker = AttackerOf(hero) != nullptr;
        if (downed::SafeDefeatNeedsHeroFallback(true, hasAttacker, originalQuestDefeated != nullptr)) {
            Entity victim(hero);
            victim.NPC.EnableStatusEffects(static_cast<GEInt>(gEStatusEffect_Poisoned), GEFalse);
            victim.Effect.StopEffect(GETrue);
            victim.Inventory.HoldStacks(-1, -1);
            victim.Routine.AIMode = gEAIMode_Down;
            victim.NPC.CombatState = 0;
            victim.NPC.AttackReason = gEAttackReason_None;
            Logf("fightmem: safe fallback Defeat for attackerless hero (%s)", victim.GetName().GetText());
            return 1;
        }
    }
    Entity none;
    GELPVoid winnerArg = downed::ShouldClearDefeatWinner(a != nullptr, a == b) ? &none : self;
    eCEntity* effectiveWinner = winnerArg == &none ? nullptr : a;
    eCEntity* puppet = PuppetFor(effectiveWinner, {effectiveWinner, b});
    const Memory first = Read(effectiveWinner), second = Read(b);
    guards::Acting acting(puppet);
    const GEInt result = originalDefeat(spu, winnerArg, other, args);
    if (puppet) {
        ++ends;
        Pass(effectiveWinner, first, puppet, "defeat");
        Pass(b, second, puppet, "defeat");
    }
    return result;
}

GEInt GE_STDCALL Cancel(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalCancel ? originalCancel(spu, self, other, args) : 0;
    eCEntity* a = Of(spu, self, true);
    if (ai::RetargetDownedBeforeCancel(a, spu)) return 1;
    eCEntity* puppet = PuppetFor(a, {Of(spu, other, false), TargetOf(a)});
    const Memory before = Read(a);
    const int reason = ReasonOf(a);
    guards::Acting acting(puppet);
    const GEInt result = originalCancel(spu, self, other, args);
    if (puppet) {
        ++ends;
        Pass(a, before, puppet, "fight called off", reason);
    }
    return result;
}

GEBool GE_STDCALL Watch(gCScriptProcessingUnit* spu)
{
    if (ai::ShuttingDown()) return originalWatch ? originalWatch(spu) : GEFalse;
    eCEntity* self = spu ? spu->GetSelfEntity() : nullptr;
    eCEntity* puppet = PuppetFor(self, {TargetOf(self), AlternativeOf(self)});
    const Memory before = Read(self);
    guards::Acting acting(puppet);
    const GEBool result = originalWatch(spu);
    if (puppet) {
        ++watched;
        Pass(self, before, puppet, "watched");
    }
    return result;
}

// ai.cpp, the attack states that run with a puppet as the player: each one's memory and attack reason before. A stack:
// a state runs others inside (an inner one cleared the single record of 26.10., and the running fight stayed unpassed).
struct Frame {
    eCEntity* npc;
    Memory before;
    int reason;
    eCEntity* puppet;
};
std::vector<Frame> frames;

template <typename T>
T Hook(HMODULE game, uintptr_t rva, void* detour)
{
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};  // this build
    auto* code = reinterpret_cast<unsigned char*>(game) + rva;
    if (std::memcmp(code, prologue, sizeof(prologue)) != 0) return nullptr;
    return reinterpret_cast<T>(ai::JumpTo(code, detour));
}
}

bool IsAngryFightOutcome(int value)
{
    return value == gELastFightAgainstPlayer_Lost || value == gELastFightAgainstPlayer_Cancel;
}

bool GrudgeExpired(Memory const& m, eCEntity* npc)
{
    if (m.value == gELastFightAgainstPlayer_Cancel) {
        if (m.reason == gEAttackReason_Kill || m.at <= 0.0f) return false;
        const float limit = (m.reason == gEAttackReason_Enemy || m.reason == gEAttackReason_Murder) ? 1.2f : 0.24f;
        return (Entity::GetPlayingTime() - m.at) >= limit;
    }
    if (m.value == gELastFightAgainstPlayer_Running) {
        if (!npc) return true;
        Entity e(npc);
        return e.IsDead() || e.IsDown() ||
               (e.Routine.AIMode != gEAIMode_Combat && e.NPC.CombatState == 0);
    }
    return false;
}

void PruneAndSyncGrudges()
{
    if (!net::Connected() || grudges.empty()) return;
    for (auto it = grudges.begin(); it != grudges.end();) {
        if (it->first.size() < 17) { it = grudges.erase(it); continue; }
        const uint8_t player = static_cast<uint8_t>(it->first[16]);
        const bool peerOnline = net::IsHost()
            ? net::PeerJoined(player)
            : (player == 1 || (player > 1 && player < net::MaxPlayers && net::Remotes()[player].connected));
        if (!peerOnline) { it = grudges.erase(it); continue; }
        auto const* guid = reinterpret_cast<uint8_t const*>(it->first.data());
        eCEntity* npc = npcs::NpcByGuid(guid);
        if (!GrudgeExpired(it->second, npc)) { ++it; continue; }
        const bool canSend = net::IsHost()
            ? net::InWorld(player)
            : (net::InWorld(net::LocalId()) && npc && !npcs::IsMirrored(npc));
        if (canSend) {
            net::WorldState clearMsg{};
            clearMsg.kind = Kind;
            std::memcpy(clearMsg.guid, guid, 16);
            clearMsg.guard = player;
            clearMsg.npc = static_cast<uint8_t>(gELastFightAgainstPlayer_None);
            net::SendWorldState(clearMsg, 0);
        }
        Logf("fightmem: %s cleared fight memory with player %u (was %d)",
             npc ? Entity(npc).GetName().GetText() : "-", player, it->second.value);
        it = grudges.erase(it);
    }
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    PruneAndSyncGrudges();
    if (installed) return;
    const DWORD now = GetTickCount();
    if (now - lastTry < 1000) return;
    lastTry = now;
    installed = true;
    HMODULE game = GetModuleHandleA("Script_Game.dll");
    if (!game) return;
    originalSound = Hook<Script>(game, 0x2a510, reinterpret_cast<void*>(&Sound));       // AssessFightSound
    originalDefeated = Hook<Script>(game, 0x29fb0, reinterpret_cast<void*>(&Defeated));  // AssessDefeat
    originalDefeat = Hook<Script>(game, 0x27510, reinterpret_cast<void*>(&Defeat));      // Defeat
    originalCancel = Hook<Script>(game, 0x1a4d0, reinterpret_cast<void*>(&Cancel));      // CancelAttack
    originalWatch = Hook<Callback>(game, 0x27e40, reinterpret_cast<void*>(&Watch));      // OnWatchFight
    InstallQuestDefeatGuard();
    Logf("fightmem: AssessFightSound %d, AssessDefeat %d, Defeat %d, CancelAttack %d, OnWatchFight %d, QuestDefeat %d",
         originalSound != nullptr, originalDefeated != nullptr, originalDefeat != nullptr, originalCancel != nullptr,
         originalWatch != nullptr, originalQuestDefeated != nullptr);
}

bool StumbleReactionState(char const* state)
{
    return !std::strncmp(state, "ZS_Stumble", 10) || !std::strncmp(state, "ZS_QuickStumble", 15) ||
           !std::strncmp(state, "ZS_PierceStumble", 16) || !std::strncmp(state, "ZS_SitKnockDown", 15) ||
           !std::strncmp(state, "ZS_StandUp", 10);
}

eCEntity* StateActor(eCEntity* self, char const* state)
{
    if (!on || !state || guards::IsLocalPlayer(self)) return nullptr;
    const bool attack = std::strncmp(state, "ZS_Attack", 9) == 0;
    const bool stumble = !attack && StumbleReactionState(state);
    eCEntity* target = TargetOf(self);
    eCEntity* attacker = AttackerOf(self);
    eCEntity* puppet = nullptr;
    switch (guards_policy::SelectFightStateActor(
        on, false, attack, stumble, target != nullptr,
        target && remote::IsPuppet(target), guards::IsLocalPlayer(target),
        attacker && remote::IsPuppet(attacker))) {
    case guards_policy::StateActorSource::Target: puppet = target; break;
    case guards_policy::StateActorSource::Attacker: puppet = attacker; break;
    default: return nullptr;
    }
    ++stateRuns;
    frames.push_back({self, Read(self), ReasonOf(self), puppet});
    return puppet;
}

void AfterState(eCEntity* self)
{
    if (frames.empty()) return;
    const Frame frame = frames.back();
    frames.pop_back();
    if (frame.npc == self) Pass(self, frame.before, frame.puppet, "attack", frame.reason);
}

void OnState(uint8_t from, net::WorldState const& s)
{
    if (s.kind != Kind || s.guard != net::LocalId() ||
        s.npc > static_cast<uint8_t>(gELastFightAgainstPlayer_Friendly)) return;
    eCEntity* npc = npcs::NpcByGuid(s.guid);
    if (!IsNpc(npc)) return;
    Entity e(npc);
    const int before = static_cast<int>(e.NPC.LastFightAgainstPlayer);
    e.NPC.LastFightAgainstPlayer = static_cast<gELastFightAgainstPlayer>(s.npc);
    if (s.npc == gELastFightAgainstPlayer_None) {
        e.NPC.LastFightTimestamp = 0.0f;
        e.NPC.LastPlayerAR = gEAttackReason_None;
    } else {
        e.NPC.LastFightTimestamp = Entity::GetPlayingTime();
    }
    ++applied;
    Logf("fightmem: %s remembers our fight (from player %u): %d -> %d", e.GetName().GetText(), from, before, static_cast<int>(s.npc));
}

// A loaded save holds only what its NPCs remembered then (review 26.10.: a fight given up against the client, then the host
// loaded an older save, and the NPC held it against the client on sight). Not saved: after a restart a grudge against the
// other player is gone, where the game keeps its own player's in the save.
void OnWorldChange()
{
    frames.clear();
    if (!forget || grudges.empty()) return;
    forgotten += static_cast<unsigned>(grudges.size());
    Logf("fightmem: %u grudge(s) of the world before forgotten", static_cast<unsigned>(grudges.size()));
    grudges.clear();
}

bool HasLocalOverride(eCEntity* npc)
{
    if (!IsNpc(npc)) return false;
    if (!net::IsHost() && IsAngryFightOutcome(Read(npc).value)) return true;
    const auto it = grudges.find(Key(npc, net::LocalId()));
    return it != grudges.end() && IsAngryFightOutcome(it->second.value);
}

// The game's grudge on sight (AssessActivePerception, its player's slot): an NPC that gave up a fight with its player
// (CancelAttack: Cancel and the time) takes it up again when it sees that player within a while, by the reason it fought
// for (IsUpset 0x115d0), with AssessTarget(self, player, LastPlayerAR). Here for another player whose fight this machine
// keeps: its memory in the NPC meanwhile, the puppet as the player. Only once the NPC sees it, as the game puts its player
// in that slot only when perceived (Script_Game 0x8bd0: in view with a free line of sight; upsetdiag 26.10.: the host's
// hero standing beside the NPC, out of view, was left alone).
int Upset(eCEntity* npc, eCEntity* puppet)
{
    if (!on || !apart || !IsNpc(npc) || !remote::IsPuppet(puppet)) return 0;
    auto it = grudges.find(Key(npc, remote::PuppetOwner(puppet)));
    if (it == grudges.end() || it->second.value != gELastFightAgainstPlayer_Cancel) return 0;
    const Entity self(npc), other(puppet);
    if (!self.IsInFOV(other) || !self.IsFreeLOS(other, GEFalse)) return 0;
    ++upsetAsked;
    const Memory local = Read(npc);
    const Memory kept = it->second;
    Write(npc, kept);
    GEInt upset = 0;
    {
        guards::Acting acting(puppet);
        upset = GetScriptAdminExt().CallScript(bCString("IsUpset"), npc, puppet, 0);
    }
    Write(npc, local);
    if (!upset) return 0;
    ++upsets;
    GetScriptAdminExt().CallScript(bCString("AssessTarget"), npc, puppet, kept.reason);
    Logf("fightmem: %s still holds the fight against player %d (reason %d, %.0f game minutes ago): task %s", Entity(npc).GetName().GetText(),
         remote::PuppetOwner(puppet), kept.reason, (Entity::GetPlayingTime() - kept.at) * 60.0f, Entity(npc).Routine.GetCurrentTask().GetText());
    return 1;
}

}
