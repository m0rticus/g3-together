// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "combat.h"
#include "combat_claims.h"
#include "combat_melee.h"
#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <string>
#include <unordered_set>

namespace g3t::combat::detail {
extern bool installed;
extern thread_local bool forwarded, heroForward;
extern std::unordered_set<std::string> killedHere;
extern unsigned kills, xpAwarded, claimsSent, claimsApplied, playerHitsSent, playerHitsTaken;
extern unsigned hostJudgedClaimsSent, hostContactJudgements, localMeleeRefused, duplicateClaimsIgnored;
extern unsigned npcHitsSent, npcHitsApplied, npcKills, npcHitsUnresolved, partyKills, reactionsStarted;
extern unsigned hitsRedirected, hitsWrongAuthority, hitReactionsApplied, forwardedJudged, forwardedBare;
extern unsigned inventoryHitsIgnored, clientJudgements, clientJudgementsWithoutDamage;
extern unsigned localNpcHits, remoteNpcHitsIgnored, invisibleContactsIgnored, friendlyHitsIgnored;
extern unsigned killsRepeated, rewardsRepeated, killsUndone, duelHitsRefused, duelKnockouts;
extern unsigned knockedOut, finishRefused, npcHitsScaled, hitEffectsPlayed;
extern uint8_t lastOutcome;

struct GroundProbe {
    eCEntity* victim;
    eCEntity* attacker;
    char const* path;
    int before;
    GroundProbe(eCEntity* who, eCEntity* by, char const* entry);
    ~GroundProbe();
};

std::string KillKey(uint8_t const guid[16]);
bool HasPlayerMemory(eCEntity* entity);
int HitPoints(Entity& entity);
bool HasDamageReceiver(eCEntity* entity);
int Pending(Entity& entity);
bool RejectDownedHit(eCEntity* victim, eCEntity* attacker, char const* entry);
GEInt CallScript(char const* name, eCEntity* self, eCEntity* other, GEInt args);
void Guid(eCEntity* entity, uint8_t out[16]);
uint8_t SideOf(eCEntity* entity);
int32_t Scaled(eCEntity* victim, eCEntity* attacker, int32_t delta);
bool MeansToKill(eCEntity* attacker, eCEntity* victim);
void NoteInventoryHit();
bool LocalNpcContact(gCScriptProcessingUnit* spu, GELPVoid other, eCEntity* attacker);
bool InventoryBlocksNpc(eCEntity* victim, eCEntity* attacker);
void LogAppliedHostNpcHit(eCEntity* victim, eCEntity* attacker, GEInt delta, int before, int after, GEInt result);
bool IsHostSimulatedNpc(eCEntity* victim);
GEInt JudgeDamage(Entity& victim, eCEntity* source, int* pending, bool apply = false);
GEInt GameHit(Entity& victim, eCEntity* source, int* pending);
eCEntity* DamageSource(eCDynamicEntity* hero, char const** name);
uint8_t Outcome(Entity& victim);
void RewardKill(uint8_t player, uint8_t const guid[16]);
void Expect(uint8_t const guid[16], uint8_t rewardPlayer = 0, bool clearDefeatedOnUndo = false);
void ClearKillXp();
}
