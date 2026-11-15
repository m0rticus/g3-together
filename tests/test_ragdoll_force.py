import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "ragdoll_force", ["tests/ragdoll_force_test.cpp"],
)

CORPSE_POLICY_CODE = r"""
#include "ragdoll_force.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {
using g3t::ragdoll::ClampedBoneCount;
using g3t::ragdoll::CorpseOriginInput;
using g3t::ragdoll::CorpseOriginResult;
using g3t::ragdoll::ExtendPausedFallbackGrace;
using g3t::ragdoll::Finite;
using g3t::ragdoll::FiniteMatrix12;
using g3t::ragdoll::IsGroundOriginSample;
using g3t::ragdoll::RagdollDeadMovementMode;
using g3t::ragdoll::SafeTraceVelocity;
using g3t::ragdoll::SelectCorpseOrigin;
using g3t::ragdoll::ShapeCorpseImpulse;
using g3t::ragdoll::ShouldDeferCorpseRagdoll;
using g3t::ragdoll::ShouldFreezeActiveRagdollPresentation;
using g3t::ragdoll::ShouldHoldPreDeathTransform;
using g3t::ragdoll::ShouldRestartUnsettledCorpseAfterPause;
using g3t::ragdoll::Vector;

bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }
bool NearVec(Vector const& a, Vector const& b, float eps = 0.01f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

int TestOriginSelection()
{
    // Live-presented client mirror: host WorldPosition jumped +45 cm to Bip01 Pelvis on RagDollDead.
    CorpseOriginInput live{};
    live.entityPos = {1000.0f, 500.0f, -2000.0f};
    live.previousPos = {1000.0f, 500.0f, -2000.0f};
    live.shownPos = {1002.0f, 545.0f, -1998.0f};
    live.livePresented = true;
    live.havePreviousAlive = true;
    live.hostRagdoll = true;
    CorpseOriginResult out{};
    if (!SelectCorpseOrigin(live, out)) return 10;
    if (!out.keepLiveMatrix || !NearVec(out.origin, live.entityPos)) return 11;

    // Un-replayed mirror with prior alive snapshot: clamp upward pelvis jump while keeping horizontal step.
    CorpseOriginInput unpresented = live;
    unpresented.livePresented = false;
    unpresented.shownPos = {1012.0f, 575.0f, -1988.0f};
    if (!SelectCorpseOrigin(unpresented, out)) return 12;
    if (out.keepLiveMatrix || !NearVec(out.origin, {1012.0f, 500.0f, -1988.0f})) return 13;

    // Streamed-in already-dead corpse with no live history places directly at shownPos.
    CorpseOriginInput streamed{};
    streamed.entityPos = {0.0f, 0.0f, 0.0f};
    streamed.shownPos = {4200.0f, 515.0f, 3100.0f};
    streamed.hostRagdoll = true;
    if (!SelectCorpseOrigin(streamed, out)) return 14;
    if (out.keepLiveMatrix || !NearVec(out.origin, streamed.shownPos)) return 15;

    // Large horizontal teleport (> 500 cm) adopts new destination instead of pinning stale origin.
    CorpseOriginInput teleported = live;
    teleported.shownPos = {3500.0f, 820.0f, -2000.0f};
    if (!SelectCorpseOrigin(teleported, out)) return 16;
    if (out.keepLiveMatrix || !NearVec(out.origin, teleported.shownPos)) return 17;

    // Live-presented mirror that drifted up toward Pelvis (+90 cm) clamps back down to durable ground Y.
    CorpseOriginInput driftedLive = live;
    driftedLive.entityPos = {1002.0f, 590.0f, -1998.0f};
    if (!SelectCorpseOrigin(driftedLive, out)) return 18;
    if (out.keepLiveMatrix || !NearVec(out.origin, {1002.0f, 500.0f, -1998.0f})) return 19;

    // Unpresented mirror right at spawn (< 150 cm) with no prior alive packet strips +90 cm Pelvis lift.
    CorpseOriginInput spawnDead{};
    spawnDead.entityPos = {4200.0f, 425.0f, 3100.0f};
    spawnDead.shownPos = {4210.0f, 515.0f, 3095.0f};
    spawnDead.hostRagdoll = true;
    if (!SelectCorpseOrigin(spawnDead, out)) return 40;
    if (out.keepLiveMatrix || !NearVec(out.origin, {4210.0f, 425.0f, 3095.0f})) return 41;

    return 0;
}

int TestPauseAndGroundPolicies()
{
    if (!IsGroundOriginSample(0, 2, {100.0f, 200.0f, 300.0f})) return 50;
    if (!IsGroundOriginSample(2, 1, {100.0f, 200.0f, 300.0f})) return 51; // downed stays on ground
    if (IsGroundOriginSample(1, 2, {100.0f, 200.0f, 300.0f})) return 52;
    if (IsGroundOriginSample(128, 2, {100.0f, 200.0f, 300.0f})) return 53;
    if (IsGroundOriginSample(0, RagdollDeadMovementMode, {100.0f, 200.0f, 300.0f})) return 54;

    if (!ShouldHoldPreDeathTransform(0, 2, 129, RagdollDeadMovementMode, 0.5f)) return 55;
    if (ShouldHoldPreDeathTransform(0, 2, 129, RagdollDeadMovementMode, 1.0f)) return 56;
    if (ShouldHoldPreDeathTransform(129, RagdollDeadMovementMode, 129, RagdollDeadMovementMode, 0.5f)) return 57;

    if (!ShouldDeferCorpseRagdoll(false, true, false)) return 58;
    if (ShouldDeferCorpseRagdoll(true, true, false)) return 59;
    if (ShouldDeferCorpseRagdoll(false, false, false)) return 60;
    if (ShouldDeferCorpseRagdoll(false, true, true)) return 61;

    if (!ShouldFreezeActiveRagdollPresentation(false, true, true, true)) return 62;
    if (!ShouldFreezeActiveRagdollPresentation(false, true, false, true)) return 63;
    if (ShouldFreezeActiveRagdollPresentation(false, true, false, false)) return 64;
    if (ShouldFreezeActiveRagdollPresentation(false, false, true, true)) return 65;

    if (!ShouldRestartUnsettledCorpseAfterPause(true, false, true, false, false)) return 66;
    if (ShouldRestartUnsettledCorpseAfterPause(true, false, true, true, false)) return 67;
    if (ShouldRestartUnsettledCorpseAfterPause(true, false, true, false, true)) return 68;

    if (ExtendPausedFallbackGrace(0, 50000) != 0) return 69;
    if (ExtendPausedFallbackGrace(10500, 50000) != 50500) return 70;
    return 0;
}

int TestCorpseImpulseShaping()
{
    Vector force{}, rel{}, worldAt{};
    const Vector ground{1200.0f, 300.0f, -800.0f};

    // Live-log Slave_02: raw force=(-3432.3, +13712.8, +9443.4) at_relative=(0, 180, 0).
    if (!ShapeCorpseImpulse({-3432.3f, 13712.8f, 9443.4f}, {0.0f, 180.0f, 0.0f}, ground,
                            force, rel, worldAt))
        return 20;
    if (!Near(force.x, -3432.3f, 0.5f) || !Near(force.y, 0.0f) || !Near(force.z, 9443.4f, 0.5f))
        return 21;
    if (!NearVec(rel, {0.0f, 100.0f, 0.0f}) || !NearVec(worldAt, {1200.0f, 400.0f, -800.0f}))
        return 22;

    // Live-log Slave_05: raw force=(9365.0, +10106.8, 5928.7) at_relative=(0, 150, 0).
    if (!ShapeCorpseImpulse({9365.0f, 10106.8f, 5928.7f}, {0.0f, 150.0f, 0.0f}, ground,
                            force, rel, worldAt))
        return 23;
    if (!Near(force.x, 9365.0f, 0.5f) || !Near(force.y, 0.0f) || !Near(force.z, 5928.7f, 0.5f))
        return 24;
    if (!NearVec(rel, {0.0f, 100.0f, 0.0f})) return 25;

    // Downward force and wide lever arm are bounded so floor-penetration bounce cannot occur.
    if (!ShapeCorpseImpulse({3000.0f, -9500.0f, 0.0f}, {80.0f, 10.0f, 60.0f}, ground,
                            force, rel, worldAt))
        return 26;
    if (!Near(force.y, -4000.0f) || !Near(rel.y, 50.0f)) return 27;
    const double radius = std::sqrt(double(rel.x) * rel.x + double(rel.z) * rel.z);
    if (std::fabs(radius - 45.0) > 0.05) return 28;

    return 0;
}

int TestBoneBoundsAndSanitization()
{
    if (ClampedBoneCount(65) != 65) return 30;
    if (ClampedBoneCount(0) != 0 || ClampedBoneCount(-4) != 0) return 31;
    if (ClampedBoneCount(400) != 256 || ClampedBoneCount(65, 32) != 32) return 32;

    const float valid[12] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        100.0f, 200.0f, -300.0f
    };
    if (!FiniteMatrix12(valid)) return 33;

    float infMat[12];
    std::memcpy(infMat, valid, sizeof(valid));
    infMat[9] = std::numeric_limits<float>::infinity();
    if (FiniteMatrix12(infMat)) return 34;

    float zeroBasis[12] = {0};
    zeroBasis[9] = 10.0f;
    if (FiniteMatrix12(zeroBasis)) return 35;

    // Live-log Slave_01: stage 0 (dt=0) with inf/-inf bone center previously evaluated inf * 0.0f = -nan(ind).
    const float inf = std::numeric_limits<float>::infinity();
    const Vector v0 = SafeTraceVelocity({inf, 0.0f, -inf}, {0.0f, 0.0f, 0.0f}, 0, false);
    const Vector v1 = SafeTraceVelocity({10.0f, 20.0f, 30.0f}, {inf, 0.0f, -inf}, 250, true);
    const Vector v2 = SafeTraceVelocity({110.0f, 20.0f, 30.0f}, {10.0f, 20.0f, 30.0f}, 250, true);
    if (!Finite(v0) || !NearVec(v0, {0.0f, 0.0f, 0.0f})) return 36;
    if (!Finite(v1) || !NearVec(v1, {0.0f, 0.0f, 0.0f})) return 37;
    if (!Finite(v2) || !NearVec(v2, {400.0f, 0.0f, 0.0f})) return 38;

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "origin") == 0) return TestOriginSelection();
    if (std::strcmp(argv[1], "corpse_impulse") == 0) return TestCorpseImpulseShaping();
    if (std::strcmp(argv[1], "sanitize") == 0) return TestBoneBoundsAndSanitization();
    if (std::strcmp(argv[1], "pause_policy") == 0) return TestPauseAndGroundPolicies();
    return 2;
}
"""

CORPSE_POLICY_TARGET = _testbuild.register_target(
    "ragdoll_corpse_policy",
    [_testbuild.inline_unit("ragdoll_corpse_policy.cpp", CORPSE_POLICY_CODE)],
)


class RagdollForceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()
        cls.corpse_executable = CORPSE_POLICY_TARGET.executable()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True)

    def run_corpse_case(self, case):
        subprocess.run([str(self.corpse_executable), case], check=True, capture_output=True, text=True)

    def test_force_is_independent_of_world_coordinates_and_contact_tracks_the_corpse(self):
        self.run_case("points")

    def test_native_forces_survive_and_larger_forces_keep_direction_within_the_bound(self):
        self.run_case("limit")

    def test_nonfinite_data_is_rejected_without_a_success_shaped_force(self):
        self.run_case("invalid")

    def test_host_gravity_late_duplicate_reload_and_new_life_decisions(self):
        self.run_case("once")

    def test_live_corpse_origin_preserves_ground_pose_against_host_pelvis_jump(self):
        self.run_corpse_case("origin")

    def test_corpse_impulse_strips_upward_lift_and_clamps_torso_lever_arm(self):
        self.run_corpse_case("corpse_impulse")

    def test_bone_bounds_matrix_validation_and_trace_velocity_reject_inf_nan(self):
        self.run_corpse_case("sanitize")

    def test_pause_deferral_ground_origin_and_unsettled_recovery_policies(self):
        self.run_corpse_case("pause_policy")


if __name__ == "__main__":
    unittest.main()
