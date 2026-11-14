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

    // Native CP 1.75 .xmot displacement token (index 13) and play speeds.
    assert(ParseMotionDisplacementCm("Orc_Stand_None_2H_P0_StumbleL_Hit_N_Back_00_%_00_P1_90.xmot") == 90.0f);
    assert(ParseMotionDisplacementCm("Hero_Stand_None_1H_P0_StumbleR_Hit_N_Fwd_00_%_00_P0_200.xmot") == 200.0f);
    assert(ParseMotionDisplacementCm("Orc_Stand_None_2H_P0_Stumble_Recover_00_0.xmot") == 0.0f);
    assert(ParseMotionDisplacementCm(nullptr) == 0.0f);

    assert(NativeStumblePlaySpeed(ReactionQuickStumble, true, false) == 2.0f);
    assert(NativeStumblePlaySpeed(ReactionQuickStumble, false, false) == 1.3f);
    assert(NativeStumblePlaySpeed(ReactionStumble, false, false) == 1.3f);
    assert(NativeStumblePlaySpeed(ReactionPierceStumble, false, false) == 1.0f);
    assert(NativeStumblePlaySpeed(ReactionQuickStumble, true, true) == 1.5f);

    // Front hits select "_Back_" recoil; hits from behind select "_Fwd_".
    assert(FrontHitDirection(0.0f, 1.0f, 0.0f, 120.0f));
    assert(!FrontHitDirection(0.0f, 1.0f, 0.0f, -120.0f));
    float pushDir[2] = {};
    assert(ComputeKnockbackStepDirection(100.0f, 300.0f, 100.0f, 150.0f, 0.0f, -1.0f, pushDir));
    assert(pushDir[0] == 0.0f && pushDir[1] > 0.999f);

    // Build 76 30 ms RTT + 45 ms snapshot delay (75 ms total lag):
    // NPC steps back immediately on impact (0..308 ms) and stays planted at +200 cm while
    // buffered host snapshots catch up, with zero late slide and zero double-stepping.
    HitKnockbackState kb;
    const float dir[2] = {1.0f, 0.0f};
    const float durSec = 0.40f / 1.3f; // 307.7 ms native StumbleL/R duration
    StartHitKnockback(kb, 1, 1000, durSec, dir, 200.0f, 0.0f);
    float prevX = 1000.0f;
    for (uint32_t t = 0; t <= 600; t += 16) {
        const uint32_t nowMs = 1000 + t;
        const uint32_t recordSent = 1000 + t - 75;
        const uint32_t ackAt = t >= 30 ? 1015u : 0u; // HitResult arrives at t=30 ms, processed on host at 1015
        const float hostStumbleAgeSec = recordSent > 1031 ? static_cast<float>(recordSent - 1031) / 1000.0f : 0.0f;
        const bool ownEcho = recordSent >= 1031 && hostStumbleAgeSec < durSec;
        const float hostPhase = ownEcho ? (hostStumbleAgeSec / durSec) : 0.0f;
        const float hostAppliedFrac = hostStumbleAgeSec <= 0.0f ? 0.0f
            : (hostStumbleAgeSec >= durSec ? 1.0f : (hostStumbleAgeSec / durSec));
        const float basePos[3] = {1000.0f + 200.0f * hostAppliedFrac, 0.0f, 0.0f};
        float outPos[3] = {};
        AdvanceHitKnockback(kb, nowMs, recordSent, ackAt, ownEcho, hostPhase, false, basePos, outPos);
        if (t == 32) assert(outPos[0] > 1015.0f); // immediate step-back in first 2 frames
        if (t >= 320) {
            // Once local stumble finishes (~308 ms), NPC remains planted at 1200 cm: no late slide!
            assert(outPos[0] >= 1198.0f && outPos[0] <= 1201.0f);
        }
        assert(outPos[0] - prevX >= -1.0f && outPos[0] - prevX <= 16.0f); // smooth, monotonic motion
        prevX = outPos[0];
    }
    assert(!kb.active);

    // Refused hit blends out smoothly instead of snapping back in one frame.
    StartHitKnockback(kb, 2, 2000, durSec, dir, 90.0f, 0.0f);
    float refusedPos[3] = {};
    const float originPos[3] = {500.0f, 0.0f, 0.0f};
    AdvanceHitKnockback(kb, 2048, 1980, 0, false, 0.0f, false, originPos, refusedPos);
    assert(refusedPos[0] > 510.0f);
    CancelHitKnockback(kb, 2048);
    float midCancelPos[3] = {};
    AdvanceHitKnockback(kb, 2096, 2028, 0, false, 0.0f, false, originPos, midCancelPos);
    assert(midCancelPos[0] > 500.5f && midCancelPos[0] < refusedPos[0]);
    float endCancelPos[3] = {};
    AdvanceHitKnockback(kb, 2200, 2132, 0, false, 0.0f, false, originPos, endCancelPos);
    assert(!kb.active && endCancelPos[0] == 500.0f);

    // Back-to-back combo hit #2 while hit #1's stumble snapshots are still draining from the buffer:
    // Hit #1's echo remains suppressed for animations, while knockback only matches kb.hitSequence == 2
    // so Hit #2 still steps back immediately without being zeroed or terminated by Hit #1's tail.
    assert(OwnImpactSequenceSeen(1, 1));
    assert(OwnImpactSequenceSeen(2, 1));
    assert(OwnImpactSequenceSeen(2, 2));
    assert(!OwnImpactSequenceSeen(1, 2));
    assert(!OwnImpactSequenceSeen(0, 1));
    StartHitKnockback(kb, 1, 3000, durSec, dir, 90.0f, 0.0f);
    float comboPos1[3] = {};
    const float comboBase1[3] = {1000.0f + 90.0f * 0.65f, 0.0f, 0.0f};
    AdvanceHitKnockback(kb, 3260, 3185, 3015, true, 0.65f, false, comboBase1, comboPos1);
    assert(comboPos1[0] >= 1075.0f);
    // Combo Hit #2 lands at t=3280 while buffered snapshot at t=3296 still carries Hit #1 (recordSeq=1, phase=0.80).
    StartHitKnockback(kb, 2, 3280, durSec, dir, 90.0f, 0.0f);
    assert(!KnockbackMatchesHostRecord(kb, 1, 1));
    assert(!KnockbackMatchesHostRecord(kb, 2, 1));
    assert(KnockbackMatchesHostRecord(kb, 2, 2));
    float comboPos2[3] = {};
    const float comboBase2[3] = {1000.0f + 90.0f * 0.80f, 0.0f, 0.0f};
    AdvanceHitKnockback(kb, 3312, 3237, 0, KnockbackMatchesHostRecord(kb, 1, 1), 0.80f, false, comboBase2, comboPos2);
    assert(kb.active && !kb.hostImpactDone);
    assert(comboPos2[0] > comboBase2[0] + 15.0f); // immediate combo #2 step-back preserved!
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
        playback_hit = (ROOT / "src" / "npcplayback_hit.cpp").read_text(encoding="utf-8")
        claims = (ROOT / "src" / "combat_claims.cpp").read_text(encoding="utf-8")
        combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        npc_send = (ROOT / "src" / "npc_send.cpp").read_text(encoding="utf-8")
        npc_contact = (ROOT / "src" / "npc_contact.cpp").read_text(encoding="utf-8")

        self.assertLess(len(playback.splitlines()), 1000)
        self.assertLess(len(playback_hit.splitlines()), 1000)
        self.assertIn("npcplayback_hit_policy::LocalStaggerActive", playback)
        self.assertIn("npcplayback_hit_policy::HitImpactMotion", playback)
        self.assertIn("npcplayback_hit_policy::HoldSuppressedImpact", playback)
        self.assertIn("npcplayback_hit_policy::AdvanceHitKnockback", playback)
        self.assertIn("detail::StartLocalHitKnockback", playback)
        self.assertIn("npcplayback_hit_policy::ParseMotionDisplacementCm", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeStumblePlaySpeed", playback_hit)
        self.assertIn("npcplayback_hit_policy::CancelHitKnockback", playback_hit)
        self.assertIn("contact::ContactRayY", playback_hit)
        self.assertIn("npcplayback_hit_policy::OwnImpactSequenceSeen", playback)
        self.assertIn("npcplayback_hit_policy::KnockbackMatchesHostRecord", playback)
        self.assertIn("if (ownImpactEcho) state.effects[i] = motion.instance;", playback)
        self.assertIn("NPCHITSYNC stage=predict-start", playback)
        self.assertIn("NPCHITSYNC stage=handoff", playback)
        self.assertNotIn("state.pendingHit || localAge < state.localDuration", playback)

        self.assertIn("npcs::LocalHit(instance, hit, reaction)", combat)
        self.assertIn("npcplayback::ResolveHitMotion(", npc_contact)
        self.assertIn("npcplayback::CancelLocalHit(", npc_contact)
        self.assertIn("reply.result.processedAt =", claims)
        self.assertIn("NPCHITSYNC stage=host-claim", claims)
        self.assertIn("npcplayback_hit_policy::HostHitStampExpired", npc_send)
        self.assertIn("NPCHITSYNC stage=host-ack", npc_contact)
        self.assertIn("NPCATTACK stage=local-miss", npc_contact)


if __name__ == "__main__":
    unittest.main()
