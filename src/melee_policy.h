// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace g3t::melee {
enum Attitude { None = 0, Friendly = 1, Neutral = 2, Angry = 3, Hostile = 4, Predator = 5, Panic = 6 };

struct Victim {
    bool partyMember = false, targeted = false, targetsPlayer = false, targetsPlayerParty = false;
    bool combat = false, hasFocus = false, focused = false, hasTarget = false;
    bool playerLedByVictim = false, ledByPlayer = false, targetOnly = false, item = true;
    int attitude = None, focusAttitude = None, targetAttitude = None;
    int relation = None, focusRelation = None;
};

enum class Decision { Allow, Party, Bystander, FightingEnemy, FriendlyCombatant, NeutralCombatant, TargetOnly };
enum class ClientDecision { Reject, Predict, HostJudge };

constexpr ClientDecision JudgeClient(bool allowed, bool relationsKnown)
{
    return !relationsKnown ? ClientDecision::HostJudge : allowed ? ClientDecision::Predict : ClientDecision::Reject;
}

constexpr bool ClaimedSwing(std::uint32_t swing, std::uint32_t currentLife, std::uint32_t unknownLife)
{
    return swing && (swing == currentLife || swing == unknownLife);
}

// Script_Game.dll CP 1.75: OnTouchDamage B492E..B4E78 (player branch), then
// DoLogicalDamage B51C0..B520C and B54F8..B5748 (melee relational gates).
constexpr Decision Judge(Victim const& v)
{
    if (v.partyMember && !v.targeted) return Decision::Party;
    if (!v.targetsPlayer && !v.partyMember) {
        if (!v.combat) {
            if (v.hasFocus && !v.focused && v.relation < v.focusRelation && v.relation < Hostile)
                return Decision::Bystander;
        } else if (v.hasFocus && !v.focused) {
            if (v.attitude != None && v.attitude < Hostile && v.attitude < v.focusAttitude)
                return Decision::Bystander;
            if (v.playerLedByVictim || v.ledByPlayer) return Decision::Party;
        } else if (v.hasTarget && v.attitude < Hostile && v.targetAttitude == Hostile) {
            return Decision::FightingEnemy;
        }
    }
    if (v.targetOnly && !v.targeted) return Decision::TargetOnly;
    if (v.item && v.combat && !v.targetsPlayer && !v.targetsPlayerParty) {
        if (v.attitude == Friendly) return Decision::FriendlyCombatant;
        if (v.attitude == Neutral && !v.targeted) return Decision::NeutralCombatant;
    }
    return Decision::Allow;
}

constexpr char const* Name(Decision decision)
{
    switch (decision) {
    case Decision::Allow: return "allow";
    case Decision::Party: return "protected-party";
    case Decision::Bystander: return "less-hostile-bystander";
    case Decision::FightingEnemy: return "fighting-hostile-npc";
    case Decision::FriendlyCombatant: return "friendly-combatant";
    case Decision::NeutralCombatant: return "neutral-combatant";
    case Decision::TargetOnly: return "target-only-move";
    }
    return "invalid-decision";
}
}
