# SPDX-License-Identifier: GPL-3.0-or-later
import subprocess
import sys
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


SIM_CODE = r"""
#include "ragdoll_force.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace g3t::ragdoll;

bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

struct SimulatedCorpse {
    Vector entityWorldPos{1000.0f, 300.0f, -500.0f};
    Vector corpseRootPos{1000.0f, 300.0f, -500.0f};
    Vector groundAnchor{0.0f, 0.0f, 0.0f};
    uint32_t groundIncarnation = 0;
    bool haveGroundAnchor = false;
    bool livePresented = true;
    bool corpsePlaced = false;
    bool killed = false;
    bool ragdollStarted = false;
    bool ragdollInitialized = false;
    uint32_t lieCheckAt = 0;
    DeathImpulse deathImpulse{};
};

void RecordGroundAnchor(SimulatedCorpse& c, uint32_t inc, Vector const& pos, bool overwrite)
{
    if (!Finite(pos)) return;
    if (!c.haveGroundAnchor || c.groundIncarnation != inc || overwrite) {
        c.groundAnchor = pos;
        c.groundIncarnation = inc;
        c.haveGroundAnchor = true;
    }
}

Vector GroundOrigin(SimulatedCorpse const& c, uint32_t inc)
{
    if (c.haveGroundAnchor && c.groundIncarnation == inc && Finite(c.groundAnchor))
        return c.groundAnchor;
    return c.entityWorldPos;
}

int SimulateDeathAndDelayedRagdollNote()
{
    SimulatedCorpse c{};
    const uint32_t inc = 1;
    const uint32_t deathNow = 10000;

    // 1. Host sends death snapshot with pelvis-shifted Y (+42 cm) while client skeleton is standing at Y=300.
    c.killed = true;
    CorpseOriginInput in{};
    in.entityPos = c.entityWorldPos;
    in.previousPos = {1000.0f, 300.0f, -500.0f};
    in.shownPos = {1001.5f, 342.0f, -499.0f};
    in.livePresented = c.livePresented;
    in.havePreviousAlive = true;
    in.hostRagdoll = true;

    CorpseOriginResult chosen{};
    if (!SelectCorpseOrigin(in, chosen)) return 10;
    if (!chosen.keepLiveMatrix) c.entityWorldPos = chosen.origin;
    RecordGroundAnchor(c, inc, chosen.origin, true);
    c.corpsePlaced = true;

    // Standing entity must NOT jump to Y=342 before ragdoll spawns NxActors.
    if (!Near(c.entityWorldPos.y, 300.0f)) return 11;

    // 2. LayDown runs on the exact death frame (0 ms gap) and initializes ragdoll immediately.
    c.ragdollStarted = true;
    c.ragdollInitialized = true;
    c.lieCheckAt = deathNow + 5000;

    // 3. Engine Novodex controller ticks ragdoll for 120 ms; entityWorldPos and corpseRoot follow pelvis to Y=365.
    c.entityWorldPos = {1004.0f, 365.0f, -496.0f};
    c.corpseRootPos = c.entityWorldPos;

    // 4. Reliable RagdollNote arrives at deathNow + 120 ms with Slave_02's +13712.8 upward head impulse.
    Vector force{}, rel{}, worldAt{};
    const Vector anchor = GroundOrigin(c, inc);
    if (!Near(anchor.y, 300.0f)) return 12;
    if (!ShapeCorpseImpulse({-3432.3f, 13712.8f, 9443.4f}, {0.0f, 180.0f, 0.0f}, anchor,
                            force, rel, worldAt))
        return 13;
    if (!c.deathImpulse.Choose(Source::Host)) return 14;

    // Force must have zero upward lift and contact at ground(300) + torso(100) = 400 cm, NOT pelvis(365) + 180 = 545 cm.
    if (!Near(force.y, 0.0f) || !Near(worldAt.y, 400.0f)) return 15;

    // 5. Incarnation 2 respawn + fresh death updates ground anchor cleanly.
    RecordGroundAnchor(c, 2, {1500.0f, 420.0f, -200.0f}, false);
    if (!Near(GroundOrigin(c, 2).y, 420.0f)) return 16;

    return 0;
}

int SimulateSkeletonOverreadProtection()
{
    // Native GetBoneMatrix checks `index <= mNodes.mLength` (65), so index 65 reads adjacent heap garbage.
    struct Mat12 { float v[12]; };
    std::vector<Mat12> heapNodes(66);
    for (int i = 0; i < 65; ++i) {
        const float m[12] = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
            1000.0f, 300.0f + float(i), -500.0f
        };
        std::memcpy(heapNodes[i].v, m, sizeof(m));
    }
    const float inf = std::numeric_limits<float>::infinity();
    const float garbage[12] = {inf, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, inf, 0.0f, -inf};
    std::memcpy(heapNodes[65].v, garbage, sizeof(garbage));

    const int safeCount = ClampedBoneCount(65, 128);
    if (safeCount != 65) return 20;

    Vector sum{0.0f, 0.0f, 0.0f};
    int valid = 0;
    for (int i = 0; i < safeCount; ++i) {
        if (!FiniteMatrix12(heapNodes[i].v)) continue;
        sum.x += heapNodes[i].v[9];
        sum.y += heapNodes[i].v[10];
        sum.z += heapNodes[i].v[11];
        ++valid;
    }
    if (valid != 65) return 21;
    const Vector center{sum.x / float(valid), sum.y / float(valid), sum.z / float(valid)};
    const Vector stage0Vel = SafeTraceVelocity(center, {0.0f, 0.0f, 0.0f}, 0, false);
    if (!Finite(center) || !Finite(stage0Vel)) return 22;

    return 0;
}

int SimulatePausedClientDeathAndUnpauseRecovery()
{
    // Reproduces Build 78 Scout_04: host kills NPC while client is paused for 109 s,
    // sends multiple dead+ragdoll packets (Pelvis Y=5231.1) + RagdollNote + FinalAndRelease.
    const uint32_t inc = 1;
    Vector lastGroundPos{91210.0f, 5141.1f, -12700.0f};
    bool haveLastGroundPos = false;
    Vector previousPos = lastGroundPos;

    // Alive sample records durable ground origin.
    if (IsGroundOriginSample(8, 2, lastGroundPos)) haveLastGroundPos = true;
    if (!haveLastGroundPos) return 30;

    // 10 dead+ragdoll packets arrive during client pause, overwriting `previousPos` with Pelvis Y=5231.1.
    for (int pkt = 0; pkt < 10; ++pkt) {
        const Vector deadPelvis{91211.0f, 5231.1f, -12699.0f};
        previousPos = deadPelvis;
        if (IsGroundOriginSample(129, RagdollDeadMovementMode, deadPelvis)) return 31;
    }

    SimulatedCorpse pausedKill{};
    pausedKill.entityWorldPos = {91211.0f, 5231.1f, -12699.0f}; // even if entity drifted
    CorpseOriginInput in{};
    in.entityPos = pausedKill.entityWorldPos;
    in.previousPos = lastGroundPos;
    in.shownPos = previousPos;
    in.livePresented = true;
    in.havePreviousAlive = haveLastGroundPos;
    in.hostRagdoll = true;

    CorpseOriginResult chosen{};
    if (!SelectCorpseOrigin(in, chosen)) return 32;
    if (chosen.keepLiveMatrix || !Near(chosen.origin.y, 5141.1f)) return 33;
    pausedKill.entityWorldPos = chosen.origin;
    RecordGroundAnchor(pausedKill, inc, chosen.origin, true);

    // Across 109 s of paused client frames, ragdoll start is deferred, impulse stays pending,
    // and fallbackGraceUntil never expires while paused.
    bool ragdollPaused = false;
    bool corpseSettled = false;
    uint32_t graceUntil = 10500;
    for (uint32_t now = 10000; now <= 119000; now += 16) {
        if (!ShouldDeferCorpseRagdoll(false, true, pausedKill.ragdollInitialized)) return 34;
        ragdollPaused = true;
        graceUntil = ExtendPausedFallbackGrace(graceUntil, now);
    }
    if (!ragdollPaused || pausedKill.ragdollInitialized || !pausedKill.deathImpulse.Pending()) return 35;
    if (static_cast<int32_t>(119016 - graceUntil) >= 0) return 36;

    // Unpause at t=119016: ragdoll starts immediately at ground feet (5141.1 -> single Pelvis 5231.1,
    // NOT double-lifted 5341.1) and consumes the preserved host RagdollNote.
    if (ShouldDeferCorpseRagdoll(false, false, pausedKill.ragdollInitialized)) return 37;
    pausedKill.ragdollStarted = true;
    pausedKill.ragdollInitialized = true;
    ragdollPaused = false;
    pausedKill.entityWorldPos.y += 90.0f; // single native SetEntityPoseFromRagDoll pelvis offset
    if (!Near(pausedKill.entityWorldPos.y, 5231.1f) || ragdollPaused) return 38;

    Vector force{}, rel{}, worldAt{};
    if (!ShapeCorpseImpulse({-9000.0f, 11000.0f, 4500.0f}, {0.0f, 175.0f, 0.0f},
                            GroundOrigin(pausedKill, inc), force, rel, worldAt))
        return 39;
    if (!pausedKill.deathImpulse.Choose(Source::Host)) return 40;
    if (!Near(worldAt.y, 5241.1f) || !Near(force.y, 0.0f)) return 41;

    // Mid-fall corpse (Slave_02) started 670 ms before pause: OnProcess is frozen during pause so
    // RagDollTick cannot call StopRagDoll, and if controller was lost before settle it restarts on unpause.
    if (!ShouldFreezeActiveRagdollPresentation(false, true, true, !corpseSettled)) return 42;
    if (!ShouldRestartUnsettledCorpseAfterPause(true, false, true, corpseSettled, false)) return 43;
    corpseSettled = true;
    if (ShouldRestartUnsettledCorpseAfterPause(true, false, true, corpseSettled, false)) return 44;

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "lifecycle") == 0) return SimulateDeathAndDelayedRagdollNote();
    if (std::strcmp(argv[1], "overread") == 0) return SimulateSkeletonOverreadProtection();
    if (std::strcmp(argv[1], "pause_recovery") == 0) return SimulatePausedClientDeathAndUnpauseRecovery();
    return 2;
}
"""

SIM_TARGET = _testbuild.register_target(
    "npc_corpse_lifecycle_sim",
    [_testbuild.inline_unit("npc_corpse_lifecycle_sim.cpp", SIM_CODE)],
)


class NpcCorpseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.corpse_cpp = _testbuild.src_text("npc_corpse.cpp")
        cls.corpse_h = _testbuild.src_text("npc_corpse.h")
        cls.ragdoll_h = _testbuild.src_text("ragdoll_force.h")
        cls.sim_exe = SIM_TARGET.executable()

    def test_touched_files_stay_under_one_thousand_lines(self):
        for name in (
            "npc_corpse.cpp",
            "npc_corpse.h",
            "ragdoll_force.h",
            "snapshot.cpp",
            "npcs.cpp",
            "npc_state.h",
            "npc_contact.cpp",
            "npc_companions.cpp",
        ):
            self.assertLess(len(_testbuild.src_text(name).splitlines()), 1000, name)

    def test_simulated_death_ragdoll_lifecycle_and_skeleton_bounds(self):
        subprocess.run([str(self.sim_exe), "lifecycle"], check=True, capture_output=True, text=True)
        subprocess.run([str(self.sim_exe), "overread"], check=True, capture_output=True, text=True)
        subprocess.run([str(self.sim_exe), "pause_recovery"], check=True, capture_output=True, text=True)

    def test_pause_deferral_and_unpause_recovery_contracts(self):
        lay = section(self.corpse_cpp, "void LayDown(Mirror& m, uint32_t now)", "void LogClientDeathImpl(")
        self.assertIn("ragdoll::ShouldDeferCorpseRagdoll(net::IsHost(), sessionPaused, m.ragdollInitialized)", lay)
        self.assertIn("m.lieCheckAt = 0;", lay)

        apply_fn = section(self.corpse_cpp, "bool TryApplyForce(", "bool HasPendingCorpseForce(")
        self.assertIn("gCSession::GetInstance().IsPaused()", apply_fn)
        self.assertIn("ragdoll::ExtendPausedFallbackGrace(m.fallbackGraceUntil, now)", apply_fn)

        update_fn = section(self.corpse_cpp, "void UpdateCorpsePresentation(", "void PresentDeadMirror(")
        self.assertIn("ragdoll::ShouldFreezeActiveRagdollPresentation(", update_fn)
        self.assertIn("ragdoll::ShouldRestartUnsettledCorpseAfterPause(", update_fn)

        npcs_cpp = _testbuild.src_text("npcs.cpp")
        on_npcs = section(npcs_cpp, "int OnNpcs(", "void SendOwned(")
        self.assertIn("ragdoll::IsGroundOriginSample(", on_npcs)
        self.assertIn("m.lastGroundPos", on_npcs)

        passive = section(npcs_cpp, "if (!HostPresentationAvailable(m.entity.GetInstance())) {", "if (m.staleHeld || m.replay.holding)")
        self.assertIn("PresentDeadMirror(it->first, m, m.latest, now);", passive)
        self.assertIn("else if (m.latest.flags & 2)", passive)

    def test_present_dead_mirror_preserves_live_ground_matrix_and_records_anchor(self):
        present = section(self.corpse_cpp, "void PresentDeadMirror(", "void OnRagdollNote(")
        self.assertIn("ragdoll::SelectCorpseOrigin(originInput, chosen)", present)
        self.assertIn("if (!chosen.keepLiveMatrix)", present)
        self.assertIn("RecordCorpseGroundAnchor(key, shown.incarnation, chosen.origin, now, true)", present)

    def test_laydown_starts_ragdoll_immediately_without_250ms_freeze_gap(self):
        lay = section(self.corpse_cpp, "void LayDown(Mirror& m, uint32_t now)", "void LogClientDeathImpl(")
        first_start = section(lay, "if (!m.ragdollStarted) {", "if (animation && animation->GetRagDollEnabled()")
        self.assertNotIn("m.lieCheckAt = now + 250", first_start)
        self.assertIn("EnsureFiniteRagdollPose(m, entity, animation)", first_start)
        self.assertIn("m.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead)", first_start)
        self.assertIn("completeRagdollStart()", lay)

    def test_force_application_is_immediate_on_ragdoll_init_and_uses_ground_anchor(self):
        avail = section(self.corpse_cpp, "bool HostForceAvailable(", "bool TryApplyForce(")
        self.assertIn("(m.killed || static_cast<int32_t>(m.shownSent - force.sent) >= 0)", avail)
        apply_fn = section(self.corpse_cpp, "bool TryApplyForce(", "bool HasPendingCorpseForce(")
        self.assertNotIn("m.shownSent - it->sent", apply_fn)
        self.assertIn("ResolveCorpseGroundAnchor(m)", apply_fn)
        self.assertIn("ragdoll::ShapeCorpseImpulse(raw, rawOffset, groundOrigin, force, offset, at)", apply_fn)
        self.assertNotIn("ragdoll::WorldPoint(offset, current)", apply_fn)

    def test_bone_iteration_and_corpse_capture_guard_against_getbonematrix_overread_and_nan(self):
        trace_fn = section(self.corpse_cpp, "void TraceCorpse(", "} // namespace")
        self.assertNotIn("i < 128", trace_fn)
        self.assertIn("SafeActorBoneCount(animation)", trace_fn)
        self.assertIn("FiniteMatrix(bone)", trace_fn)
        self.assertIn("ragdoll::SafeTraceVelocity(", trace_fn)

        bone_count = section(self.corpse_cpp, "int SafeActorBoneCount(", "bool ActorBonesFinite(")
        self.assertIn("ragdoll::ClampedBoneCount(skeleton->mNodes.mLength)", bone_count)

        capture = section(self.corpse_cpp, "void CaptureCorpsePose(", "void RestoreCorpseActor(")
        self.assertIn("!FiniteMatrix(entity->GetWorldMatrix()) || !ActorBonesFinite(animation)", capture)

    def test_corpse_ground_anchors_clear_across_world_lease_and_life_transitions(self):
        identity_cpp = _testbuild.src_text("npc_identity.cpp")
        contact_cpp = _testbuild.src_text("npc_contact.cpp")
        self.assertIn("ClearCorpseGroundAnchors()", identity_cpp)
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npcs.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", contact_cpp)
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npc_companions.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npc_send.cpp"))
        # Journal corpses bind hostSession = net::WelcomeSerial() so PresenceFacts keeps them visible.
        lay_corpse = section(self.corpse_cpp, "void LayDownCorpse(", "} // namespace g3t::npcs")
        self.assertIn("c.hostSession = net::WelcomeSerial();", lay_corpse)
        self.assertIn("existing->second.hostSession = net::WelcomeSerial();", lay_corpse)
        corpse_tick = section(self.corpse_cpp, "void CorpseTick(", "void ForgetCorpseGroundAnchor(")
        self.assertIn("c.hostSession = net::WelcomeSerial();", corpse_tick)
        # PresentedContactPoints clamps mNodes.mLength via ragdoll::ClampedBoneCount(..., 128) to avoid off-by-one GetBoneMatrix overread.
        self.assertIn("ragdoll::ClampedBoneCount(skeleton->mNodes.mLength, 128)", contact_cpp)
        # Disconnect/host presence reconciliation restores hidden entries via hiddenClientStates without wiping out-of-ROI states.
        reconcile = section(identity_cpp, "void ReconcileClientPresence()", "void ResetWorldNpcTables()")
        self.assertNotIn("hiddenClientStates.clear()", reconcile)
        self.assertIn("for (auto it = hiddenClientStates.begin(); it != hiddenClientStates.end();)", reconcile)


if __name__ == "__main__":
    unittest.main()
