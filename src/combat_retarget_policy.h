// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::combat_retarget {

struct NpcCombatSnapshot {
    uintptr_t currentTarget = 0;
    uintptr_t currentAttacker = 0;
    uintptr_t lastAttacker = 0;
    bool inCombatMode = false;
    uint32_t combatState = 0;
};

struct HitRetargetPlan {
    uintptr_t nextLastAttacker = 0;
    uintptr_t nextCurrentAttacker = 0;
    uintptr_t immediateCombatTarget = 0;
    bool shiftAttackers = false;
    bool applyReactionTask = false;
    bool runImmediateAssessAgressor = false;
};

// Mirrors native AssessHit (Script_Game.dll 0x2b580) + AssessAgressor (0x24490) for host-applied remote hits:
// 1. Predicted claims (!hostJudged) shift CurrentAttacker -> LastAttacker and record attacker in CurrentAttacker
//    before running stumble/retarget logic, matching native AssessHit.
// 2. Stumble reaction tasks (ZS_QuickStumble / ZS_Stumble / ZS_PierceStumble / ZS_SitKnockDown) call AssessAgressor
//    at task completion; running React() immediately as well either overwrote the stumble task or double-counted a
//    friendly NPC's warning blow. Immediate React() runs only when no stumble reaction task is active.
// 3. When the NPC is already in AIMode_Combat with another target and AssessAgressor's retarget condition holds
//    (attacker == LastAttacker || CombatState == 0), switch CurrentTarget immediately so interleaved companion blows
//    during the stumble animation cannot overwrite LastAttacker before ZS_*Stumble finishes.
inline HitRetargetPlan PlanHostNpcHit(NpcCombatSnapshot const& current, uintptr_t attacker,
                                      bool hostJudged, bool hasReactionCode, bool stumbleReactionActive) noexcept
{
    HitRetargetPlan plan{};
    if (!attacker) return plan;
    plan.shiftAttackers = !hostJudged;
    plan.nextLastAttacker = hostJudged ? current.lastAttacker : current.currentAttacker;
    plan.nextCurrentAttacker = hostJudged ? current.currentAttacker : attacker;
    plan.applyReactionTask = !hostJudged && hasReactionCode;
    plan.runImmediateAssessAgressor = !stumbleReactionActive;
    if (current.inCombatMode && current.currentTarget != attacker &&
        (plan.nextLastAttacker == attacker || current.combatState == 0)) {
        plan.immediateCombatTarget = attacker;
    }
    return plan;
}

} // namespace g3t::combat_retarget
