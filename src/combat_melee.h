// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "combat.h"
#include "melee_policy.h"
#include "net.h"
#include <string>
#include <unordered_map>

class eCEntity;
class Entity;

namespace g3t::combat::detail {
struct MeleeVerdict {
    melee::Victim victim;
    eCEntity* focus = nullptr;
    int relation = -1;
    char const* decision = "missing-claimant";
    bool allowed = false, relationsKnown = false;
};
struct PlayerMeleeContact {
    net::MeleeContact contact;
    MeleeVerdict verdict;
};
extern std::unordered_map<std::string, PlayerMeleeContact> playerMeleeContacts;
extern std::unordered_map<std::string, uint32_t> playerSwings, playerClaims;

bool InBout(Entity& npc);
bool OutsideBout(Entity& npc, eCEntity* attacker);
bool IsPuppetMeleeJudgement(eCEntity* victim, eCEntity* attacker, eCEntity* source);
bool IsLocalMelee(eCEntity* attacker, eCEntity* source);
net::MeleeContact CaptureMelee(eCEntity* attacker, eCEntity* source);
MeleeVerdict JudgeMelee(eCEntity* victim, eCEntity* claimant, uint8_t player, net::MeleeContact const& contact);
void LogMelee(char const* entry, uint8_t player, uint32_t sequence, eCEntity* victim,
              net::MeleeContact const& contact, MeleeVerdict const& verdict);
bool LocalMeleeAllowed(eCEntity* victim, eCEntity* attacker, net::MeleeContact const& contact);
bool SendHostJudgedMelee(eCEntity* victim, net::MeleeContact contact);
bool IsExplicitFinishingBlow(eCEntity* attacker, eCEntity* victim, eCEntity* source);
bool SendFinishingBlow(eCEntity* attacker, eCEntity* victim, eCEntity* source, bool allowEpisodeRetry = false);
void BeginFinishingExecution();
bool ClaimedPlayerSwing(uint8_t const identity[16], uint32_t swing);
void PlayHitEffect(eCEntity* victim, bool blocked);
void ClearMelee();
}
