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
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "lifecycle") == 0) return SimulateDeathAndDelayedRagdollNote();
    if (std::strcmp(argv[1], "overread") == 0) return SimulateSkeletonOverreadProtection();
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
        self.assertLess(len(self.corpse_cpp.splitlines()), 1000)
        self.assertLess(len(self.corpse_h.splitlines()), 1000)
        self.assertLess(len(self.ragdoll_h.splitlines()), 1000)

    def test_simulated_death_ragdoll_lifecycle_and_skeleton_bounds(self):
        subprocess.run([str(self.sim_exe), "lifecycle"], check=True, capture_output=True, text=True)
        subprocess.run([str(self.sim_exe), "overread"], check=True, capture_output=True, text=True)

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
        self.assertIn("ClearCorpseGroundAnchors()", _testbuild.src_text("npc_identity.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npcs.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npc_contact.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npc_companions.cpp"))
        self.assertIn("ForgetCorpseGroundAnchor(key)", _testbuild.src_text("npc_send.cpp"))


if __name__ == "__main__":
    unittest.main()
