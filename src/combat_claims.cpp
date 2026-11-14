// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_internal.h"
#include "combat_retarget_policy.h"
#include "ai.h"
#include "enclaves.h"
#include "hud.h"
#include "guards.h"
#include "downed.h"
#include "guid.h"
#include "log.h"
#include "melee_policy.h"
#include "net.h"
#include "npcs.h"
#include "npcremoval.h"
#include "npclease_runtime.h"
#include "npcplayback.h"
#include "remote.h"
#include "summon.h"
#include "witness.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace g3t::combat {
using namespace detail;
namespace detail {
unsigned kills = 0, xpAwarded = 0, claimsApplied = 0, playerHitsTaken = 0;
unsigned hostContactJudgements = 0, npcHitsApplied = 0, npcKills = 0, npcHitsUnresolved = 0, partyKills = 0;
unsigned reactionsStarted = 0, hitsRedirected = 0, hitsWrongAuthority = 0, hitReactionsApplied = 0;
unsigned forwardedJudged = 0, forwardedBare = 0, remoteNpcHitsIgnored = 0;
unsigned killsRepeated = 0, rewardsRepeated = 0, killsUndone = 0, duelKnockouts = 0;
unsigned knockedOut = 0, finishRefused = 0, npcHitsScaled = 0;
thread_local uint32_t hostAppliedHitIdentity = 0;
void LogAppliedHostNpcHit(eCEntity* victim, eCEntity* attacker, GEInt delta, int before, int after, GEInt result)
{
    if (delta >= 0 || !IsHostSimulatedNpc(victim)) return;
    char const* victimName = Entity(victim).GetName().GetText();
    char const* attackerName = attacker ? Entity(attacker).GetName().GetText() : "-";
    if (hostAppliedOriginKind == HostAppliedOriginClaim) {
        Logf("combat: applied-hit entry=claim attacker=claim-player-%u attacker_name=%s sequence=%u hit=claim-%u npc=%s hp=%d -> %d delta=%d result=%#x",
             hostAppliedOriginPlayer, attackerName, hostAppliedOriginSequence, hostAppliedOriginSequence,
             victimName, before, after, static_cast<int>(delta), result);
    } else if (hostAppliedOriginKind == HostAppliedOriginNpc ||
               (attacker && attacker->GetPropertySet(eEPropertySetType_NPC))) {
        const uint32_t identity = ++hostAppliedHitIdentity;
        Logf("combat: applied-hit entry=npc-origin attacker=%s hit=npc-origin-%u npc=%s hp=%d -> %d delta=%d result=%#x",
             attackerName, identity, victimName, before, after, static_cast<int>(delta), result);
    } else {
        const uint32_t identity = ++hostAppliedHitIdentity;
        Logf("combat: applied-hit entry=host-native attacker=%s hit=host-native-%u npc=%s hp=%d -> %d delta=%d result=%#x",
             attackerName, identity, victimName, before, after, static_cast<int>(delta), result);
    }
}

struct PendingNpcHit {
    uint8_t from, guid[16], weapon[16], npc[16], outcome, reaction;
    int32_t delta;
    uint32_t seen;
    uint32_t sequence, incarnation, generation;
    net::MeleeContact contact;
};
std::vector<PendingNpcHit> pendingNpcHits;
std::unordered_set<std::string> killedHere;  // host: victims killed here for a client's hit or its companion's
std::unordered_set<std::string> rewarded;    // client: victims this player was rewarded for
struct Unconfirmed {
    uint8_t guid[16];
    DWORD undoDue, expires;
    uint8_t rewardPlayer;
    bool clearDefeatedOnUndo;
    uint32_t generation, incarnation;
};
std::vector<Unconfirmed> unconfirmed;
struct PendingKillReward {
    uint8_t guid[16];
    uint32_t incarnation;
    DWORD expires;
};
std::vector<PendingKillReward> pendingKillRewards;
std::unordered_map<std::string, DWORD> knockedAt;  // owner: when a claim knocked the victim out (IsDown follows a frame later)
std::unordered_map<std::string, killconfirm::PlayerDamageStamp> playerDamaged;

bool IsCoopPlayerSide(eCEntity* entity)
{
    if (!entity) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (entity == hero || entity == remote::ControlledBody() ||
        remote::PuppetOwner(entity) != 0 || npcs::CompanionOwner(entity) != 0) {
        return true;
    }
    if (entity->GetPropertySet(eEPropertySetType_Party)) {
        Entity member(entity);
        if (static_cast<int>(member.Party.PartyMemberType) != 5 && SideOf(entity) != 0) return true;
    }
    if (hero && hero->GetPropertySet(eEPropertySetType_Party) &&
        Entity(hero).Party.GetPartyLeader().GetInstance() == entity) {
        return true;
    }
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        eCEntity* puppet = id == net::LocalId() ? nullptr : remote::PuppetOf(id);
        if (puppet && puppet->GetPropertySet(eEPropertySetType_Party) &&
            Entity(puppet).Party.GetPartyLeader().GetInstance() == entity) {
            return true;
        }
    }
    return false;
}

bool OwnsNpcDefeatXp(eCEntity* victim)
{
    if (!victim || !victim->GetPropertySet(eEPropertySetType_NPC) || HasPlayerMemory(victim) ||
        remote::IsPuppet(victim) || victim == gCSession::GetInstance().GetHero() ||
        victim == remote::ControlledBody()) {
        return false;
    }
    if (net::IsHost()) return IsHostSimulatedNpc(victim);
    return net::Connected() && npclease::LocalAuthority(victim);
}

bool PendingClaimFor(std::string const& key)
{
    if (killedHere.count(key) != 0) return true;
    for (auto const& u : unconfirmed) {
        if (KillKey(u.guid) == key) return true;
    }
    return false;
}

void AwardLocalDefeatXp(eCEntity* victim, char const* reason)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !victim || !gCSession::GetInstance().IsGameRunning()) return;
    guards::LocalPlayer localPlayer;
    Entity self(hero);
    const int before = static_cast<int>(self.PlayerMemory.XP);
    const GEInt result = CallScript("GiveXP", victim, hero, -1);
    ++xpAwarded;
    Logf("combat: rewarded co-op %s of %s: GiveXP=%d xp %d -> %d",
         reason, Entity(victim).GetName().GetText(), result, before, static_cast<int>(self.PlayerMemory.XP));
}

void NotePlayerCombatDamage(eCEntity* victim, eCEntity* attacker)
{
    if (!OwnsNpcDefeatXp(victim) || !attacker || attacker == victim || !IsCoopPlayerSide(attacker)) return;
    uint8_t guid[16];
    Guid(victim, guid);
    playerDamaged[KillKey(guid)] = {npcs::Incarnation(guid), GetTickCount()};
}

void ConfirmSharedKnockouts(DWORD now)
{
    if (!gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero()) return;
    for (auto it = playerDamaged.begin(); it != playerDamaged.end();) {
        uint8_t const* guid = reinterpret_cast<uint8_t const*>(it->first.data());
        const uint32_t currentIncarnation = npcs::Incarnation(guid);
        if (!killconfirm::RecentPlayerAssist(&it->second, currentIncarnation, now)) {
            it = playerDamaged.erase(it);
            continue;
        }
        eCEntity* rawVictim = npcs::NpcByGuid(guid);
        if (!rawVictim) {
            ++it;
            continue;
        }
        Entity victim(rawVictim);
        if (victim.NPC.DefeatedByPlayer || victim.IsDead()) {
            it = playerDamaged.erase(it);
            continue;
        }
        if (OwnsNpcDefeatXp(rawVictim) && victim.IsDown() && Outcome(victim) == net::HitKnockOut &&
            killconfirm::ShouldPaySharedDefeat(false, PendingClaimFor(it->first),
                                               IsCoopPlayerSide(victim.NPC.GetCurrentAttacker().GetInstance()),
                                               &it->second, currentIncarnation, now)) {
            victim.NPC.DefeatedByPlayer = GETrue;
            it = playerDamaged.erase(it);
            AwardLocalDefeatXp(rawVictim, "knockout");
            continue;
        }
        ++it;
    }
}
thread_local HostAppliedOriginKind hostAppliedOriginKind = HostAppliedOriginNone;
thread_local uint8_t hostAppliedOriginPlayer = 0;
thread_local uint32_t hostAppliedOriginSequence = 0;
HostAppliedOriginScope::HostAppliedOriginScope(HostAppliedOriginKind kind, uint8_t player, uint32_t sequence)
    : previousKind(hostAppliedOriginKind), previousPlayer(hostAppliedOriginPlayer), previousSequence(hostAppliedOriginSequence)
{
    hostAppliedOriginKind = kind;
    hostAppliedOriginPlayer = player;
    hostAppliedOriginSequence = sequence;
}
HostAppliedOriginScope::~HostAppliedOriginScope()
{
    hostAppliedOriginKind = previousKind;
    hostAppliedOriginPlayer = previousPlayer;
    hostAppliedOriginSequence = previousSequence;
}
// How a struck NPC takes a player's blow, as the game's AssessAgressor (0x24490, re/out/aggressor-b.c) does for its own
// player: in its routine an NPC friendly to the attacker complains first (ZS_ReactToDamage) and fights back at the next
// blow, any other fights back (AssessTarget, ReactToDamage); a fleeing one flees on; in a fight it picks its target.
// AssessTarget alone made a friend fight back at the first blow (aggressordiag 26.10.: Hamlar, struck by the host's hero,
// complained for 6 s; struck by the client, he attacked). A scoped puppet uses the same identity in GetPlayer,
// IsPlayer and GetAttitude; it must not be replaced by the host's hero partway through those checks.
// Not again while it already fights the attacker.
void React(Entity& target, eCEntity* attacker)
{
    guards::Acting player(remote::IsPuppet(attacker) && guards::PlayerContextReady() ? attacker : nullptr);
    if (target.NPC.GetCurrentTarget().GetInstance() == attacker && target.Routine.AIMode == gEAIMode_Combat) return;
    const int mode = static_cast<int>(static_cast<gEAIMode>(target.Routine.AIMode));
    CallScript("AssessAgressor", target.GetInstance(), attacker, 0);
    if (reactionsStarted < 20)
        Logf("combat: %s took a blow of player %d (AI mode %d): task %s", target.GetName().GetText(), remote::PuppetOwner(attacker), mode,
             target.Routine.GetCurrentTask().GetText());
    ++reactionsStarted;
}

// What the game's hit judgement (AssessHit, before AddHitPoints) decided for the victim: the task it set.
uint8_t Outcome(Entity& victim)
{
    const bCString task = npcs::IsMirrored(victim.GetInstance()) ? bCString(npcplayback::HitTask(victim.GetInstance()))
                                                              : victim.Routine.GetCurrentTask();
    for (char const* ko : {"ZS_Unconscious", "ZS_LieKnockDown", "ZS_PiercedKO"})
        if (task == bCString(ko)) return net::HitKnockOut;
    for (char const* dead : {"ZS_RagDollDead", "ZS_PiercedDead", "ZS_Dead"})
        if (task == bCString(dead)) return net::HitDeath;
    return net::HitUnjudged;
}

uint8_t Reaction(Entity& victim)
{
    const bCString task = npcs::IsMirrored(victim.GetInstance()) ? bCString(npcplayback::HitTask(victim.GetInstance()))
                                                              : victim.Routine.GetCurrentTask();
    if (task == bCString("ZS_QuickStumble")) return net::HitQuickStumble;
    if (task == bCString("ZS_Stumble")) return net::HitStumble;
    if (task == bCString("ZS_PierceStumble")) return net::HitPierceStumble;
    if (task == bCString("ZS_SitKnockDown")) return net::HitSitKnockDown;
    return net::HitNoReaction;
}

void ApplyReaction(Entity& victim, eCEntity* attacker, uint8_t reaction)
{
    if (RejectDownedHit(victim.GetInstance(), attacker, "received-reaction")) return;
    char const* task = nullptr;
    switch (reaction) {
    case net::HitQuickStumble: task = "ZS_QuickStumble"; break;
    case net::HitStumble: task = "ZS_Stumble"; break;
    case net::HitPierceStumble: task = "ZS_PierceStumble"; break;
    case net::HitSitKnockDown: task = "ZS_SitKnockDown"; break;
    default: return;
    }
    if (!attacker || victim.IsDead() || victim.IsDown()) return;
    victim.NPC.SetCurrentAttacker(Entity(attacker));
    victim.Routine.FullStop();
    victim.Routine.SetTask(bCString(task));
    if (++hitReactionsApplied <= 20)
        Logf("combat: native hit reaction on %s: %s", victim.GetName().GetText(), victim.Routine.GetCurrentTask().GetText());
}

eCEntity* PlayerActor(uint8_t player)
{
    return player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
}

void OnKilled(uint8_t const guid[16], uint32_t incarnation);
bool TryApplyKillReward(uint8_t const guid[16], uint32_t incarnation);
void DrainPendingKillRewards(DWORD now);
void RewardKill(uint8_t player, uint8_t const guid[16])
{
    const uint32_t incarnation = npcs::Incarnation(guid);
    if (player == net::LocalId()) {
        OnKilled(guid, incarnation);
    } else {
        net::SendKilled(player, guid, incarnation);
        net::Flush();
    }
}

void Expect(uint8_t const guid[16], uint8_t rewardPlayer = 0, bool clearDefeatedOnUndo = false)
{
    const DWORD now = GetTickCount();
    Unconfirmed u{{}, now + killconfirm::UndoDelayMs, now + killconfirm::DeathTaskExpiryMs,
                  rewardPlayer, clearDefeatedOnUndo,
                  rewardPlayer ? net::PeerGeneration(rewardPlayer) : 0, npcs::Incarnation(guid)};
    std::memcpy(u.guid, guid, 16);
    unconfirmed.push_back(u);
}

void ConfirmKills()
{
    const DWORD now = GetTickCount();
    DrainPendingKillRewards(now);
    ConfirmSharedKnockouts(now);
    for (auto it = unconfirmed.begin(); it != unconfirmed.end();) {
        Entity victim(npcs::NpcByGuid(it->guid));
        const bool present = victim.GetInstance() != nullptr;
        const bool dead = present && victim.IsDead();
        const bool deathTask = present && !dead && Outcome(victim) == net::HitDeath;
        const bool sameIncarnation = npcs::Incarnation(it->guid) == it->incarnation;
        const bool rewardEligible = it->rewardPlayer != 0 &&
            (it->rewardPlayer == net::LocalId() ||
             (net::PeerJoined(it->rewardPlayer) && net::PeerGeneration(it->rewardPlayer) == it->generation));
        const killconfirm::Action action = killconfirm::Decide(
            now, it->undoDue, it->expires, sameIncarnation, present, dead, deathTask,
            it->rewardPlayer, rewardEligible);
        if (action == killconfirm::Action::Wait) {
            ++it;
            continue;
        }
        if (action == killconfirm::Action::Reward) {
            RewardKill(it->rewardPlayer, it->guid);
        } else if (action == killconfirm::Action::UndoAndDrop) {
            if (it->clearDefeatedOnUndo) victim.NPC.DefeatedByPlayer = GEFalse;
            killedHere.erase(KillKey(it->guid));
            ++killsUndone;
            Logf("combat: the kill of %s did not take (task %s, %d hit points); its next hit counts again", victim.GetName().GetText(),
                 victim.Routine.GetCurrentTask().GetText(), HitPoints(victim));
        }
        it = unconfirmed.erase(it);
    }
}

// The species the game knocks out instead of killing (OnCheatDefeat: human and orc go to ZS_Unconscious).
bool KnocksOut(Entity& npc)
{
    const gESpecies species = npc.NPC.Species;
    return species == gESpecies_Human || species == gESpecies_Orc;
}

bool HostileToClaimant(Entity& victim, eCEntity* claimant)
{
    if (!victim.GetInstance() || !claimant || !remote::IsPuppet(claimant)) return false;
    guards::Acting player(claimant);
    GEInt attitude = CallScript("GetAttitude", victim.GetInstance(), claimant, 0);
    if (attitude == gEAttitude_Panic) attitude = ai::PlainAttitude(victim.GetInstance(), claimant);
    return attitude == gEAttitude_Hostile || attitude == gEAttitude_Predator;
}

// Owner of the NPC: another machine's NPC hit it there (where their regions meet). Applied with that NPC (our
// mirror of it) as the attacker, so this one turns on it. A lethal hit kills like a player's claim, but nobody
// earns experience: marked as defeated, the death script pays no one (static analysis of Kill).
void OnNpcHit(uint8_t from, Entity& target, int32_t delta, uint8_t const npc[16], bool party)
{
    HostAppliedOriginScope appliedOrigin(HostAppliedOriginNpc, from, 0);
    uint8_t victimGuid[16]; Guid(target.GetInstance(), victimGuid);
    if (killedHere.count(KillKey(victimGuid))) return;
    Entity attacker(npcs::NpcByGuid(npc));
    if (IgnoreFriendlyHit(target.GetInstance(), attacker.GetInstance(), party ? from : 0)) return;
    // An attacker not found here (an entity spawned only on the sender's machine, such as a weapon without an owner)
    // still hurts: dropping those hits lost all 14 of one fight (stage E4). The victim stands in as the source then.
    if (!attacker.GetInstance()) ++npcHitsUnresolved;
    // npc_hp, npc_damage, friendly_fire (code review 28.10. P2: these hits went in unscaled, a companion's blow on an enemy
    // among them). The sender does not scale an NPC's hit it forwards (Filter rule 2). An attacker unknown here that the
    // sender marked as its party's counts as that player's side.
    const int32_t unscaled = delta;
    // The sender's party mark counts first: its companion's copy here may have no party leader (stageNpcHitScale 28.10.: Lester's
    // hits unscaled).
    if (party) delta = Scaled(target.GetInstance(), PlayerActor(from), delta);
    else if (attacker.GetInstance()) delta = Scaled(target.GetInstance(), attacker.GetInstance(), delta);
    if (delta != unscaled) ++npcHitsScaled;
    ++npcHitsApplied;
    HitDispatchScope pass(forwarded, heroForward);
    eCEntity* victim = target.GetInstance();
    if (HitPoints(target) + delta > 0) {
        const int before = HitPoints(target);
        CallScript("AddHitPoints", victim, attacker.GetInstance() ? attacker.GetInstance() : victim, delta);
        if (party && HitPoints(target) < before) NotePlayerCombatDamage(victim, PlayerActor(from));
        witness::HitSeen(victim, attacker.GetInstance());  // those around hear the fight (P7)
        return;
    }
    uint8_t guid[16];
    Guid(victim, guid);
    killedHere.insert(KillKey(guid));  // before the death call: OnHitNpc drops further hits on it (R9)
    if (party) enclaves::NoteRemoteKill(guid, from, attacker.GetInstance());  // a murder by the sender's party counts (P7)
    witness::MurderSeen(victim, attacker.GetInstance());  // and those around saw the killing, as where it was decided
    const bool paysXp = !target.NPC.DefeatedByPlayer;
    target.NPC.DefeatedByPlayer = GETrue;
    CallScript("AddHitPoints", victim, victim, -HitPoints(target));
    CallScript("OnCheatKill", victim, victim, 0);
    Expect(guid, party && paysXp && !target.IsDead() ? from : 0, paysXp);
    ++npcKills;
    if (party) {  // the sender's companion or summon killed it: the sender's hero earns the experience
        if (paysXp && target.IsDead()) RewardKill(from, guid);
        ++partyKills;
    }
    Logf("combat: %s of player %u killed %s", attacker.GetInstance() ? attacker.GetName().GetText() : "an NPC", from, target.GetName().GetText());
}

// Host: a client's hit on one of the host's NPCs.
void OnHitNpc(uint8_t from, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16], uint8_t outcome,
              uint32_t seen, uint8_t reaction, uint32_t sequence, uint32_t incarnation, net::MeleeContact const& contact)
{
    (void)weapon;
    struct Reply {
        uint8_t to;
        net::HitResult result;
        ~Reply() { net::SendHitResult(to, result); }
    } reply{from, {}};
    HostAppliedOriginScope appliedOrigin(HostAppliedOriginClaim, from, sequence);
    std::memcpy(reply.result.guid, guid, 16);
    reply.result.sequence = sequence;
    const uint32_t hostIncarnation = npcs::Incarnation(guid);
    reply.result.incarnation = hostIncarnation;
    const uint32_t processedNow = net::Now();
    reply.result.processedAt = processedNow ? processedNow : 1;
    const bool hostJudged = contact.melee == net::HostJudgedMelee;
    if (!net::ValidHitContact(delta, incarnation, seen, outcome, reaction, contact)) {
        Logf("combat: hit %u refused: invalid contact", sequence);
        return;
    }
    if (!net::IsHost() || !melee::AcceptIncomingHitIncarnation(incarnation, seen, contact.melee, hostIncarnation)) {
        Logf("combat: hit %u refused: stale NPC incarnation", sequence);
        return;
    }
    Entity target(npcs::NpcByGuid(guid));  // an NPC only: no chest, door, hero or puppet (Fable 1.7)
    if (!target.GetInstance() || target.IsDead()) return;
    if (npclease::Fenced(target.GetInstance())) return;
    if (RejectDownedHit(target.GetInstance(), PlayerActor(from), "received-NPC-hit")) return;
    if (killedHere.count(KillKey(guid))) {  // killed already, its death task not running yet (R9)
        ++killsRepeated;
        return;
    }
    static const uint8_t zeros[16] = {};
    if (std::memcmp(npc, zeros, 16)) {
        Logf("combat: player %u's NPC-origin hit refused; NPC damage is host-simulated", from);
        return;
    }
    eCEntity* attacker = PlayerActor(from);
    if (!attacker) {
        Logf("combat: hit %u refused victim=%s player=%u: missing claimant", sequence, target.GetName().GetText(), from);
        return;
    }
    if (!net::InWorld(from) || downed::CombatUnavailable(attacker) ||
        !npclease::ContactAllowed(target.GetInstance(), attacker) || summon::HitBlocked(target.GetInstance())) {
        Logf("combat: hit %u refused: claimant unavailable, summon copy, or NPC authority fenced", sequence);
        return;
    }
    if (contact.melee) {
        const auto verdict = JudgeMelee(target.GetInstance(), attacker, from, contact);
        LogMelee("host-claim", from, sequence, target.GetInstance(), contact, verdict);
        if (melee::EvaluatePlayerMelee(contact.melee, verdict.allowed, verdict.relationsKnown) !=
            melee::ClientDecision::Predict) return;
    }
    if (IgnoreFriendlyHit(target.GetInstance(), attacker, from)) return;
    if (OutsideBout(target, attacker)) {
        ++duelHitsRefused;
        Logf("combat: player %u hit %s, who fights a duel or arena bout with someone else; not counted", from, target.GetName().GetText());
        return;
    }
    // Native judgement can start a new knockout; only an already-downed victim is a finishing blow.
    bool finishing = false;
    if (attacker) {
        const auto at = knockedAt.find(KillKey(guid));
        const DWORD since = at != knockedAt.end() ? GetTickCount() - at->second : 0xffffffff;
        if (target.IsDown() || since < 3000) {
            if (since < 2000 || (InBout(target))) {
                ++finishRefused;
                return;
            }
            finishing = true;
        }
    }
    if (hostJudged) {
        guards::Acting player(attacker, false);
        HitDispatchScope scope(forwarded, heroForward);
        npcplayback::HitOrigin origin(target.GetInstance(), from, sequence);
        char const* kind = "";
        int pending = 0;
        ++hostContactJudgements;
        const GEInt result = JudgeDamage(target, DamageSource(static_cast<eCDynamicEntity*>(attacker), &kind), &pending);
        if (!result || pending <= 0 || pending > 1000000) {
            Logf("combat: host-judged hit %u on %s produced no valid damage result=%#x pending=%d",
                 sequence, target.GetName().GetText(), result, pending);
            return;
        }
        delta = -pending;
        outcome = Outcome(target);
        reaction = Reaction(target);
    }
    delta = Scaled(target.GetInstance(), attacker, delta);  // friendly_fire, npc_hp (the claim carries the game's damage)
    remote::NoteContact(target.GetInstance(), attacker);  // only an eligible hit may turn it on the puppet
    npcplayback::HitOrigin origin(target.GetInstance(), from, sequence);
    // Applied with the sender's puppet as attacker; the filter's rule for hits by puppets must not drop it.
    HitDispatchScope pass(forwarded, heroForward);
    if (attacker && target.Routine.AniState == gEAniState_Parade)
        Logf("combat: a player's blow on %s parrying: parade_mode=%d can_parade=%d fov=%d", target.GetName().GetText(),
             CallScript("IsInParadeMode", target.GetInstance(), nullptr, 0), CallScript("CanParadeMoveOf", target.GetInstance(), attacker, 0),
             target.IsInFOV(Entity(attacker)) ? 1 : 0);
    // The client's view wins (owner 3 Nov.): a blow it saw land is never turned into a block because the host's copy parried.
    // P7: a blow on one lying knocked out is the game's finishing blow, whatever its hit points (the host's hero: five blows
    // without effect while it fell, then dead). Judged here: the sender's copy is not down there, so its game judged every
    // such blow a new knock-out (stageMurder 24.10.). The fall is timed for our own knock-outs only (2 s).
    ++claimsApplied;
    reply.result.accepted = 1;
    if (!finishing && (HitPoints(target) + delta > 0 || !attacker)) {
        guards::Acting player(attacker && guards::PlayerContextReady() ? attacker : nullptr);
        const int before = HitPoints(target);
        const GEInt result = CallScript("AddHitPoints", target.GetInstance(), attacker, delta);
        static DWORD lastNoEffectLog = 0;
        const DWORD now = GetTickCount();
        if (claimsApplied <= 100 || (HitPoints(target) == before && now - lastNoEffectLog >= 5000)) {
            lastNoEffectLog = now;
            Logf("combat: remote hit from player %u on %s hp %d -> %d (delta=%d result=%#x immortal=%d owner=%u mirror=%d)",
                 from, target.GetName().GetText(), before, HitPoints(target), delta, result, target.NPC.Immortal ? 1 : 0,
                 npcs::OwnerOf(target.GetInstance()), npcs::IsMirrored(target.GetInstance()));
        }
        PlayHitEffect(target.GetInstance(), false);
        eCEntity* prevTarget = target.NPC.GetCurrentTarget().GetInstance();
        // Match native AssessHit (0x2b580) + AssessAgressor (0x24490): shift CurrentAttacker -> LastAttacker before
        // recording attacker, let active stumble tasks run AssessAgressor once on completion instead of overwriting
        // them immediately, and commit earned in-combat retargets immediately so companion blows cannot steal them.
        if (attacker && !target.IsDead() && !target.IsDown()) {
            const combat_retarget::NpcCombatSnapshot snap{
                reinterpret_cast<uintptr_t>(prevTarget),
                reinterpret_cast<uintptr_t>(target.NPC.GetCurrentAttacker().GetInstance()),
                reinterpret_cast<uintptr_t>(target.NPC.GetLastAttacker().GetInstance()),
                target.Routine.AIMode == gEAIMode_Combat,
                static_cast<uint32_t>(target.NPC.CombatState),
            };
            const bool hasReactionCode = reaction == net::HitQuickStumble || reaction == net::HitStumble ||
                                         reaction == net::HitPierceStumble || reaction == net::HitSitKnockDown;
            if (!hostJudged) {
                target.NPC.SetLastAttacker(target.NPC.GetCurrentAttacker());
                target.NPC.SetCurrentAttacker(Entity(attacker));
                ApplyReaction(target, attacker, reaction);
            }
            const auto plan = combat_retarget::PlanHostNpcHit(
                snap, reinterpret_cast<uintptr_t>(attacker), hostJudged, hasReactionCode,
                Reaction(target) != net::HitNoReaction);
            if (plan.immediateCombatTarget) {
                target.NPC.SetCurrentTarget(Entity(attacker));
                target.NPC.AttackReason = gEAttackReason_ReactToDamage;
            }
            if (plan.runImmediateAssessAgressor) React(target, attacker);
        }
        if (hostJudged || reaction != net::HitNoReaction) npcs::HostHit(target.GetInstance(), from, sequence);
        if (claimsApplied <= 200) {
            char id[40] = {};
            guid::Format(guid, id, sizeof(id));
            eCEntity* nextTarget = target.NPC.GetCurrentTarget().GetInstance();
            eCEntity* lastAttacker = target.NPC.GetLastAttacker().GetInstance();
            Logf("NPCHITSYNC stage=host-claim player=%u seq=%u guid=%s target=%s reaction=%u host_judged=%d "
                 "prev_target=%s next_target=%s last_attacker=%s processed_at=%u",
                 from, sequence, id, target.GetName().GetText(), reaction, hostJudged ? 1 : 0,
                 prevTarget ? prevTarget->GetName().GetText() : "-",
                 nextTarget ? nextTarget->GetName().GetText() : "-",
                 lastAttacker ? lastAttacker->GetName().GetText() : "-",
                 reply.result.processedAt);
        }
        witness::HitSeen(target.GetInstance(), attacker);  // and those around hear the fight (P7)
        return;
    }
    if (InBout(target)) {
        // The bout's end, as the game ends it: knocked out by its opponent (ZS_Unconscious takes CurrentAttacker and runs
        // Defeat: AIMode 8, the bout's quest and victory notice), not killed.
        eCEntity* victim = target.GetInstance();
        target.NPC.SetCurrentAttacker(Entity(attacker));
        CallScript("AddHitPoints", victim, attacker, -HitPoints(target));
        target.Routine.FullStop();
        target.Routine.SetTask(bCString("ZS_Unconscious"));
        ++duelKnockouts;
        Logf("combat: player %u won the duel or arena bout against %s (knocked out, not killed)", from, target.GetName().GetText());
        return;
    }
    if (!finishing) {
        const bool hostileToClaimant = HostileToClaimant(target, attacker);
        // As the game decided on the sender's machine; a hit judged there as neither (that machine's copy had more hit points
        // left) by the species rule.
        if (!hostileToClaimant && (outcome == net::HitKnockOut || (outcome == net::HitUnjudged && KnocksOut(target)))) {
            // The game's way, as in the bout above: ZS_Unconscious runs Defeat(CurrentAttacker, victim). The puppet as attacker;
            // remote.cpp lets go of it when the puppet goes. The experience on the sender's machine, once per victim (OnKilled).
            eCEntity* victim = target.GetInstance();
            const bool paysXp = !target.NPC.DefeatedByPlayer;
            target.NPC.SetCurrentAttacker(Entity(attacker));
            target.NPC.DefeatedByPlayer = GETrue;
            CallScript("AddHitPoints", victim, attacker, -HitPoints(target));
            target.Routine.FullStop();
            target.Routine.SetTask(bCString("ZS_Unconscious"));
            knockedAt[KillKey(guid)] = GetTickCount();
            witness::KnockOutSeen(victim, attacker);
            ++knockedOut;
            if (paysXp) RewardKill(from, guid);
            Logf("combat: player %u knocked %s out (outcome %u)", from, target.GetName().GetText(), outcome);
            return;
        }
    }
    // Lethal. Not a hit judged by the game: for any attacker but the local player the hit judgement caps the
    // damage and never kills (the puppet's hit left the NPC standing at 0 hit points). Not the death task set
    // by hand either (a raider killed that way in mid-fight crashed the host in GetCombatMoveLength the next
    // frame). The game's own way to kill an NPC from outside is its kill cheat, OnCheatKill (stop, clear the
    // target, ZS_RagDollDead or ZS_Dead by species); the dying NPC's Kill script then does the bookkeeping.
    // The NPC itself as the killer, not the sender's puppet: its death states keep the killer, and a puppet removed
    // right after (its player left) made them use a deleted entity (eCEntity::QueryEntityProxyInternal crash on a
    // client in stage E2). The real killer is rewarded on its own machine (SendKilled).
    eCEntity* victim = target.GetInstance();
    const bool paysXp = !target.NPC.DefeatedByPlayer;
    killedHere.insert(KillKey(guid));  // before the death call (R9)
    knockedAt.erase(KillKey(guid));
    enclaves::NoteRemoteKill(guid, from, attacker);  // its Kill counts the murder for the sender (P7)
    witness::MurderSeen(victim, attacker);  // those around saw it, as the finishing blow tells them
    target.NPC.DefeatedByPlayer = GETrue;
    CallScript("AddHitPoints", victim, victim, -HitPoints(target));
    CallScript("OnCheatKill", victim, victim, 0);  // like the console: self = the one who kills, other = the target
    Expect(guid, paysXp && !target.IsDead() ? from : 0, paysXp);
    ++kills;
    if (paysXp && target.IsDead()) RewardKill(from, guid);
    Logf("combat: player %u killed %s (OnCheatKill%s)", from, target.GetName().GetText(), finishing ? ", finishing blow" : "");  // ZS_RagDollDead takes effect next frame
}

void QueueHitNpc(uint8_t from, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16],
                 uint8_t outcome, uint32_t seen, uint8_t reaction, uint32_t sequence, uint32_t incarnation,
                 net::MeleeContact const& contact)
{
    if (pendingNpcHits.size() >= 1024) {
        Log("combat: NPC hit queue full; hit refused");
        net::HitResult result;
        std::memcpy(result.guid, guid, 16);
        result.sequence = sequence; result.incarnation = incarnation;
        net::SendHitResult(from, result);
        return;
    }
    PendingNpcHit hit{};
    hit.from = from; hit.delta = delta; hit.outcome = outcome; hit.seen = seen; hit.reaction = reaction;
    hit.sequence = sequence; hit.incarnation = incarnation;
    hit.generation = net::PeerGeneration(from);
    hit.contact = contact;
    std::memcpy(hit.guid, guid, 16);
    std::memcpy(hit.weapon, weapon, 16);
    std::memcpy(hit.npc, npc, 16);
    pendingNpcHits.push_back(hit);
}

bool TryApplyKillReward(uint8_t const guid[16], uint32_t incarnation)
{
    if (npcremoval::Suppressed(guid)) return true;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    _GUID raw;
    std::memcpy(&raw, guid, 16);
    Entity victim = guid::Resolve(raw);
    if (!hero || !victim.GetInstance() || !gCSession::GetInstance().IsGameRunning()) return false;
    std::string identity(reinterpret_cast<char const*>(guid), 16);
    identity.append(reinterpret_cast<char const*>(&incarnation), sizeof(incarnation));
    if (!rewarded.insert(identity).second) {
        ++rewardsRepeated;
        Logf("combat: the kill of %s was rewarded already; not again", victim.GetName().GetText());
        return true;
    }
    guards::LocalPlayer localPlayer;
    Entity self(hero);
    const int before = static_cast<int>(self.PlayerMemory.XP);
    const GEInt result = CallScript("GiveXP", victim.GetInstance(), hero, -1);  // like the death script: self = the NPC, other = who earns it
    ++xpAwarded;
    Logf("combat: rewarded the kill of %s: GiveXP=%d xp %d -> %d", victim.GetName().GetText(), result, before,
         static_cast<int>(self.PlayerMemory.XP));
    return true;
}

void DrainPendingKillRewards(DWORD now)
{
    for (auto it = pendingKillRewards.begin(); it != pendingKillRewards.end();) {
        if (TryApplyKillReward(it->guid, it->incarnation) ||
            static_cast<int32_t>(now - it->expires) >= 0) {
            it = pendingKillRewards.erase(it);
        } else {
            ++it;
        }
    }
}

// Client: this player's hit killed a host NPC: the game's own reward (GiveXP computes it from the NPC's level).
void OnKilled(uint8_t const guid[16], uint32_t incarnation)
{
    if (TryApplyKillReward(guid, incarnation)) return;
    if (pendingKillRewards.size() >= 64) return;
    PendingKillReward pending{};
    std::memcpy(pending.guid, guid, 16);
    pending.incarnation = incarnation;
    pending.expires = GetTickCount() + killconfirm::PendingRewardExpiryMs;
    pendingKillRewards.push_back(pending);
}

// Client: something on the host hit this player's puppet; the real hero takes the damage.
void OnHitPlayer(int32_t delta, uint8_t const[16], uint8_t attackerPlayer, bool meansToKill)
{
    if (!attackerPlayer) {
        if (++remoteNpcHitsIgnored <= 10)
            Log("combat: remote NPC hit ignored; the local game decides contacts with its own hero");
        return;
    }
    // P6: in an animal form the creature the player controls takes it, not the hidden human body (GetHero).
    eCEntity* hero = gCSession::GetInstance().GetHero() ? remote::ControlledBody() : nullptr;
    if (!hero || delta >= 0) return;
    GroundProbe ground(hero, remote::PuppetOf(attackerPlayer), "received-player-hit");
    if (RejectDownedHit(hero, remote::PuppetOf(attackerPlayer), "received-player-hit")) return;
    if (IgnoreFriendlyHit(hero, remote::PuppetOf(attackerPlayer), attackerPlayer)) return;
    ++playerHitsTaken;
    if (meansToKill) downed::NoteBlow();
    eCEntity* by = remote::PuppetOf(attackerPlayer);
    if (!by || by == hero) {
        // Bare hit points, as before: without the hit judgement the hero neither stumbles nor falls, and at 0 hit
        // points kept walking (stage A exploration).
        delta = Scaled(hero, remote::PuppetOf(attackerPlayer), delta);
        CallScript("AddHitPoints", hero, nullptr, delta);
        ++forwardedBare;
        return;
    }
    // The hit judged here against this hero, with the attacker's weapon: the game's own stumble, knock-out and
    // death decisions (a death becomes a knock-out, downed.cpp).
    char const* kind = "";
    Entity self(hero);
    int pending = 0;
    const int hpBefore = self.PlayerMemory.GetHitPoints(), spBefore = self.PlayerMemory.GetStaminaPoints();
    GEInt result;
    {
        HitDispatchScope scope(forwarded, heroForward, true);
        result = GameHit(self, DamageSource(static_cast<eCDynamicEntity*>(by), &kind), &pending);
    }
    ++forwardedJudged;
    if (forwardedJudged <= 100)  // QA guard; the first 100 for stageDamageHits (the game's damage and the applied)
        Logf("combat: forwarded blow judged here: result=%#x pending=%d hp %d -> %d sp %d -> %d", result, pending, hpBefore,
             self.PlayerMemory.GetHitPoints(), spBefore, self.PlayerMemory.GetStaminaPoints());
}
void DrainQueuedNpcHits()
{
    if (!installed || !gCSession::GetInstance().IsGameRunning() || pendingNpcHits.empty()) return;
    auto hits = std::move(pendingNpcHits);
    pendingNpcHits.clear();
    for (auto const& hit : hits) {
        if (!net::PeerJoined(hit.from) || net::PeerGeneration(hit.from) != hit.generation) {
            Logf("combat: queued hit %u refused after player %u's connection changed", hit.sequence, hit.from);
            continue;
        }
        OnHitNpc(hit.from, hit.guid, hit.delta, hit.weapon, hit.npc, hit.outcome, hit.seen, hit.reaction,
                 hit.sequence, hit.incarnation, hit.contact);
    }
}
void ClearClaims()
{
    pendingNpcHits.clear();
    pendingKillRewards.clear();
    killedHere.clear();
    unconfirmed.clear();
    rewarded.clear();
    knockedAt.clear();
    playerDamaged.clear();
}
}
void ResolveKillXp(eCEntity* victim, eCEntity* killer, bool wasDefeated)
{
    if (!victim || !victim->GetPropertySet(eEPropertySetType_NPC)) return;
    uint8_t guid[16];
    Guid(victim, guid);
    const std::string key = KillKey(guid);
    const auto found = playerDamaged.find(key);
    const killconfirm::PlayerDamageStamp stamp = found != playerDamaged.end() ? found->second : killconfirm::PlayerDamageStamp{};
    const bool hasStamp = found != playerDamaged.end();
    if (hasStamp) playerDamaged.erase(found);
    if (!OwnsNpcDefeatXp(victim)) return;
    Entity victimEntity(victim);
    const bool alreadyDefeated = wasDefeated || victimEntity.NPC.DefeatedByPlayer != GEFalse;
    if (!killconfirm::ShouldPaySharedDefeat(alreadyDefeated, PendingClaimFor(key), IsCoopPlayerSide(killer),
                                            hasStamp ? &stamp : nullptr, npcs::Incarnation(guid), GetTickCount())) {
        return;
    }
    victimEntity.NPC.DefeatedByPlayer = GETrue;
    AwardLocalDefeatXp(victim, "shared kill");
}
void SyncReport(LogFn log)
{
    char line[768];
    Format(line, "SYNCCOMBAT claims_sent=%u claims_applied=%u local_judgements=%u no_pending_damage=%u "
                 "player_hits_sent=%u player_hits_received=%u inventory_open=%d inventory_hits_ignored=%u "
                 "redirected=%u stale_authority=%u hit_reactions=%u local_npc_hits=%u "
                 "remote_npc_hits_ignored=%u invisible_contacts_ignored=%u friendly_hits_ignored=%u duplicate_claims_ignored=%u "
                 "host_judged_claims_sent=%u host_contact_judgements=%u local_melee_refused=%u",
           claimsSent, claimsApplied, clientJudgements, clientJudgementsWithoutDamage, playerHitsSent, playerHitsTaken,
           hud::InventoryOpen() ? 1 : 0, inventoryHitsIgnored, hitsRedirected, hitsWrongAuthority, hitReactionsApplied,
           localNpcHits, remoteNpcHitsIgnored, invisibleContactsIgnored, friendlyHitsIgnored, duplicateClaimsIgnored,
           hostJudgedClaimsSent, hostContactJudgements, localMeleeRefused);
    log(line);
}
bool LethalHit(eCEntity* victim)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity target(victim);
    if (!hero || !victim || target.IsDead()) return false;
    const int hp = HitPoints(target);
    if (hp > 1) CallScript("AddHitPoints", victim, hero, 1 - hp);
    char const* kind = "";
    int pending = 0;
    GameHit(target, DamageSource(hero, &kind), &pending);
    return true;
}
}
