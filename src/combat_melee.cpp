// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_internal.h"
#include "ai.h"
#include "body.h"
#include "config.h"
#include "hud.h"
#include "guards.h"
#include "downed.h"
#include "guid.h"
#include "log.h"
#include "melee_policy.h"
#include "net.h"
#include "npcs.h"
#include "npclease_runtime.h"
#include "npclease_presentation.h"
#include "npcplayback.h"
#include "remote.h"
#include "shot.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstring>
#include <string>
#include <unordered_map>

namespace g3t::combat {
using namespace detail;
namespace detail {
unsigned hostJudgedClaimsSent = 0, localMeleeRefused = 0;
unsigned friendlyHitsIgnored = 0, hitEffectsPlayed = 0;
uint8_t SideOf(eCEntity* entity)
{
    if (!entity) return 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const uint8_t local = net::LocalId() ? net::LocalId() : 1;
    if (entity == hero || entity == remote::ControlledBody()) return local;
    if (const int owner = remote::PuppetOwner(entity)) return static_cast<uint8_t>(owner);
    // Host: a companion a client took over is that client's (its copy here has no party leader; stageNpcHitScale 28.10.)
    if (const uint8_t owner = npcs::CompanionOwner(entity)) return owner;
    if (entity->GetPropertySet(eEPropertySetType_Party)) {
        eCEntity* leader = Entity(entity).Party.GetPartyLeader().GetInstance();
        if (leader && leader != entity) {
            if (leader == hero || leader == remote::ControlledBody()) return local;
            if (const int owner = remote::PuppetOwner(leader)) return static_cast<uint8_t>(owner);
        }
    }
    return 0;
}

// The damage as applied: between two players' sides friendly_fire percent of it; by an NPC (no companion) on a player's side
// times npc_damage; by a player's side on an NPC (no companion) divided by npc_hp. Any blow keeps at least 1 point.
DWORD lastInventoryHitLog = 0;
bool LocalNpcContact(gCScriptProcessingUnit* spu, GELPVoid other, eCEntity* attacker)
{
    (void)attacker;
    // Melee never touches the hero natively through a mirror's weapon; it is judged by LocalBlow() at the visible swing.
    // What does arrive natively is a replicated NPC projectile, which flies here and hits only this machine's hero.
    if (shot::InNpcReplicaContact()) return true;
    eCEntity* source = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    return source && source->GetPropertySet(eEPropertySetType_Projectile) && !source->IsProcessingDisabled();
}

bool InventoryBlocksNpc(eCEntity* victim, eCEntity* attacker)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return hero && victim && (victim == hero || victim == remote::ControlledBody()) && attacker &&
           attacker != hero && attacker != remote::ControlledBody() && !remote::IsPuppet(attacker) &&
           attacker->GetPropertySet(eEPropertySetType_NPC) && hud::InventoryOpen();
}

void NoteInventoryHit()
{
    ++inventoryHitsIgnored;
    const DWORD now = GetTickCount();
    if (!lastInventoryHitLog || now - lastInventoryHitLog >= 5000) {
        lastInventoryHitLog = now ? now : 1;
        Logf("combat: NPC damage ignored while inventory is open (%u so far)", inventoryHitsIgnored);
    }
}
std::string KillKey(uint8_t const guid[16])
{
    std::string key(reinterpret_cast<char const*>(guid), 16);
    const uint32_t incarnation = npcs::Incarnation(guid);
    key.append(reinterpret_cast<char const*>(&incarnation), sizeof(incarnation));
    return key;
}

static_assert(melee::Friendly == static_cast<int>(gEAttitude_Friendly) &&
              melee::Neutral == static_cast<int>(gEAttitude_Neutral) &&
              melee::Hostile == static_cast<int>(gEAttitude_Hostile));
std::unordered_map<std::string, uint32_t> playerSwings;
std::unordered_map<std::string, uint32_t> playerClaims;
struct RefusedMelee { uint32_t motion = 0; std::string reason; };
std::unordered_map<std::string, RefusedMelee> refusedMelee;
bool ClaimedPlayerSwing(uint8_t const identity[16], uint32_t swing)
{
    const auto current = playerClaims.find(KillKey(identity));
    std::string unknown(reinterpret_cast<char const*>(identity), 16);
    const uint32_t life = 0;
    unknown.append(reinterpret_cast<char const*>(&life), sizeof(life));
    const auto initial = playerClaims.find(unknown);
    return melee::ClaimedSwing(swing, current == playerClaims.end() ? 0 : current->second,
                              initial == playerClaims.end() ? 0 : initial->second);
}
bool InBout(Entity& npc)
{
    if (!npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) return false;
    const gEAttackReason reason = npc.NPC.AttackReason;
    return reason == gEAttackReason_Duel || reason == gEAttackReason_Arena;
}

// A hit by `attacker` on an NPC in a bout with someone else: not counted.
bool OutsideBout(Entity& npc, eCEntity* attacker)
{
    return attacker && InBout(npc) && npc.NPC.GetCurrentTarget().GetInstance() != attacker;
}
bool IsPuppetMeleeJudgement(eCEntity* victim, eCEntity* attacker, eCEntity* source)
{
    return net::IsHost() && !forwarded &&
           victim && victim->GetPropertySet(eEPropertySetType_NPC) &&
           !remote::IsPuppet(victim) && !npcs::IsMirrored(victim) &&
           attacker && (remote::IsPuppet(attacker) || remote::PuppetOwner(attacker) != 0) &&
           body::LocallyAttacking(attacker) &&
           (!source || !source->GetPropertySet(eEPropertySetType_Projectile));
}

bool HasIdentity(uint8_t const identity[16])
{
    static const uint8_t zero[16] = {};
    return std::memcmp(identity, zero, 16) != 0;
}

bool IsLocalMelee(eCEntity* attacker, eCEntity* source)
{
    return attacker && attacker == remote::ControlledBody() &&
           (!source || (!source->GetPropertySet(eEPropertySetType_Projectile) &&
            (!source->GetPropertySet(eEPropertySetType_Interaction) || !Entity(source).Interaction.GetSpell().GetInstance())));
}

net::MeleeContact CaptureMelee(eCEntity* attacker, eCEntity* source)
{
    Entity player(attacker);
    net::MeleeContact contact;
    contact.melee = 1;
    contact.item = source && source->GetPropertySet(eEPropertySetType_Item);
    const gEAction action = player.Routine.Action;
    contact.targetOnly = action == gEAction_PierceAttack || action == gEAction_HackAttack;
    contact.motion = npcplayback::MotionInstance(attacker);
    guid::CopyEntity(player.Focus.GetFocusEntity().GetInstance(), contact.focus);
    guid::CopyEntity(player.NPC.GetCurrentTarget().GetInstance(), contact.target);
    return contact;
}

eCEntity* PartyLeader(eCEntity* entity)
{
    return entity && entity->GetPropertySet(eEPropertySetType_Party) ? Entity(entity).Party.GetPartyLeader().GetInstance() : nullptr;
}

int RawAttitude(eCEntity* entity, eCEntity* claimant)
{
    if (auto const* record = npcs::CombatRecord(entity)) return record->attitude;
    if (entity && claimant && remote::IsPuppet(claimant) &&
        entity->GetPropertySet(eEPropertySetType_NPC)) {
        guards::Acting player(claimant, false);
        return ai::PlainAttitude(entity, claimant);
    }
    return entity && entity->GetPropertySet(eEPropertySetType_NPC)
        ? static_cast<int>(static_cast<gEAttitude>(Entity(entity).NPC.AttitudeToPlayer2)) : melee::None;
}

std::unordered_map<std::string, PlayerMeleeContact> playerMeleeContacts;

MeleeVerdict JudgeMelee(eCEntity* victim, eCEntity* claimant, uint8_t player, net::MeleeContact const& contact)
{
    MeleeVerdict result;
    if (!claimant || !claimant->GetPropertySet(eEPropertySetType_NPC)) return result;
    auto const* record = npcs::CombatRecord(victim);
    if (npcs::IsMirrored(victim) && !record) {
        result.decision = "missing-victim-record";
        return result;
    }
    result.focus = HasIdentity(contact.focus) ? guid::Resolve(contact.focus).GetInstance() : nullptr;
    eCEntity* aimed = HasIdentity(contact.target) ? guid::Resolve(contact.target).GetInstance() : nullptr;
    if ((HasIdentity(contact.focus) && !result.focus) || (HasIdentity(contact.target) && !aimed)) {
        result.decision = "unresolved-intent";
        return result;
    }
    eCEntity* opponent = nullptr;
    if (!npcs::CombatTarget(victim, opponent)) {
        result.decision = "unresolved-opponent";
        return result;
    }
    if ((npcs::IsMirrored(result.focus) && !npcs::CombatRecord(result.focus)) ||
        (npcs::IsMirrored(opponent) && !npcs::CombatRecord(opponent))) {
        result.decision = "missing-relation-record";
        return result;
    }
    auto& v = result.victim;
    v.attitude = RawAttitude(victim, claimant);
    v.focusAttitude = RawAttitude(result.focus, claimant);
    v.targetAttitude = RawAttitude(opponent, claimant);
    v.combat = (record ? record->aiMode : body::AIModeOf(victim)) == gEAIMode_Combat;
    v.hasFocus = result.focus != nullptr;
    v.focused = result.focus == victim;
    v.targeted = aimed == victim;
    v.hasTarget = opponent != nullptr;
    v.targetsPlayer = opponent == claimant;
    v.ledByPlayer = (record && record->partyPlayer == player) || PartyLeader(victim) == claimant ||
        npcs::CompanionOwner(victim) == player;
    v.playerLedByVictim = (record && record->guidePlayer == player) || PartyLeader(claimant) == victim;
    auto const* opponentRecord = npcs::CombatRecord(opponent);
    v.targetsPlayerParty = opponent &&
        ((opponentRecord && opponentRecord->partyPlayer == player) || PartyLeader(opponent) == claimant ||
         npcs::CompanionOwner(opponent) == player);
    v.targetOnly = contact.targetOnly != 0;
    v.item = contact.item != 0;
    if (claimant->GetPropertySet(eEPropertySetType_Party)) {
        const auto members = Entity(claimant).Party.GetMembers(GEFalse);
        for (int i = 0; i < members.GetCount(); ++i)
            if (members[i] == victim) v.partyMember = true;
    }
    // Recruiting can be represented in the host's companion registry before its native party is bound.
    v.partyMember = v.partyMember || v.ledByPlayer;
    if (!ai::TryPlayerAttitude(victim, claimant, result.relation)) {
        result.decision = "attitude-unavailable";
        return result;
    }
    v.relation = result.relation;
    if (!v.combat && v.hasFocus && !v.focused && !v.targetsPlayer && !v.partyMember &&
        !ai::TryPlayerAttitude(result.focus, claimant, v.focusRelation)) {
        result.decision = "focus-attitude-unavailable";
        return result;
    }
    result.relationsKnown = true;
    Entity npc(victim);
    if (InBout(npc) && !v.targetsPlayer) {
        result.decision = "outside-bout";
        return result;
    }
    const auto decision = melee::Judge(v);
    result.allowed = decision == melee::Decision::Allow;
    result.decision = melee::Name(decision);
    return result;
}

void LogMelee(char const* entry, uint8_t player, uint32_t sequence, eCEntity* victim,
              net::MeleeContact const& contact, MeleeVerdict const& verdict)
{
    char identity[40], focus[40], target[40];
    guid::FormatEntity(victim, identity, sizeof(identity));
    guid::Format(contact.focus, focus, sizeof(focus));
    guid::Format(contact.target, target, sizeof(target));
    const bCString name = Entity(victim).GetName();
    Logf("combat: melee-victim entry=%s player=%u sequence=%u motion=%u victim=%s guid=%s attitude=%d raw=%d "
         "focus=%s target=%s focused=%d targeted=%d combat=%d decision=%s",
         entry, player, sequence, contact.motion, name.GetText(), identity, verdict.relation, verdict.victim.attitude,
         focus, target, verdict.victim.focused ? 1 : 0, verdict.victim.targeted ? 1 : 0,
         verdict.victim.combat ? 1 : 0, verdict.decision);
}

bool LocalMeleeAllowed(eCEntity* victim, eCEntity* attacker, net::MeleeContact const& contact)
{
    auto verdict = JudgeMelee(victim, attacker, net::LocalId(), contact);
    if ((verdict.allowed || !verdict.relationsKnown) && contact.item && !Entity(attacker).IsInFOV(Entity(victim))) {
        verdict.allowed = false;
        verdict.relationsKnown = true;
        verdict.decision = "outside-native-fov";
    }
    uint8_t identity[16]; Guid(victim, identity);
    const auto key = KillKey(identity);
    if (melee::EvaluatePlayerMelee(contact.melee, verdict.allowed, verdict.relationsKnown) != melee::ClientDecision::Reject) {
        playerMeleeContacts[key] = {contact, verdict};
        return true;
    }
    auto& refused = refusedMelee[key];
    if (refused.motion != contact.motion || refused.reason != verdict.decision) {
        refused.motion = contact.motion;
        refused.reason = verdict.decision;
        ++localMeleeRefused;
        LogMelee("local-refused", net::LocalId(), 0, victim, contact, verdict);
    }
    return false;
}

bool SendHostJudgedMelee(eCEntity* victim, net::MeleeContact contact)
{
    uint8_t identity[16], weapon[16] = {};
    Guid(victim, identity);
    const auto key = KillKey(identity);
    if (ClaimedPlayerSwing(identity, contact.motion)) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !contact.motion) {
        Log("combat: contact claim has no local hero or swing identity");
        return false;
    }
    if (IgnoreFriendlyHit(victim, remote::ControlledBody())) return false;
    if (npclease::CosmeticContact(!net::IsHost(), npcplayback::IsPassive(victim),
            npcs::HostPresentationAvailable(victim), true, npclease::Fenced(victim),
            Entity(victim).IsDead() || Entity(victim).IsDown() || downed::HarmProtected(victim), contact.motion))
        npcplayback::CosmeticHit(victim, contact.motion);
    const GEInt held = Entity(hero).Inventory.FindStackIndex(gESlot_RightHand);
    if (held >= 0) {
        const Template item = Entity(hero).Inventory.GetTemplate(held);
        bCPropertyID const& id = item;
        std::memcpy(weapon, &id.GetCore(), 16);
    }
    contact.melee = net::HostJudgedMelee;
    const uint32_t hit = net::SendHitNpc(1, identity, 0, weapon, nullptr, net::HitUnjudged,
        npcs::SeenAt(victim), net::HitNoReaction, npcs::Incarnation(identity), contact);
    if (!hit) return false;
    playerClaims[key] = contact.motion;
    ++claimsSent;
    ++hostJudgedClaimsSent;
    npcs::AwaitHit(victim, hit);
    const auto context = playerMeleeContacts.find(key);
    if (context != playerMeleeContacts.end()) {
        static DWORD lastMissingRelationLog = 0;
        const DWORD now = GetTickCount();
        if (hostJudgedClaimsSent <= 8 || now - lastMissingRelationLog >= 5000) {
            lastMissingRelationLog = now ? now : 1;
            LogMelee("claim-host-judged", net::LocalId(), hit, victim, contact, context->second.verdict);
        }
    }
    return true;
}
void PlayHitEffect(eCEntity* victim, bool blocked)
{
    if (!victim) return;
    const bCString name(blocked ? "eff_col_wh_01_me_me" : "eff_col_wh_02_me_fl");
    if (!EffectSystem::ExistsEffect(name)) return;
    EffectSystem::StartEffect(name, Entity(victim));
    ++hitEffectsPlayed;
}
void ClearMelee()
{
    playerSwings.clear();
    playerClaims.clear();
    playerMeleeContacts.clear();
    refusedMelee.clear();
}
}
bool IgnoreFriendlyHit(eCEntity* victim, eCEntity* source, unsigned player)
{
    if (GetConfig().friendlyFire != 0 || !victim) return false;
    const uint8_t hit = SideOf(victim);
    if (!hit) return false;
    // Area effects and projectiles can own one another before reaching the caster.
    for (unsigned depth = 0; source && depth < 8; ++depth) {
        if (source == victim) return false;
        if (SideOf(source)) {
            ++friendlyHitsIgnored;
            return true;
        }
        Entity owner = Entity(source).Interaction.GetOwner();
        eCEntity* next = owner.GetInstance();
        if (!next || next == source) break;
        source = next;
    }
    if (player > 0 && player < net::MaxPlayers) {
        ++friendlyHitsIgnored;
        return true;
    }
    return false;
}
bool InParadeMode(eCEntity* entity) { return entity && CallScript("IsInParadeMode", entity, nullptr, 0) != 0; }

// One blow of a mirrored NPC on this machine's own hero, judged by this machine's game as a swing it showed (npcs.cpp calls this
// at the hit part of a visible swing, in reach and facing the hero). The game's own judgement then decides parade, armour and
// stumble; the hero's hit points are never set by the host.
bool LocalBlow(eCEntity* attacker)
{
    eCEntity* body = remote::ControlledBody();
    if (!installed || !body || !attacker || gCSession::GetInstance().GetCheats().GetCheatGodEnabled()) return false;
    Entity self(body);
    GroundProbe ground(body, attacker, "visible-NPC-contact");
    if (RejectDownedHit(body, attacker, "visible-NPC-contact")) return false;
    if (self.IsDead() || self.IsDown()) return false;
    if (hud::InventoryOpen()) { NoteInventoryHit(); return false; }
    char const* kind = "";
    eCEntity* source = DamageSource(static_cast<eCDynamicEntity*>(attacker), &kind);
    int pending = 0;
    if (MeansToKill(attacker, body)) downed::NoteBlow();
    auto stamina = [&]() {
        return HasPlayerMemory(body) ? self.PlayerMemory.GetStaminaPoints()
            : HasDamageReceiver(body) ? static_cast<int>(self.DamageReceiver.StaminaPoints) : -1;
    };
    const int hpBefore = HitPoints(self), spBefore = stamina();
    const bool blocking = InParadeMode(body);
    GEInt result;
    {
        HitDispatchScope scope(forwarded, heroForward, true);
        result = GameHit(self, source, &pending);
    }
    ++localNpcHits;
    Logf("combat: NPCATTACK stage=local-result npc=%s client_judged=%u block_state=%d result=%#x pending=%d "
         "hp=%d->%d stamina=%d->%d",
         Entity(attacker).GetName().GetText(), net::Now(), blocking ? 1 : 0, result, pending,
         hpBefore, HitPoints(self), spBefore, stamina());
    return true;
}

bool PlayerBlow(eCEntity* victim)
{
    eCEntity* hero = remote::ControlledBody();
    if (!installed || net::IsHost() || !hero || !victim || !npcplayback::IsPassive(victim) ||
        !body::LocallyAttacking(hero) || IgnoreFriendlyHit(victim, hero)) return false;
    Entity player(hero), target(victim);
    if (player.IsDown() || player.IsDead() || target.IsDead()) return false;
    char const* kind = "";
    eCEntity* source = DamageSource(static_cast<eCDynamicEntity*>(hero), &kind);
    const auto contact = CaptureMelee(hero, source);
    if (!LocalMeleeAllowed(victim, hero, contact)) return false;
    uint8_t identity[16]; Guid(victim, identity);
    const auto context = playerMeleeContacts.find(KillKey(identity));
    if (context == playerMeleeContacts.end()) return false;
    if (!context->second.verdict.relationsKnown)
        return SendHostJudgedMelee(victim, contact);
    int pending = 0;
    const GEInt result = GameHit(target, source, &pending);
    if (result && pending > 0) PlayHitEffect(victim, false);
    return result && pending > 0;
}
}
