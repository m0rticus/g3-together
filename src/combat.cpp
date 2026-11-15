// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_internal.h"
#include "options.h"

#include "ai.h"
#include "body.h"

#include "config.h"
#include "hud.h"
#include "downed.h"
#include "guid.h"
#include "log.h"
#include "melee_policy.h"
#include "net.h"
#include "npcs.h"
#include "npclease_runtime.h"
#include "npcplayback.h"
#include "remote.h"
#include "shot.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <string>

namespace g3t::combat {
using namespace detail;
namespace detail {
bool installed = false;
thread_local bool forwarded = false;  // client: applying a hit the host forwarded, judged here (Filter lets it through)
DWORD lastTry = 0;
gFScript original = nullptr;
unsigned filterCalls = 0;
unsigned dropped = 0;  // client: hits between host-owned bodies, which the host simulates itself
unsigned claimsSent = 0, playerHitsSent = 0;
unsigned duplicateClaimsIgnored = 0;
unsigned npcHitsSent = 0;
unsigned inventoryHitsIgnored = 0, clientJudgements = 0, clientJudgementsWithoutDamage = 0;
unsigned localNpcHits = 0, invisibleContactsIgnored = 0;
unsigned duelHitsRefused = 0;
char const* const Judgements[] = {"DoLogicalDamage", "OnDamage", "AssessHit", "CanBePoisoned", "CanBeDiseased", "CanBurn", "CanFreeze"};
constexpr int JudgementCount = sizeof(Judgements) / sizeof(Judgements[0]);
gFScript judge[JudgementCount] = {};
unsigned paradesPassedOn = 0;  // blows on a parrying puppet judged as unblocked here, so that its player judges the block
unsigned mirrorParadesPassedOn = 0;
uint8_t lastOutcome = 0;  // client: the outcome of the last claim sent (QA HITNPC line)
unsigned foreignJudged[3] = {}, foreignSkipped = 0;
gFScript musicTick = nullptr;
DWORD fightSeenAt = 0;
unsigned fightMusicTriggers = 0, musicTicks = 0;
constexpr DWORD FightMusicHold = 5000;  // ms
unsigned scaledHits = 0;
thread_local bool heroForward = false;  // OnHitPlayer: the hit judged here on our hero comes from the other machine (scaled here)
template <typename T>
T* Find(bTValMap<bCString, T*>& map, char const* name)
{
    T* found = nullptr;
    return map.GetAt(bCString(name), found) ? found : nullptr;
}

// The hero and puppets made from his template keep hit points in PlayerMemory, NPCs in DamageReceiver.
bool HasPlayerMemory(eCEntity* entity) { return entity && entity->GetPropertySet(eEPropertySetType_PlayerMemory); }

int HitPoints(Entity& entity)
{
    if (HasPlayerMemory(entity.GetInstance())) return entity.PlayerMemory.GetHitPoints();
    return entity.DamageReceiver.HitPoints;
}

bool HasDamageReceiver(eCEntity* entity) { return entity && entity->GetPropertySet(eEPropertySetType_DamageReceiver); }

int Pending(Entity& entity) { return HasDamageReceiver(entity.GetInstance()) ? static_cast<int>(entity.DamageReceiver.DamageAmount) : 0; }

bool RejectDownedHit(eCEntity* victim, eCEntity* attacker, char const* entry)
{
    if (!downed::CombatUnavailable(victim) && !downed::HarmProtected(victim)) return false;
    if (downed::Terminal() && victim && (victim == gCSession::GetInstance().GetHero() || victim == remote::ControlledBody()))
        return false;
    downed::RejectHarm(victim, attacker, entry);
    Entity player(victim);
    static unsigned refused = 0;
    static uint32_t lastLog = 0;
    const uint32_t now = net::Now();
    if (++refused <= 40 || now - lastLog >= 5000) {
        lastLog = now;
        Logf("DOWNEDHIT entry=%s victim=%s ptr=%p hp=%d pending=%d refused=%u decision=reject-before-effects",
             entry, player.GetName().GetText(), victim,
             victim->GetPropertySet(eEPropertySetType_PlayerMemory) ? player.PlayerMemory.GetHitPoints() : HitPoints(player),
             Pending(player), refused);
        downed::LogCombatDecision(attacker, victim, entry, true);
    }
    if (HasDamageReceiver(victim)) player.DamageReceiver.DamageAmount = 0;
    return true;
}

GroundProbe::GroundProbe(eCEntity* who, eCEntity* by, char const* entry)
    : victim(nullptr), attacker(by), path(entry), before(0)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (who && hero && (who == hero || who == remote::ControlledBody())) {
        victim = who;
        Entity player(who);
        before = HitPoints(player);
    }
}
GroundProbe::~GroundProbe()
{
    if (victim) {
        Entity player(victim);
        downed::LogGroundEvent(victim, attacker, path, before, HitPoints(player), "complete");
    }
}
GEInt CallScript(char const* name, eCEntity* self, eCEntity* other, GEInt args)
{
    return GetScriptAdminExt().CallScript(bCString(name), self, other, args);
}

void Guid(eCEntity* entity, uint8_t out[16])
{
    guid::CopyEntity(entity, out);
}

// Hit points of an NPC that took a client's blow must not rise again except through a real heal. They did in play: the NPC
// was dropped from and re-added to the host's processing range and the engine's re-entry refilled it.
// Who dealt the hit: a weapon's Interaction owner, else the damage source itself.
eCEntity* Attacker(gCScriptProcessingUnit* spu, GELPVoid other)
{
    Entity source = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    eCEntity* originalSource = source.GetInstance();
    eCEntity* player = remote::ControlledBody();
    for (unsigned depth = 0; source.GetInstance() && depth < 8; ++depth) {
        eCEntity* entity = source.GetInstance();
        if (entity == player || entity == gCSession::GetInstance().GetHero() ||
            remote::IsPuppet(entity) || entity->GetPropertySet(eEPropertySetType_NPC)) return entity;
        if (player && player->GetPropertySet(eEPropertySetType_Inventory)) {
            Entity hero(player);
            if (hero.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance() == entity ||
                hero.Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance() == entity) return player;
        }
        eCEntity* owner = entity->GetPropertySet(eEPropertySetType_Interaction) ? source.Interaction.GetOwner().GetInstance() : nullptr;
        if (!owner || owner == entity) owner = entity->GetParent();
        if (!owner || owner == entity) break;
        source = Entity(owner);
    }
    return originalSource;
}

GEInt GE_STDCALL MusicTick(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return musicTick ? musicTick(spu, self, other, args) : 0;
    const GEInt result = musicTick(spu, self, other, args);
    ++musicTicks;
    const DWORD now = GetTickCount();
    if (npcs::MirrorFightsHero()) fightSeenAt = now ? now : 1;
    if (fightSeenAt && now - fightSeenAt < FightMusicHold) {
        music.TriggerFight();
        ++fightMusicTriggers;
    }
    return result;
}

GEInt Judge(int which, gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    // DoLogicalDamage takes (source, victim); OnDamage/AssessHit take (victim, source).
    Entity victim = (which == 0 ? other : self) ? *static_cast<Entity*>(which == 0 ? other : self)
        : Entity(spu ? (which == 0 ? spu->GetOtherEntity() : spu->GetSelfEntity()) : nullptr);
    GELPVoid recipientArg = which == 0 ? other : self;
    options::NpcJudgement difficulty(!net::IsHost() && npcs::OwnerOf(victim.GetInstance()) == 1);
    (void)recipientArg;
    GELPVoid sourceArg = which == 0 ? self : other;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity damageSource = sourceArg ? *static_cast<Entity*>(sourceArg)
        : Entity(spu ? (which == 0 ? spu->GetSelfEntity() : spu->GetOtherEntity()) : nullptr);
    eCEntity* attacker = Attacker(nullptr, &damageSource);
    GroundProbe ground(victim.GetInstance(), attacker, Judgements[which]);
    eCEntity* source = damageSource.GetInstance();
    const bool localMelee = !forwarded && npcs::IsMirrored(victim.GetInstance()) &&
        IsLocalMelee(attacker, source) && ai::detail::damageReceiver != victim.GetInstance();
    if (!ai::ShuttingDown() && !localMelee && !npclease::ContactAllowed(victim.GetInstance(), attacker)) return 0;
    if (!ai::ShuttingDown() && RejectDownedHit(victim.GetInstance(), attacker, Judgements[which])) return 0;
    if (localMelee) {
        const auto contact = CaptureMelee(attacker, source);
        if (!LocalMeleeAllowed(victim.GetInstance(), attacker, contact)) return 0;
        uint8_t identity[16]; Guid(victim.GetInstance(), identity);
        const auto context = playerMeleeContacts.find(KillKey(identity));
        if (context == playerMeleeContacts.end()) return 0;
        if (!context->second.verdict.relationsKnown) {
            if (which < 2) SendHostJudgedMelee(victim.GetInstance(), contact);
            return 0;
        }
    }
    if (ai::ShuttingDown()) return judge[which] ? judge[which](spu, self, other, args) : 0;
    if (IsPuppetMeleeJudgement(victim.GetInstance(), attacker, source)) {
        ++foreignSkipped;
        return 0;
    }
    if (npcs::IsMirrored(victim.GetInstance()) &&
        attacker != hero && attacker != remote::ControlledBody()) {
        ++foreignSkipped;
        return 0;
    }
    if (IgnoreFriendlyHit(victim.GetInstance(), attacker)) return 0;
    uint32_t* consumed = nullptr;
    uint32_t swing = 0;
    if (which < 2 && npcs::IsMirrored(victim.GetInstance()) && IsLocalMelee(attacker, source)) {
        swing = npcplayback::MotionInstance(attacker);
        if (swing) {
            uint8_t guid[16]; Guid(victim.GetInstance(), guid);
            const std::string key = KillKey(guid);
            if (ClaimedPlayerSwing(guid, swing)) return 0;
            if (source == attacker || source == Entity(attacker).Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance()) {
                consumed = &playerSwings[key];
                if (*consumed == swing) return 0;
            }
        }
    }
    struct ConsumeSwing {
        uint32_t* consumed;
        uint32_t swing;
        ~ConsumeSwing() { if (consumed) *consumed = swing; }
    } consume{consumed, swing};
    if (which >= 2) return judge[which](spu, self, other, args);
    if (InventoryBlocksNpc(victim.GetInstance(), attacker)) {
        NoteInventoryHit();
        return 0;
    }
    const bool localVictim = victim.GetInstance() && (victim.GetInstance() == hero || victim.GetInstance() == remote::ControlledBody());
    if (shot::InNpcReplicaContact() && !localVictim) return 0;
    if (!forwarded && localVictim && npcs::IsMirrored(attacker) && !LocalNpcContact(nullptr, &damageSource, attacker)) {
        ++invisibleContactsIgnored;
        return 0;
    }
    const bool playerHitMirror = attacker && (attacker == hero || attacker == remote::ControlledBody()) &&
                                 npcs::OwnerOf(victim.GetInstance()) && !victim.IsDead();
    if (playerHitMirror && ai::detail::damageReceiver != victim.GetInstance())
        npcplayback::BeginHit(victim.GetInstance());
    ai::DamagePerception perception(playerHitMirror ? victim.GetInstance() : nullptr);
    const bool leaseHit = npclease::LocalAuthority(victim.GetInstance());
    const int leaseHp = leaseHit ? HitPoints(victim) : 0;
    static thread_local int heroJudgeDepth = 0;
    const bool trackHeroJudge = which <= 2 && localVictim;
    if (trackHeroJudge) {
        ++heroJudgeDepth;
        downed::NoteHeroJudgedHit(attacker, true);
    }
    auto observed = [&](GEInt result) {
        if (trackHeroJudge) {
            --heroJudgeDepth;
            if (!result) downed::ClearHeroJudgedHit();
        }
        if (leaseHit) npclease::NoteHit(victim.GetInstance(), attacker, Judgements[which], leaseHp, HitPoints(victim));
        if (attacker && (attacker == hero || attacker == remote::ControlledBody()) && npcs::IsMirrored(victim.GetInstance())) {
            ++clientJudgements;
            if (Pending(victim) <= 0) {
                ++clientJudgementsWithoutDamage;
                if (clientJudgementsWithoutDamage <= 40)
                    Logf("combat: local hit on mirror %s produced no pending damage (script=%s result=%#x immortal=%d defeated=%d)",
                         victim.GetName().GetText(), Judgements[which], result, victim.NPC.Immortal ? 1 : 0,
                         victim.NPC.DefeatedByPlayer ? 1 : 0);
            }
        }
        return result;
    };
    if (!forwarded && attacker && attacker != hero && (remote::IsPuppet(attacker) || npcs::IsMirrored(attacker))) {
        eCEntity* hit = victim.GetInstance();
        const int kind = hit && (hit == hero || hit == remote::ControlledBody()) ? 0
                         : hit && (npcs::IsMirrored(hit) || remote::PuppetOwner(hit)) ? 2 : 1;
        ++foreignJudged[kind];

    }
    if (victim.Routine.AniState != gEAniState_Parade) return observed(judge[which](spu, self, other, args));
    const bool puppet = remote::PuppetOwner(victim.GetInstance());
    const bool mirror = npcs::IsMirrored(victim.GetInstance()) && attacker && (attacker == hero || attacker == remote::ControlledBody());
    if (!puppet && !mirror) return observed(judge[which](spu, self, other, args));
    victim.Routine.AniState = gEAniState_Stand;
    const GEInt result = judge[which](spu, self, other, args);
    if (victim.Routine.AniState == gEAniState_Stand) victim.Routine.AniState = gEAniState_Parade;  // the snapshots set it anyway
    ++(puppet ? paradesPassedOn : mirrorParadesPassedOn);
    return observed(result);
}
GEInt GE_STDCALL JudgeLogical(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args) { return Judge(0, spu, self, other, args); }
GEInt GE_STDCALL JudgeOnDamage(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args) { return Judge(1, spu, self, other, args); }
template <int I>
GEInt GE_STDCALL JudgeStatus(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args) { return Judge(I, spu, self, other, args); }
int32_t Scaled(eCEntity* victim, eCEntity* attacker, int32_t delta)
{
    if (delta >= 0) return delta;
    const uint8_t hit = SideOf(victim), by = SideOf(attacker);
    float factor = 1.0f;
    if (hit && by && hit != by) factor = GetConfig().friendlyFire / 100.0f;
    else if (!hit && by) factor = 1.0f / GetConfig().npcHp;
    else if (hit && !by && attacker && attacker->GetPropertySet(eEPropertySetType_NPC)) factor = GetConfig().npcDamage;
    if (factor == 1.0f) return delta;
    ++scaledHits;
    if (factor <= 0.0f) return 0;
    const int32_t scaled = static_cast<int32_t>(std::lround(delta * factor));
    return scaled < 0 ? scaled : -1;
}

// Whether `attacker` (an NPC this machine simulates) means to kill `victim` (a puppet that stands for a player here): the game's
// own HasAttackReasonToKill, asked of the puppet as the player (guards.cpp). Sent with the hit: the victim's machine has only a
// mirror of the attacker, which has neither target nor attack reason (downed.cpp: a lethal blow is no duel, arena or arrest).
bool MeansToKill(eCEntity* attacker, eCEntity* victim)
{
    if (!attacker || !victim || attacker == gCSession::GetInstance().GetHero() || !attacker->GetPropertySet(eEPropertySetType_NPC)) return false;
    return CallScript("HasAttackReasonToKill", attacker, victim, 0) != 0;
}

bool IsHostSimulatedNpc(eCEntity* victim)
{
    if (!net::IsHost() || !victim || !victim->GetPropertySet(eEPropertySetType_NPC) ||
        victim == gCSession::GetInstance().GetHero() || victim == remote::ControlledBody() ||
        remote::IsPuppet(victim) || npcs::IsMirrored(victim) || npcs::OwnerOf(victim) != 0) return false;
    return true;
}

GEInt CallOriginalAndLog(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    Entity recipient = self ? *static_cast<Entity*>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
    const bool logApplied = args < 0 && IsHostSimulatedNpc(recipient.GetInstance());
    const bool leaseHit = args < 0 && npclease::LocalAuthority(recipient.GetInstance());
    const bool localAuthority = logApplied || leaseHit;
    const int before = localAuthority ? HitPoints(recipient) : 0;
    eCEntity* attacker = Attacker(spu, other);
    const GEInt result = original(spu, self, other, args);
    const int after = localAuthority ? HitPoints(recipient) : 0;
    if (localAuthority && after < before) NotePlayerCombatDamage(recipient.GetInstance(), attacker);
    if (leaseHit) npclease::NoteHit(recipient.GetInstance(), attacker, "AddHitPoints", before, after);
    if (logApplied) LogAppliedHostNpcHit(recipient.GetInstance(), attacker, args, before, after, result);
    return result;
}

GEInt GE_STDCALL Filter(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return original ? original(spu, self, other, args) : 0;
    ++filterCalls;
    Entity recipient = self ? *static_cast<Entity*>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
    GroundProbe ground(recipient.GetInstance(), Attacker(spu, other), "AddHitPoints");
    if (args < 0 && RejectDownedHit(recipient.GetInstance(), Attacker(spu, other), "AddHitPoints")) return GETrue;
    if (shot::InNpcReplicaContact()) {
        if (recipient.GetInstance() != remote::ControlledBody()) return GETrue;
    }
    if (args >= 0) {
        if (npcs::IsMirrored(recipient.GetInstance())) return GETrue;
        return original(spu, self, other, args);
    }
    if (IgnoreFriendlyHit(recipient.GetInstance(), Attacker(spu, other))) return GETrue;
    if (recipient.GetInstance() &&
        (recipient.GetInstance() == gCSession::GetInstance().GetHero() ||
         recipient.GetInstance() == remote::ControlledBody())) {
        eCEntity* hitAttacker = Attacker(spu, other);
        if (!hitAttacker || hitAttacker == recipient.GetInstance() || remote::IsPuppet(hitAttacker) ||
            !hitAttacker->GetPropertySet(eEPropertySetType_NPC)) {
            downed::NoteHeroJudgedHit(hitAttacker, true);
        }
    }
    if (forwarded) {
        if (heroForward) {  // a hit on our hero judged here with the other machine's attacker
            Entity hit = self ? *static_cast<Entity*>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
            args = Scaled(hit.GetInstance(), Attacker(spu, other), args);
        }
        return CallOriginalAndLog(spu, self, other, args);
    }
    Entity victim = self ? *static_cast<Entity*>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* instance = victim.GetInstance();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* attacker = Attacker(spu, other);
    eCEntity* localMeleeActor = remote::ControlledBody();
    eCEntity* source = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (!npclease::ContactAllowed(instance, attacker)) return GETrue;
    if (InventoryBlocksNpc(instance, attacker)) {
        NoteInventoryHit();
        return GETrue;
    }
    // P6, an animal form: the player fights as the creature it controls (Entity::GetPlayer), the human body (GetHero) stays
    // hidden; the creature's hits are the player's, as the game counts them, not those of one of our NPCs (no experience,
    // no knock-out judgement, no murder for the player before).
    if (attacker && attacker != hero && attacker == remote::ControlledBody()) attacker = hero;
    // Every NPC is simulated on one machine, its owner (the host near the host, a client in its own region); the
    // others mirror it. Same rules on every machine:
    // 1. A hit by a body simulated elsewhere (another player's puppet, a mirrored NPC) is decided where that body
    //    really acts: the player's own hits arrive as claims, an NPC owner sends its NPC's hits on our hero.
    const bool localVictim = instance && (instance == hero || instance == remote::ControlledBody());
    const bool mirroredNpc = attacker && npcs::OwnerOf(attacker);
    if (localVictim && mirroredNpc) {
        if (!LocalNpcContact(spu, other, attacker)) {
            ++invisibleContactsIgnored;
            return GETrue;
        }
        ++localNpcHits;
        return CallOriginalAndLog(spu, self, other, Scaled(instance, attacker, args));
    }
    if (attacker && attacker != hero && (remote::IsPuppet(attacker) || mirroredNpc)) {
        ++dropped;
        return GETrue;
    }
    // 2. A hit on an NPC simulated elsewhere goes to its owner, who applies it (and kills); the result comes back
    //    through mirroring. Our hero's hits, and our own NPCs' hits (fights where two players' regions meet).
    uint8_t owner = npcs::OwnerOf(instance);
    if (!owner && !net::IsHost() && net::Connected() && instance && instance != hero &&
        instance != localMeleeActor && !remote::IsPuppet(instance) &&
        instance->GetPropertySet(eEPropertySetType_NPC) && attacker == hero &&
        !npclease::LocalAuthority(instance) && npcs::CompanionOwner(instance) != net::LocalId() &&
        !IsLocalMelee(localMeleeActor, source)) {
        uint8_t identity[16]; Guid(instance, identity);
        if (!npcs::Incarnation(identity) && !npcs::SeenAt(instance)) owner = 1;
    }
    if (owner) {
        if (attacker && attacker != hero && attacker->GetPropertySet(eEPropertySetType_NPC)) {
            uint8_t guid[16], npc[16], flags[16] = {};
            Guid(instance, guid);
            Guid(attacker, npc);
            // P3: a companion or summon of our hero (its party leader). For NPC hits the weapon field carries flags;
            // byte 0 = 1: a kill pays this player (the game pays the player for its party's kills in its death script).
            if (attacker->GetPropertySet(eEPropertySetType_Party) && Entity(attacker).Party.GetPartyLeader().GetInstance() == hero)
                flags[0] = 1;
            net::SendHitNpc(owner, guid, args, flags, npc);
            ++npcHitsSent;
            return GETrue;
        }
        if (attacker != hero) { ++dropped; return GETrue; }
        uint8_t guid[16], weapon[16] = {};
        Guid(instance, guid);
        // The weapon in the right hand, so that the owner can judge a lethal hit with the same weapon.
        Entity player(hero);
        const GEInt held = player.Inventory.FindStackIndex(gESlot_RightHand);
        if (held >= 0) {
            const Template item = player.Inventory.GetTemplate(held);
            bCPropertyID const& id = item;
            std::memcpy(weapon, &id.GetCore(), 16);
        }
        lastOutcome = Outcome(victim);
        uint8_t reaction = Reaction(victim);
        if (reaction == net::HitNoReaction && body::LocallyAttacking(remote::ControlledBody())) reaction = net::HitQuickStumble;
        uint32_t swing = 0;
        std::string claimKey;
        if (IsLocalMelee(localMeleeActor, source) && attacker == hero) {
            swing = npcplayback::MotionInstance(localMeleeActor);
            if (swing) {
                claimKey = KillKey(guid);
                if (ClaimedPlayerSwing(guid, swing)) {
                    ++duplicateClaimsIgnored;
                    static DWORD lastDuplicateClaimLog = 0;
                    const DWORD now = GetTickCount();
                    if (duplicateClaimsIgnored <= 40 || now - lastDuplicateClaimLog >= 5000) {
                        lastDuplicateClaimLog = now ? now : 1;
                        Logf("combat: duplicate claim ignored victim=%s motion=%u (%u so far)",
                             victim.GetName().GetText(), swing, duplicateClaimsIgnored);
                    }
                    return GETrue;
                }
            }
        }
        net::MeleeContact contact;
        if (IsLocalMelee(localMeleeActor, source)) {
            const auto context = playerMeleeContacts.find(KillKey(guid));
            if (context != playerMeleeContacts.end() && context->second.contact.motion == npcplayback::MotionInstance(localMeleeActor)) {
                contact = context->second.contact;
            } else {
                contact = CaptureMelee(localMeleeActor, source);
                if (!LocalMeleeAllowed(instance, localMeleeActor, contact)) return GETrue;
            }
            const auto judgedContext = playerMeleeContacts.find(KillKey(guid));
            if (judgedContext == playerMeleeContacts.end()) return GETrue;
            if (!judgedContext->second.verdict.relationsKnown) {
                SendHostJudgedMelee(instance, contact);
                return GETrue;
            }
        }
        const uint32_t seen = npcs::SeenAt(instance);
        const uint32_t incarnation = melee::ResolveOutgoingHitIncarnation(npcs::Incarnation(guid), seen, contact.melee);
        const uint32_t hit = net::SendHitNpc(owner, guid, args, weapon, nullptr, lastOutcome, seen,
                                             reaction, incarnation, contact);
        if (hit) {
            ++claimsSent;
            if (swing) playerClaims[claimKey] = swing;
            if (contact.melee) {
                const auto context = playerMeleeContacts.find(KillKey(guid));
                if (context != playerMeleeContacts.end())
                    LogMelee("claim-sent", net::LocalId(), hit, instance, contact, context->second.verdict);
            }
            if (reaction != net::HitNoReaction) npcs::LocalHit(instance, hit, reaction);
        }
        npcplayback::BeginHit(nullptr);
        return GETrue;
    }
    // 3. An NPC this machine simulates in a duel or arena bout with someone else: our hero's hit does not count.
    if (attacker && attacker == hero && OutsideBout(victim, hero)) {
        ++duelHitsRefused;
        return GETrue;
    }
    // 4. A hit on another player's puppet: that player's real hero takes it, judged on that player's machine
    //    against its own hero (armour, stumble, knock-out), so the attacker goes along.
    if (const int owner = remote::PuppetOwner(instance)) {
        if (attacker && attacker != hero) remote::NoteContact(attacker, instance);
        // A hit on a puppet is not evidence that the remote hero was hit in that player's game.
        if (attacker && attacker != hero) {
            if (net::IsHost() && body::LocallyAttacking(attacker))
                Logf("combat: NPCATTACK stage=host-contact npc=%s player=%d instance=%u host_judged=%u "
                     "phase=%.4f puppet_block=%d damage_forwarded=0 compensation_ms=0 authority=defender-local",
                     Entity(attacker).GetName().GetText(), owner, npcplayback::MotionInstance(attacker), net::Now(),
                     body::SwingProgress(attacker), InParadeMode(instance) ? 1 : 0);
            return GETrue;
        }
        uint8_t guid[16] = {};
        uint8_t attackerPlayer = 0;
        if (attacker && attacker == hero) attackerPlayer = net::LocalId();
        else if (attacker) Guid(attacker, guid);
        net::SendHitPlayer(static_cast<uint8_t>(owner), args, guid, attackerPlayer, MeansToKill(attacker, instance));
        ++playerHitsSent;
        return GETrue;
    }
    if (net::IsHost() && instance && instance->GetPropertySet(eEPropertySetType_NPC))
        npcs::HostHit(instance, 0, 0);
    if (net::IsHost() && attacker && attacker->GetPropertySet(eEPropertySetType_NPC) &&
        instance && instance->GetPropertySet(eEPropertySetType_NPC) && !remote::IsPuppet(instance)) {
        const uint8_t party = npcs::CompanionOwner(attacker);
        if (party > 1) {
            uint8_t attackerGuid[16]; Guid(attacker, attackerGuid);
            OnNpcHit(party, victim, args, attackerGuid, true);
            return GETrue;
        }
    }
    return CallOriginalAndLog(spu, self, other, Scaled(instance, attacker, args));
}

// A hit for QA, the way the game's hit paths do it: judge (through OnDamage, which runs the same judgement
// as the paths above but without their swing-in-progress gates), then AddHitPoints(-recorded damage).
// The weapon's damage type is None outside a swing (the game sets it for the swing); set it for this hit.
GEInt JudgeDamage(Entity& victim, eCEntity* source, int* pending, bool apply)
{
    Entity weapon(source);
    const bool armed = source && source->GetPropertySet(eEPropertySetType_Damage);
    const gEDamageType type = armed ? static_cast<gEDamageType>(weapon.Damage.DamageType) : gEDamageType_None;
    if (armed && type == gEDamageType_None) weapon.Damage.DamageType = gEDamageType_Blade;
    victim.DamageReceiver.DamageAmount = 0;
    const GEInt result = CallScript("OnDamage", victim.GetInstance(), source, 0);
    *pending = Pending(victim);
    if (apply && result) CallScript("AddHitPoints", victim.GetInstance(), source, -*pending);
    if (armed) weapon.Damage.DamageType = type;
    return result;
}

GEInt GameHit(Entity& victim, eCEntity* source, int* pending)
{
    GroundProbe ground(victim.GetInstance(), source, "synthetic-hit");
    *pending = 0;
    if (RejectDownedHit(victim.GetInstance(), source, "synthetic-hit")) return 0;
    if (downed::SyntheticProne(victim.GetInstance())) {
        victim.DamageReceiver.DamageAmount = 0;
        downed::LogGroundEvent(victim.GetInstance(), source, "synthetic-hit", ground.before, ground.before, "vanilla-prone-reject");
        return 0;
    }
    return JudgeDamage(victim, source, pending, true);
}
gFScript originalTouchDamage = nullptr;

GEInt GE_STDCALL TouchDamage(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalTouchDamage ? originalTouchDamage(spu, self, other, args) : 0;
    eCEntity* victim = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    eCEntity* source = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    GroundProbe ground(victim, source, "OnTouchDamage");
    if (RejectDownedHit(victim, source, "OnTouchDamage")) return 0;
    return originalTouchDamage(spu, self, other, args);
}

gFScript WrapDamageScript(gSScript& script, gFScript replacement, char const* name)
{
    gFScript originalFunction = script.m_funcScript;
    if (!originalFunction) {
        Logf("combat: %s has no registered function; it was not wrapped", name);
        return nullptr;
    }
    HMODULE owner = nullptr;
    auto* code = reinterpret_cast<unsigned char*>(originalFunction);
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (originalFunction &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(originalFunction), &owner) &&
        owner == GetModuleHandleA("Script_Game.dll") && !std::memcmp(code, prologue, sizeof(prologue))) {
        if (void* trampoline = ai::JumpTo(code, reinterpret_cast<void*>(replacement))) {
            script.m_funcScript = replacement;
            ai::TrackPointer(reinterpret_cast<void*>(&script.m_funcScript),
                             reinterpret_cast<void*>(originalFunction),
                             reinterpret_cast<void*>(replacement), "combat native script table");
            Logf("combat: %s wrapped at native entry and script table", name);
            return reinterpret_cast<gFScript>(trampoline);
        }
    }
    script.m_funcScript = replacement;
    ai::TrackPointer(reinterpret_cast<void*>(&script.m_funcScript),
                     reinterpret_cast<void*>(originalFunction),
                     reinterpret_cast<void*>(replacement), "combat script wrapper");
    Logf("combat: %s wrapped in script table only; native entry did not match the checked prologue", name);
    return originalFunction;
}
eCEntity* DamageSource(eCDynamicEntity* hero, char const** name)
{
    Entity weapon = Entity(hero).Inventory.GetItemFromSlot(gESlot_RightHand);
    if (!weapon.GetInstance()) weapon = Entity(hero).GetWeapon(GEFalse);
    *name = weapon.GetInstance() ? "weapon" : "hands";
    return weapon.GetInstance() ? weapon.GetInstance() : hero;
}
}
void Start()
{
    net::SetHitHandlers(&QueueHitNpc, &OnHitPlayer);
    net::SetKilledHandler(&OnKilled);
}
void InstallHooks()
{
    if (installed || !gCSession::GetInstance().IsGameRunning()) return;
    const DWORD now = GetTickCount();
    if (lastTry && now - lastTry < 1000) return;
    lastTry = now ? now : 1;
    gSScript* script = Find(GetScriptAdminExt().GetScripts(), "AddHitPoints");
    if (!script || !script->m_funcScript) return;
    original = WrapDamageScript(*script, &Filter, "AddHitPoints");
    installed = true;
    Log("combat: AddHitPoints filter installed");
    const gFScript wrappers[] = {&JudgeLogical, &JudgeOnDamage, &JudgeStatus<2>, &JudgeStatus<3>, &JudgeStatus<4>,
                                &JudgeStatus<5>, &JudgeStatus<6>};
    static_assert(sizeof(wrappers) / sizeof(wrappers[0]) == JudgementCount, "one wrapper per damage/status gate");
    for (int i = 0; i < JudgementCount; ++i) {
        gSScript* judgement = Find(GetScriptAdminExt().GetScripts(), Judgements[i]);
        if (!judgement || !judgement->m_funcScript) { Logf("combat: no %s script; its native friendly-fire protection is unavailable", Judgements[i]); continue; }
        judge[i] = WrapDamageScript(*judgement, wrappers[i], Judgements[i]);
    }
    gSScript* touch = Find(GetScriptAdminExt().GetScripts(), "OnTouchDamage");
    if (touch && touch->m_funcScript) originalTouchDamage = WrapDamageScript(*touch, &TouchDamage, "OnTouchDamage");
    else Log("combat: ANOMALY OnTouchDamage unavailable; ground contact guard not installed");
    // The OnTick callbacks run through the pointer stored at their registration (SG 0xe1ef0), not through the script table:
    // replacing the table's entry left the game's tick untouched (stageFightMusic 28.10.: music_ticks=0). Jump from the
    // function itself, after checking its first bytes (CP 1.75.14).
    static const unsigned char head[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0xd4, 0x04, 0x00, 0x00};
    auto* scriptGame = reinterpret_cast<unsigned char*>(GetModuleHandleA("Script_Game.dll"));
    unsigned char* code = scriptGame ? scriptGame + 0xb4070 : nullptr;
    if (code && !std::memcmp(code, head, sizeof(head)))
        musicTick = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&MusicTick)));
    Logf("combat: HandleMusicSituation %s", musicTick ? "wrapped (fight music for mirrors)" : code ? "has other bytes; no fight music for mirrors"
                                                                                              : "not found; no fight music for mirrors");
}
void Tick()
{
    InstallHooks();
    DrainQueuedNpcHits();
    ConfirmKills();
}
void OnWorldChange()
{
    ClearClaims();
    ClearMelee();
}
}
