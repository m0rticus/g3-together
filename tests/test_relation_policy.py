from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "relation_policy", ["tests/relation_policy_test.cpp"], msvc_vcvars=False,
)

NONLETHAL_POLICY_CODE = r"""
#undef NDEBUG
#include <cassert>
#include <initializer_list>
#include "relation_policy.h"

int main()
{
    using namespace g3t::relation;

    // Human (0) and Orc (5) use knockout rules unless combat is lethal; beasts/monsters are always lethal.
    assert(HumanoidCombatSpecies(0));
    assert(HumanoidCombatSpecies(5));
    for (int species : {1, 2, 3, 4, 6, 7, 12}) {
        assert(!HumanoidCombatSpecies(species));
        assert(IsLethalSharedCombat(HumanoidCombatSpecies(species), false, 9, Neutral, false));
    }

    // Non-lethal human/orc brawls (ReactToDamage=9, Theft=1, non-lethal PlayerZone=3, ReactToWeapon=4)
    // promote to Angry (3, orange), never Hostile (4, red).
    for (int reason : {1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16}) {
        assert(NonLethalCombatAttackReason(reason));
        assert(!LethalCombatAttackReason(reason));
        for (int base : {None, Friendly, Neutral, Angry}) {
            assert(!IsLethalSharedCombat(true, false, reason, base, false));
            SharedCombatInput in;
            in.aiMode = 3;
            in.attackReason = reason;
            in.targetsCoopSide = true;
            in.storedAttitude = base;
            in.lethal = IsLethalSharedCombat(true, false, reason, base, false);
            assert(ActiveSharedCombatHostility(in));
            const int eff = EffectiveAttitude(base, in);
            assert(eff == Angry);
            assert(NameColour(eff, false, false) == Colour::Orange);
            assert(!HostileTag(eff, false));
        }
    }

    // Preserves existing Hostile, Predator, and Panic even when in.lethal is false.
    {
        SharedCombatInput in;
        in.aiMode = 3;
        in.attackReason = 9;
        in.targetsCoopSide = true;
        in.lethal = false;
        assert(EffectiveAttitude(Hostile, in) == Hostile);
        assert(EffectiveAttitude(Predator, in) == Predator);
        assert(EffectiveAttitude(Panic, in) == Panic);
    }

    // Lethal attack reasons (Enemy=12, Murder=13, Revolution=19, Kill=20), HasAttackReasonToKill
    // (e.g. red security zone with PlayerZone=3), remoteLethal, or resting Hostile/Predator promote to Hostile (4, red).
    for (int reason : {12, 13, 19, 20}) {
        assert(LethalCombatAttackReason(reason));
        assert(IsLethalSharedCombat(true, false, reason, Neutral, false));
    }
    assert(IsLethalSharedCombat(true, true, 3, Friendly, false));
    assert(IsLethalSharedCombat(true, false, 9, Hostile, false));
    assert(IsLethalSharedCombat(true, false, 9, Predator, false));
    assert(IsLethalSharedCombat(true, false, 9, Neutral, true));
    {
        SharedCombatInput in;
        in.aiMode = 3;
        in.attackReason = 3;
        in.targetsCoopSide = true;
        in.lethal = IsLethalSharedCombat(true, true, 3, Neutral, false);
        const int eff = EffectiveAttitude(Neutral, in);
        assert(eff == Hostile);
        assert(NameColour(eff, false, false) == Colour::Red);
        assert(HostileTag(eff, false));
    }

    // Mirror shared hostility includes non-lethal Angry co-op brawls (targetsCoopSide / remoteSharedHostile)
    // while excluding an Angry town guard fighting a wild animal.
    assert(MirrorSharesCombatHostility(true, true, true, Angry, true, false, true));
    assert(MirrorSharesCombatHostility(true, true, true, Angry, false, true, false));
    assert(!MirrorSharesCombatHostility(true, true, true, Angry, true, false, false));

    // Weapon-drawn focus priority allows targeting non-lethal Angry co-op brawlers, not Angry guards fighting wolves.
    {
        FocusPriorityInput fi;
        fi.combatTask = true;
        fi.mirrored = true;
        fi.inCombat = true;
        fi.attitude = Angry;
        fi.sharedHostile = true;
        fi.targetsCoopSide = true;
        assert(CombatFocusPriorityOverride(fi) == 3);

        fi.sharedHostile = false;
        fi.targetsCoopSide = false;
        assert(CombatFocusPriorityOverride(fi) == -1);
    }
    return 0;
}
"""

NONLETHAL_TARGET = _testbuild.register_target(
    "relation_nonlethal_policy",
    [_testbuild.inline_unit("relation_nonlethal_policy_test.cpp", NONLETHAL_POLICY_CODE)],
    msvc_vcvars=False,
)


class RelationPolicyTests(unittest.TestCase):
    def test_production_relation_policy(self):
        result = TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_nonlethal_vs_lethal_combat_attitude_policy(self):
        result = NONLETHAL_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
