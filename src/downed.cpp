// SPDX-License-Identifier: GPL-3.0-or-later
#include "downed.h"
#include "downed_terminal.h"
#include "player_immunity.h"
#include "ai.h"
#include "body.h"
#include "combat_hostility.h"
#include "crashlog.h"
#include "hud.h"
#include "i18n.h"
#include "items.h"
#include "log.h"
#include "npcs.h"
#include "persist.h"
#include "remote.h"
#include "movement.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace g3t::downed {
LocalLifeState life;
PeerLifeState peers[net::MaxPlayers];
namespace {
auto& recovery = life.recovery;
auto& convergence = life.convergence;
auto& lethal = life.lethal;
auto& lethalSeenDown = life.lethalSeenDown;
auto& lethalAt = life.lethalAt;
auto& lastHp = life.lastHp;
auto& lethalBlowAt = life.lethalBlowAt;
auto& nativeLethal = life.nativeLethal;
auto& nativeNoted = life.nativeNoted;
auto& recoveryUnknownNoted = life.recoveryUnknownNoted;
auto& nativeTerminalKills = life.nativeTerminalKills;
auto& nativeTerminalKillChecked = life.nativeTerminalKillChecked;
auto& lastJudgedHitValid = life.lastJudgedHitValid;
auto& lastJudgedLethal = life.lastJudgedLethal;
auto& lastJudgedHitAt = life.lastJudgedHitAt;
auto& lastJudgedSpecies = life.lastJudgedSpecies;
auto& lastJudgedReason = life.lastJudgedReason;
auto& lastJudgedAttackerName = life.lastJudgedAttackerName;
auto& terminalTask = life.terminalTask;
auto& inHand = life.inHand;
auto& wasDown = life.wasDown;
auto& aloneSince = life.aloneSince;
auto& revivedBy = life.revivedBy;
auto& pendingReviveSequence = life.pendingReviveSequence;
auto& transitions = life.transitions;
auto& corrections = life.corrections;
auto& lastCorrection = life.lastCorrection;
auto& standRefusals = life.standRefusals;
auto& redirects = life.redirects;
auto& revives = life.revives;
}
char const* const KnockOutTask = "ZS_LieKnockDown";
void SyncStates() {
    auto const* remotes = net::Remotes();
    const uint32_t now = net::Now();
    for (int i = 0; i < net::MaxPlayers; ++i) {
        auto& peer = peers[i];
        const uint32_t generation = net::PeerGeneration(static_cast<uint8_t>(i));
        const bool changed = remotes[i].connected && generation && peer.payment.generation && generation != peer.payment.generation;
        if (!remotes[i].connected || changed) { peer.Reset(); ResetPeerImmunity(i); }
        peer.payment.ObserveConnection(remotes[i].connected, generation);
        if (remotes[i].connected && remotes[i].latestTicks != 0) {
            const uint8_t flags = remotes[i].latest.flags;
            peer.payment.ObservePlayerState((flags & 1) != 0 && (flags & 14) == 0,
                                            remotes[i].latest.downedSequence);
        }
        if (peer.reviveInFlightSeq &&
            (!remotes[i].connected ||
             (remotes[i].activity.known &&
              ((remotes[i].activity.flags & 8) == 0 ||
               remotes[i].activity.downedSequence != peer.reviveInFlightSeq)) ||
             static_cast<int32_t>(now - peer.reviveInFlightUntil) >= 0)) {
            peer.reviveInFlightSeq = 0;
            peer.reviveInFlightUntil = 0;
        }
    }
}
void Transition(char const* reason, char const* source)
{
    Logf("downed: transition=%u player=%u role=%s held=%d terminal=%d sequence=%u reason=%s source=%s",
         ++transitions, net::LocalId(), net::IsHost() ? "host" : "client", life.held ? 1 : 0,
         life.terminal ? 1 : 0, life.sequence, reason, source);
}
void __fastcall SetMode(gCScriptRoutine_PS* self, void* edx, bTPropertyContainer<gEAIMode> const& value)
{
    eCEntity* entity = self ? self->GetEntity() : nullptr;
    const int requested = static_cast<int>(value.GetValue());
    if ((requested == gEAIMode_Down || requested == gEAIMode_Dead) && InteractionOf(entity) &&
        RejectHarm(entity, nullptr, "SetAIMode")) return;
    char modeText[24];
    Format(modeText, sizeof(modeText), "%d", static_cast<int>(value.GetValue()));
    movement::Touch(entity, "SetAIMode", modeText, _ReturnAddress());
    if (LocalTerminal(entity)) {
        const int mode = static_cast<int>(value.GetValue());
        const bool refused = !life.TerminalModeAllowed(mode);
        if (refused || static_cast<int>(self->GetAIMode().GetValue()) != mode) {
            char text[24];
            Format(text, "%d", mode);
            Mutation("SetAIMode", "ai", text, refused, _ReturnAddress());
        }
        if (refused) return;
    }
    originalSetMode(self, edx, value);
    ObserveTerminal(entity, "SetAIMode");
}
void __fastcall SetTask(gCScriptProcessingUnit* self, void* edx, bCString task, GEBool caught)
{
    eCEntity* entity = self ? self->GetSelfEntity() : nullptr;
    movement::Touch(entity, "AISetTask", task.GetText(), _ReturnAddress());
    if (RejectReaction(entity, task.GetText(), "AISetTask")) return;
    if (RejectHeldTransition(entity, task.GetText(), "AISetTask")) return;
    if (LocalTerminal(entity)) {
        const bool refused = !life.TerminalTaskAllowed(task.GetText());
        Mutation("AISetTask", "task", task.GetText(), refused, _ReturnAddress());
        if (refused) return;
    }
    originalSetTask(self, edx, task, caught);
    ObserveTerminal(entity, "AISetTask");
}
void __fastcall SetState(gCScriptProcessingUnit* self, void* edx, bCString state)
{
    eCEntity* entity = self ? self->GetSelfEntity() : nullptr;
    movement::Touch(entity, "AISetState", state.GetText(), _ReturnAddress());
    if (RejectReaction(entity, state.GetText(), "AISetState")) return;
    if (RejectHeldTransition(entity, state.GetText(), "AISetState")) return;
    if (LocalTerminal(entity)) {
        const bool refused = !life.TerminalStateAllowed(state.GetText());
        Mutation("AISetState", "state", state.GetText(), refused, _ReturnAddress());
        if (refused) return;
    }
    originalSetState(self, edx, state);
    ObserveTerminal(entity, "AISetState");
}
void InstallHpGuards(void* module)
{
    auto game = static_cast<HMODULE>(module);
    static unsigned char const hpBytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56};
    static unsigned char const valueBytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08};
    auto guard = [&](char const* name, auto const& bytes, void* detour) -> SetHpFn {
        auto* entry = reinterpret_cast<unsigned char*>(GetProcAddress(game, name));
        if (!entry || std::memcmp(entry, bytes, sizeof(bytes))) return nullptr;
        return reinterpret_cast<SetHpFn>(ai::JumpTo(entry, detour, sizeof(bytes)));
    };
    originalSetHp = guard("?SetBaseValue@gCPlayerMemory_PS@@QAE_NABVbCString@@H@Z", hpBytes, reinterpret_cast<void*>(&SetHp));
    originalAddHp = guard("?AddBaseValue@gCPlayerMemory_PS@@QAE_NABVbCString@@H@Z", hpBytes, reinterpret_cast<void*>(&AddHp));
    originalSetValue = guard("?SetValue@gCPlayerMemory_PS@@QAE_NABVbCString@@H@Z", valueBytes, reinterpret_cast<void*>(&SetValue));
}
bool DownTask(char const* task)
{
    return !std::strcmp(task, KnockOutTask) || !std::strcmp(task, "ZS_Unconscious") || !std::strcmp(task, "ZS_PiercedKO");
}

bool StableHeld(Entity& hero)
{
    if (!hero.IsDown() || hero.Routine.GetCurrentState() != "ZS_Unconscious_Loop" ||
        static_cast<gEAniState>(hero.Routine.AniState) != gEAniState_LieKnockOut) return false;
    char motion[256] = {};
    body::CurrentMotion(hero.GetInstance(), motion, sizeof(motion));
    return !std::strstr(motion, "GetUp") && !std::strstr(motion, "StandUp") &&
           (!std::strstr(motion, "KnockDown") || std::strstr(motion, "_Ambient_"));
}

void KeepDown(Entity& hero, char const* source)
{
    MaintainHp maintenance;
    const bCString task = hero.Routine.GetCurrentTask(), state = hero.Routine.GetCurrentState();
    const uint32_t now = net::Now();
    const bool repair = convergence.Observe(StableHeld(hero) && DownTask(task.GetText()),
                                            body::MotionFraction(hero.GetInstance()), now);
    const int hp = hero.PlayerMemory.GetHitPoints();
    if (repair || hp > 1) {
        ++corrections;
        if (repair || !lastCorrection || net::Now() - lastCorrection >= 5000) {
            lastCorrection = net::Now();
            Logf("downed: hold correction=%u player=%u sequence=%u source=%s task=%s state=%s ai=%d ani=%d hp=%d timer=%.3f repair=%d",
                 corrections, net::LocalId(), life.sequence, source, task.GetText(), state.GetText(),
                 static_cast<int>(hero.Routine.AIMode), static_cast<int>(hero.Routine.AniState), hp,
                 hero.Routine.GetTaskTime(), repair ? 1 : 0);
        }
    }
    if (repair) {
        // Repair outside an executing script callback; never replay the knockdown entry.
        hero.Routine.FullStop();
        hero.CharacterMovement.StopMovement();
        hero.StopPlayAni();
        auto* routine = static_cast<gCScriptRoutine_PS*>(hero.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine));
        routine->AIStopPlayAnimation();
        routine->SetCurrentTask(bCString(KnockOutTask));
        hero.Routine.AniState = gEAniState_LieKnockOut;
        routine->AISetState(bCString("ZS_Unconscious_Loop"));
        Logf("downed: held settled player=%u sequence=%u source=%s refused=%u",
             net::LocalId(), life.sequence, source, standRefusals);
    }
    // OnPlayerGameKeyPressed refuses AI mode Down (Script_Game.dll 0x10061640).
    hero.Routine.AIMode = gEAIMode_Down;
    hero.Routine.SetTaskTime(0.0f);
    if (hp != 1) hero.PlayerMemory.SetHitPoints(1);
}

void CancelAlone(char const* source);

void Hold(char const* reason, char const* source)
{
    if (life.held || life.terminal) return;
    recovery.Clear();
    convergence.Clear();
    life.Restore(true);
    CancelAlone("new-held-life");
    revivedBy = 0;
    pendingReviveSequence = 0;
    Transition(reason, source);
    hud::Message(hud::Bad, i18n::Text(i18n::KnockedOutNeedsPotion));
}

void CancelAlone(char const* source)
{
    if (!aloneSince) return;
    const uint32_t now = net::Now();
    // World retirement/restore must not inspect the old hero to report cancellation.
    LogHelpers(nullptr, "alone-cancel", now);
    Logf("downed: alone reset player=%u source=%s elapsed_ms=%u", net::LocalId(), source, now - aloneSince);
    aloneSince = 0;
}

bool IsNpcAttacker(eCEntity const* attacker, eCEntity const* self)
{
    return attacker && attacker != self && !remote::IsPuppet(attacker) &&
           attacker->GetPropertySet(eEPropertySetType_NPC);
}

bool IsLethalNpcAttacker(eCEntity* attacker, Entity& hero)
{
    if (!IsNpcAttacker(attacker, hero.GetInstance()))
        return true;
    Entity npc(attacker);
    const int species = static_cast<int>(npc.NPC.Species);
    const bool humanoid = IsHumanoidSpecies(species);
    const int attackReason = static_cast<int>(npc.NPC.AttackReason);
    int attitude = 0;
    if (npcs::IsMirrored(attacker)) {
        if (combat_hostility::MirrorSharesCombatHostility(attacker)) {
            if (!ai::LocalAttitude(attacker, attitude)) npcs::HostAttitude(attacker, attitude);
        } else if (!npcs::HostAttitude(attacker, attitude)) {
            ai::LocalAttitude(attacker, attitude);
        }
    } else {
        ai::LocalAttitude(attacker, attitude);
    }
    const bool meansToKill =
        GetScriptAdminExt().CallScript(bCString("HasAttackReasonToKill"), attacker, hero.GetInstance(), 0) != 0;
    return IsLethalDefeat(true, humanoid, meansToKill, attackReason, attitude);
}

bool RedirectDeath(eCEntity* self, char const* state)
{
    if (life.terminal || !self || self != gCSession::GetInstance().GetHero()) return false;
    if (std::strcmp(state, "ZS_Dead") != 0 && std::strcmp(state, "ZS_RagDollDead") != 0) return false;
    if (RejectHarm(self, nullptr, "redirect-death")) return true;
    Entity hero(self);
    const uint32_t now = net::Now();
    bool lethalDefeat = true;
    char const* attackerName = "-";
    int attackerSpecies = -1, attackerReason = -1;
    if (!life.held && !persist::HeroPending()) {
        if (lastJudgedHitValid && now - lastJudgedHitAt < 1500) {
            lastJudgedHitValid = false;
            lethalDefeat = lastJudgedLethal;
            attackerName = lastJudgedAttackerName.empty() ? "-" : lastJudgedAttackerName.c_str();
            attackerSpecies = lastJudgedSpecies;
            attackerReason = lastJudgedReason;
        } else {
            lastJudgedHitValid = false;
            eCEntity* attacker = hero.NPC.GetCurrentAttacker().GetInstance();
            if (IsNpcAttacker(attacker, self)) {
                Entity npc(attacker);
                lethalDefeat = IsLethalNpcAttacker(attacker, hero);
                attackerName = npc.GetName().GetText();
                attackerSpecies = static_cast<int>(npc.NPC.Species);
                attackerReason = static_cast<int>(npc.NPC.AttackReason);
            }
        }
    }
    hero.Routine.FullStop();
    hero.Routine.SetTask(bCString(KnockOutTask));
    if (!lethalDefeat) {
        MaintainHp maintenance;
        hero.Routine.AIMode = gEAIMode_Down;
        lethal = false;
        lethalSeenDown = false;
        lethalBlowAt = 0;
        nativeNoted = false;
        Logf("downed: non-lethal defeat (%s) by %s (species=%d reason=%d); staying in native unconsciousness (hp=%d)",
             state, attackerName, attackerSpecies, attackerReason, hero.PlayerMemory.GetHitPoints());
        return true;
    }
    lethal = life.held || !persist::HeroPending();
    lethalSeenDown = false;
    lethalAt = now;
    if (!persist::HeroPending()) Hold("redirect-death", state);
    ++redirects;
    Logf("downed: hero would die (%s), knocked out instead (hp=%d)", state, hero.PlayerMemory.GetHitPoints());
    return true;
}

bool GuardState(eCEntity* self, char const* state)
{
    if (RejectReaction(self, state, "AI-state")) return true;
    if ((!life.held && !life.terminal) || !self || self != gCSession::GetInstance().GetHero() ||
        !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading()) return false;
    Entity hero(self);
    if (life.terminal) {
        if (life.TerminalStateAllowed(state)) return false;
        Mutation(state, "AI", state, true);
        KeepTerminal(hero, "GuardState");
        return true;
    }
    // A module tick is not a script clock boundary: a long frame or another script can otherwise wake the hero first.
    if (DownTask(state) || !std::strcmp(state, "ZS_Unconscious_Loop")) {
        MaintainHp maintenance;
        if (DownTask(state) && convergence.settled) {
            convergence.Request();
            return true;
        }
        if (!std::strcmp(state, "ZS_Unconscious_Loop") && hero.Routine.GetTaskTime() > 5.0f)
            Logf("downed: stand refused player=%u sequence=%u source=native-wake timer=%.3f",
                 net::LocalId(), life.sequence, hero.Routine.GetTaskTime());
        hero.Routine.AIMode = gEAIMode_Down;
        hero.Routine.SetTaskTime(0.0f);
        if (hero.PlayerMemory.GetHitPoints() != 1) hero.PlayerMemory.SetHitPoints(1);
        return false;
    }
    StandRefused(state, "AI");
    if (!StableHeld(hero)) convergence.Request();
    MaintainHp maintenance;
    hero.Routine.AIMode = gEAIMode_Down;
    hero.Routine.SetTaskTime(0.0f);
    if (hero.PlayerMemory.GetHitPoints() != 1) hero.PlayerMemory.SetHitPoints(1);
    return true;
}

void Tick()
{
    SyncStates();
    LocalInteractionFlags();
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* instance = session.GetHero();
    if (!instance) {
        CancelAlone("no-local-hero");
        revivedBy = 0;
        lastHp = -1;
        lethalBlowAt = 0;
        return;
    }
    if (!session.IsGameRunning()) {
        CancelAlone("local-not-playing");
        revivedBy = 0;
        return;
    }
    Entity hero(instance);
    const uint32_t now = net::Now();
    if (recovery.rising) {
        char motion[256] = {};
        const bool known = body::CurrentMotion(instance, motion, sizeof(motion)) && motion[0];
        const int pose = static_cast<int>(hero.Routine.AniState);
        if (!known && !recoveryUnknownNoted)
            Logf("downed: recovery waiting player=%u sequence=%u ani=%d reason=no-current-motion", net::LocalId(), life.sequence, pose);
        recoveryUnknownNoted = !known;
        recovery.Observe(RecoveryStanding(pose, hero.IsDown(), known ? motion : nullptr), now);
        if (!recovery.rising)
            Logf("downed: recovery standing player=%u sequence=%u grace_ms=%u motion=%s", net::LocalId(), life.sequence, ReviveGraceMs, motion);
    } else {
        recovery.Observe(false, now);
    }
    net::Remote const* remotes = net::Remotes();
    const uint8_t self = net::LocalId();
    wchar_t name[32], text[200];

    // Tell this player when someone else goes down or dies.
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == self) continue;
        TrackPuppetRecovery(i);
        const bool present = Playing(HelperState(remotes[i], now));
        const bool dead = present && (remotes[i].activity.flags & 4);
        const bool down = present && (remotes[i].activity.flags & 8) && !dead;
        Wide(remotes[i].latest.name, name, 32);
        if (down && !peers[i].told) {
            swprintf_s(text, i18n::Text(i18n::PartnerDownNeedsPotion), name);
            hud::Message(hud::Warning, text);
        }
        if (dead && !peers[i].toldDead) {
            swprintf_s(text, i18n::Text(i18n::PartnerDied), name);
            hud::Message(hud::Bad, text);
        }
        peers[i].told = down;
        peers[i].toldDead = dead;
    }

    if (life.terminal) {
        life.held = false;
        lethal = false;
        lethalSeenDown = false;
        nativeNoted = false;
        revivedBy = 0;
        KeepTerminal(hero, "Tick-terminal");
        return;
    }
    if (hero.IsDead() && !life.held) { lethal = false; revivedBy = 0; return; }
    if (persist::HeroPending()) {
        if (life.held) KeepDown(hero, "hero-restore");
        CancelAlone("hero-restore");
        return;
    }
    if (!net::IsHost() && life.pendingHostWipeEpoch && life.pendingHostWipeEpoch == net::WorldEpoch() &&
        net::InWorld(net::LocalId())) {
        life.pendingHostWipeEpoch = 0;
        Die(hero, "host-wipe");
        return;
    }
    if (LocalInteractionFlags()) {
        lastHp = hero.PlayerMemory.GetHitPoints();
        lethalBlowAt = 0;
    } else {
        WatchBlows(hero, now);
    }
    const bool down = hero.IsDown();
    if (!down && !life.held) {
        for (int i = 0; i < 2; ++i) {
            Entity item = hero.Inventory.GetItemFromSlot(i ? gESlot_LeftHand : gESlot_RightHand);
            inHand[i] = item.GetInstance() ? item.GetName().GetText() : "";
        }
    } else if (down && !wasDown && !net::IsHost()) {
        for (auto const& n : inHand)
            if (!n.empty()) items::NoteOwnDrop(n.c_str());
    }
    wasDown = down;
    if (down && lethal) lethalSeenDown = true;
    if (!down && !life.held) {
        // The knock-out task RedirectDeath sets makes the hero lie a frame or more later: not standing up yet (22.10.:
        // reset at once, every intercepted death went the game's own way and nobody could help, stages A and A2).
        if (lethal && !lethalSeenDown && now - lethalAt < 3000) return;
        lethal = false;
        lethalSeenDown = false;
        nativeNoted = false;
        CancelAlone("not-held");
        revivedBy = 0;
        OfferHelp(hero, remotes, now);
        return;
    }
    if (!life.held) {
        // A knock-out of the game's own at 0 hit points after a blow that meant to kill: a death the game turned into a
        // knock-out; the partner helps it up, else it is one.
        if (!lethal && !nativeNoted && hero.PlayerMemory.GetHitPoints() <= 0 && lethalBlowAt &&
            now - lethalBlowAt < 3000 && PartnerPresent(remotes, now)) {
            lethal = true;
            lethalSeenDown = true;
            lethalAt = now;
            ++nativeLethal;
            Logf("downed: lethal blow by someone who means to kill; knocked out like a death (hp=%d)", hero.PlayerMemory.GetHitPoints());
        }
        if (!lethal) {
            if (!nativeNoted) {
                nativeNoted = true;
                Logf("downed: hero knocked out by the game's own rules (not a death; hp=%d); it stands up by itself",
                     hero.PlayerMemory.GetHitPoints());
            }
            return;
        }
        Hold("lethal-knockout", "tick");
        Logf("downed: hero knocked out, waiting for a partner (hp=%d)", hero.PlayerMemory.GetHitPoints());
    }
    KeepDown(hero, "tick");

    if (revivedBy && !Able(remotes[revivedBy], now)) {
        Logf("downed: revive refused player=%u helper=%d sequence=%u reason=helper-no-longer-able",
             self, revivedBy, pendingReviveSequence);
        revivedBy = 0;
        pendingReviveSequence = 0;
    }
    if (revivedBy) {
        const int by = revivedBy;
        revivedBy = 0;
        if (life.terminal || !life.Revive(pendingReviveSequence)) return;
        CancelAlone("paid-revive");
        life.Reset();
        convergence.Clear();
        recovery.Begin();
        ++revives;
        Transition("paid-revive", "network");
        StandUp(hero);
        Wide(remotes[by].latest.name, name, 32);
        swprintf_s(text, i18n::Text(i18n::RevivedByPartner), name);
        hud::Message(hud::Good, text);
        Logf("downed: revived by player %d (hp=%d)", by, hero.PlayerMemory.GetHitPoints());
        return;
    }

    if (net::IsHost()) {
        const uint32_t before = aloneSince;
        const EndDecision decision = HostWipeDecision(life.held || life.terminal, remotes, peers, now, aloneSince);
        if (before && !aloneSince) LogHelpers(&hero, "alone-cancel", now);
        if (!before && aloneSince) LogHelpers(&hero, "alone-start", now);
        if (decision == EndDecision::AllDown) {
            BroadcastHostWipe();
            Die(hero, "all-down");
            return;
        }
        if (decision == EndDecision::AloneGrace) {
            LogHelpers(&hero, "alone-expiry", now);
            if (net::Connected()) BroadcastHostWipe();
            Die(hero, "alone-grace");
        }
    } else {
        const EndDecision decision = DecideClientEnd(net::Connected(), false, now, aloneSince);
        if (decision == EndDecision::AloneGrace) {
            LogHelpers(&hero, "alone-expiry", now);
            Die(hero, "alone-grace");
        }
    }
}

void Revived(int byPlayer, uint32_t sequence)
{
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* instance = session.GetHero();
    const bool wipePending = !net::IsHost() && life.pendingHostWipeEpoch && life.pendingHostWipeEpoch == net::WorldEpoch();
    const bool helper = byPlayer > 0 && byPlayer < net::MaxPlayers && byPlayer != net::LocalId() &&
                        Able(net::Remotes()[byPlayer], net::Now());
    if (!helper || !life.held || !sequence || sequence != life.sequence || life.terminal || wipePending ||
        !instance || !session.IsGameRunning() || persist::HeroPending()) {
        Logf("downed: revive refused player=%u helper=%d sequence=%u current=%u held=%d terminal=%d reason=stale-or-ineligible",
             net::LocalId(), byPlayer, sequence, life.sequence, life.held ? 1 : 0, life.terminal ? 1 : 0);
        return;
    }
    revivedBy = byPlayer;
    pendingReviveSequence = sequence;
    Logf("downed: revive accepted player=%u helper=%d sequence=%u source=network", net::LocalId(), byPlayer, sequence);
}

void HelperHasNoPotion(int byPlayer)
{
    if (byPlayer <= 0 || byPlayer >= net::MaxPlayers || !life.held) return;
    wchar_t name[32], text[160];
    Wide(net::Remotes()[byPlayer].latest.name, name, 32);
    swprintf_s(text, i18n::Text(i18n::HelperHasNoPotion), name);
    hud::Message(hud::Info, text);
}

bool SyntheticProne(eCEntity* victim)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return victim && hero && (victim == hero || victim == remote::ControlledBody()) &&
           !life.terminal && !persist::NativeLoading() && !persist::HeroPending() &&
           VanillaProne(static_cast<int>(Entity(victim).Routine.AniState));
}

bool CombatUnavailable(eCEntity* entity)
{
    if (!entity) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (hero && (entity == hero || entity == remote::ControlledBody())) {
        Entity player(entity);
        return CombatUnavailable(true, life.held, life.terminal, player.IsDown(), player.IsDead(), 0);
    }
    const int id = remote::PuppetOwner(entity);
    if (!id) return false;
    net::Remote const& r = net::Remotes()[id];
    // Latest accepted player state is authority, not the talkable puppet's native mode or lying animation.
    return r.connected && r.latestTicks && r.latest.world == remote::CurrentWorld() &&
           CombatUnavailable(true, false, false, false, false, r.latest.flags);
}

uint32_t UnavailablePlayers()
{
    uint32_t players = 0;
    const unsigned local = net::LocalId();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* controlled = hero ? remote::ControlledBody() : nullptr;
    if (local && local < net::MaxPlayers && (CombatUnavailable(hero) || CombatUnavailable(controlled)))
        players |= uint32_t{1} << local;
    for (unsigned id = 1; id < net::MaxPlayers; ++id)
        if (id != local && CombatUnavailable(remote::PuppetOf(id))) players |= uint32_t{1} << id;
    return players;
}

void LogCombatDecision(eCEntity* npc, eCEntity* player, char const* decision, bool transition)
{
    static unsigned decisions = 0;
    static uint32_t lastLog = 0;
    const uint32_t now = net::Now();
    ++decisions;
    if (!transition && decisions > 40 && now - lastLog < 5000) return;
    lastLog = now;
    const int remoteId = remote::PuppetOwner(player);
    const unsigned id = remoteId ? static_cast<unsigned>(remoteId) : net::LocalId();
    const uint8_t flags = remoteId ? net::Remotes()[remoteId].latest.flags : 0;
    const uint32_t sequence = remoteId ? net::Remotes()[remoteId].latest.downedSequence : life.sequence;
    const uint32_t age = remoteId ? now - net::Remotes()[remoteId].latestTicks : 0;
    const bool hasNpc = npc && npc->GetPropertySet(eEPropertySetType_NPC);
    const bool hasRoutine = npc && npc->GetPropertySet(eEPropertySetType_ScriptRoutine);
    Entity actor(npc);
    Logf("DOWNEDTARGET role=%s player=%u episode=%u flags=%u age=%u excluded=%#x held=%d terminal=%d decision=%s npc=%s ptr=%p ai=%d task=%s state=%s",
         net::IsHost() ? "host" : "client", id, sequence, flags, age, UnavailablePlayers(),
         !remoteId && life.held ? 1 : 0, !remoteId && life.terminal ? 1 : 0, decision,
         npc ? actor.GetName().GetText() : "-", npc, hasRoutine ? static_cast<int>(actor.Routine.AIMode) : -1,
         hasRoutine ? actor.Routine.GetCurrentTask().GetText() : "-",
         hasRoutine ? actor.Routine.GetCurrentState().GetText() : "-");
    if (hasNpc)
        Logf("DOWNEDBIND npc=%p player=%p current=%p alternative=%p attacker=%p last=%p spell=%p alignment=%p inflictor=%p combat=%u",
             npc, player, actor.NPC.GetCurrentTarget().GetInstance(), actor.NPC.GetAlternativeTarget().GetInstance(),
             actor.NPC.GetCurrentAttacker().GetInstance(), actor.NPC.GetLastAttacker().GetInstance(),
             actor.NPC.GetSpellTarget().GetInstance(), actor.GetAlignmentTarget().GetInstance(),
             npc->GetPropertySet(eEPropertySetType_DamageReceiver) ? actor.DamageReceiver.GetLastInflictor().GetInstance() : nullptr,
             static_cast<unsigned>(actor.NPC.CombatState));
}

uint32_t Sequence() { return life.held ? life.sequence : 0; }
bool Held() { return life.held; }
bool Terminal() { return life.terminal; }

void OnFrameResume()
{
    if (life.held && !life.terminal) convergence.Request();
}

void Restore(bool waiting, char const* source)
{
    if (life.terminal) return;
    if (waiting && !life.held) {
        Hold("saved-downed-life", source);
        lethal = true;
    } else if (life.held) {
        Transition("preserve-downed-life", source);
    }
}

void NoteBlow()
{
    if (LocalInteractionFlags()) return;
    lethalBlowAt = net::Now() ? net::Now() : 1;
}

void ClearBlow()
{
    lethalBlowAt = 0;
}

bool IsLethalHeroAttacker(eCEntity* attacker)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !IsNpcAttacker(attacker, hero))
        return true;
    return IsLethalNpcAttacker(attacker, Entity(hero));
}

void ClearHeroJudgedHit()
{
    lastJudgedHitValid = false;
}

void NoteHeroJudgedHit(eCEntity* attacker, bool applied)
{
    if (!applied) {
        lastJudgedHitValid = false;
        return;
    }
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    const uint32_t now = net::Now();
    lastJudgedHitValid = true;
    lastJudgedHitAt = now ? now : 1;
    if (!IsNpcAttacker(attacker, hero)) {
        lastJudgedLethal = true;
        lastJudgedSpecies = -1;
        lastJudgedReason = -1;
        lastJudgedAttackerName = attacker ? Entity(attacker).GetName().GetText() : "-";
        return;
    }
    Entity npc(attacker);
    Entity localHero(hero);
    lastJudgedLethal = IsLethalNpcAttacker(attacker, localHero);
    lastJudgedSpecies = static_cast<int>(npc.NPC.Species);
    lastJudgedReason = static_cast<int>(npc.NPC.AttackReason);
    lastJudgedAttackerName = npc.GetName().GetText();
}

void NoteReviveSent(int targetPlayer, uint32_t sequence)
{
    if (!net::IsHost() || targetPlayer < 2 || targetPlayer >= net::MaxPlayers || !sequence) return;
    peers[targetPlayer].reviveInFlightSeq = sequence;
    peers[targetPlayer].reviveInFlightUntil = net::Now() + 2000;
}

void HostWipe(uint32_t epoch)
{
    if (net::IsHost() || !epoch || epoch != net::WorldEpoch()) {
        Logf("downed: host wipe ignored role=%s epoch=%08x local_epoch=%08x",
             net::IsHost() ? "host" : "client", epoch, net::WorldEpoch());
        return;
    }
    life.pendingHostWipeEpoch = epoch;
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* instance = session.GetHero();
    if (!instance || !session.IsGameRunning() || persist::NativeLoading() ||
        persist::HeroPending() || !net::InWorld(net::LocalId())) {
        Logf("downed: host wipe deferred epoch=%08x", epoch);
        return;
    }
    Entity hero(instance);
    life.pendingHostWipeEpoch = 0;
    Die(hero, "host-wipe");
}

bool NativeDown()
{
    eCDynamicEntity* instance = gCSession::GetInstance().GetHero();
    if (!instance || life.held || lethal || life.terminal || !gCSession::GetInstance().IsGameRunning()) return false;
    Entity hero(instance);
    return hero.IsDown() && !hero.IsDead();
}

void OnWorldChange() {
    CancelAlone("world-change");
    life.OnWorldChange();
    life.Reset(true);
    recovery.Clear();
    convergence.Clear();
    nativeTerminalKills = 0;
    nativeTerminalKillChecked = false;
    terminalTask.clear();
    pendingReviveSequence = 0;
    interaction.Clear();
    interactionHero = nullptr;
    interactionWorldActive = true;
    lastInteractionFlags = 0;
    for (int i = 0; i < net::MaxPlayers; ++i) {
        puppetRecovery[i].Clear();
        peers[i].payment.Reset();
        peers[i].Reset();
    }
    crashlog::EndStep("world-change");
    Transition(life.held ? "carry-downed-life" : "reset-terminal-life", "world-change");
}
void OnWorldDeactivate() {
    CancelAlone("world-deactivate");
    life.Reset(true);
    recovery.Clear();
    convergence.Clear();
    interaction.Clear();
    interactionHero = nullptr;
    interactionWorldActive = false;
    lastInteractionFlags = 0;
    for (int i = 0; i < net::MaxPlayers; ++i) {
        puppetRecovery[i].Clear();
        peers[i].payment.Reset();
        peers[i].Reset();
    }
}
}
