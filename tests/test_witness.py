# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


TARGET = _testbuild.register_target(
    "witness_policy",
    [_testbuild.inline_unit(
        "witness_policy",
        r'''
#include "witness_policy.h"
#include <cassert>
#include <initializer_list>

int main()
{
    using namespace g3t::witness::policy;

    // Host-presented mirrors run with AIMode=Routine (0) and silenced empty CurrentTask: must open task.
    {
        const auto plan = PlanMirrorPerception(true, false, true, false, 100.0f * 100.0f, AimRoutine, true, false);
        assert(plan.eligible && !plan.openMode && plan.openTask);
    }
    // Watching mirrors in ZS_ObserveSuspect run with AIMode=Reaction (1) and silenced empty CurrentTask: must open task.
    {
        const auto plan = PlanMirrorPerception(true, false, true, false, 290.0f * 290.0f, AimReaction, true, false);
        assert(plan.eligible && !plan.openMode && plan.openTask);
    }
    // Pacified mirrors with AIMode=Abandoned and empty CurrentTask: must open both mode and task.
    {
        const auto plan = PlanMirrorPerception(true, false, true, false, 500.0f * 500.0f, AimAbandoned, true, false);
        assert(plan.eligible && plan.openMode && plan.openTask);
    }
    // Pacified mirrors with AIMode=Abandoned and non-empty CurrentTask: open mode only.
    {
        const auto plan = PlanMirrorPerception(true, false, true, false, 500.0f * 500.0f, AimAbandoned, false, false);
        assert(plan.eligible && plan.openMode && !plan.openTask);
    }
    // Mirrors already in Routine with non-empty task need no temporary mutation.
    {
        const auto plan = PlanMirrorPerception(true, false, true, false, 500.0f * 500.0f, AimRoutine, false, false);
        assert(!plan.eligible && !plan.openMode && !plan.openTask);
    }
    // Sleeping, talking, fleeing, downed, dead, unconscious, distant, or non-mirrored NPCs are never opened.
    for (int mode : {AimFlee, AimTalk, AimSleep, AimDown, AimDead}) {
        const auto plan = PlanMirrorPerception(true, false, true, false, 100.0f, mode, true, false);
        assert(!plan.eligible);
    }
    assert(!PlanMirrorPerception(true, false, true, false, 100.0f, AimRoutine, false, true).eligible);
    assert(!PlanMirrorPerception(true, false, true, true, 100.0f, AimRoutine, true, false).eligible);
    assert(!PlanMirrorPerception(true, true, true, false, 100.0f, AimRoutine, true, false).eligible);
    assert(!PlanMirrorPerception(false, false, true, false, 100.0f, AimRoutine, true, false).eligible);
    assert(!PlanMirrorPerception(true, false, false, false, 100.0f, AimRoutine, true, false).eligible);
    assert(!PlanMirrorPerception(true, false, true, false, 2001.0f * 2001.0f, AimRoutine, true, false).eligible);

    // Witness owner resolution: host-simulated mirrors on a client must still report to host (1)
    // even if OwnerOf returned 0.
    assert(ResolveWitnessOwner(0, false, true) == 1);
    assert(ResolveWitnessOwner(1, false, true) == 1);
    assert(ResolveWitnessOwner(0, false, false) == 0);
    assert(ResolveWitnessOwner(0, true, false) == 0);
    assert(ResolveWitnessOwner(2, true, true) == 2);

    // WitnessHolds must only send and apply when witness system and Together mode are active.
    assert(!ShouldSendWitnessHolds(true, false, 0, 3));
    assert(!ShouldSendWitnessHolds(false, true, 0, 3));
    assert(!ShouldSendWitnessHolds(true, true, 3, 3));
    assert(ShouldSendWitnessHolds(true, true, 0, 3));

    assert(!ShouldApplyWitnessHolds(true, false, false, 3, 0, 5));
    assert(!ShouldApplyWitnessHolds(false, true, false, 3, 0, 5));
    assert(!ShouldApplyWitnessHolds(true, true, true, 3, 0, 5));
    assert(!ShouldApplyWitnessHolds(true, true, false, 3, 3, 5));
    assert(!ShouldApplyWitnessHolds(true, true, false, 5, 0, 5));
    assert(ShouldApplyWitnessHolds(true, true, false, 3, 0, 5));

    // Watching mirrors (AIMode=Reaction, e.g., ZS_ObserveSuspect) within 10 m in the same indoor zone or with clear
    // LOS catch chest opening/looting even when mirror body yaw or head-look misses IsInFOV.
    auto makeWatch = []() {
        MirrorChestUseInput in{};
        in.enabled = true;
        in.mirrorsPerceive = true;
        in.mirror.mirrored = true;
        in.mirror.hasNpcAndRoutine = true;
        in.mirror.distanceSqCm = 290.0f * 290.0f;
        in.mirror.aiMode = AimReaction;
        return in;
    };
    {
        auto in = makeWatch();
        in.sameIndoorZone = true;
        assert(ShouldMirrorCatchChestUse(in));
    }
    {
        auto in = makeWatch();
        in.freeLosToHero = true;
        assert(ShouldMirrorCatchChestUse(in));
    }
    {
        auto in = makeWatch();
        in.freeLosToChest = true;
        assert(ShouldMirrorCatchChestUse(in));
    }
    // Watching mirrors behind closed walls (different zone and no LOS) never catch through walls.
    {
        auto in = makeWatch();
        in.inFovToHero = true;
        in.inFovToChest = true;
        assert(!ShouldMirrorCatchChestUse(in));
    }
    // Routine or Abandoned mirrors require FOV to hero or chest, plus zone/LOS, and <= 10 m.
    {
        auto in = makeWatch();
        in.mirror.aiMode = AimRoutine;
        in.sameIndoorZone = true;
        in.freeLosToHero = true;
        in.freeLosToChest = true;
        assert(!ShouldMirrorCatchChestUse(in));
        in.inFovToChest = true;
        assert(ShouldMirrorCatchChestUse(in));
    }
    {
        auto in = makeWatch();
        in.mirror.aiMode = AimAbandoned;
        in.freeLosToHero = true;
        in.inFovToHero = true;
        assert(ShouldMirrorCatchChestUse(in));
    }
    {
        auto in = makeWatch();
        in.mirror.distanceSqCm = 1001.0f * 1001.0f;
        in.sameIndoorZone = true;
        in.freeLosToHero = true;
        in.inFovToHero = true;
        assert(!ShouldMirrorCatchChestUse(in));
    }
    {
        auto in = makeWatch();
        in.chestDistanceSqCm = 1001.0f * 1001.0f;
        in.sameIndoorZone = true;
        in.freeLosToHero = true;
        in.inFovToHero = true;
        assert(!ShouldMirrorCatchChestUse(in));
    }
    // Disabled, non-mirrored, unconscious, mist/ghost status, or sleeping/talking/fleeing/down/dead mirrors never catch.
    {
        auto base = makeWatch();
        base.sameIndoorZone = true;
        base.freeLosToHero = true;
        base.inFovToHero = true;

        auto disabled = base; disabled.enabled = false;
        assert(!ShouldMirrorCatchChestUse(disabled));
        auto perceiveOff = base; perceiveOff.mirrorsPerceive = false;
        assert(!ShouldMirrorCatchChestUse(perceiveOff));
        auto nonMirror = base; nonMirror.mirror.mirrored = false;
        assert(!ShouldMirrorCatchChestUse(nonMirror));
        auto unconscious = base; unconscious.mirror.taskUnconscious = true;
        assert(!ShouldMirrorCatchChestUse(unconscious));
        auto mist = base; mist.npcHasMistEffect = true;
        assert(!ShouldMirrorCatchChestUse(mist));
        auto ghost = base; ghost.heroHasGhostEffect = true;
        assert(!ShouldMirrorCatchChestUse(ghost));
        for (int mode : {AimFlee, AimTalk, AimSleep, AimDown, AimDead}) {
            auto m = base;
            m.mirror.aiMode = mode;
            assert(!ShouldMirrorCatchChestUse(m));
        }
    }

    // Do not interrupt an NPC already actively executing ZS_Attack against that same puppet.
    assert(!ShouldApplyWitnessReaction(AimCombat, true, true));
    assert(ShouldApplyWitnessReaction(AimCombat, false, true));
    assert(ShouldApplyWitnessReaction(AimReaction, true, false));
    assert(ShouldApplyWitnessReaction(AimRoutine, true, false));
    return 0;
}
''',
        includes=[ROOT / "src"],
    )],
    allow_msvc=False,
    respect_env=False,
)


class WitnessPolicyTests(unittest.TestCase):
    def test_witness_policy_unit(self):
        executed = TARGET.run(timeout=60)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_witness_source_contracts(self):
        witness = _testbuild.src_text("witness.cpp")
        policy = _testbuild.src_text("witness_policy.h")
        containers = _testbuild.src_text("containers.cpp")
        self.assertLess(len(witness.splitlines()), 1000)
        self.assertLess(len(policy.splitlines()), 1000)
        self.assertLess(len(containers.splitlines()), 1000)
        self.assertIn("policy::PlanMirrorPerception(", witness)
        self.assertIn("policy::ResolveWitnessOwner(", witness)
        self.assertIn("policy::ShouldSendWitnessHolds(", witness)
        self.assertIn("policy::ShouldApplyWitnessHolds(", witness)
        self.assertIn("policy::ShouldMirrorCatchChestUse(", witness)
        self.assertIn("policy::ShouldApplyWitnessReaction(", witness)
        self.assertIn("in.chestDistanceSqCm =", witness)
        self.assertIn("IsBotheredByPlayerTheftOf", witness)
        self.assertNotIn('CallScript(bCString("AssessUseInteractObject"), body, chest', witness)
        self.assertIn("PerceiveChestUse(", witness)
        self.assertIn("witness::PerceiveChestUse(", containers)
        self.assertIn("PS_Interact_Chest_Loop", containers)
        self.assertIn("entry.openTask", witness)
        self.assertIn("ZS_HangAround_DoNothing", witness)
        self.assertIn("if (!enabled || !enclaves::Together() || witness.IsDead())", witness)
        for text in (witness, policy, containers):
            self.assertNotIn("RateLimit", text)
            self.assertNotIn("Throttle", text)


if __name__ == "__main__":
    unittest.main()
