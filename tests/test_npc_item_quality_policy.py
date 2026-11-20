# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

POLICY_CODE = r"""
#undef NDEBUG
#include "npc_item_quality_policy.h"
#include <cassert>
#include <initializer_list>

int main()
{
    using namespace g3t::npc_item_quality;

    // Non-ammo NPC weapons (1H/2H/Bow/CrossBow/Staff/Halberd/Axe) and shields receive Worn (256),
    // matching Script_Game.dll GenerateWeaponryFromTreasureSet (0x100ac1c0).
    for (int useType : {2 /* 1H */, 3 /* 2H */, 5 /* Bow */, 6 /* CrossBow */, 12 /* Staff */,
                        51 /* Halberd */, 52 /* Axe */}) {
        assert(ShouldMarkMirroredWeaponWorn(CategoryWeapon, useType));
        assert(MirroredSpawnQuality(CategoryWeapon, useType, 0) == QualityWorn);
        assert(MirroredSpawnQuality(CategoryWeapon, useType, 32 /* Sharp */) == (QualityWorn | 32u));
        assert(MirroredSpawnQuality(CategoryWeapon, useType, 1024u | 32u) == (QualityWorn | 32u));
    }
    assert(ShouldMarkMirroredWeaponWorn(2 /* Armor */, UseShield));
    assert(MirroredSpawnQuality(2 /* Armor */, UseShield, 0) == QualityWorn);

    // Ammo (Arrow=4, Bolt=7), Fist, and non-weapon props (Torch, Cup, Spellbook) stay unworn (0).
    for (int ammoUse : {UseArrow, UseBolt}) {
        assert(!ShouldMarkMirroredWeaponWorn(CategoryWeapon, ammoUse));
        assert(MirroredSpawnQuality(CategoryWeapon, ammoUse, 0) == 0u);
    }
    for (int fistUse : {UseFist, UsePhysicalFist}) {
        assert(!ShouldMarkMirroredWeaponWorn(CategoryWeapon, fistUse));
        assert(!ShouldMarkMirroredWeaponWorn(0 /* None */, fistUse));
        assert(MirroredSpawnQuality(CategoryWeapon, fistUse, 0) == 0u);
    }
    assert(!ShouldMarkMirroredWeaponWorn(6 /* Misc */, 24 /* Torch */));
    assert(MirroredSpawnQuality(6 /* Misc */, 24 /* Torch */, 0) == 0u);
    assert(!ShouldMarkMirroredWeaponWorn(9 /* Spellbook */, 15 /* Cast */));
    assert(MirroredSpawnQuality(9 /* Spellbook */, 15 /* Cast */, 0) == 0u);
    assert(!ShouldMarkMirroredWeaponWorn(-1, -1));
    assert(MirroredSpawnQuality(-1, -1, 0) == 0u);
    return 0;
}
"""

TARGET = _testbuild.register_target(
    "npc_item_quality_policy",
    [_testbuild.inline_unit("npc_item_quality_policy_test.cpp", POLICY_CODE)],
    respect_env=False,
)


def function_body(source, signature):
    match = re.search(signature, source)
    if not match:
        raise AssertionError(f"missing {signature}")
    opening = source.find("{", match.end())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated {signature}")


class NpcItemQualityPolicyTests(unittest.TestCase):
    def test_mirrored_npc_weapon_quality_policy(self):
        result = TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ensure_item_applies_mirrored_weapon_quality(self):
        npc_state = _testbuild.src_text("npc_state.h")
        self.assertIn('#include "npc_item_quality_policy.h"', npc_state)
        for enum_name in ("gEItemQuality_Worn", "gEItemCategory_Weapon", "gEUseType_Arrow",
                          "gEUseType_Bolt", "gEUseType_Fist", "gEUseType_Shield", "gEUseType_PhysicalFist"):
            self.assertIn(enum_name, npc_state)
        body = function_body(npc_state, r"\binline\s+void\s+EnsureItem\s*\(")
        self.assertIn("MirroredItemQuality(", body)
        self.assertNotIn(", 0, 1)", body)


if __name__ == "__main__":
    unittest.main()
