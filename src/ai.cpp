// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai.h"

#include "log.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"
#include "items.h"
#include "downed.h"
#include "downed_policy.h"
#include "npc_retarget_policy.h"
#include "guards.h"
#include "fightmem.h"
#include "infocmd.h"
#include "ransack.h"
#include "npcplayback.h"
#include "npclease_runtime.h"
#include "net.h"
#include "movement.h"
#include "relation_policy.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace g3t::ai {
namespace {
char const* const Name = "CanReceivePerceptionFrom";
bool installed = false;
bool searched = false;
DWORD lastTry = 0;
unsigned blocked = 0;
gFScript originalScript = nullptr;
gFScriptAICallback originalCallback = nullptr;
// The game's script ContinueRoutine (Script_Game.dll 0x100a5ea0) ends a reaction (stumble, slide) and hands the body
// to SelectRoutineTask (0x100a9440): for a puppet that set ZS_HangAround every frame for ~3 s, which our driver undid
// every frame, and the puppet turned on the spot (stage2 red 2 of 20; trace with call chains and scriptfind, 20.10.).
// A player's puppet has no routine: its life is the player's.
gFScript originalContinue = nullptr;
gFScript originalPlayerKeys = nullptr;
bool continueBlock = true;  // QA negative control: ContinueRoutineOn
unsigned continueBlocked = 0, continueRun = 0;
// The script GetCombatMoveLength (Script_Game.dll 0xaa270), called by name from sAICombatMoveStart with other unset,
// binds the SPU's other (the fight's saved target) into a wrapper before answering from self's species and the action
// alone. A stale target crashed AttachTo there (Script.dll 0x13ea3, ~1 s after an orc died in the raid; 20.10.
// 22:56, re/findings/attachto-guard-c.md). Hand it an empty wrapper so it never touches that target.
gFScript originalMoveLength = nullptr;
bool moveLengthSearched = false;
bool moveLengthGuard = true;  // QA negative control: movelengthoff
unsigned moveLengthCalls = 0, moveLengthSanitized = 0, moveLengthUnknown = 0;
bool statesWrapped = false;
unsigned skipped = 0;
// QA aicount: how often AI states or functions whose name starts with the prefix ran, by the local hero and by anyone else.
std::string countPrefix;
std::unordered_map<std::string, std::pair<unsigned, unsigned>> counted;
unsigned unknown = 0;  // calls whose stack top named no wrapped state: must stay 0
unsigned wrapped = 0;
std::unordered_map<std::string, gFScriptAIState> originalStates;
std::unordered_map<std::string, gFScriptAIFunction> originalFunctions;
char const* const IdleTaskName = "ZS_HangAround_DoNothing";

// NPC reactions that only a villager has: watching a fight (and walking to a spot to watch it), applauding, observing a
// suspect, reacting to a weapon or a zone, small talk. A player body that runs one of them is a bug (2 Nov. play session:
// the host's hero stood in OnWatchFight, spectated a fight and applauded a death); it is put back on its routine.
bool SpectatorState(char const* name)
{
    static char const* const prefixes[] = {"ZS_WatchFight", "OnWatchFight", "ZS_Applaud", "ZS_OrcParty_Spectator", "ZS_Observe",
                                           "ZS_ReactTo", "ZS_Smalltalk", "ZS_Goblin_Smalltalk"};
    for (char const* prefix : prefixes)
        if (!std::strncmp(name, prefix, std::strlen(prefix))) return true;
    return false;
}
unsigned playerSpectatorRefused = 0, heroPerceptionBlocked = 0;
std::unordered_map<std::string, unsigned> playerStatesSeen;  // the first run of each state by the player body, for the log

// The SDK's gCScriptAdminExt::GetScript* call bTValMap::GetAt(key), which dereferences null for a missing
// name (it crashed the first lookup of an unregistered name). Look up safely instead.
template <typename T>
T* Find(bTValMap<bCString, T*>& map, char const* name)
{
    T* found = nullptr;
    return map.GetAt(bCString(name), found) ? found : nullptr;
}

bool Driven(eCEntity* entity)
{
    return entity && (remote::IsPuppet(entity) || npcs::IsMirrored(entity) || (!net::IsHost() && talk::IsHeld(entity)));
}

eCEntity* Self(gCScriptProcessingUnit* spu, GELPVoid self)
{
    if (self) return static_cast<Entity*>(self)->GetInstance();
    return spu ? spu->GetSelfEntity() : nullptr;
}

bool HostNpc(eCEntity* entity)
{
    return (net::IsHost() || npclease::LocalAuthority(entity)) && entity && entity->GetPropertySet(eEPropertySetType_NPC) &&
           !guards::IsLocalPlayer(entity) && !Driven(entity);
}

bool RejectDownedTarget(eCEntity* npc, eCEntity* target, char const* decision)
{
    if (!HostNpc(npc) || !downed::CombatUnavailable(target)) return false;
    downed::LogCombatDecision(npc, target, decision);
    return true;
}

using NextEnemy = Entity (GE_STDCALL *)(Entity);
static_assert(sizeof(Entity) == 0xA8, "Native next-enemy ABI requires an SDK Entity by value");

NextEnemy NativeNextEnemy()
{
    static bool checked = false;
    static NextEnemy nextEnemy = nullptr;
    if (checked) return nextEnemy;
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    checked = true;
    auto* base = reinterpret_cast<unsigned char*>(module);
    static const unsigned char head[] = {0x81, 0xec, 0xe0, 0x08, 0x00, 0x00, 0x53, 0x55, 0x56,
                                         0x8b, 0xb4, 0x24, 0xf0, 0x08, 0x00, 0x00, 0x57};
    static const unsigned char tail[] = {0xc2, 0xac, 0x00};
    // Script_Game+0xA070: hidden Entity return + by-value Entity, RET 0xAC.
    if (!std::memcmp(base + 0xA070, head, sizeof(head)) && !std::memcmp(base + 0xAF1B, tail, sizeof(tail))) {
        nextEnemy = reinterpret_cast<NextEnemy>(base + 0xA070);
        Log("ai: native defeated-opponent selector verified");
    } else {
        Log("ai: native defeated-opponent selector UNAVAILABLE; signature/ABI mismatch, using visible fallback only");
    }
    return nextEnemy;
}

bool EligibleOpponent(eCEntity* npc, Entity const& target)
{
    eCEntity* instance = target.GetInstance();
    const bool properties = instance && instance->GetPropertySet(eEPropertySetType_NPC) &&
                            instance->GetPropertySet(eEPropertySetType_ScriptRoutine);
    return retarget::EligibleTarget(properties, instance == npc, downed::CombatUnavailable(instance),
                                    properties ? static_cast<int>(target.Routine.AIMode) : -1) &&
           npclease::ContactAllowed(npc, instance);
}

bool ReplacementEnemy(eCEntity* npc, Entity const& target, bool visibleFallback)
{
    if (!EligibleOpponent(npc, target)) return false;
    Entity self(npc);
    if (visibleFallback && (!retarget::VisibleFallback(self.GetDistanceTo(target)) ||
                            !self.IsFreeLOS(target, GETrue))) return false;
    const int attitude = GetScriptAdminExt().CallScript(bCString("GetAttitude"), npc, target.GetInstance(), 0);
    return retarget::EnemyAttitude(attitude);
}

Entity SelectReplacement(Entity const& npc, Entity const& current, Entity const& alternative,
                         uint32_t bindings, retarget::Source& source)
{
    if (!(bindings & downed::Current) && EligibleOpponent(npc.GetInstance(), current)) {
        source = retarget::Source::Current;
        return current;
    }
    Entity native;
    if (NextEnemy select = NativeNextEnemy()) native = select(npc);
    const bool nativeValid = ReplacementEnemy(npc.GetInstance(), native, false);
    const bool alternativeValid = !nativeValid && ReplacementEnemy(npc.GetInstance(), alternative, true);
    std::array<Entity, net::MaxPlayers> actors;
    std::array<retarget::PlayerCandidate, net::MaxPlayers> players{};
    if (!nativeValid && !alternativeValid) {
        for (unsigned id = 1; id < net::MaxPlayers; ++id) {
            eCEntity* actor = id == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(id);
            if (!actor) continue;
            actors[id] = Entity(actor);
            players[id].eligible = ReplacementEnemy(npc.GetInstance(), actors[id], true);
            players[id].distanceCm = npc.GetDistanceTo(actors[id]);
        }
    }
    const retarget::Selection selected = retarget::Select(bindings, false, nativeValid, alternativeValid,
                                                         players.data(), players.size());
    source = selected.source;
    switch (source) {
    case retarget::Source::Native: return native;
    case retarget::Source::Alternative: return alternative;
    case retarget::Source::Player: return actors[selected.player];
    default: return Entity();
    }
}

thread_local eCEntity* releasingDownedNpc = nullptr;

struct DownedReleaseScope {
    eCEntity* before;
    explicit DownedReleaseScope(eCEntity* npc) : before(releasingDownedNpc) { releasingDownedNpc = npc; }
    ~DownedReleaseScope() { releasingDownedNpc = before; }
};

bool ReleaseDownedTargets(eCEntity* instance, char const* boundary, gCScriptProcessingUnit* spu = nullptr)
{
    if (instance && instance == releasingDownedNpc) return true;
    if (!HostNpc(instance) || !downed::UnavailablePlayers() ||
        !instance->GetPropertySet(eEPropertySetType_ScriptRoutine)) return false;
    Entity npc(instance);
    if (npc.IsDead() || npc.IsDown()) return false;
    const Entity current = npc.NPC.GetCurrentTarget(), alternative = npc.NPC.GetAlternativeTarget(),
                 attacker = npc.NPC.GetCurrentAttacker(), last = npc.NPC.GetLastAttacker(),
                 spell = npc.NPC.GetSpellTarget(), alignment = npc.GetAlignmentTarget();
    const Entity inflictor = instance->GetPropertySet(eEPropertySetType_DamageReceiver)
        ? npc.DamageReceiver.GetLastInflictor() : Entity();
    uint32_t bindings = 0;
    eCEntity* down = nullptr;
    auto check = [&](Entity const& target, downed::TargetBinding binding) {
        if (downed::CombatUnavailable(target.GetInstance())) {
            bindings |= binding;
            if (!down) down = target.GetInstance();
        }
    };
    check(current, downed::Current);
    check(alternative, downed::Alternative);
    check(attacker, downed::Attacker);
    check(last, downed::LastAttacker);
    check(spell, downed::Spell);
    check(alignment, downed::Alignment);
    check(inflictor, downed::Inflictor);
    if (!bindings) return false;
    DownedReleaseScope scope(instance);
    downed::LogCombatDecision(instance, down, boundary, true);
    const bool stop = downed::StopTargetInstruction(bindings);
    const bCString taskBefore = npc.Routine.GetCurrentTask(), stateBefore = npc.Routine.GetCurrentState();
    const int modeBefore = npc.Routine.AIMode, weaponBefore = gCEntity::GetCombatMode(instance);
    const unsigned combatBefore = npc.NPC.CombatState;
    const bool fighting = modeBefore == gEAIMode_Combat || combatBefore == 1 ||
        !std::strncmp(taskBefore.GetText(), "ZS_Attack", 9) ||
        !std::strncmp(taskBefore.GetText(), "ZS_RevolutionAttack", 19);
    // A combat instruction owns saved target arguments. Retire it before changing its target proxies.
    if (stop) {
        npc.Routine.FullStop();
        npc.Routine.StopAICombatMove();
        npc.Routine.StopAIGoto();
        npc.CharacterMovement.StopMovement();
    }
    Entity none;
    if (bindings & downed::Current) npc.NPC.SetCurrentTarget(none);
    if (bindings & downed::Alternative) npc.NPC.SetAlternativeTarget(none);
    if (bindings & downed::Attacker) npc.NPC.SetCurrentAttacker(none);
    if (bindings & downed::LastAttacker) npc.NPC.SetLastAttacker(none);
    if (bindings & downed::Spell) npc.NPC.SetSpellTarget(none);
    if (bindings & downed::Alignment) npc.SetAlignmentTarget(none);
    if (bindings & downed::Inflictor) npc.DamageReceiver.SetLastInflictor(none);
    if (stop) {
        if (!spu) spu = &static_cast<gCScriptRoutine_PS*>(instance->GetPropertySet(eEPropertySetType_ScriptRoutine))->GetSPU();
        if (downed::CombatUnavailable(spu->GetOtherEntity())) spu->SetOtherEntity(nullptr);
        retarget::Source source = retarget::Source::None;
        Entity next = fighting ? SelectReplacement(npc, current, alternative, bindings, source) : Entity();
        char const* reason = retarget::Reason(source);
        if (next.GetInstance()) {
            npc.SetAlignmentTarget(next);
            if (source == retarget::Source::Current) {
                npc.Routine.SetTask(bCString("ZS_Attack"));
            } else {
                // ZS_Attack_End+0x1A811 uses this same setter/assessment path before any weapon stow.
                const int attitude = GetScriptAdminExt().CallScript(bCString("GetAttitude"), instance, next.GetInstance(), 0);
                const gEAttackReason attackReason = npc.NPC.AttackReason == gEAttackReason_Revolution
                    ? gEAttackReason_Revolution : attitude == gEAttitude_Hostile
                    ? gEAttackReason_Enemy : attitude == gEAttitude_Predator ? gEAttackReason_Prey : gEAttackReason_ReactToDamage;
                const int assessed = GetScriptAdminExt().CallScript(bCString("AssessTarget"), instance, next.GetInstance(), attackReason);
                if (!assessed || npc.NPC.GetCurrentTarget().GetInstance() != next.GetInstance()) {
                    Logf("ai: downed retarget assessment failed npc=%s target=%s result=%d",
                         npc.GetName().GetText(), next.GetName().GetText(), assessed);
                    npc.NPC.SetCurrentTarget(none);
                    npc.SetAlignmentTarget(none);
                    next = none;
                    reason = "assessment-failed";
                }
            }
        }
        if (!next.GetInstance()) {
            if (fighting) {
                // Empty target skips finishing/looting a revivable player; native End owns stow/wait/relax.
                if (std::strcmp(npc.Routine.GetCurrentTask().GetText(), "ZS_Attack"))
                    npc.Routine.SetTask(bCString("ZS_Attack"));
                npc.Routine.SetState(bCString("ZS_Attack_End"));
            } else {
                npc.NPC.CombatState = 0;
                npc.Routine.ContinueRoutine();
            }
        }
        Logf("DOWNEDRETARGET npc=%.64s old=%.48s new=%.48s boundary=%s reason=%s task=%.40s->%.40s state=%.40s->%.40s ai=%d->%d combat=%u->%u weapon=%d->%d",
             npc.GetName().GetText(), down ? Entity(down).GetName().GetText() : "-",
             next.GetInstance() ? next.GetName().GetText() : "-", boundary, reason,
             taskBefore.GetText(), npc.Routine.GetCurrentTask().GetText(),
             stateBefore.GetText(), npc.Routine.GetCurrentState().GetText(), modeBefore, static_cast<int>(npc.Routine.AIMode),
             combatBefore, static_cast<unsigned>(npc.NPC.CombatState), weaponBefore, static_cast<int>(gCEntity::GetCombatMode(instance)));
    }
    downed::LogCombatDecision(instance, down, "released", true);
    return stop;
}

gFScript originalRetargetAttack = nullptr;
bool retargetAttackSearched = false;

GEInt GE_STDCALL FilterRetargetAttack(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (!ShuttingDown() && ReleaseDownedTargets(Self(spu, self), "attack-release", spu)) return 1;
    return originalRetargetAttack(spu, self, other, args);
}

void WrapRetargetAttack(gCScriptAdminExt& admin)
{
    auto* script = Find(admin.GetScripts(), "OnAttack");
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    auto* code = module ? reinterpret_cast<unsigned char*>(module) + 0x147C0 : nullptr;
    static const unsigned char head[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x03, 0x00, 0x00};
    if (script && script->m_funcScript && reinterpret_cast<void*>(script->m_funcScript) == code &&
        code && !std::memcmp(code, head, sizeof(head))) {
        originalRetargetAttack = reinterpret_cast<gFScript>(JumpTo(code, reinterpret_cast<void*>(&FilterRetargetAttack)));
        if (originalRetargetAttack) {
            script->m_funcScript = &FilterRetargetAttack;
            TrackPointer(&script->m_funcScript, reinterpret_cast<void*>(code),
                         reinterpret_cast<void*>(&FilterRetargetAttack), "ai downed OnAttack script");
            Log("ai: downed OnAttack retarget guard installed at native and script entries");
            return;
        }
    }
    Log("ai: downed OnAttack retarget guard UNAVAILABLE; native signature/entry or jump did not match");
}

void SyncDownedTargets()
{
    if (!net::IsHost() || !gCSession::GetInstance().IsGameRunning()) return;
    // Native Down selects loot before AssessTalk. Combat exclusion uses owner life truth, not puppet AIMode.
    if (!downed::UnavailablePlayers()) return;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i)
        ReleaseDownedTargets(all.GetAt(i).GetInstance(), "frame-release");
}

template <typename Function>
Function WrapPerceptionEntry(Function function, void* replacement)
{
    HMODULE owner = nullptr;
    auto* code = reinterpret_cast<unsigned char*>(function);
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (function &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(function), &owner) &&
        owner == GetModuleHandleA("Script_Game.dll") && !std::memcmp(code, prologue, sizeof(prologue))) {
        if (void* trampoline = JumpTo(code, replacement)) {
            Log("ai: damage-aware perception wrapped at native entry");
            return reinterpret_cast<Function>(trampoline);
        }
    }
    Log("ai: perception wrapped by registered entry only; native prologue did not match");
    return function;
}

GEInt GE_STDCALL FilterScript(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ShuttingDown()) return originalScript ? originalScript(spu, self, other, args) : 0;
    eCEntity* receiver = Self(spu, self);
    if (PerceptionBlocked(receiver, Driven(receiver))) { ++blocked; return 0; }
    // While another player's puppet stands for the game's player (an Acting scope), this machine's own hero is no player to the
    // scripts and would take part in the fight it hears as a villager does (ZS_WatchFight, ZS_Applaud; 2 Nov.): it perceives nothing then.
    if (receiver && guards::CurrentActor() && guards::IsLocalPlayer(receiver)) { ++heroPerceptionBlocked; return 0; }
    eCEntity* sender = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (!npclease::ContactAllowed(receiver, sender)) return 0;
    if (RejectDownedTarget(receiver, sender, "perception-refused")) return 0;
    if (receiver && receiver == detail::damageReceiver) return GETrue;
    return originalScript(spu, self, other, args);
}

// One wrapper for every AI state and AI function: the entry on top of the run-time stack names the one
// being run. Driven bodies run nothing except the idle task; everyone else runs the game's original.
template <typename Fn>
GEBool Run(std::unordered_map<std::string, Fn> const& originals, bTObjStack<gScriptRunTimeSingleState>& stack,
           gCScriptProcessingUnit* spu)
{
    char const* name = stack.GetCount() ? stack.Peek().m_strScriptName.GetText() : "";
    if (ShuttingDown()) {
        auto it = originals.find(name);
        if (it != originals.end()) return it->second(stack, spu);
        return GEFalse;
    }
    eCEntity* self = spu ? spu->GetSelfEntity() : nullptr;
    guards::LocalPlayer localPlayer(guards::IsLocalPlayer(self));
    if (downed::GuardState(self, name)) return GEFalse;
    if (npcplayback::IsPassive(self)) { ++skipped; return GEFalse; }
    if (ReleaseDownedTargets(self, "state-release", spu)) return GEFalse;
    // A mirrored NPC runs its death state only once the host reported it dead (npcs.cpp sets it then);
    // the client's own hit judgement must not decide that.
    const bool dying = (strncmp(name, "ZS_Dead", 7) == 0 || strncmp(name, "ZS_RagDollDead", 14) == 0) && npcs::DiedOnHost(self);
    // Our own blow's stagger on a mirror runs as the game's state (the client's hit feels as it does in single-player).
    const bool reacting = Driven(self) && npcs::LocalReaction(self, name);
    // A mirrored NPC in a dialog with this client's hero runs its talk states (facing, gestures).
    const bool talking = (strcmp(name, "ZS_Talk") == 0 || strcmp(name, "ZS_Talk_Loop") == 0) && talk::TalkingWithHero(self);
    // The idle task of a mirror plays its standing motions every frame and replaced every attack the owner sent
    // (first human test: the client saw 11 attack motions where the host played 147); it waits while one plays.
    const bool acting = strcmp(name, IdleTaskName) == 0 &&
                        (npcs::DiedOnHost(self) || npcs::InAction(self) || talk::InGesture(self));
    // Tried on Fable's advice (2026-10-18), no effect on attack motions: returning true here (E3) and running no idle
    // task at all for mirrors (E2). The stance's ambient loop is re-selected outside the AI states.
    if ((Driven(self) && strcmp(name, IdleTaskName) != 0 && !dying && !talking && !reacting) || acting) {
        ++skipped;
        return GEFalse;  // nothing ran; our driver puts the idle task back
    }
    auto it = originals.find(name);
    if (it == originals.end()) { ++unknown; return GEFalse; }
    if (self && guards::IsLocalPlayer(self)) {
        if (!strncmp(name, "ZS_", 3) && playerStatesSeen.size() < 200 && playerStatesSeen[name]++ == 0)
            Logf("ai: the player body %s runs %s", Entity(self).GetName().GetText(), name);
        if (SpectatorState(name)) {
            ++playerSpectatorRefused;
            if (playerSpectatorRefused <= 20) {
                eCEntity* actingFor = guards::CurrentActor();
                Logf("ai: the player body %s was put into %s (acting for %s); back to its routine", Entity(self).GetName().GetText(), name,
                     actingFor ? Entity(actingFor).GetName().GetText() : "-");
            }
            Entity body(self);
            body.Routine.FullStop();
            body.Routine.ContinueRoutine();
            return GEFalse;
        }
    }
    if (!countPrefix.empty() && !std::strncmp(name, countPrefix.c_str(), countPrefix.size())) {
        auto& c = counted[name];
        ++(self && self == gCSession::GetInstance().GetHero() ? c.first : c.second);
    }
    if (downed::RedirectDeath(self, name)) return GEFalse;  // the local hero is knocked out instead of dying
    if (strcmp(name, "IS_Running_ProcessInfo") == 0) infocmd::Observe(spu);  // a dialog script line runs
    if (strcmp(name, "ZS_RansackBody") == 0 && ransack::OnRobbing(spu)) return GEFalse;
    if (self && self == gCSession::GetInstance().GetHero() && strcmp(name, "_AI_TakeItem") == 0) {
        // The world item this hero takes vanishes into a stack (its GUID is gone afterwards): note it first.
        if (!items::BeforeTake(spu)) return GETrue;
        const GEBool result = it->second(stack, spu);
        items::AfterTake(spu);
        return result;
    }
    // A guard's reaction to another player follows that player; an attack on a puppet is a fight with its player (fightmem.cpp).
    eCEntity* actor = guards::StateActor(self, name);
    eCEntity* fighter = actor ? nullptr : fightmem::StateActor(self, name);
    guards::Acting player(actor ? actor : fighter);
    eCEntity* localHero = gCSession::GetInstance().GetHero();
    const bool localTerminalState = localHero && self == localHero && downed::Terminal() &&
                                    std::strcmp(name, "ZS_RagDollDead") == 0;
    if (localTerminalState && !downed::ObserveDeathState(self, true)) return GEFalse;
    const GEBool result = it->second(stack, spu);
    if (localTerminalState) downed::ObserveDeathState(self, false, result != GEFalse);
    if (fighter) fightmem::AfterState(self);
    downed::ObserveTerminal(self, name);
    return result;
}

GEBool GE_STDCALL FilterState(bTObjStack<gScriptRunTimeSingleState>& stack, gCScriptProcessingUnit* spu)
{
    return Run(originalStates, stack, spu);
}

GEBool GE_STDCALL FilterFunction(bTObjStack<gScriptRunTimeSingleState>& stack, gCScriptProcessingUnit* spu)
{
    return Run(originalFunctions, stack, spu);
}

void WrapStates(gCScriptAdminExt& admin)
{
    // Walk the admin's own name -> entry maps (its per-DLL arrays held a null entry in this build and
    // crashed the first attempt); swap each entry's function pointer for the filter.
    GE_MAP_FOR_EACH (name, state, admin.GetScriptAIStates())
    {
        if (!state || state->m_funcScriptAIState == &FilterState || !state->m_funcScriptAIState) continue;
        originalStates[name.GetText()] = state->m_funcScriptAIState;
        state->m_funcScriptAIState = &FilterState;
        TrackPointer(reinterpret_cast<void*>(&state->m_funcScriptAIState),
                     reinterpret_cast<void*>(originalStates[name.GetText()]),
                     reinterpret_cast<void*>(&FilterState), "ai script AI state");
        ++wrapped;
    }
    GE_MAP_FOR_EACH (name, function, admin.GetScriptAIFunctions())
    {
        if (!function || function->m_funcScriptAIFunction == &FilterFunction || !function->m_funcScriptAIFunction) continue;
        originalFunctions[name.GetText()] = function->m_funcScriptAIFunction;
        function->m_funcScriptAIFunction = &FilterFunction;
        TrackPointer(reinterpret_cast<void*>(&function->m_funcScriptAIFunction),
                     reinterpret_cast<void*>(originalFunctions[name.GetText()]),
                     reinterpret_cast<void*>(&FilterFunction), "ai script AI function");
        ++wrapped;
    }
    statesWrapped = true;
    Logf("ai: %u AI states and functions wrapped for driven bodies", wrapped);
}

// NPCs choose targets through the script GetAttitude, called by name from the perception scanner and the combat
// retarget (Script_Game.dll 0x92F0, 0xA070; re/findings/target-selection.md). The player is special only by identity
// (Entity::GetPlayer), so a puppet was an ordinary human NPC to them: attitude 0, never a target (first human test).
// Here a puppet counts as the local hero: whoever is hostile to the hero is hostile to the puppet.
gFScript originalAttitude = nullptr;
bool attitudeInstalled = false;
bool attitudeOn = true;  // QA negative control: attitudeoff
bool fleeKeep = true;    // QA negative control: fleekeepoff
unsigned attitudeAsked = 0, attitudeRedirected = 0, attitudeFleeKept = 0;

bool attitudeNative = false;
// The name-colour writers (ContinueRoutine, AssessTarget, AssessActivePerception) store GetFocusNameAttitude (Script_Game.dll
// 0xF340) = GetAttitude(self, Entity::GetPlayer()) into gCNPC_PS.AttitudeToPlayer2 through one direct call at 0xF3D7: its
// return address tells the name-colour query from every targeting query. 0: call site not verified in this build.
uintptr_t focusNameReturn = 0;
unsigned relationQueries = 0, relationScopeCleared = 0, relationDownedAllowed = 0, relationMirrorAnswered = 0;
eCEntity* redirectedFor = nullptr;  // the puppet whose GetAttitude runs as the local hero's right now
bool plainAsk = false;              // QA attchain: one question answered without the redirect (as attitudeoff would)
thread_local unsigned playerAttitudeQueries = 0;

// Script_Game.dll of this build: GetAttitude at RVA 0xE3B0 starts with push ebp; mov ebp,esp; and esp,-8 (6 bytes
// of complete, position-independent instructions), then sub esp,0x354.
unsigned char* NativeAttitude()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x03, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + 0xE3B0;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

// GetFocusNameAttitude (0xF340: push ebp; mov ebp,esp; and esp,-8; sub esp,0x1fc) calls GetAttitude at 0xF3D7 (E8 rel32 to
// 0xE3B0; the return address is 0xF3DC). Checked against the function start, which is not patched yet or already jumps.
uintptr_t VerifiedFocusNameReturn()
{
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0xfc, 0x01, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return 0;
    auto* base = reinterpret_cast<unsigned char*>(module);
    unsigned char const* call = base + 0xF3D7;
    int32_t relative = 0;
    std::memcpy(&relative, call + 1, sizeof(relative));
    if (std::memcmp(base + 0xF340, prologue, sizeof(prologue)) != 0 || call[0] != 0xE8 ||
        call + 5 + relative != base + 0xE3B0) return 0;
    return reinterpret_cast<uintptr_t>(call + 5);
}

}

namespace {
volatile LONG shutdownFlag = 0;

struct PatchRecord {
    void* address;
    size_t size;
    std::vector<unsigned char> original;
    std::vector<unsigned char> installed;
    char const* name;
};

std::vector<PatchRecord>& PatchRegistry()
{
    static std::vector<PatchRecord> registry;
    return registry;
}

void RestoreOne(PatchRecord& patch, PatchRestoreResult& result)
{
    std::vector<unsigned char> current(patch.size);
    std::memcpy(current.data(), patch.address, patch.size);
    if (std::memcmp(current.data(), patch.installed.data(), patch.size) != 0) {
        ++result.preserved;
        Logf("ai: patch preserved; %s bytes changed after installation", patch.name ? patch.name : "unnamed");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(patch.address, patch.size, PAGE_EXECUTE_READWRITE, &old)) {
        ++result.failed;
        Logf("ai: patch restore failed (protect); %s error=%lu", patch.name ? patch.name : "unnamed", GetLastError());
        return;
    }
    std::memcpy(patch.address, patch.original.data(), patch.size);
    DWORD ignored = 0;
    if (!VirtualProtect(patch.address, patch.size, old, &ignored)) {
        ++result.failed;
        Logf("ai: patch restore failed (restore protection); %s error=%lu", patch.name ? patch.name : "unnamed", GetLastError());
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), patch.address, patch.size);
    ++result.restored;
}
}

void BeginShutdown()
{
    InterlockedExchange(&shutdownFlag, 1);
}

bool ShuttingDown()
{
    return InterlockedCompareExchange(&shutdownFlag, 0, 0) != 0;
}

void TrackPatch(void* address, void const* original, void const* installed, size_t size, char const* name)
{
    if (!address || !original || !installed || !size) return;
    PatchRecord patch;
    patch.address = address;
    patch.size = size;
    patch.original.assign(static_cast<unsigned char const*>(original),
                          static_cast<unsigned char const*>(original) + size);
    patch.installed.assign(static_cast<unsigned char const*>(installed),
                           static_cast<unsigned char const*>(installed) + size);
    patch.name = name;
    PatchRegistry().push_back(std::move(patch));
}

void TrackPointer(void* slot, void* original, void* installed, char const* name)
{
    if (!slot) return;
    void* originalBytes[1] = {original};
    void* installedBytes[1] = {installed};
    TrackPatch(slot, originalBytes, installedBytes, sizeof(void*), name);
}

void* PatchPointer(void** slot, void* replacement, char const* name)
{
    if (!slot) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* previous = *slot;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    TrackPointer(slot, previous, replacement, name);
    return previous;
}

PatchRestoreResult RestorePatches()
{
    PatchRestoreResult result = {};
    auto& registry = PatchRegistry();
    for (size_t i = registry.size(); i != 0; --i)
        RestoreOne(registry[i - 1], result);
    registry.clear();
    Logf("ai: patches removed restored=%u preserved=%u failed=%u", result.restored, result.preserved, result.failed);
    return result;
}

// Points one import of `module` (by DLL and function name) at `replacement`; returns what it pointed at.
void* PatchImport(void* module, char const* dll, char const* function, void* replacement)
{
    auto* base = static_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char const*>(base + imp->Name), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(byName->Name), function)) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return nullptr;
            void* previous = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<DWORD>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &slots->u1.Function, sizeof(slots->u1.Function));
            TrackPointer(reinterpret_cast<void*>(&slots->u1.Function), previous, replacement, "import patch");
            return previous;
        }
    }
    return nullptr;
}

// Jump from `target` to a small shutdown-aware guard and onward to `detour`.
// The returned trampoline runs the stolen bytes and continues in the original function.
void* JumpTo(unsigned char* target, void* detour, size_t stolen)
{
    if (!target || !detour || stolen < 5 || stolen > 27) return nullptr;
    if (ShuttingDown()) return nullptr;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    constexpr size_t originalOffset = 24;
    std::memcpy(trampoline + originalOffset, target, stolen);
    trampoline[originalOffset + stolen] = 0xE9;
    const int32_t back = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(target + stolen) -
        reinterpret_cast<intptr_t>(trampoline + originalOffset + stolen + 5));
    std::memcpy(trampoline + originalOffset + stolen + 1, &back, 4);

    // pushfd/cmp/conditional/popfd/jmp: no general register is changed, and EFLAGS is restored
    // before either tail jump. 0x74 is taken only while the shutdown flag is clear.
    trampoline[0] = 0x9C;
    trampoline[1] = 0x83;
    trampoline[2] = 0x3D;
    const uintptr_t flag = reinterpret_cast<uintptr_t>(&shutdownFlag);
    std::memcpy(trampoline + 3, &flag, sizeof(flag));
    trampoline[7] = 0;
    trampoline[8] = 0x74;
    trampoline[9] = 6; // normal popfd/jmp at 16
    trampoline[10] = 0x9D;
    trampoline[11] = 0xE9;
    const int32_t originalJump = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(trampoline + originalOffset) -
        reinterpret_cast<intptr_t>(trampoline + 16));
    std::memcpy(trampoline + 12, &originalJump, 4);
    trampoline[16] = 0x9D;
    trampoline[17] = 0xE9;
    const int32_t detourJump = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(detour) - reinterpret_cast<intptr_t>(trampoline + 22));
    std::memcpy(trampoline + 18, &detourJump, 4);

    DWORD old = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    unsigned char original[27];
    unsigned char installed[27];
    std::memcpy(original, target, stolen);
    target[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(trampoline) - reinterpret_cast<intptr_t>(target + 5));
    std::memcpy(target + 1, &to, 4);
    for (size_t i = 5; i < stolen; ++i) target[i] = 0x90;
    std::memcpy(installed, target, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    TrackPatch(target, original, installed, stolen, "inline JumpTo");
    return trampoline + originalOffset;
}

namespace {

bool LedByRemotePlayer(eCEntity* npc)
{
    const uint8_t owner = npcs::CompanionOwner(npc);
    return owner && owner != net::LocalId();
}

// The relation the game shows in the name tag and every later script reads. It is the NPC's relation to this machine's
// player: a downed player is no reason to forget it, a puppet standing in for the player (Acting scope) is not whom the tag
// is for, and a passive mirror has the host's relation, never this machine's own script's.
GEInt FocusNameAttitude(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    ++relationQueries;
    eCEntity* judge = Self(spu, self);
    int hostAttitude = 0;
    if (judge && npcs::IsMirrored(judge) && npcs::HostAttitude(judge, hostAttitude)) {
        ++relationMirrorAnswered;
        return hostAttitude;
    }
    eCEntity* actor = guards::CurrentActor();
    if (judge && relation::FocusNameContext(actor != nullptr, actor && LedByRemotePlayer(judge)) == relation::Context::LocalPlayer) {
        ++relationScopeCleared;
        guards::LocalPlayer local(true);
        Entity player(remote::ControlledBody());
        if (player.GetInstance() && player.GetInstance() != judge)
            return originalAttitude(spu, self, &player, args);
    }
    eCEntity* target = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (HostNpc(judge) && downed::CombatUnavailable(target)) ++relationDownedAllowed;
    return originalAttitude(spu, self, other, args);
}

GEInt GE_STDCALL FilterAttitude(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ShuttingDown()) return originalAttitude ? originalAttitude(spu, self, other, args) : 0;
    if (focusNameReturn && reinterpret_cast<uintptr_t>(_ReturnAddress()) == focusNameReturn)
        return FocusNameAttitude(spu, self, other, args);
    eCEntity* target = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (relation::RefuseDownedAttitude(false, true) && RejectDownedTarget(Self(spu, self), target, "attitude-refused")) return gEAttitude_None;
    if (playerAttitudeQueries) return originalAttitude(spu, self, other, args);
    if (target && remote::IsPuppet(target)) {
        ++attitudeAsked;
        eCEntity* judge = Self(spu, self);
        remote::NoteContact(judge, target);
        // A scoped remote player must remain the same player for GetPlayer, IsPlayer and attitude checks.
        if (guards::CurrentActor() == target) return originalAttitude(spu, self, other, args);
        eCEntity* hero = gCSession::GetInstance().GetHero();
        if (attitudeOn && !plainAsk && hero && judge && judge != hero && !Driven(judge)) {
            // GetAttitude answers Panic when self flees and its current target is other. With the hero in place of
            // the puppet that check fails, and an NPC fleeing from the puppet turned on it (re/findings/attitude-callers.md).
            Entity npc(judge);
            if (fleeKeep && judge->GetPropertySet(eEPropertySetType_NPC) && npc.Routine.AIMode == gEAIMode_Flee &&
                npc.NPC.GetCurrentTarget().GetInstance() == target) {
                ++attitudeFleeKept;
                return originalAttitude(spu, self, other, args);
            }
            ++attitudeRedirected;
            Entity asHero(hero);
            eCEntity* const before = redirectedFor;
            redirectedFor = target;
            const GEInt result = originalAttitude(spu, self, &asHero, args);
            redirectedFor = before;
            return result;
        }
    }
    return originalAttitude(spu, self, other, args);
}

GEInt GE_STDCALL FilterContinue(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ShuttingDown()) return originalContinue ? originalContinue(spu, self, other, args) : 0;
    guards::LocalPlayer localPlayer(guards::IsLocalPlayer(Self(spu, self)));
    if (downed::GuardState(Self(spu, self), "ContinueRoutine")) return 0;
    if (continueBlock && remote::IsPuppet(Self(spu, self))) { ++continueBlocked; return 0; }
    ++continueRun;
    return originalContinue(spu, self, other, args);
}

GEInt GE_STDCALL FilterPlayerKeys(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    // This is the local keyboard dispatcher; its script self can be unset.
    guards::LocalPlayer localPlayer;
    movement::KeyDispatch(args, downed::Held() || downed::Terminal());
    if (downed::Held() || downed::Terminal()) return 0;
    return originalPlayerKeys(spu, self, other, args);
}

gFScriptAIRoutine originalPlayerRoutine = nullptr;

GEBool GE_STDCALL FilterPlayerRoutine(gCScriptProcessingUnit* spu)
{
    guards::LocalPlayer localPlayer(guards::IsLocalPlayer(spu ? spu->GetSelfEntity() : nullptr));
    if (!ShuttingDown() && downed::GuardState(spu ? spu->GetSelfEntity() : nullptr, "Rtn_Player")) return GETrue;
    const GEBool result = originalPlayerRoutine(spu);
    downed::ObserveTerminal(spu ? spu->GetSelfEntity() : nullptr, "Rtn_Player");
    return result;
}

GEInt GE_STDCALL SafeMoveLength(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ShuttingDown()) return originalMoveLength ? originalMoveLength(spu, self, other, args) : 0;
    ++moveLengthCalls;
    // Other feeds no branch of the original; a valid empty wrapper takes its copy branch instead of GetOtherEntity.
    Entity noOther;
    if (moveLengthGuard) ++moveLengthSanitized;
    const GEInt length = originalMoveLength(spu, self, moveLengthGuard ? &noOther : other, args);
    if (length < 0) ++moveLengthUnknown;  // the original answers -1 for actions it has no length for
    return length;
}

GEBool GE_STDCALL FilterCallback(gCScriptProcessingUnit* spu)
{
    if (ShuttingDown()) return originalCallback ? originalCallback(spu) : GEFalse;
    eCEntity* receiver = spu ? spu->GetSelfEntity() : nullptr;
    if (PerceptionBlocked(receiver, Driven(receiver))) { ++blocked; return GEFalse; }
    if (receiver && guards::CurrentActor() && guards::IsLocalPlayer(receiver)) { ++heroPerceptionBlocked; return GEFalse; }
    if (RejectDownedTarget(receiver, spu ? spu->GetOtherEntity() : nullptr, "perception-refused")) return GEFalse;
    if (receiver && receiver == detail::damageReceiver) return GETrue;
    return originalCallback(spu);
}

gFScript originalAssessTarget = nullptr;
bool assessTargetSearched = false;

GEInt GE_STDCALL FilterAssessTarget(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ShuttingDown()) return originalAssessTarget ? originalAssessTarget(spu, self, other, args) : 0;
    eCEntity* target = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (RejectDownedTarget(Self(spu, self), target, "assessment-refused")) return 0;
    return originalAssessTarget(spu, self, other, args);
}

void WrapAssessTarget(gCScriptAdminExt& admin)
{
    auto* script = Find(admin.GetScripts(), "AssessTarget");
    if (!script || !script->m_funcScript) {
        Log("ai: AssessTarget not found; downed target native assessment guard unavailable");
        return;
    }
    const gFScript native = script->m_funcScript;
    originalAssessTarget = native;
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    auto* code = module ? reinterpret_cast<unsigned char*>(module) + 0x31470 : nullptr;
    static const unsigned char head[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x01, 0x00, 0x00};
    bool nativeGuard = false;
    if (code && reinterpret_cast<void*>(native) == code && !std::memcmp(code, head, sizeof(head))) {
        if (void* trampoline = JumpTo(code, reinterpret_cast<void*>(&FilterAssessTarget))) {
            originalAssessTarget = reinterpret_cast<gFScript>(trampoline);
            nativeGuard = true;
        }
    }
    script->m_funcScript = &FilterAssessTarget;
    TrackPointer(reinterpret_cast<void*>(&script->m_funcScript), reinterpret_cast<void*>(native),
                 reinterpret_cast<void*>(&FilterAssessTarget), "ai downed AssessTarget script");
    Log(nativeGuard ? "ai: downed AssessTarget guard installed at native and script entries"
                    : "ai: downed AssessTarget guard SCRIPT ONLY; native address/prologue or jump unavailable");
}
}

bool PerceptionFiltered() { return installed; }
bool AttitudeInstalled() { return attitudeInstalled; }
bool AttitudeNative() { return attitudeNative; }
eCEntity* RedirectedFor() { return redirectedFor; }
int PlainAttitude(eCEntity* self, eCEntity* other)
{
    plainAsk = true;
    const GEInt result = GetScriptAdminExt().CallScript(bCString("GetAttitude"), self, other, 0);
    plainAsk = false;
    return static_cast<int>(result);
}
bool TryPlayerAttitude(eCEntity* self, eCEntity* player, int& attitude)
{
    if (!self || !player || !originalAttitude || !guards::PlayerContextReady()) return false;
    guards::Acting acting(player, false);
    struct Query {
        Query() { ++playerAttitudeQueries; }
        ~Query() { --playerAttitudeQueries; }
    } query;
    Entity npc(self), claimant(player);
    attitude = static_cast<int>(originalAttitude(nullptr, &npc, &claimant, 0));
    return true;
}
unsigned AttitudeAsked() { return attitudeAsked; }
bool LocalAttitude(eCEntity* self, int& attitude)
{
    if (!self || !originalAttitude || ShuttingDown() || !guards::PlayerContextReady()) return false;
    guards::LocalPlayer local(true);
    Entity player(remote::ControlledBody());
    if (!player.GetInstance() || player.GetInstance() == self) return false;
    struct Query {
        Query() { ++playerAttitudeQueries; }
        ~Query() { --playerAttitudeQueries; }
    } query;
    Entity npc(self);
    attitude = static_cast<int>(originalAttitude(nullptr, &npc, &player, 0));
    return true;
}
void RelationStats(unsigned& queries, unsigned& scopeCleared, unsigned& downedAllowed, unsigned& mirrorAnswered)
{
    queries = relationQueries;
    scopeCleared = relationScopeCleared;
    downedAllowed = relationDownedAllowed;
    mirrorAnswered = relationMirrorAnswered;
}
bool RelationHooked() { return focusNameReturn != 0; }
unsigned AttitudeRedirected() { return attitudeRedirected; }
unsigned AttitudeFleeKept() { return attitudeFleeKept; }
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
unsigned BlockedCount() { return blocked; }
unsigned SkippedCount() { return skipped; }
unsigned WrappedCount() { return wrapped; }
unsigned UnknownCount() { return unknown; }
void ContinueRoutineReport(void (*log)(char const*))
{
    char line[200];
    Format(line, "CONTINUEROUTINE wrapped=%d block=%d blocked=%u run=%u player_spectator_refused=%u hero_perception_blocked=%u", originalContinue != nullptr, continueBlock ? 1 : 0,
              continueBlocked, continueRun, playerSpectatorRefused, heroPerceptionBlocked);
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

bool RetargetDownedBeforeCancel(eCEntity* npc, void* spu)
{
    return ReleaseDownedTargets(npc, "cancel-release", static_cast<gCScriptProcessingUnit*>(spu));
}

void Tick()
{
    SyncDownedTargets();
    if (!retargetAttackSearched && gCSession::GetInstance().IsGameRunning()) {
        retargetAttackSearched = true;
        NativeNextEnemy();
        WrapRetargetAttack(GetScriptAdminExt());
    }
    if (!assessTargetSearched && gCSession::GetInstance().IsGameRunning()) {
        assessTargetSearched = true;
        WrapAssessTarget(GetScriptAdminExt());
    }

    if (!attitudeInstalled && gCSession::GetInstance().IsGameRunning()) {
        if (auto* script = Find(GetScriptAdminExt().GetScripts(), "GetAttitude")) {
            // The registered function is the native one 29 functions call directly (OnAttack, AssessTarget, damage
            // rules, ...); swapping the table entry only reaches calls by name. Jump from the function itself.
            if (unsigned char* native = NativeAttitude(); native && reinterpret_cast<void*>(script->m_funcScript) == native) {
                originalAttitude = reinterpret_cast<gFScript>(JumpTo(native, reinterpret_cast<void*>(&FilterAttitude)));
                attitudeNative = originalAttitude != nullptr;
            }
            if (!attitudeNative) {
                originalAttitude = script->m_funcScript;
                script->m_funcScript = &FilterAttitude;
                TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                             reinterpret_cast<void*>(originalAttitude),
                             reinterpret_cast<void*>(&FilterAttitude), "ai GetAttitude script");
            }
            attitudeInstalled = true;
            if (attitudeNative) focusNameReturn = VerifiedFocusNameReturn();
            Logf("ai: GetAttitude wrapped (%s; puppets count as the local hero)", attitudeNative ? "native jump" : "script table only");
            Logf("ai: name-colour writers (GetFocusNameAttitude call site) %s",
                 focusNameReturn ? "verified: the local relation is stored, never None for a downed player, never a puppet's"
                                 : "NOT verified in this build; relation fixes inactive");
        }
    }
    if (gCSession::GetInstance().IsGameRunning()) downed::InstallTerminalGuards();
    if ((installed || searched) && statesWrapped && originalContinue && originalPlayerKeys &&
        originalPlayerRoutine && moveLengthSearched) return;
    const DWORD now = GetTickCount();
    if (now - lastTry < 1000) return;
    lastTry = now;
    gCScriptAdminExt& admin = GetScriptAdminExt();
    // The game's scripts are registered once the game runs; wrap them then (our DLL loads first).
    if (!statesWrapped && gCSession::GetInstance().IsGameRunning()) WrapStates(admin);
    if (!originalPlayerKeys && gCSession::GetInstance().IsGameRunning()) {
        if (auto* script = Find(admin.GetScripts(), "OnPlayerGameKeyPressed"); script && script->m_funcScript) {
            originalPlayerKeys = script->m_funcScript;
            script->m_funcScript = &FilterPlayerKeys;
            TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                         reinterpret_cast<void*>(originalPlayerKeys),
                         reinterpret_cast<void*>(&FilterPlayerKeys), "downed player gameplay keys");
            Log("downed: gameplay input guard installed");
        }
    }
    if (!originalPlayerRoutine && gCSession::GetInstance().IsGameRunning()) {
        if (auto* routine = Find(admin.GetScriptAIRoutines(), "Rtn_Player"); routine && routine->m_funcScriptAIRoutine) {
            originalPlayerRoutine = routine->m_funcScriptAIRoutine;
            routine->m_funcScriptAIRoutine = &FilterPlayerRoutine;
            TrackPointer(reinterpret_cast<void*>(&routine->m_funcScriptAIRoutine),
                         reinterpret_cast<void*>(originalPlayerRoutine),
                         reinterpret_cast<void*>(&FilterPlayerRoutine), "downed Rtn_Player");
            Log("downed: Rtn_Player terminal guard installed");
        }
    }
    if (!originalContinue && gCSession::GetInstance().IsGameRunning()) {
        if (auto* script = Find(admin.GetScripts(), "ContinueRoutine"); script && script->m_funcScript && script->m_funcScript != &FilterContinue) {
            originalContinue = script->m_funcScript;
            script->m_funcScript = &FilterContinue;
            TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                         reinterpret_cast<void*>(originalContinue),
                         reinterpret_cast<void*>(&FilterContinue), "ai ContinueRoutine script");
            Log("ai: ContinueRoutine wrapped (puppets keep no routine)");
        }
    }
    if (!moveLengthSearched && gCSession::GetInstance().IsGameRunning()) {
        moveLengthSearched = true;
        auto* script = Find(admin.GetScripts(), "GetCombatMoveLength");
        if (script && script->m_funcScript && script->m_funcScript != &SafeMoveLength) {
            originalMoveLength = script->m_funcScript;
            script->m_funcScript = &SafeMoveLength;
            TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                         reinterpret_cast<void*>(originalMoveLength),
                         reinterpret_cast<void*>(&SafeMoveLength), "ai GetCombatMoveLength script");
        }
        Log(originalMoveLength ? "ai: GetCombatMoveLength wrapped (fight target not bound)" : "ai: GetCombatMoveLength NOT found, not wrapped");
    }
    if (installed || searched) return;
    if (auto* script = Find(admin.GetScripts(), Name); script && script->m_funcScript) {
        gFScript nativeScript = script->m_funcScript;
        originalScript = WrapPerceptionEntry(nativeScript, reinterpret_cast<void*>(&FilterScript));
        script->m_funcScript = &FilterScript;
        TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                     reinterpret_cast<void*>(nativeScript),
                     reinterpret_cast<void*>(&FilterScript), "ai perception script");
        installed = true;
        Log("ai: perception filter installed (script)");
    } else if (auto* callback = Find(admin.GetScriptAICallbacks(), Name); callback && callback->m_funcScriptAICallback) {
        gFScriptAICallback nativeCallback = callback->m_funcScriptAICallback;
        originalCallback = WrapPerceptionEntry(nativeCallback, reinterpret_cast<void*>(&FilterCallback));
        callback->m_funcScriptAICallback = &FilterCallback;
        TrackPointer(reinterpret_cast<void*>(&callback->m_funcScriptAICallback),
                     reinterpret_cast<void*>(nativeCallback),
                     reinterpret_cast<void*>(&FilterCallback), "ai perception callback");
        installed = true;
        Log("ai: perception filter installed (AI callback)");
    } else if (gCSession::GetInstance().IsGameRunning()) {
        // The game is running and the function is in neither table: stop looking, say so once.
        searched = true;
        Logf("ai: %s not found as script or AI callback; perception filter NOT installed", Name);
    }
}
}

namespace g3t::ai {
bool EnterTerminalDeath(eCEntity* hero)
{
    if (!hero || hero != gCSession::GetInstance().GetHero() || !downed::Terminal() || ShuttingDown()) return false;
    auto* routine = static_cast<gCScriptRoutine_PS*>(hero->GetPropertySet(eEPropertySetType_ScriptRoutine));
    if (originalStates.find("ZS_RagDollDead") == originalStates.end()) WrapStates(GetScriptAdminExt());
    if (!routine || originalStates.find("ZS_RagDollDead") == originalStates.end()) {
        Log("downed: ANOMALY native terminal entry unavailable");
        return false;
    }
    // Native SetState replaces the SPU's active stack. Do not dispatch on that stack or recursively pump ProcessScript.
    bTObjStack<gScriptRunTimeSingleState> stack;
    gScriptRunTimeSingleState state;
    state.m_strScriptName = "ZS_RagDollDead";
    stack.Add(state);
    guards::Acting localPlayer(hero, false); // native Kill's GO_GameOver comparison must use this PC's hero
    return Run(originalStates, stack, &routine->GetSPU()) != GEFalse;
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
}
