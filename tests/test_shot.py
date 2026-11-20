import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild
from test_downed_policy import function_body


SHOT_SWEEP_CODE = r"""
#include "shot_sweep.h"

#include <cassert>
#include <cmath>

int main()
{
    using namespace g3t::shot_sweep;

    // 1. Valid world AABB around relocated passive NPC (far from its original spawn).
    const float worldPos[3] = {5000.0f, 200.0f, -3000.0f};
    const float boxMin[3] = {4965.0f, 200.0f, -3035.0f};
    const float boxMax[3] = {5035.0f, 380.0f, -2965.0f};
    const Cylinder cyl = BuildTargetCylinder(worldPos, boxMin, boxMax, 180.0f);
    assert(std::fabs(cyl.cx - 5000.0f) < 0.1f);
    assert(std::fabs(cyl.cz - (-3000.0f)) < 0.1f);
    assert(cyl.radius >= 30.0f && cyl.radius <= 50.0f);
    assert(cyl.bottomY == 200.0f && cyl.topY == 380.0f);

    // Direct torso arrow step hits cylinder cleanly.
    const float p0[3] = {4900.0f, 310.0f, -3000.0f};
    const float p1[3] = {5100.0f, 305.0f, -3000.0f};
    float hitT = -1.0f;
    float hitPt[3] = {};
    assert(SweepSegmentCylinder(p0, p1, cyl, 6.0f, hitT, hitPt));
    assert(hitT > 0.2f && hitT < 0.5f);
    assert(std::fabs(hitPt[0] - (5000.0f - (cyl.radius + 6.0f))) < 0.5f);

    // Lateral miss, overhead miss, and underfoot miss all return false.
    const float missSide0[3] = {4900.0f, 310.0f, -2900.0f};
    const float missSide1[3] = {5100.0f, 310.0f, -2900.0f};
    assert(!SweepSegmentCylinder(missSide0, missSide1, cyl, 6.0f, hitT, hitPt));

    const float missHigh0[3] = {4900.0f, 420.0f, -3000.0f};
    const float missHigh1[3] = {5100.0f, 420.0f, -3000.0f};
    assert(!SweepSegmentCylinder(missHigh0, missHigh1, cyl, 6.0f, hitT, hitPt));

    const float missLow0[3] = {4900.0f, 180.0f, -3000.0f};
    const float missLow1[3] = {5100.0f, 180.0f, -3000.0f};
    assert(!SweepSegmentCylinder(missLow0, missLow1, cyl, 6.0f, hitT, hitPt));

    // 2. Degenerate/stale AABB falls back to worldPos + characterHeight.
    const float staleMin[3] = {0.0f, 0.0f, 0.0f};
    const float staleMax[3] = {60.0f, 180.0f, 60.0f};
    const Cylinder fallback = BuildTargetCylinder(worldPos, staleMin, staleMax, 180.0f);
    assert(std::fabs(fallback.cx - 5000.0f) < 0.1f);
    assert(std::fabs(fallback.cz - (-3000.0f)) < 0.1f);
    assert(SweepSegmentCylinder(p0, p1, fallback, 6.0f, hitT, hitPt));

    // 2b. Vertically stale AABB with matching X/Z falls back to worldPos + characterHeight.
    const float vertStaleMin[3] = {4965.0f, 900.0f, -3035.0f};
    const float vertStaleMax[3] = {5035.0f, 1080.0f, -2965.0f};
    const Cylinder vertFallback = BuildTargetCylinder(worldPos, vertStaleMin, vertStaleMax, 180.0f);
    assert(std::fabs(vertFallback.bottomY - 200.0f) < 0.1f && std::fabs(vertFallback.topY - 380.0f) < 0.1f);
    assert(SweepSegmentCylinder(p0, p1, vertFallback, 6.0f, hitT, hitPt));

    // 3. Point-blank frame-1 launch origin seeding catches close NPCs without projecting behind shooter.
    const float shooterChest[3] = {4880.0f, 310.0f, -3000.0f};
    const float firstFrameArrow[3] = {5060.0f, 308.0f, -3000.0f};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    float seed[3] = {};
    SeedLaunchOrigin(shooterChest, firstFrameArrow, dir, seed);
    assert(seed[0] < 4900.0f && seed[0] >= 4879.0f);
    assert(SweepSegmentCylinder(seed, firstFrameArrow, cyl, 6.0f, hitT, hitPt));

    // NPC standing behind the shooter is NOT hit by seeded segment.
    const float behindPos[3] = {4800.0f, 200.0f, -3000.0f};
    const Cylinder behindCyl = BuildTargetCylinder(behindPos, staleMin, staleMax, 180.0f);
    assert(!SweepSegmentCylinder(seed, firstFrameArrow, behindCyl, 6.0f, hitT, hitPt));
    return 0;
}
"""

TARGET = _testbuild.register_target(
    "shot_sweep_unit",
    [_testbuild.inline_unit("shot_sweep_unit.cpp", SHOT_SWEEP_CODE, pedantic=True)],
    respect_env=False, msvc_vcvars=False,
)


class ShotAndRangedCombatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shot = _testbuild.src_text("shot.cpp")
        cls.combat = _testbuild.src_text("combat.cpp")
        cls.combat_claims = _testbuild.src_text("combat_claims.cpp")

    def test_swept_cylinder_intersection_and_point_blank_origin(self):
        executed = TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_passive_npc_sweep_and_player_replica_tracking(self):
        spawn_shot = function_body(self.shot, r"\bvoid\s+SpawnShot\s*\(")
        self.assertIn("playerReplicas.emplace", spawn_shot)

        hook_touch = function_body(self.shot, r"\bvoid\s+__fastcall\s+HookTouch\s*\(")
        self.assertIn("playerReplicas.empty()", hook_touch)
        self.assertIn("PlayerReplica(arrow.GetInstance())", hook_touch)

        sweep = function_body(self.shot, r"\bvoid\s+SweepLocalProjectiles\s*\(")
        self.assertIsNotNone(sweep)
        self.assertIn("shot_sweep::SweepSegmentCylinder", sweep)
        self.assertIn("combat::IsGroundImmuneNpc(e)", sweep)
        self.assertIn("npcremoval::Suppressed(e)", sweep)
        self.assertIn("npcs::RedundantClientEntity(e)", sweep)
        self.assertIn("!npcs::ClientPresent(e)", sweep)
        self.assertIn("combat::ProjectileHit(bestNpc, instance)", sweep)
        self.assertIn("FlushPendingShot(instance)", sweep)
        self.assertIn("gESlot_RightHand", spawn_shot)

        own_caster = function_body(self.shot, r"\bbool\s+OwnCaster\s*\(")
        self.assertIn("net::IsHost()", own_caster)
        self.assertIn("RecordClaimedArrowInRemaining", self.shot)

    def test_spells_replica_lease_and_area_gates(self):
        spells = _testbuild.src_text("spells.cpp")
        wrap = function_body(spells, r"\bGEInt\s+GE_STDCALL\s+Wrap\s*\(")
        self.assertIn("shot::InPlayerReplicaContact()", wrap)
        self.assertIn("targetPlayer", wrap)
        area = function_body(spells, r"\bbTObjArray<Entity>\s+GE_STDCALL\s+AreaTargets\s*\(")
        self.assertIn("npcs::IsMirrored(t)", area)
        self.assertIn("npclease::Fenced(t)", area)
        on_npc = function_body(spells, r"\bvoid\s+OnNpcSpell\s*\(")
        self.assertIn("npclease::LocalAuthority(target.GetInstance())", on_npc)
        self.assertIn("targetPlayer", on_npc)
        norm = function_body(spells, r"\bbool\s+NormalizeCoopEarner\s*\(")
        self.assertIn("compOwner != localId", norm)

    def test_ranged_claim_routing_bow_slot_fallback_and_lethal_arrows(self):
        filt = function_body(self.combat, r"\bGEInt\s+GE_STDCALL\s+Filter\s*\(")
        self.assertIn("AllowsPlayerReplicaDamage", filt)
        self.assertIn("witness::KnockOutSeen(instance, attacker);", filt)
        self.assertIn("enclaves::NoteRemoteKill(guid, static_cast<uint8_t>(killerPlayer), attacker);", filt)
        self.assertIn("witness::MurderSeen(instance, attacker);", filt)
        self.assertNotIn("DefeatedByPlayer = GETrue", filt)
        self.assertIn("gESlot_LeftHand", filt)
        self.assertIn("if (npcs::Incarnation(identity) || !npcs::SeenAt(instance)) owner = 1;", filt)

        on_hit_npc = function_body(self.combat_claims, r"\bvoid\s+OnHitNpc\s*\(")
        self.assertIn("(outcome == net::HitKnockOut || (contact.melee != net::NotMelee && KnocksOut(target)))", on_hit_npc)

    def test_touched_source_files_stay_under_1000_lines(self):
        for rel in (
            "src/ranged_anim.h",
            "src/ranged_overlay.cpp",
            "src/ranged.h",
            "src/ranged.cpp",
            "src/shot_sweep.h",
            "src/shot.h",
            "src/shot.cpp",
            "src/body.h",
            "src/body.cpp",
            "src/remote.cpp",
            "src/movement_policy.h",
            "src/player_state.h",
            "src/playerwire.h",
            "src/combat.h",
            "src/combat.cpp",
            "src/combat_claims.cpp",
        ):
            line_count = len((ROOT / rel).read_text(encoding="utf-8").splitlines())
            self.assertLess(line_count, 1000, f"{rel} has {line_count} lines (must be < 1000)")


if __name__ == "__main__":
    unittest.main()
