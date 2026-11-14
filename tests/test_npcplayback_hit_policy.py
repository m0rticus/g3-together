from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


class NpcPlaybackHitPolicyTests(unittest.TestCase):
    def test_predicted_stagger_never_freezes_across_recover_or_delayed_ack(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "npcplayback_hit_policy_check.cpp"
            cpp.write_text(r'''
#include "npcplayback_hit_policy.h"
#include <cassert>

int main() {
    using namespace g3t::npcplayback_hit_policy;

    // Only initial Stumble hit-impact clips duplicate LocalHit; Stumble_Recover must never be suppressed.
    assert(HitImpactMotion("Orc_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_%_00_P0_200.xmot"));
    assert(HitImpactMotion("Hero_Stand_None_1H_P0_QuickStumble_Hit_N_Fwd_00_%_00_P0_90.xmot"));
    assert(HitImpactMotion("Orc_Stand_None_2H_P0_ParadeStumble_Hit_N_Back_00.xmot"));
    assert(!HitImpactMotion("Orc_Stand_None_2H_P0_StumbleR_Recover_N_Fwd_00_%_00_P0_100.xmot"));
    assert(!HitImpactMotion("Hero_Stand_None_1H_P0_QuickStumble_Recover_N_Back_00.xmot"));
    assert(!HitImpactMotion("Orc_Stand_None_2H_P0_Attack_Hit_N_Fwd_00.xmot"));
    assert(!HitImpactMotion(nullptr));

    // Scenario 1 (Build 75 20 ms RTT + 35 ms buffer = 55 ms offset):
    // Local predicted Stumble_Hit runs 0..450 ms; at 450 ms host Stumble_Hit is already at phase ~0.88 (>= 0.75),
    // then enters Stumble_Recover at 505..1005 ms. Zero frozen frames occur.
    const float localDuration = 0.45f;
    unsigned frozenEchoFrames20ms = 0;
    for (uint32_t t = 0; t <= 1100; t += 16) {
        const float localAge = static_cast<float>(t) / 1000.0f;
        const bool local = LocalStaggerActive(true, localAge, localDuration);
        char const* hostMotion = t < 55 ? "Orc_Stand_None_2H_P0_Ambient_Loop_00.xmot"
            : (t < 505 ? "Orc_Stand_None_2H_P0_StumbleR_Hit_N_Back_00.xmot"
                       : "Orc_Stand_None_2H_P0_StumbleR_Recover_N_Fwd_00.xmot");
        const float hostPhase = t < 55 ? 0.2f
            : (t < 505 ? static_cast<float>(t - 55) / 450.0f : static_cast<float>(t - 505) / 500.0f);
        const bool suppressed = HitImpactMotion(hostMotion);
        const bool holdEcho = HoldSuppressedImpact(local, true, suppressed, localAge, localDuration, true, hostPhase);
        if (!local && holdEcho) ++frozenEchoFrames20ms;
    }
    assert(frozenEchoFrames20ms == 0);

    // Scenario 2 (Host accepted claim without stumbling, or echo delayed):
    // LocalStaggerActive ends strictly at localDuration even if pendingHit is still non-zero.
    assert(LocalStaggerActive(true, 0.0f, 0.45f));
    assert(LocalStaggerActive(true, 0.44f, 0.45f));
    assert(!LocalStaggerActive(true, 0.45f, 0.45f));
    assert(!LocalStaggerActive(true, 0.90f, 0.45f));
    assert(!LocalStaggerActive(false, 0.10f, 0.45f));

    // Scenario 3 (High 180 ms RTT where host Stumble_Hit is only at phase 0.50 when local finishes):
    // Bridges briefly (< 150 ms) to avoid rewinding, then hands off cleanly to host pose.
    assert(HoldSuppressedImpact(false, true, true, 0.46f, 0.45f, true, 0.50f));
    assert(!HoldSuppressedImpact(false, true, true, 0.61f, 0.45f, true, 0.50f));
    assert(!HoldSuppressedImpact(false, true, true, 0.46f, 0.45f, true, 0.80f));
    assert(!HoldSuppressedImpact(false, true, false, 0.46f, 0.45f, true, 0.20f));

    // PendingHit safety expiry and host hit-stamp cleanup after stagger ends.
    assert(!PendingHitExpired(7, 1000, 2999));
    assert(PendingHitExpired(7, 1000, 3000));
    assert(!PendingHitExpired(0, 1000, 5000));

    assert(!HostHitStampExpired(1000, 1150, false, "Orc_Stand_None_2H_P0_Ambient_Loop_00.xmot"));
    assert(!HostHitStampExpired(1000, 1500, true, "Orc_Stand_None_2H_P0_Ambient_Loop_00.xmot"));
    assert(!HostHitStampExpired(1000, 1500, false, "Orc_Stand_None_2H_P0_StumbleR_Recover_N_Fwd_00.xmot"));
    assert(HostHitStampExpired(1000, 1500, false, "Orc_Stand_None_2H_P0_Ambient_Loop_00.xmot"));
    return 0;
}
''', encoding="utf-8")
            target = _testbuild.NativeTarget(
                "npcplayback-hit-policy-check",
                [_testbuild.unit(cpp, std="c++17", warnings="strict", includes=(ROOT / "src",))],
            )
            executed = target.run(timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_playback_and_claim_sources_wire_non_freezing_handoff_and_logs(self):
        playback = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        claims = (ROOT / "src" / "combat_claims.cpp").read_text(encoding="utf-8")
        npc_send = (ROOT / "src" / "npc_send.cpp").read_text(encoding="utf-8")
        npc_contact = (ROOT / "src" / "npc_contact.cpp").read_text(encoding="utf-8")

        self.assertIn("npcplayback_hit_policy::LocalStaggerActive", playback)
        self.assertIn("npcplayback_hit_policy::HitImpactMotion", playback)
        self.assertIn("npcplayback_hit_policy::HoldSuppressedImpact", playback)
        self.assertIn("if (ownImpactEcho) state.effects[i] = motion.instance;", playback)
        self.assertIn("NPCHITSYNC stage=predict-start", playback)
        self.assertIn("NPCHITSYNC stage=handoff", playback)
        self.assertNotIn("state.pendingHit || localAge < state.localDuration", playback)

        self.assertIn("reply.result.processedAt =", claims)
        self.assertIn("NPCHITSYNC stage=host-claim", claims)
        self.assertIn("npcplayback_hit_policy::HostHitStampExpired", npc_send)
        self.assertIn("NPCHITSYNC stage=host-ack", npc_contact)
        self.assertIn("NPCATTACK stage=local-miss", npc_contact)


if __name__ == "__main__":
    unittest.main()
