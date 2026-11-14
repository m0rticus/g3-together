// SPDX-License-Identifier: GPL-3.0-or-later
#include "downed.h"
#include "downed_terminal.h"
#include "player_immunity.h"
#include "ai.h"
#include "body.h"
#include "crashlog.h"
#include "hud.h"
#include "i18n.h"
#include "items.h"
#include "log.h"
#include "persist.h"
#include "remote.h"
#include "movement.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace g3t::downed {
namespace {
auto& recovery = life.recovery;
auto& convergence = life.convergence;
auto& maintainingHp = life.maintainingHp;
auto& lethal = life.lethal;
auto& lethalSeenDown = life.lethalSeenDown;
auto& terminalStateEntries = life.terminalStateEntries;
auto& terminalStateReturns = life.terminalStateReturns;
auto& nativeTerminalKills = life.nativeTerminalKills;
auto& nativeTerminalKillChecked = life.nativeTerminalKillChecked;
auto& terminalAt = life.terminalAt;
auto& terminalSampleAt = life.terminalSampleAt;
auto& terminalTask = life.terminalTask;
auto& terminalState = life.terminalState;
auto& terminalHp = life.terminalHp;
auto& terminalAi = life.terminalAi;
auto& terminalMovement = life.terminalMovement;
auto& terminalMutations = life.terminalMutations;
auto& terminalMutationAt = life.terminalMutationAt;
auto& revivedBy = life.revivedBy;
auto& pendingReviveSequence = life.pendingReviveSequence;
auto& lastStandRefusal = life.lastStandRefusal;
auto& standRefusals = life.standRefusals;
auto& deaths = life.deaths;
}
SetModeFn originalSetMode = nullptr;
SetHpFn originalSetHp = nullptr, originalAddHp = nullptr, originalSetValue = nullptr;
SetTaskFn originalSetTask = nullptr;
SetStateFn originalSetState = nullptr;
namespace { bool terminalGuardsInstalled = false; }

void EndStep(char const* step)
{
    crashlog::EndStep(step);
    Logf("downed: end step=%s player=%u role=%s held=%d terminal=%d native_kills=%u sequence=%u",
         step, net::LocalId(), net::IsHost() ? "host" : "client", life.held ? 1 : 0,
         life.terminal ? 1 : 0, nativeTerminalKills, life.sequence);
}

bool LocalTerminal(eCEntity* entity)
{
    return life.terminal && entity && entity == gCSession::GetInstance().GetHero() &&
           !persist::NativeLoading() && !ai::ShuttingDown();
}

void Caller(void* address, char* text, size_t size)
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, path, MAX_PATH)) {
        char const* name = std::strrchr(path, '\\');
        Format(text, size, "%s+0x%x", name ? name + 1 : path,
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else {
        Format(text, size, "%p", address);
    }
}

void Mutation(char const* source, char const* field, char const* requested, bool refused, void* caller)
{
    const uint32_t now = net::Now();
    ++terminalMutations;
    if (refused && terminalMutations > 40 && now - terminalMutationAt < 1000) return;
    terminalMutationAt = now;
    char from[MAX_PATH + 16] = "-";
    if (caller) Caller(caller, from, sizeof(from));
    Logf("downed: terminal mutation=%u player=%u role=%s source=%s field=%s requested=%s refused=%d caller=%s",
         terminalMutations, net::LocalId(), net::IsHost() ? "host" : "client", source, field, requested,
         refused ? 1 : 0, from);
    if (!caller) return;
    void* frames[6] = {};
    const USHORT count = CaptureStackBackTrace(2, 6, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        Caller(frames[i], from, sizeof(from));
        Logf("downed: terminal caller mutation=%u frame=%u at=%s", terminalMutations, i, from);
    }
}

bool RejectHp(eCEntity* entity, bCString const& tag, int value, bool additive, char const* path)
{
    if (tag != "HP" || maintainingHp || !entity) return false;
    const int before = Entity(entity).PlayerMemory.GetHitPoints();
    if ((additive ? value < 0 : value < before) && RejectHarm(entity, nullptr, path)) return true;
    return false;
}

GEBool __fastcall SetHp(gCPlayerMemory_PS* self, void* edx, bCString const& tag, GEInt value)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    if (RejectHp(entity, tag, value, false, "SetBaseValue(HP)")) return GEFalse;
    if (LocalTerminal(entity) && tag == "HP") {
        const bool refused = !life.TerminalHpAllowed(value);
        if (refused || Entity(entity).PlayerMemory.GetHitPoints() != value) {
            char text[24];
            Format(text, "%d", value);
            Mutation("SetBaseValue(HP)", "hp", text, refused, _ReturnAddress());
        }
        if (refused) return GEFalse;
    }
    const int before = entity && tag == "HP" ? Entity(entity).PlayerMemory.GetHitPoints() : 0;
    const GEBool result = originalSetHp(self, edx, tag, value);
    if (entity && tag == "HP" && !maintainingHp)
        LogGroundEvent(entity, nullptr, "SetBaseValue(HP)", before, Entity(entity).PlayerMemory.GetHitPoints(), "native");
    ObserveTerminal(entity, "SetBaseValue");
    return result;
}

GEBool __fastcall AddHp(gCPlayerMemory_PS* self, void* edx, bCString const& tag, GEInt value)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    if (RejectHp(entity, tag, value, true, "AddBaseValue(HP)")) return GEFalse;
    if (LocalTerminal(entity) && tag == "HP" && value > 0) {
        Mutation("AddBaseValue(HP)", "hp", "positive delta", true, _ReturnAddress());
        return GEFalse;
    }
    const int before = entity && tag == "HP" ? Entity(entity).PlayerMemory.GetHitPoints() : 0;
    const GEBool result = originalAddHp(self, edx, tag, value);
    if (entity && tag == "HP")
        LogGroundEvent(entity, nullptr, "AddBaseValue(HP)", before, Entity(entity).PlayerMemory.GetHitPoints(), "native");
    ObserveTerminal(entity, "AddBaseValue");
    return result;
}

GEBool __fastcall SetValue(gCPlayerMemory_PS* self, void* edx, bCString const& tag, GEInt value)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    if (RejectHp(entity, tag, value, false, "SetValue(HP)")) return GEFalse;
    if (LocalTerminal(entity) && tag == "HP" && !life.TerminalHpAllowed(value)) {
        Mutation("SetValue(HP)", "hp", "positive value", true, _ReturnAddress());
        return GEFalse;
    }
    const int before = entity && tag == "HP" ? Entity(entity).PlayerMemory.GetHitPoints() : 0;
    const GEBool result = originalSetValue(self, edx, tag, value);
    if (entity && tag == "HP")
        LogGroundEvent(entity, nullptr, "SetValue(HP)", before, Entity(entity).PlayerMemory.GetHitPoints(), "native");
    ObserveTerminal(entity, "SetValue");
    return result;
}

template <size_t N>
void* InstallGuard(HMODULE game, char const* name, unsigned char const (&bytes)[N], void* detour)
{
    auto* entry = reinterpret_cast<unsigned char*>(GetProcAddress(game, name));
    if (!entry || std::memcmp(entry, bytes, N)) {
        Logf("downed: ANOMALY terminal guard prologue mismatch function=%s; not installed", name);
        return nullptr;
    }
    void* original = ai::JumpTo(entry, detour, N);
    if (!original) Logf("downed: ANOMALY terminal guard patch failed function=%s", name);
    return original;
}

void KeepTerminal(Entity& hero, char const* source)
{
    ObserveTerminal(hero.GetInstance(), source);
    if (hero.PlayerMemory.GetHitPoints() > 0) {
        Mutation(source, "hp", "0", false);
        hero.PlayerMemory.SetHitPoints(0);
    }
    if (!life.deathFinished) return;
    auto* routine = static_cast<gCScriptRoutine_PS*>(hero.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine));
    if (hero.Routine.AIMode != gEAIMode_Dead) {
        Mutation(source, "ai", "9", false);
        hero.Routine.AIMode = gEAIMode_Dead;
    }
    // Repair the displayed task without re-queuing the fatal entry; only its corpse loop may be restarted.
    if (hero.Routine.GetCurrentTask() != "ZS_RagDollDead") {
        Mutation(source, "task", "ZS_RagDollDead (field only)", false);
        routine->SetCurrentTask(bCString("ZS_RagDollDead"));
    }
    if (hero.Routine.GetCurrentState() != "ZS_RagDollDead_Loop") {
        Mutation(source, "state", "ZS_RagDollDead_Loop", false);
        routine->AISetState(bCString("ZS_RagDollDead_Loop"));
    }
}

void InstallTerminalGuards()
{
    if (terminalGuardsInstalled || ai::ShuttingDown()) return;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return; // scripts/engine have not loaded yet
    terminalGuardsInstalled = true;
    // Complete, position-independent instructions verified against CP 1.75.20108.14.
    static unsigned char const modeBytes[] = {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0x8b, 0x50, 0x58};
    static unsigned char const taskBytes[] = {0x57, 0x8b, 0xf9, 0x80, 0x7f, 0x64, 0x01};
    static unsigned char const stateBytes[] = {0x8d, 0x44, 0x24, 0x04, 0x50};
    originalSetMode = reinterpret_cast<SetModeFn>(InstallGuard(game,
        "?SetAIMode@gCScriptRoutine_PS@@QAEXABV?$bTPropertyContainer@W4gEAIMode@@@@@Z", modeBytes,
        reinterpret_cast<void*>(&SetMode)));
    InstallHpGuards(game);
    originalSetTask = reinterpret_cast<SetTaskFn>(InstallGuard(game,
        "?AISetTask@gCScriptProcessingUnit@@QAEXVbCString@@_N@Z", taskBytes, reinterpret_cast<void*>(&SetTask)));
    originalSetState = reinterpret_cast<SetStateFn>(InstallGuard(game,
        "?AISetState@gCScriptProcessingUnit@@QAEXVbCString@@@Z", stateBytes, reinterpret_cast<void*>(&SetState)));
    Logf("downed: terminal native guards installed mode=%d hp=%d task=%d state=%d",
         originalSetMode != nullptr, originalSetHp != nullptr, originalSetTask != nullptr, originalSetState != nullptr);
    Logf("downed: ground HP guards set=%d add=%d value=%d revive_grace_ms=%u",
         originalSetHp != nullptr, originalAddHp != nullptr, originalSetValue != nullptr, ReviveGraceMs);
}

void ObserveTerminal(eCEntity* self, char const* source)
{
    if (!LocalTerminal(self)) return;
    Entity hero(self);
    const bCString task = hero.Routine.GetCurrentTask(), state = hero.Routine.GetCurrentState();
    const int hp = hero.PlayerMemory.GetHitPoints(), aiMode = static_cast<int>(hero.Routine.AIMode);
    const int movement = static_cast<int>(hero.CharacterMovement.GetMovementMode());
    const uint32_t now = net::Now();
    const bool changed = terminalTask != task.GetText() || terminalState != state.GetText() ||
                         terminalHp != hp || terminalAi != aiMode || terminalMovement != movement;
    if (!changed && (now - terminalAt > 5000 || now - terminalSampleAt < 100)) return;
    terminalSampleAt = now;
    terminalTask = task.GetText();
    terminalState = state.GetText();
    terminalHp = hp;
    terminalAi = aiMode;
    terminalMovement = movement;
    auto* routine = static_cast<gCScriptRoutine_PS*>(self->GetPropertySet(eEPropertySetType_ScriptRoutine));
    Logf("downed: end-state player=%u role=%s elapsed_ms=%u source=%s changed=%d hp=%d ai=%d task=%s "
         "state=%s movement=%d output=%d task_pos=%d state_pos=%d entered=%u returned=%u native_kills=%u",
         net::LocalId(), net::IsHost() ? "host" : "client", now - terminalAt, source, changed ? 1 : 0,
         hp, aiMode, task.GetText(), state.GetText(), movement, routine->GetSPU().IsOutputPlaying() ? 1 : 0,
         routine->GetSPU().AIGetTaskPosition(), routine->GetSPU().AIGetStatePosition(),
         terminalStateEntries, terminalStateReturns, nativeTerminalKills);
}

bool ObserveDeathState(eCEntity* self, bool entered, bool successful)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!life.terminal || !self || self != hero) return true;
    Entity entity(hero);
    if (entered) {
        if (!life.BeginDeath()) {
            Logf("downed: ANOMALY terminal native entry admission failed player=%u", net::LocalId());
            return false;
        }
        ++terminalStateEntries;
        if (terminalStateEntries == 1) {
            Logf("downed: local ZS_RagDollDead entry player=%u native_sound=DEAD owner=local "
                 "task=%s hp=%d ai=%d native_player_is_hero=%d",
                 net::LocalId(), entity.Routine.GetCurrentTask().GetText(),
                 entity.PlayerMemory.GetHitPoints(), static_cast<int>(entity.Routine.AIMode),
                 Entity::GetPlayer().GetInstance() == hero ? 1 : 0);
        } else {
            Logf("downed: ANOMALY repeated ZS_RagDollDead entry player=%u count=%u",
                 net::LocalId(), terminalStateEntries);
        }
        return true;
    }
    ++terminalStateReturns;
    auto* animation = static_cast<eCVisualAnimation_PS*>(hero->GetPropertySet(eEPropertySetType_Animation));
    const bool ragdoll = animation && animation->GetRagDollEnabled();
    const bool controller = animation && animation->GetRagDollController();
    Logf("downed: local ZS_RagDollDead return player=%u count=%u task=%s state=%s hp=%d ai=%d "
         "movement=%d ragdoll=%d controller=%d dead=%d",
         net::LocalId(), terminalStateReturns, entity.Routine.GetCurrentTask().GetText(),
         entity.Routine.GetCurrentState().GetText(), entity.PlayerMemory.GetHitPoints(),
         static_cast<int>(entity.Routine.AIMode),
         static_cast<int>(entity.CharacterMovement.GetMovementMode()),
         ragdoll ? 1 : 0, controller ? 1 : 0, entity.IsDead() ? 1 : 0);
    if (successful && !nativeTerminalKillChecked) {
        nativeTerminalKillChecked = true;
        const bool dead = entity.IsDead();
        const bool deadMode = entity.Routine.AIMode == gEAIMode_Dead &&
                              static_cast<int>(entity.Routine.AIMode) == 9;
        if (dead && deadMode) {
            life.FinishDeath(true);
            nativeTerminalKills = 1;
            Logf("downed: native Kill confirmed from ZS_RagDollDead player=%u killer=null victim=hero "
                 "dead=%d ai=%d",
                 net::LocalId(), dead ? 1 : 0, static_cast<int>(entity.Routine.AIMode));
        } else {
            Logf("downed: ANOMALY native Kill not confirmed from ZS_RagDollDead player=%u killer=null victim=hero "
                 "dead=%d ai=%d",
                 net::LocalId(), dead ? 1 : 0, static_cast<int>(entity.Routine.AIMode));
        }
    }
    return true;
}

void StandRefused(char const* state, char const* source)
{
    const uint32_t now = net::Now();
    if (++standRefusals == 1 || now - lastStandRefusal >= 5000) {
        lastStandRefusal = now;
        Logf("downed: stand refused player=%u sequence=%u source=%s state=%s count=%u repair=module-boundary",
             net::LocalId(), life.sequence, source, state, standRefusals);
    }
}

bool RejectHeldTransition(eCEntity* entity, char const* state, char const* source)
{
    if (!life.held || life.terminal || !entity || entity != gCSession::GetInstance().GetHero() ||
        !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading() ||
        persist::HeroPending() || ai::ShuttingDown()) return false;
    if (DownTask(state) || !std::strcmp(state, "ZS_Unconscious_Loop")) return false;
    if (!convergence.settled) convergence.Request();
    StandRefused(state, source);
    return true;
}

Availability HelperState(net::Remote const& r, uint32_t now)
{
    return HelperAvailability(r.connected, r.activity.known, r.activity.ticks, r.activity.flags, now);
}

bool Able(net::Remote const& r, uint32_t now)
{
    return HelperState(r, now) == Availability::Able;
}

bool AllOthersDown(net::Remote const* remotes, uint32_t now)
{
    bool present = false;
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        net::Remote const& r = remotes[i];
        if (!r.connected) continue;
        if (!Playing(HelperState(r, now))) return false;
        present = true;
        if (HelperState(r, now) != Availability::Down) return false;
    }
    return present;
}

char const* HelperReason(Availability state)
{
    switch (state) {
    case Availability::Able: return "able";
    case Availability::Down: return "down-or-dead";
    case Availability::Disconnected: return "disconnected";
    case Availability::Unknown: return "no-activity";
    case Availability::Stale: return "activity-stale";
    case Availability::NotPlaying: return "loading-or-menu";
    }
    return "invalid";
}

void LogHelpers(Entity* hero, char const* event, uint32_t now)
{
    float here[3] = {};
    if (hero) {
        const bCVector& at = hero->GetInstance()->GetWorldPosition();
        here[0] = at.GetX(); here[1] = at.GetY(); here[2] = at.GetZ();
    }
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        net::Remote const& r = net::Remotes()[i];
        Logf("downed: helpers event=%s player=%u partner=%d name=%s connected=%d joined=%d gate=%d "
             "activity_known=%d activity_age=%u activity_packet=%u flags=%u loading=%d menu=%d "
             "pose_known=%d pose_age=%u pose_packet=%u pose_flags=%u sequence=%u "
             "world=%08x partner_world=%08x pose_world=%08x epoch=%08x partner_epoch=%08x "
             "distance_cm=%.0f local_position_known=%d zone=not-sent reject=%s",
             event, net::LocalId(), i, *r.latest.name ? r.latest.name : "-", r.connected ? 1 : 0,
             (net::IsHost() ? net::PeerJoined(static_cast<uint8_t>(i)) : net::Joined() && r.connected) ? 1 : 0,
             net::InWorld(static_cast<uint8_t>(i)) ? 1 : 0, r.activity.known ? 1 : 0,
             r.activity.known ? now - r.activity.ticks : 0, r.activity.sequence, r.activity.flags,
             (!(r.activity.flags & 1) && (r.activity.flags & 32)) ? 1 : 0,
             (!(r.activity.flags & 1) && (r.activity.flags & 64)) ? 1 : 0,
             r.latestTicks ? 1 : 0, r.latestTicks ? now - r.latestTicks : 0, r.latest.sequence,
             r.latest.flags, r.activity.downedSequence, hero ? remote::CurrentWorld() : 0, r.activity.world,
             r.latest.world, net::WorldEpoch(), r.activity.epoch,
             hero && r.latestTicks ? body::Distance(r.latest.position, here) : -1.0f,
             hero ? 1 : 0, HelperReason(HelperState(r, now)));
    }
}

bool PartnerPresent(net::Remote const* remotes, uint32_t now)
{
    for (int i = 1; i < net::MaxPlayers; ++i)
        if (i != net::LocalId() && Playing(HelperState(remotes[i], now))) return true;
    return false;
}

void Die(Entity& hero, char const* reason)
{
    if (!life.End()) {
        EndStep("skipped-terminal");
        return;
    }
    recovery.Clear();
    lethal = false;
    lethalSeenDown = false;
    revivedBy = 0;
    pendingReviveSequence = 0;
    EndStep("decision");
    Transition(reason, "end-policy");
    ++deaths;
    Logf("downed: nobody can help; the hero dies reason=%s hero=%p hp=%d task=%s state=%s ai=%d",
         reason, hero.GetInstance(), hero.PlayerMemory.GetHitPoints(), hero.Routine.GetCurrentTask().GetText(),
         hero.Routine.GetCurrentState().GetText(), static_cast<int>(hero.Routine.AIMode));
    const uint32_t now = net::Now();
    terminalAt = now;
    terminalSampleAt = now - 100;
    LogHelpers(&hero, reason, now);
    hud::Message(hud::Bad, i18n::Text(i18n::NobodyCanHelp));
    EndStep("set-hp");
    hero.PlayerMemory.SetHitPoints(0);
    EndStep("death-full-stop");
    hero.Routine.FullStop();
    hero.CharacterMovement.StopMovement();
    // ZS_RagDollDead obtains the native killer and lethal-hit force from these fields. Clear both
    // before entering the state so native Kill receives killer=null and no stale inflictor can AddForce.
    Entity clearAttacker;
    Entity clearInflictor;
    hero.NPC.SetCurrentAttacker(clearAttacker);
    hero.DamageReceiver.SetLastInflictor(clearInflictor);
    const bool attackerClear = hero.NPC.GetCurrentAttacker().GetInstance() == nullptr;
    const bool inflictorClear = hero.DamageReceiver.GetLastInflictor().GetInstance() == nullptr;
    Logf("downed: terminal native inputs clear player=%u attacker_clear=%d inflictor_clear=%d "
         "force=no-force killer=null victim=hero",
         net::LocalId(), attackerClear ? 1 : 0, inflictorClear ? 1 : 0);
    EndStep("death-native-inputs-cleared");
    // ZS_RagDollDead is the native death-presentation entry: it requests DEAD, calls its direct
    // native Kill, and enables ragdoll movement mode 14.
    hero.Routine.AIMode = gEAIMode_Reaction;
    auto* routine = static_cast<gCScriptRoutine_PS*>(hero.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine));
    routine->AISetTask(bCString("ZS_RagDollDead"));
    EndStep("death-state-dispatch");
    const bool ran = ai::EnterTerminalDeath(hero.GetInstance());
    if (!ran || !life.deathFinished) {
        EndStep("death-state-failed");
        Logf("downed: ANOMALY terminal native entry failed player=%u ran=%d entered=%u returned=%u dead=%d",
             net::LocalId(), ran ? 1 : 0, terminalStateEntries, terminalStateReturns, hero.IsDead() ? 1 : 0);
    } else {
        EndStep("death-state-complete");
    }
    ObserveTerminal(hero.GetInstance(), "Die-return");
}
}
