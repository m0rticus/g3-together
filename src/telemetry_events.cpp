// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry_internal.h"

#include "npcs.h"
#include "remote.h"

#include <cstring>

namespace g3t::telemetry {
namespace {

using detail::AppendEntity;
using detail::CountHeroGold;
using detail::CrimeLabel;
using detail::DistanceMeters;
using detail::Event;
using detail::groundFilter;
using detail::lastCombatCause;
using detail::lastCrimeCause;
using detail::lastLootCause;
using detail::lastQuestCause;
using detail::NowMs;
using detail::OutcomeLabel;
using detail::PeerLabel;
using detail::ReactionLabel;
using detail::ReasonLabel;
using detail::recentJudge;
using detail::SafeEntityName;
using schema::QuestStatusLabel;

} // namespace

void NoteCombatJudge(char const* script, eCEntity* victim, eCEntity* attacker, eCEntity* source, int hpBefore)
{
    const uint32_t now = NowMs();
    recentJudge.atMs = now;
    recentJudge.victim = victim ? eCEntityProxy(victim) : eCEntityProxy();
    recentJudge.attacker = attacker ? eCEntityProxy(attacker) : eCEntityProxy();
    recentJudge.source = source ? eCEntityProxy(source) : eCEntityProxy();
    Format(recentJudge.script, "%s", script ? script : "judge");
    Format(recentJudge.attackerName, "%s", SafeEntityName(attacker));
    Format(recentJudge.sourceName, "%s", SafeEntityName(source));
    if (victim && (victim == gCSession::GetInstance().GetHero() || victim == remote::ControlledBody())) {
        char detailText[96];
        Format(detailText, "%s(hp_before=%d,weapon=%s)", recentJudge.script, hpBefore, recentJudge.sourceName);
        lastCombatCause.Record(LastTelemetrySeq() + 1, now, "combat_judge",
                               recentJudge.attackerName, SafeEntityName(victim), detailText);
    }
}

void OnCombatHit(char const* path, eCEntity* attacker, eCEntity* victim, eCEntity* weapon,
                 int hpBefore, int hpAfter, int delta, uint8_t outcome, uint8_t reaction, uint32_t seq)
{
    const uint32_t now = NowMs();
    char const* fallbackAttackerName = nullptr;
    if (!attacker && victim && recentJudge.victim.GetEntity() == victim &&
        static_cast<uint32_t>(now - recentJudge.atMs) <= 250) {
        attacker = recentJudge.attacker.GetEntity();
        fallbackAttackerName = recentJudge.attackerName;
    }
    Event ev;
    ev.kv.Str("path", path ? path : "hit").Int("delta", delta).TransitionInt("hp", hpBefore, hpAfter)
      .Str("outcome", OutcomeLabel(outcome)).Str("reaction", ReactionLabel(reaction))
      .Uint("hit_seq", seq).Str("weapon", SafeEntityName(weapon));
    AppendEntity(ev.kv, "attacker", attacker, nullptr, fallbackAttackerName);
    AppendEntity(ev.kv, "victim", victim);
    const uint64_t tseq = ev.Send("combat.hit");
    char detailText[96];
    Format(detailText, "%s:%d->%d(%s)", path ? path : "hit", hpBefore, hpAfter, OutcomeLabel(outcome));
    char const* actorName = attacker ? SafeEntityName(attacker) : (fallbackAttackerName ? fallbackAttackerName : "-");
    lastCombatCause.Record(tseq, now, "combat.hit", actorName, SafeEntityName(victim), detailText);
}

void OnCombatIgnored(char const* reason, eCEntity* attacker, eCEntity* victim, uint8_t attackerPlayer)
{
    Event ev;
    ev.kv.Str("reason", reason ? reason : "ignored").Uint("attacker_player", attackerPlayer);
    AppendEntity(ev.kv, "attacker", attacker);
    AppendEntity(ev.kv, "victim", victim);
    ev.Send("combat.ignored");
}

void OnCombatClaim(char const* decision, uint8_t fromPlayer, eCEntity* victim, uint8_t const guid[16],
                   int32_t delta, uint8_t outcome, uint8_t reaction, uint32_t seq, int hpBefore, int hpAfter)
{
    Event ev;
    ev.kv.Str("decision", decision ? decision : "claim").Uint("from_player", fromPlayer)
      .Str("from_name", net::PeerName(fromPlayer)).Int("delta", delta).TransitionInt("hp", hpBefore, hpAfter)
      .Str("outcome", OutcomeLabel(outcome)).Str("reaction", ReactionLabel(reaction)).Uint("claim_seq", seq);
    AppendEntity(ev.kv, "victim", victim, guid);
    const uint64_t tseq = ev.Send("combat.claim");
    if (hpAfter != hpBefore) {
        char detailText[96];
        Format(detailText, "claim#%u:%d->%d", seq, hpBefore, hpAfter);
        lastCombatCause.Record(tseq, NowMs(), "combat.claim", net::PeerName(fromPlayer),
                               SafeEntityName(victim), detailText);
    }
}

void OnCombatDefeat(char const* kind, eCEntity* killer, eCEntity* victim, uint8_t killerPlayer,
                    bool finishing, bool paysXp)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("kind", kind ? kind : "defeat").Uint("killer_player", killerPlayer)
      .Bool("finishing", finishing).Bool("pays_xp", paysXp);
    AppendEntity(ev.kv, "killer", killer);
    AppendEntity(ev.kv, "victim", victim);
    const uint64_t tseq = ev.Send("combat.defeat");
    lastCombatCause.Record(tseq, now, kind ? kind : "defeat", SafeEntityName(killer), SafeEntityName(victim), "defeated");
    lastCrimeCause.Record(tseq, now, kind ? kind : "defeat", SafeEntityName(killer), SafeEntityName(victim), "defeated");
}

void OnGroundHit(eCEntity* victim, eCEntity* attacker, char const* entry, int beforeHp, int afterHp,
                 char const* source, char const* protection, char const* decision, uint32_t episode)
{
    if (!groundFilter.ShouldEmit(beforeHp, afterHp, entry, decision, episode)) return;
    const uint32_t now = NowMs();
    char resolvedVia[32] = "direct";
    char const* fallbackAttackerName = nullptr;
    if (!attacker && victim) {
        if (recentJudge.victim.GetEntity() == victim && static_cast<uint32_t>(now - recentJudge.atMs) <= 300) {
            attacker = recentJudge.attacker.GetEntity();
            fallbackAttackerName = recentJudge.attackerName;
            Format(resolvedVia, "judge:%s", recentJudge.script);
        } else if (victim->GetPropertySet(eEPropertySetType_NPC)) {
            if (eCEntity* cur = Entity(victim).NPC.GetCurrentAttacker().GetInstance()) {
                attacker = cur;
                Format(resolvedVia, "current_attacker");
            }
        }
    }
    char callerStack[320] = "-";
    if (!attacker && !fallbackAttackerName && beforeHp != afterHp)
        trace::CallerChain(2, 5, callerStack, sizeof(callerStack));
    Event ev;
    ev.kv.Str("entry", entry ? entry : "-").Str("source", source ? source : "-")
      .TransitionInt("hp", beforeHp, afterHp).Int("delta", afterHp - beforeHp)
      .Str("protection", protection ? protection : "none")
      .Str("decision", decision ? decision : "complete").Uint("episode", episode).Str("attacker_via", resolvedVia);
    AppendEntity(ev.kv, "victim", victim);
    AppendEntity(ev.kv, "attacker", attacker, nullptr, fallbackAttackerName);
    if (callerStack[0] != '-') ev.kv.Str("stack", callerStack);
    lastCombatCause.AppendTo(ev.kv, now, 1500, "cause");
    ev.Send("combat.ground_hit");
}

void OnDownedEvent(char const* event, uint8_t player, uint32_t episode, bool held, bool terminal,
                   int hp, eCEntity* attacker, uint8_t helper, char const* reason)
{
    const uint32_t now = NowMs();
    char const* fallbackAttackerName = nullptr;
    if (!attacker && static_cast<uint32_t>(now - recentJudge.atMs) <= 500) {
        eCEntity* judgedVictim = recentJudge.victim.GetEntity();
        const bool matchesPlayer = judgedVictim &&
            ((player == net::LocalId() &&
              (judgedVictim == gCSession::GetInstance().GetHero() || judgedVictim == remote::ControlledBody())) ||
             (player != 0 && player != net::LocalId() && judgedVictim == remote::PuppetOf(player)));
        if (matchesPlayer) {
            attacker = recentJudge.attacker.GetEntity();
            fallbackAttackerName = recentJudge.attackerName;
        }
    }
    Event ev;
    ev.kv.Str("event", event ? event : "downed").Uint("player", player).Uint("episode", episode)
      .Bool("held", held).Bool("terminal", terminal).Int("hp", hp).Uint("helper", helper)
      .Str("reason", reason ? reason : "-");
    AppendEntity(ev.kv, "attacker", attacker, nullptr, fallbackAttackerName);
    lastCombatCause.AppendTo(ev.kv, now, 3000, "cause");
    ev.Send("downed.state");
}

void OnShotFired(uint8_t shooterPlayer, eCEntity* casterNpc, char const* projectile, char const* spell,
                 uint32_t netId, uint8_t const targetGuid[16], uint8_t targetPlayer, int32_t damage, float velocity)
{
    Event ev;
    ev.kv.Uint("shooter_player", shooterPlayer).Hex("net_id", netId).Str("projectile", projectile)
      .Str("spell", spell).Int("damage", damage).Float1("velocity", velocity).Uint("target_player", targetPlayer);
    if (casterNpc) AppendEntity(ev.kv, "caster_npc", casterNpc);
    if (targetGuid) AppendEntity(ev.kv, "target", nullptr, targetGuid);
    ev.Send("shot.fire");
}

void OnShotSpawned(uint8_t fromPlayer, char const* projectile, char const* spell, uint32_t netId, uint8_t targetPlayer)
{
    Event ev;
    ev.kv.Uint("from_player", fromPlayer).Hex("net_id", netId).Str("projectile", projectile)
      .Str("spell", spell).Uint("target_player", targetPlayer);
    ev.Send("shot.spawn");
}

void OnXpEvent(char const* source, uint8_t fromPlayer, int beforeXp, int afterXp, int delta, unsigned level)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("source", source ? source : "xp").Uint("from_player", fromPlayer)
      .TransitionInt("xp", beforeXp, afterXp).Int("delta", delta).Uint("level", level);
    lastCombatCause.AppendTo(ev.kv, now, 3000, "combat_cause");
    lastQuestCause.AppendTo(ev.kv, now, 3000, "quest_cause");
    ev.Send("xp.change");
}

void OnArenaBind(eCEntity* fighter, eCEntity* arenaEntity, char const* arenaName, char const* questName,
                 uint8_t challenger, bool awaitingSync)
{
    eCEntity* challengerBody = (challenger == net::LocalId())
        ? static_cast<eCEntity*>(gCSession::GetInstance().GetHero())
        : remote::PuppetOf(challenger);
    Event ev;
    ev.kv.Str("arena", arenaName).Str("quest", questName).Uint("challenger", challenger)
      .Str("challenger_name", PeerLabel(challenger))
      .Bool("awaiting_sync", awaitingSync).Float1("dist_to_challenger_m", DistanceMeters(fighter, challengerBody));
    AppendEntity(ev.kv, "fighter", fighter);
    AppendEntity(ev.kv, "challenger_body", challengerBody);
    if (arenaEntity && arenaEntity->GetPropertySet(eEPropertySetType_Arena))
        ev.kv.Int("arena_status", static_cast<int>(Entity(arenaEntity).Arena.Status));
    ev.Send("arena.bind");
}

void OnArenaEvent(char const* event, char const* arenaName, char const* questName, eCEntity* fighter,
                  uint8_t challenger, int prevStatus, int nextStatus, char const* detail)
{
    eCEntity* challengerBody = (challenger == net::LocalId())
        ? static_cast<eCEntity*>(gCSession::GetInstance().GetHero())
        : (challenger ? remote::PuppetOf(challenger) : nullptr);
    Event ev;
    ev.kv.Str("event", event ? event : "state").Str("arena", arenaName).Str("quest", questName)
      .Uint("challenger", challenger)
      .TransitionStr("quest_status", QuestStatusLabel(prevStatus), QuestStatusLabel(nextStatus))
      .Float1("dist_to_challenger_m", DistanceMeters(fighter, challengerBody));
    if (detail && *detail) ev.kv.Str("detail", detail);
    if (fighter) {
        AppendEntity(ev.kv, "fighter", fighter);
        if (fighter->GetPropertySet(eEPropertySetType_NPC)) {
            auto* ps = static_cast<gCNPC_PS*>(fighter->GetPropertySet(eEPropertySetType_NPC));
            ev.kv.Str("fighter_crime", CrimeLabel(static_cast<int>(static_cast<gEPlayerCrime const&>(ps->AccessLastPlayerCrime()))))
              .Str("fighter_attack_reason", ReasonLabel(static_cast<int>(Entity(fighter).NPC.AttackReason)));
        }
    }
    ev.Send("arena.state");
}

void OnContainerAccess(char const* action, eCEntity* actor, eCEntity* container, bool tradeMode, char const* result)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("action", action ? action : "open").Bool("trade_mode", tradeMode).Str("result", result ? result : "ok");
    AppendEntity(ev.kv, "actor", actor);
    AppendEntity(ev.kv, "container", container);
    if (container && container->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity c(container);
        ev.kv.Int("stacks", c.Inventory.GetStackCount()).Int("gold", CountHeroGold(c));
    }
    const uint64_t seq = ev.Send("loot.container");
    if (!tradeMode && container) {
        lastLootCause.Record(seq, now, action ? action : "chest", SafeEntityName(actor),
                             SafeEntityName(container), result ? result : "open");
        lastCrimeCause.Record(seq, now, "chest_access", SafeEntityName(actor),
                              SafeEntityName(container), result ? result : "open");
    }
}

void OnWorldPickup(char const* action, uint8_t player, char const* itemName, uint8_t const guid[16],
                   uint32_t amount, eCEntity* entity)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("action", action ? action : "take").Uint("player", player).Str("item", itemName).Uint("amount", amount);
    AppendEntity(ev.kv, "entity", entity, guid);
    const uint64_t seq = ev.Send("loot.pickup");
    char detailText[96];
    Format(detailText, "%s x%u", itemName ? itemName : "item", amount);
    char const* actor = PeerLabel(player);
    lastLootCause.Record(seq, now, "pickup", actor, itemName, detailText);
    lastCrimeCause.Record(seq, now, "pickup", actor, itemName, detailText);
}

void OnTradeOffer(char const* stage, uint8_t buyer, uint8_t seller, uint32_t serial, int32_t gold,
                  uint16_t itemCount, char const* summary, uint8_t resultCode)
{
    Event ev;
    ev.kv.Str("stage", stage ? stage : "offer")
      .Uint("buyer", buyer).Str("buyer_name", PeerLabel(buyer))
      .Uint("seller", seller).Str("seller_name", PeerLabel(seller))
      .Uint("serial", serial).Int("gold", gold).Uint("entry_count", itemCount).Uint("result_code", resultCode)
      .Str("items", summary && *summary ? summary : "none");
    const uint64_t seq = ev.Send("trade.exchange");
    lastLootCause.Record(seq, NowMs(), "player_trade", PeerLabel(buyer), PeerLabel(seller), summary);
}

void OnCrimeTransfer(char const* kind, uint8_t actorPlayer, uint8_t victimPlayer, eCEntity* npc,
                     int32_t goldAmount, uint32_t seqNum)
{
    Event ev;
    ev.kv.Str("kind", kind ? kind : "crime_transfer").Uint("actor_player", actorPlayer)
      .Uint("victim_player", victimPlayer).Int("gold", goldAmount).Uint("seq", seqNum);
    AppendEntity(ev.kv, "npc", npc);
    const uint64_t seq = ev.Send("crime.transfer");
    lastLootCause.Record(seq, NowMs(), kind ? kind : "ransack", SafeEntityName(npc), "gold", kind);
}

void OnWitnessEvent(char const* event, eCEntity* witnessNpc, eCEntity* subject, uint8_t player,
                    int reason, int crimeBefore, int crimeAfter, char const* detail)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("event", event ? event : "witness").Uint("player", player)
      .Int("reason_id", reason).Str("reason", ReasonLabel(reason))
      .TransitionStr("crime_mem", CrimeLabel(crimeBefore), CrimeLabel(crimeAfter));
    if (detail && *detail) ev.kv.Str("detail", detail);
    AppendEntity(ev.kv, "witness", witnessNpc);
    AppendEntity(ev.kv, "subject", subject);
    lastLootCause.AppendTo(ev.kv, now, 3000, "loot_cause");
    const uint64_t seq = ev.Send("witness.react");
    lastCrimeCause.Record(seq, now, event ? event : "witness", SafeEntityName(witnessNpc),
                          SafeEntityName(subject), ReasonLabel(reason));
}

void OnEnclaveEvent(char const* event, char const* enclaveName, uint8_t fromPlayer, char const* modeName,
                    int32_t const crimeBefore[5], int32_t const crimeAfter[5],
                    int32_t knownBefore, int32_t knownAfter, int32_t fameBefore, int32_t fameAfter)
{
    const uint32_t now = NowMs();
    int32_t const zero[5] = {};
    int32_t const* cb = crimeBefore ? crimeBefore : zero;
    int32_t const* ca = crimeAfter ? crimeAfter : zero;
    Event ev;
    ev.kv.Str("event", event ? event : "change").Str("enclave", enclaveName).Uint("from_player", fromPlayer)
      .Str("rep_mode", modeName ? modeName : "-")
      .TransitionInt("fame", fameBefore, fameAfter)
      .TransitionInt("theft", cb[gEPlayerCrime_Theft], ca[gEPlayerCrime_Theft])
      .TransitionInt("murder", cb[gEPlayerCrime_Murder], ca[gEPlayerCrime_Murder])
      .TransitionInt("attack", cb[gEPlayerCrime_Attack], ca[gEPlayerCrime_Attack])
      .TransitionInt("livestock", cb[gEPlayerCrime_Livestock], ca[gEPlayerCrime_Livestock])
      .TransitionStr("known", CrimeLabel(knownBefore), CrimeLabel(knownAfter));
    lastCrimeCause.AppendTo(ev.kv, now, 5000, "crime_cause");
    lastLootCause.AppendTo(ev.kv, now, 5000, "loot_cause");
    lastQuestCause.AppendTo(ev.kv, now, 5000, "quest_cause");
    ev.Send("enclave.state");
}

void OnGuardEvent(char const* event, eCEntity* guardNpc, eCEntity* targetBody, int attackReason,
                  int lastCrime, bool lethal)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("event", event ? event : "guard").Str("attack_reason", ReasonLabel(attackReason))
      .Int("attack_reason_id", attackReason).Str("last_crime", CrimeLabel(lastCrime)).Bool("means_to_kill", lethal);
    AppendEntity(ev.kv, "guard", guardNpc);
    AppendEntity(ev.kv, "target", targetBody);
    lastCrimeCause.AppendTo(ev.kv, now, 8000, "crime_cause");
    ev.Send("guard.intent");
}

void OnTalkEvent(char const* event, uint8_t player, eCEntity* npc, uint8_t const guid[16], char const* detail)
{
    Event ev;
    ev.kv.Str("event", event ? event : "talk").Uint("player", player).Str("player_name", PeerLabel(player));
    if (detail && *detail) ev.kv.Str("detail", detail);
    AppendEntity(ev.kv, "npc", npc, guid);
    ev.Send("dialog.sync");
}

void OnNpcProgression(char const* source, uint8_t fromPlayer, eCEntity* npc, uint8_t dialogBits, uint8_t npcBits,
                      uint8_t guardStatus, uint8_t npcClass, uint8_t alignment, uint8_t immortal,
                      uint8_t enclaveStatus, bool moved, char const* routine)
{
    char flags[192];
    Format(flags, "teach:%d,trade:%d,party:%d,mob:%d,slave:%d,talked:%d,defeated:%d",
           (dialogBits & 0x01) != 0, (dialogBits & 0x02) != 0, (dialogBits & 0x04) != 0,
           (dialogBits & 0x08) != 0, (dialogBits & 0x10) != 0, (dialogBits & 0x20) != 0, (npcBits & 0x04) != 0);
    Event ev;
    ev.kv.Str("source", source ? source : "snapshot").Uint("from_player", fromPlayer).Str("flags", flags)
      .Uint("guard_status", guardStatus).Uint("npc_class", npcClass).Uint("alignment", alignment)
      .Uint("immortal", immortal).Uint("enclave_status", enclaveStatus)
      .Bool("moved", moved).Str("routine", routine && *routine ? routine : "-");
    AppendEntity(ev.kv, "npc", npc);
    ev.Send("npc.progression");
}

void OnSectorChange(char const* source, uint8_t fromPlayer, char const* sectorName, bool enabled)
{
    Event ev;
    ev.kv.Str("source", source ? source : "sector").Uint("from_player", fromPlayer)
      .Str("sector", sectorName).Bool("enabled", enabled);
    ev.Send("world.sector");
}

void OnGuideEvent(char const* event, uint8_t player, eCEntity* npc, uint8_t const guid[16],
                  bool follower, char const* questOrRoutine)
{
    Event ev;
    ev.kv.Str("event", event ? event : "guide").Uint("player", player)
      .Str("role_kind", follower ? "follower" : "guide").Str("quest_or_routine", questOrRoutine);
    AppendEntity(ev.kv, "npc", npc, guid);
    ev.Send("companion.guide");
}

void OnOccupancyEvent(char const* event, uint8_t player, eCEntity* object, uint8_t const guid[16],
                      eCEntity* user, char const* reason)
{
    Event ev;
    ev.kv.Str("event", event ? event : "occupancy").Uint("player", player).Str("reason", reason);
    AppendEntity(ev.kv, "object", object, guid);
    if (user) AppendEntity(ev.kv, "user", user);
    ev.Send("interact.occupancy");
}

void OnLeaseEvent(char const* event, uint8_t ownerBefore, uint8_t ownerAfter, uint8_t const guid[16],
                  eCEntity* npc, uint32_t epoch, uint32_t incarnation, char const* reason)
{
    if (!npc && guid) npc = npcs::NpcByGuid(guid);
    eCEntity* localHero = gCSession::GetInstance().GetHero();
    eCEntity* clientPuppet = (ownerAfter > 1) ? remote::PuppetOf(ownerAfter)
                                              : ((ownerBefore > 1) ? remote::PuppetOf(ownerBefore) : nullptr);
    Event ev;
    ev.kv.Str("event", event ? event : "lease").TransitionInt("owner", ownerBefore, ownerAfter)
      .Hex("lease_epoch", epoch).Uint("incarnation", incarnation).Str("reason", reason)
      .Float1("local_dist_m", DistanceMeters(npc, localHero)).Float1("peer_dist_m", DistanceMeters(npc, clientPuppet));
    AppendEntity(ev.kv, "npc", npc, guid);
    ev.Send("npc.lease");
}

void OnVisibilityEvent(char const* event, eCEntity* npc, uint8_t const guid[16], bool visible, char const* reason)
{
    Event ev;
    ev.kv.Str("event", event ? event : "visibility").Bool("visible", visible).Str("reason", reason);
    AppendEntity(ev.kv, "npc", npc, guid);
    ev.Send("npc.visibility");
}

void OnNpcRemoval(char const* event, char const* name, uint8_t const guid[16], eCEntity* npc, char const* reason)
{
    Event ev;
    ev.kv.Str("event", event ? event : "suppress").Str("authored_name", name).Str("reason", reason);
    AppendEntity(ev.kv, "npc", npc, guid);
    ev.Send("npc.removal");
}

void OnStoryDelta(char const* source, uint8_t fromPlayer, char const* domain, char const* entryName,
                  uint32_t beforeVal, uint32_t afterVal)
{
    const uint32_t now = NowMs();
    Event ev;
    ev.kv.Str("source", source ? source : "story").Uint("from_player", fromPlayer).Str("domain", domain)
      .Str("entry", entryName).TransitionInt("value", beforeVal, afterVal);
    const bool isQuest = domain && std::strcmp(domain, "quest") == 0;
    if (isQuest) {
        ev.kv.TransitionStr("quest_status", QuestStatusLabel(static_cast<int>(beforeVal)),
                            QuestStatusLabel(static_cast<int>(afterVal)));
    }
    const uint64_t seq = ev.Send("story.delta");
    if (isQuest || (domain && std::strcmp(domain, "info") == 0)) {
        char detailText[96];
        if (isQuest) {
            Format(detailText, "%s:%s->%s", entryName ? entryName : "quest",
                   QuestStatusLabel(static_cast<int>(beforeVal)), QuestStatusLabel(static_cast<int>(afterVal)));
        } else {
            Format(detailText, "%s:%u->%u", entryName ? entryName : "info", beforeVal, afterVal);
        }
        lastQuestCause.Record(seq, now, domain, PeerLabel(fromPlayer), entryName, detailText);
    }
}

void OnPersistEvent(char const* event, char const* slotOrSave, char const* worldId, uint32_t epoch,
                    bool ok, char const* detail)
{
    Event ev;
    ev.kv.Str("event", event ? event : "persist").Str("save", slotOrSave).Str("world_id", worldId)
      .Hex("target_epoch", epoch).Bool("ok", ok);
    if (detail && *detail) ev.kv.Str("detail", detail);
    ev.Send("persist.lifecycle");
}

void OnAdmissionStage(char const* status, char const* cause, uint32_t epoch, uint8_t kind, char const* campaign,
                      bool ownHeroReady, bool storyReady, bool doorsReady, bool hostAck, bool inWorld)
{
    Event ev;
    ev.kv.Str("status", status).Str("cause", cause).Hex("target_epoch", epoch)
      .Uint("world_kind", kind).Str("campaign", campaign)
      .Bool("own_hero", ownHeroReady).Bool("story_sync", storyReady)
      .Bool("doors_sync", doorsReady).Bool("host_ack", hostAck).Bool("in_world", inWorld);
    ev.Send("persist.admission");
}

} // namespace g3t::telemetry
