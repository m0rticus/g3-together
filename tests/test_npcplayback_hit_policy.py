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
#include <cmath>
#include <cstring>

int main() {
    using namespace g3t::npcplayback_hit_policy;

    // HitImpactMotion vs HitRecoverMotion vs HitStumbleSequenceMotion:
    // Recover must NEVER trigger a second blood-spark impact effect, while sequence suppression
    // recognizes both Hit and Recover echoes of our own stagger.
    assert(HitImpactMotion("Orc_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_%_00_P0_200.xmot"));
    assert(HitImpactMotion("Hero_Stand_None_1H_P0_QuickStumble_Hit_N_Fwd_00_%_00_P0_90.xmot"));
    assert(HitImpactMotion("Orc_Stand_None_2H_P0_ParadeStumble_Hit_N_Back_00.xmot"));
    assert(!HitImpactMotion("Orc_Stand_None_2H_P0_StumbleR_Recover_N_Fwd_00_%_00_P0_100.xmot"));
    assert(!HitImpactMotion("Hero_Stand_None_1H_P0_QuickStumble_Recover_N_Back_00.xmot"));
    assert(!HitImpactMotion("Orc_Stand_None_2H_P0_Attack_Hit_N_Fwd_00.xmot"));
    assert(!HitImpactMotion(nullptr));

    assert(HitRecoverMotion("Hum_Stand_None_2H_P1_StumbleR_Recover_N_Back_00_00_00_P1_0.xmot"));
    assert(HitRecoverMotion("Orc_Stand_None_2H_P1_QuickStumble_Recover_N_Back_00_01_00_P1_0.xmot"));
    assert(!HitRecoverMotion("Hum_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_01_00_P1_90.xmot"));
    assert(!HitRecoverMotion("Orc_Stand_None_2H_P0_Attack_Recover_N_Fwd_00.xmot"));

    assert(HitStumbleSequenceMotion("Hum_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_01_00_P1_90.xmot"));
    assert(HitStumbleSequenceMotion("Hum_Stand_None_2H_P1_StumbleR_Recover_N_Back_00_00_00_P1_0.xmot"));
    assert(!HitStumbleSequenceMotion("Hum_Stand_None_2H_P1_Attack_Hit_N_Fwd_00.xmot"));

    // Native Script_Game.dll _AI_Stumble / _AI_QuickStumble rules:
    // 1. Contains(currentMovementAni, "_L.") -> StumbleL (true), else StumbleR (false).
    assert(NativeStumbleLeftFoot("Hum_Stand_None_2H_P0_Strafe_Loop_N_Left_00_00_00_P0_150_L.xmot"));
    assert(!NativeStumbleLeftFoot("Hum_Stand_None_2H_P0_Ambient_Loop_N_Fwd_00_02_00_P0_0.xmot"));
    assert(!NativeStumbleLeftFoot(nullptr));

    // 2. _AI_QuickStumble whitelist: Orcs always allowed; non-Orcs only 1H + {None, Shield, Torch}.
    assert(NativeQuickStumbleAllowed(SpeciesOrc, 3 /* 2H */, UseNone));
    assert(NativeQuickStumbleAllowed(0 /* Human */, Use1H, UseNone));
    assert(NativeQuickStumbleAllowed(0 /* Human */, Use1H, UseShield));
    assert(NativeQuickStumbleAllowed(0 /* Human */, Use1H, UseTorch));
    assert(!NativeQuickStumbleAllowed(0 /* Human */, 3 /* 2H */, UseNone));
    assert(!NativeQuickStumbleAllowed(0 /* Human */, 1 /* Fist */, UseNone));
    assert(!NativeQuickStumbleAllowed(0 /* Human */, Use1H, Use1H /* dual wield */));

    // 3. PrimaryPose extraction + Recover GetAniName stem synthesis from resolved Hit clip.
    assert(ParseMotionPrimaryPose("Hum_Stand_None_2H_P0_Ambient_Loop_00.xmot") == PoseP0);
    assert(ParseMotionPrimaryPose("Hum_Stand_None_2H_P1_PowerAttack_Hit_00.xmot") == PoseP1);
    assert(ParseMotionPrimaryPose("Orc_Stand_None_2H_P3_Ambient_Loop_00.xmot") == PoseP3);
    char stem[96] = {};
    assert(BuildRecoverStemFromHitMotion(
        "Orc_Stand_None_2H_P0_StumbleL_Hit_N_Back_00_%_00_P1_90.xmot", stem, sizeof(stem), true));
    assert(std::strcmp(stem, "Orc_Stand_None_2H_P1_StumbleL_Recover_N_Back_00_") == 0);
    assert(BuildRecoverStemFromHitMotion(
        "Hum_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_01_00_P1_90.xmot", stem, sizeof(stem), true));
    assert(std::strcmp(stem, "Hum_Stand_None_2H_P1_StumbleR_Recover_N_Back_00_") == 0);
    assert(BuildRecoverStemFromHitMotion(
        "Hum_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_01_00_P1_90.xmot", stem, sizeof(stem), false));
    assert(std::strcmp(stem, "Hum_Stand_None_2H_P0_StumbleR_Recover_N_Back_00_") == 0);
    assert(BuildRecoverStemFromHitMotion(
        "Orc_Stand_None_2H_P1_QuickStumble_Hit_N_Fwd_00_%_00_P10_200.xmot", stem, sizeof(stem), true));
    assert(std::strcmp(stem, "Orc_Stand_None_2H_P0_QuickStumble_Recover_N_Fwd_00_") == 0);
    assert(BuildRecoverStemFromHitMotion(
        "Orc_Stand_None_2H_P1_QuickStumble_Hit_N_Fwd_00_%_00_P10_200.xmot", stem, sizeof(stem), false));
    assert(std::strcmp(stem, "Orc_Stand_None_2H_P1_QuickStumble_Recover_N_Fwd_00_") == 0);

    // 4. Local Hit -> Recover timeline and smoothstep crossfade alpha.
    const auto stepHit = EvaluateLocalStumbleTimeline(true, true, 0.15f, 0.30f, 0.30f, false);
    assert(stepHit.stage == LocalStumbleStage::Hit && std::fabs(stepHit.phase - 0.5f) < 1e-4f);
    const auto stepRec = EvaluateLocalStumbleTimeline(true, true, 0.45f, 0.30f, 0.30f, false);
    assert(stepRec.stage == LocalStumbleStage::Recover && std::fabs(stepRec.phase - 0.5f) < 1e-4f);
    const auto stepInterrupted = EvaluateLocalStumbleTimeline(true, true, 0.35f, 0.30f, 0.30f, true);
    assert(stepInterrupted.stage == LocalStumbleStage::None); // yields immediately if host starts an attack after Hit!
    assert(PoseBlendAlpha(true, 1000, 1000, 0.10f) == 0.0f);
    assert(std::fabs(PoseBlendAlpha(true, 1000, 1050, 0.10f) - 0.5f) < 1e-4f);
    assert(PoseBlendAlpha(true, 1000, 1100, 0.10f) == 1.0f);

    // Scenario 1 (30 ms RTT + 45 ms buffer = 75 ms lag, chained Hit + Recover):
    // Local runs Hit (0..308 ms) + Recover (308..616 ms); at 616 ms host Recover is already at phase ~0.75 (>= 0.55).
    // Zero frozen echo frames occur!
    const float hitDur = 0.40f / 1.3f;
    const float recDur = 0.40f / 1.3f;
    unsigned frozenEchoFrames = 0;
    for (uint32_t t = 0; t <= 800; t += 16) {
        const float localAge = static_cast<float>(t) / 1000.0f;
        const auto step = EvaluateLocalStumbleTimeline(true, true, localAge, hitDur, recDur, false);
        const bool local = LocalStaggerActive(true, localAge, step.totalDurationSec) &&
                           step.stage != LocalStumbleStage::None;
        char const* hostMotion = t < 75 ? "Hum_Stand_None_2H_P0_Ambient_Loop_00.xmot"
            : (t < 383 ? "Hum_Stand_None_2H_P0_StumbleR_Hit_N_Back_00_01_00_P1_90.xmot"
            : (t < 691 ? "Hum_Stand_None_2H_P1_StumbleR_Recover_N_Back_00_00_00_P1_0.xmot"
                       : "Hum_Stand_None_2H_P1_Ambient_Loop_00.xmot"));
        const float hostPhase = t < 75 ? 0.2f
            : (t < 383 ? static_cast<float>(t - 75) / 308.0f
            : (t < 691 ? static_cast<float>(t - 383) / 308.0f : 0.1f));
        const bool suppressed = HitStumbleSequenceMotion(hostMotion);
        const bool recoverEcho = HitRecoverMotion(hostMotion);
        const bool holdEcho = HoldSuppressedImpact(
            local, true, suppressed, localAge, step.totalDurationSec, true, hostPhase, recoverEcho);
        if (!local && holdEcho) ++frozenEchoFrames;
    }
    assert(frozenEchoFrames == 0);

    // Scenario 2: LocalStaggerActive ends strictly at totalDurationSec.
    assert(LocalStaggerActive(true, 0.0f, 0.45f));
    assert(LocalStaggerActive(true, 0.44f, 0.45f));
    assert(!LocalStaggerActive(true, 0.45f, 0.45f));
    assert(!LocalStaggerActive(false, 0.10f, 0.45f));

    // Scenario 3: High 180 ms RTT bridges briefly (< 150 ms) only if host is still early in Hit/Recover.
    assert(HoldSuppressedImpact(false, true, true, 0.62f, 0.61f, true, 0.30f, true));
    assert(!HoldSuppressedImpact(false, true, true, 0.62f, 0.61f, true, 0.60f, true));
    assert(!HoldSuppressedImpact(false, true, true, 0.77f, 0.61f, true, 0.30f, true));

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

    // Build 77 reproduction (Warrior_04 front hit: Back_90 @ 1.3x = 308 ms Hit + 308 ms Recover, 75 ms lag):
    // Client moves from 1000 -> 1090 cm (EXACT 1.0x 90 cm native host displacement, never 200 cm or 290 cm!),
    // holds cleanly at 1090 cm across Recover while host snapshots catch up, and settles with zero overshoot.
    HitKnockbackState kb;
    const float dir[2] = {1.0f, 0.0f};
    const float anchor0[3] = {1000.0f, 0.0f, 0.0f};
    StartHitKnockback(kb, 1, 1000, hitDur, recDur, anchor0, dir, 90.0f, 0.0f);
    float prevX = 1000.0f;
    float maxX = 1000.0f;
    for (uint32_t t = 0; t <= 750; t += 16) {
        const uint32_t nowMs = 1000 + t;
        const uint32_t recordSent = 1000 + t - 75;
        const uint32_t ackAt = t >= 30 ? 1015u : 0u;
        const float hostStumbleAgeSec = recordSent > 1031 ? static_cast<float>(recordSent - 1031) / 1000.0f : 0.0f;
        const bool ownHitEcho = recordSent >= 1031 && hostStumbleAgeSec < hitDur;
        const bool ownRecEcho = recordSent >= 1031 && hostStumbleAgeSec >= hitDur && hostStumbleAgeSec < (hitDur + recDur);
        const float hostPhase = ownHitEcho ? (hostStumbleAgeSec / hitDur)
            : (ownRecEcho ? ((hostStumbleAgeSec - hitDur) / recDur) : 0.0f);
        const float hostAppliedFrac = hostStumbleAgeSec <= 0.0f ? 0.0f
            : (hostStumbleAgeSec >= hitDur ? 1.0f : (hostStumbleAgeSec / hitDur));
        const float basePos[3] = {1000.0f + 90.0f * hostAppliedFrac, 0.0f, 0.0f};
        float outPos[3] = {};
        AdvanceHitKnockback(kb, nowMs, recordSent, ackAt, ownHitEcho, ownRecEcho, hostPhase, false, basePos, outPos);
        if (t == 32) assert(outPos[0] > 1008.0f); // immediate step-back on impact
        if (t >= 320) assert(outPos[0] >= 1089.5f && outPos[0] <= 1090.5f); // stays planted at exact +90 cm!
        assert(outPos[0] - prevX >= -0.5f && outPos[0] - prevX <= 8.0f); // smooth, monotonic 1x motion
        if (outPos[0] > maxX) maxX = outPos[0];
        prevX = outPos[0];
    }
    assert(std::fabs(maxX - 1090.0f) < 0.5f); // strictly 1.0x 90 cm!
    assert(!kb.active);

    // Back-to-back 3-hit combo (seq=1 at t=3000, seq=2 at t=3350 during Recover, seq=3 at t=3700):
    // Anchoring each new hit at kb.lastPresentedPos yields exact 3 * 90 = 270 cm total displacement,
    // never stacking carryOffset on top of moving host snapshots!
    HitKnockbackState comboKb;
    const float comboOrigin[3] = {2000.0f, 0.0f, 0.0f};
    StartHitKnockback(comboKb, 1, 3000, hitDur, recDur, comboOrigin, dir, 90.0f, 0.0f);
    float posA[3] = {};
    const float hostMid1[3] = {2060.0f, 0.0f, 0.0f};
    AdvanceHitKnockback(comboKb, 3350, 3275, 3015, true, false, 0.67f, false, hostMid1, posA);
    assert(std::fabs(posA[0] - 2090.0f) < 0.5f); // Hit #1 reached +90 cm

    // Combo Hit #2 lands at t=3350 anchored at comboKb.lastPresentedPos (2090 cm) while buffered host is still at 2075 cm:
    StartHitKnockback(comboKb, 2, 3350, hitDur, recDur, comboKb.lastPresentedPos, dir, 90.0f, 0.0f);
    assert(!KnockbackMatchesHostRecord(comboKb, 1, 1));
    assert(!KnockbackMatchesHostRecord(comboKb, 2, 1));
    assert(KnockbackMatchesHostRecord(comboKb, 2, 2));
    float posB[3] = {};
    const float hostLate1[3] = {2085.0f, 0.0f, 0.0f};
    AdvanceHitKnockback(comboKb, 3382, 3307, 0, false, false, 0.90f, false, hostLate1, posB);
    assert(posB[0] > 2095.0f && posB[0] < 2110.0f); // smooth continuation from 2090 cm without jumping!
    const float hostDone2[3] = {2180.0f, 0.0f, 0.0f};
    AdvanceHitKnockback(comboKb, 3700, 3625, 3365, false, true, 0.20f, false, hostDone2, posB);
    assert(std::fabs(posB[0] - 2180.0f) < 0.5f); // Hit #2 reached exact 2 * 90 = +180 cm (never > 2180)!

    // Combo Hit #3 lands at t=3700 anchored at comboKb.lastPresentedPos (2180 cm):
    StartHitKnockback(comboKb, 3, 3700, hitDur, recDur, comboKb.lastPresentedPos, dir, 90.0f, 0.0f);
    float posC[3] = {};
    const float hostDone3[3] = {2270.0f, 0.0f, 0.0f};
    AdvanceHitKnockback(comboKb, 4250, 4175, 3715, false, true, 0.50f, false, hostDone3, posC);
    assert(std::fabs(posC[0] - 2270.0f) < 0.5f); // Exact 3 * 90 = +270 cm total!

    // Refused hit blends out smoothly from lastPresentedPos back to authoritative basePos over 120 ms.
    HitKnockbackState refusedKb;
    const float originPos[3] = {500.0f, 0.0f, 0.0f};
    StartHitKnockback(refusedKb, 4, 5000, hitDur, recDur, originPos, dir, 90.0f, 0.0f);
    float refusedPos[3] = {};
    AdvanceHitKnockback(refusedKb, 5064, 4989, 0, false, false, 0.0f, false, originPos, refusedPos);
    assert(refusedPos[0] > 515.0f);
    CancelHitKnockback(refusedKb, 5064);
    float midCancelPos[3] = {};
    AdvanceHitKnockback(refusedKb, 5124, 5049, 0, false, false, 0.0f, false, originPos, midCancelPos);
    assert(midCancelPos[0] > 502.0f && midCancelPos[0] < refusedPos[0]);
    float endCancelPos[3] = {};
    AdvanceHitKnockback(refusedKb, 5200, 5125, 0, false, false, 0.0f, false, originPos, endCancelPos);
    assert(!refusedKb.active && endCancelPos[0] == 500.0f);
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
        playback_h = (ROOT / "src" / "npcplayback.h").read_text(encoding="utf-8")
        playback_hit = (ROOT / "src" / "npcplayback_hit.cpp").read_text(encoding="utf-8")
        playback_policy = (ROOT / "src" / "npcplayback_hit_policy.h").read_text(encoding="utf-8")
        claims = (ROOT / "src" / "combat_claims.cpp").read_text(encoding="utf-8")
        combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        npc_send = (ROOT / "src" / "npc_send.cpp").read_text(encoding="utf-8")
        npc_contact = (ROOT / "src" / "npc_contact.cpp").read_text(encoding="utf-8")

        for src_text in (playback, playback_h, playback_hit, playback_policy, npc_contact):
            self.assertLess(len(src_text.splitlines()), 1000)

        # Native GetAni direction selection via SetCurrentAniDirection (never GetAniEx action-override misuse).
        self.assertNotIn("GetAniEx(", playback_hit)
        self.assertIn("SetCurrentAniDirection(", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeQuickStumbleAllowed", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeStumbleLeftFoot", playback_hit)
        self.assertIn("npcplayback_hit_policy::BuildRecoverStemFromHitMotion", playback_hit)
        self.assertIn("npcplayback_hit_policy::ParseMotionDisplacementCm", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeStumblePlaySpeed", playback_hit)
        self.assertIn("npcplayback_hit_policy::CancelHitKnockback", playback_hit)
        self.assertIn("CaptureEntityPoseBlend(", playback_hit)
        self.assertIn("EvaluateLocalActorPose(", playback_hit)
        self.assertIn("ApplyPoseBlend(", playback_hit)
        self.assertIn("contact::ContactRayY", playback_hit)
        self.assertIn("NPCHITSYNC stage=predict-start", playback_hit)

        # Smooth Hit -> Recover -> HostPose crossfade and echo suppression in Replay.
        self.assertIn("npcplayback_hit_policy::EvaluateLocalStumbleTimeline", playback)
        self.assertIn("npcplayback_hit_policy::LocalStaggerActive", playback)
        self.assertIn("npcplayback_hit_policy::HitImpactMotion", playback)
        self.assertIn("npcplayback_hit_policy::HitRecoverMotion", playback)
        self.assertIn("npcplayback_hit_policy::HitStumbleSequenceMotion", playback)
        self.assertIn("npcplayback_hit_policy::HoldSuppressedImpact", playback)
        self.assertIn("npcplayback_hit_policy::AdvanceHitKnockback", playback)
        self.assertIn("detail::StartLocalHitPresentation", playback)
        self.assertIn("detail::EvaluateLocalActorPose", playback)
        self.assertIn("detail::ApplyPoseBlend", playback)
        self.assertIn("npcplayback_hit_policy::OwnImpactSequenceSeen", playback)
        self.assertIn("npcplayback_hit_policy::KnockbackMatchesHostRecord", playback)
        self.assertIn("if (ownImpactEcho) state.effects[i] = motion.instance;", playback)
        self.assertIn("NPCHITSYNC stage=recover-start", playback)
        self.assertIn("NPCHITSYNC stage=handoff", playback)
        self.assertNotIn("state.pendingHit || localAge < state.localDuration", playback)

        self.assertIn("npcs::LocalHit(instance, hit, reaction)", combat)
        self.assertIn("npcplayback::ResolveHitMotion(", npc_contact)
        self.assertIn("npcplayback_hit_policy::NativeStumbleLeftFoot", npc_contact)
        self.assertIn("npcplayback::CancelLocalHit(", npc_contact)
        self.assertIn("reply.result.processedAt =", claims)
        self.assertIn("NPCHITSYNC stage=host-claim", claims)
        self.assertIn("npcplayback_hit_policy::HostHitStampExpired", npc_send)
        self.assertIn("NPCHITSYNC stage=host-ack", npc_contact)
        self.assertIn("NPCATTACK stage=local-miss", npc_contact)


if __name__ == "__main__":
    unittest.main()
