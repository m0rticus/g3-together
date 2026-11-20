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

    // 1) Native SPU loop-cutoff calibration matches Game.dll sAICombatMoveStart / sAICombatMoveItlLoop (~186 cm on 200 cm clip).
    const float eff200 = EffectiveNativeKnockbackCm(200.0f, 0.615f);
    assert(eff200 > 184.0f && eff200 < 188.0f);
    const float eff90 = EffectiveNativeKnockbackCm(90.0f, 0.615f);
    assert(eff90 > 82.5f && eff90 < 85.0f);

    // 2) Live character capsule obstacle clamp (e.g., Gorn/Milten/Diego behind an Orc) and walkable slope check.
    assert(std::fabs(ClampKnockbackToCharacterObstacle(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 186.0f, 120.0f, 0.0f, 0.0f, 82.0f) - 38.0f) < 0.5f);
    assert(std::fabs(ClampKnockbackToCharacterObstacle(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 186.0f, 120.0f, 0.0f, 95.0f, 82.0f) - 186.0f) < 0.01f);
    assert(std::fabs(ClampKnockbackToCharacterObstacle(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 186.0f, 120.0f, 260.0f, 0.0f, 82.0f) - 186.0f) < 0.01f);
    assert(WalkableKnockbackGroundDelta(100.0f, 40.0f));
    assert(!WalkableKnockbackGroundDelta(40.0f, 72.0f));

    // 3) Authoritative HitResult confirmation at 30 ms RTT:
    //    (a) Predicted 186 cm Fwd_200 corrected by host to 84 cm Back_90 without position jump at ack time.
    HitKnockbackState authKb;
    const float authOrigin[3] = {3000.0f, 0.0f, 0.0f};
    StartHitKnockback(authKb, 11, 1000, 0.615f, 0.538f, authOrigin, dir, 186.3f, 0.0f, 0.615f);
    float ackFramePos[3] = {};
    AdvanceHitKnockback(authKb, 1030, 955, 0, false, false, 0.0f, false, authOrigin, ackFramePos);
    const float preConfirmX = ackFramePos[0];
    const float hostGoal84[3] = {3083.8f, 0.0f, 0.0f};
    ConfirmHitKnockback(authKb, 11, 1030, ReactionStumble, 615, 615, authOrigin, hostGoal84, 83.8f);
    AdvanceHitKnockback(authKb, 1030, 955, 1015, false, false, 0.0f, false, authOrigin, ackFramePos);
    assert(std::fabs(ackFramePos[0] - preConfirmX) < 0.05f); // zero discontinuity on ack!
    AdvanceHitKnockback(authKb, 1615, 1540, 1015, false, true, 0.25f, false, hostGoal84, ackFramePos);
    assert(std::fabs(ackFramePos[0] - 3083.8f) < 0.4f); // converged cleanly to host's 83.8 cm!

    //    (b) Host HitNoReaction (0-step) stops outward prediction at ~9 cm instead of sliding 186 cm.
    HitKnockbackState noReactKb;
    StartHitKnockback(noReactKb, 12, 2000, 0.615f, 0.538f, authOrigin, dir, 186.3f, 0.0f, 0.615f);
    float nrPos[3] = {};
    AdvanceHitKnockback(noReactKb, 2030, 1955, 0, false, false, 0.0f, false, authOrigin, nrPos);
    assert(nrPos[0] > 3008.0f && nrPos[0] < 3012.0f);
    ConfirmHitKnockback(noReactKb, 12, 2030, ReactionNone, 0, 0, authOrigin, authOrigin, 0.0f);
    AdvanceHitKnockback(noReactKb, 2046, 1971, 2015, false, false, 0.0f, false, authOrigin, nrPos);
    assert(nrPos[0] <= 3010.5f); // never slides past ack position!

    //    (c) Blocked combo Hit #2 when client was already ahead of hostGoalPos does not stack overshoot.
    HitKnockbackState blockedComboKb;
    const float overshotStart[3] = {3060.0f, 0.0f, 0.0f};
    const float blockedHostGoal[3] = {3035.0f, 0.0f, 0.0f};
    StartHitKnockback(blockedComboKb, 13, 3000, 0.615f, 0.538f, overshotStart, dir, 186.3f, 0.0f, 0.615f);
    ConfirmHitKnockback(blockedComboKb, 13, 3030, ReactionStumble, 615, 115, authOrigin, blockedHostGoal, 35.0f);
    float bcPos[3] = {};
    AdvanceHitKnockback(blockedComboKb, 3300, 3225, 3015, true, false, 0.50f, false, blockedHostGoal, bcPos, 186.3f);
    assert(bcPos[0] <= 3060.5f); // clamped in place instead of sliding to 3246 cm!

    // 4) Closed-loop ownImpactEcho stall braking + host-pose-tail hold + velocity-bounded smooth reconciliation:
    //    Suppose a moving companion blocks the host NPC mid-stumble at +45 cm (unknown at t=0).
    HitKnockbackState stallKb;
    StartHitKnockback(stallKb, 14, 4000, 0.615f, 0.538f, authOrigin, dir, 186.3f, 0.0f, 0.615f);
    const float stalledBase[3] = {3045.0f, 0.0f, 0.0f};
    float stPos[3] = {};
    AdvanceHitKnockback(stallKb, 4180, 4105, 4015, true, false, 0.22f, false, stalledBase, stPos, 186.3f);
    AdvanceHitKnockback(stallKb, 4260, 4185, 4015, true, false, 0.35f, false, stalledBase, stPos, 186.3f);
    AdvanceHitKnockback(stallKb, 4340, 4265, 4015, true, false, 0.48f, false, stalledBase, stPos, 186.3f);
    AdvanceHitKnockback(stallKb, 4615, 4540, 4015, true, false, 0.93f, false, stalledBase, stPos, 186.3f);
    // Mid-stumble echo braking kept total displacement vastly below 3186 cm, and phase=0.93 tail did not settle early!
    assert(stPos[0] < 3120.0f && !stallKb.settling);

    // Once host enters Recover at 3045 cm, residual offset reconciles smoothly at <= 90 cm/s (never a 160 ms teleport!).
    float settlePrevX = stPos[0];
    AdvanceHitKnockback(stallKb, 4631, 4556, 4015, false, true, 0.05f, false, stalledBase, stPos);
    assert(stallKb.settling);
    for (uint32_t t = 4647; t <= 4967; t += 16) {
        AdvanceHitKnockback(stallKb, t, t - 75, 4015, false, true, 0.30f, false, stalledBase, stPos);
        const float stepDelta = std::fabs(stPos[0] - settlePrevX);
        assert(stepDelta <= 90.0f * 0.016f + 0.25f); // strictly bounded per-frame reconciliation velocity!
        settlePrevX = stPos[0];
    }
    // And while reconciling, if the host NPC begins walking forward (+4 cm per 16 ms = 250 cm/s), outPos tracks live motion smoothly.
    HitKnockbackState walkSettleKb;
    StartHitKnockback(walkSettleKb, 15, 6000, 0.50f, 0.40f, authOrigin, dir, 90.0f, 0.0f, 0.50f);
    const float settleHost0[3] = {3060.0f, 0.0f, 0.0f};
    float wsPos[3] = {};
    AdvanceHitKnockback(walkSettleKb, 6500, 6425, 6015, false, true, 0.10f, false, settleHost0, wsPos);
    assert(walkSettleKb.settling && std::fabs(wsPos[0] - 3090.0f) < 0.5f);
    const float settleHost1[3] = {3064.0f, 0.0f, 0.0f}; // NPC moved +4 cm toward +X while offset was +30 cm
    AdvanceHitKnockback(walkSettleKb, 6516, 6441, 6015, false, false, 0.15f, false, settleHost1, wsPos);
    assert(wsPos[0] > 3090.5f && wsPos[0] < 3094.0f); // preserves live NPC motion while gently decaying offset!

    // 5) Death / downed (incapacitated=true) unconditionally clears both active knockback and settling reconcileOffset,
    //    and never caches corpse/downed ragdoll pelvis positions into lastPresentedPos.
    AdvanceHitKnockback(walkSettleKb, 6532, 6457, 6015, false, false, 0.20f, true, settleHost1, wsPos);
    assert(!walkSettleKb.active && !walkSettleKb.settling && !walkSettleKb.hasPresentedPos);
    assert(wsPos[0] == settleHost1[0] && walkSettleKb.reconcileOffset[0] == 0.0f);
    const float corpsePelvisPos[3] = {3064.0f, 92.0f, 0.0f};
    AdvanceHitKnockback(walkSettleKb, 6548, 6473, 6015, false, false, 0.0f, true, corpsePelvisPos, wsPos);
    assert(!walkSettleKb.active && !walkSettleKb.hasPresentedPos && wsPos[1] == 92.0f);

    // 6) Reliable HitResult arriving after unreliable NpcRecord cleared pendingHit still confirms active knockback,
    //    while late unqueued rejections or mismatched/already-confirmed sequences are ignored.
    HitKnockbackState oooKb;
    StartHitKnockback(oooKb, 21, 7000, 0.615f, 0.538f, authOrigin, dir, 186.3f, 0.0f, 0.615f);
    assert(ShouldAcceptHitResult(21, oooKb, 21, true));
    assert(ShouldAcceptHitResult(21, oooKb, 21, false));
    assert(ShouldAcceptHitResult(0, oooKb, 21, true));   // pendingHit cleared by fast NpcRecord, kb still unconfirmed!
    assert(!ShouldAcceptHitResult(0, oooKb, 21, false)); // without pendingHit, late refusal is ignored
    assert(!ShouldAcceptHitResult(0, oooKb, 22, true));  // wrong sequence
    ConfirmHitKnockback(oooKb, 21, 7030, ReactionStumble, 615, 615, authOrigin, hostGoal84, 83.8f);
    assert(!ShouldAcceptHitResult(0, oooKb, 21, true));  // already confirmed

    // 7) 16-bit sequence wrapping preserves non-zero values at multiples of 65536; signed elapsed time never wraps.
    assert(WrapNonZeroHitSeq(0u) == 0u);
    assert(WrapNonZeroHitSeq(65536u) == 1u);
    assert(WrapNonZeroHitSeq(65535u) == 65535u);
    assert(ElapsedSecondsSigned(995u, 1000u) == 0.0f);
    assert(std::fabs(ElapsedSecondsSigned(1050u, 1000u) - 0.05f) < 1e-5f);

    // 8) Zero-step knockback (!kb.active) still tracks hostImpactSeen / hostImpactDone for Recover cut-off.
    HitKnockbackState zeroStepKb;
    StartHitKnockback(zeroStepKb, 31, 8000, 0.30f, 0.30f, authOrigin, dir, 0.0f, 0.0f);
    assert(!zeroStepKb.active);
    float zsPos[3] = {};
    AdvanceHitKnockback(zeroStepKb, 8050, 7980, 8010, true, false, 0.2f, false, authOrigin, zsPos);
    assert(zeroStepKb.hostImpactSeen && !zeroStepKb.hostImpactDone);
    AdvanceHitKnockback(zeroStepKb, 8350, 8280, 8010, false, true, 0.1f, false, authOrigin, zsPos);
    assert(zeroStepKb.hostImpactDone);
    HitKnockbackState zeroConfirmKb;
    StartHitKnockback(zeroConfirmKb, 32, 9000, 0.30f, 0.30f, authOrigin, dir, 0.0f, 0.0f);
    ConfirmHitKnockback(zeroConfirmKb, 32, 9020, ReactionNone, 0, 0, authOrigin, authOrigin, 0.0f);
    assert(zeroConfirmKb.hostImpactDone);

    // 9) QuickStumble candidate list omits QuickStumble when allowQuickClip is false.
    int cands[4] = {};
    const int disallowedCount = BuildHitReactionCandidates(ReactionQuickStumble, false, 10, 11, 12, 13, 14, 15, cands);
    assert(disallowedCount == 3 && cands[0] == 10 && cands[1] == 11 && cands[2] == 12);
    const int allowedCount = BuildHitReactionCandidates(ReactionQuickStumble, true, 10, 11, 12, 13, 14, 15, cands);
    assert(allowedCount == 4 && cands[0] == 13);
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
        net_h = (ROOT / "src" / "net.h").read_text(encoding="utf-8")
        net_internal = (ROOT / "src" / "net_internal.h").read_text(encoding="utf-8")
        net_messages = (ROOT / "src" / "net_messages.cpp").read_text(encoding="utf-8")
        npc_send = (ROOT / "src" / "npc_send.cpp").read_text(encoding="utf-8")
        npc_send_h = (ROOT / "src" / "npc_send.h").read_text(encoding="utf-8")
        npc_contact = (ROOT / "src" / "npc_contact.cpp").read_text(encoding="utf-8")
        npcs_cpp = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")

        for src_text in (playback, playback_h, playback_hit, playback_policy, claims, net_h, net_internal,
                         net_messages, npc_contact, npc_send, npc_send_h, npcs_cpp):
            self.assertLess(len(src_text.splitlines()), 1000)
        self.assertLessEqual(len(playback.splitlines()), 990)
        self.assertLessEqual(len(npcs_cpp.splitlines()), 975)

        self.assertIn("LegacyHitResultMsgSize = offsetof(HitResultMsg, result) + offsetof(HitResult, knockbackValid);", net_internal)
        self.assertIn("packet->dataLength == LegacyHitResultMsgSize", net_messages)

        reset_body = playback_hit.split("void ResetLocalHitState(State& state)", 1)[1].split("void CancelLocalHit(", 1)[0]
        for cleared in ("state.pendingHit = 0;", "state.localInRecover = false;",
                        "state.localMotion[0] = '\\0';", "state.localRecoverMotion[0] = '\\0';",
                        "state.poseBlend.active = false;", "state.knockback = {};"):
            self.assertIn(cleared, reset_body)
        self.assertIn("void ResetLocalHitState(State& state);", playback_h)
        self.assertIn("if (record.flags & 3) ResetLocalHitState(state);", playback)
        self.assertGreaterEqual(npcs_cpp.count("npcplayback::ResetLocalHitState(m.replay);"), 2)
        self.assertIn("npcplayback::ResetLocalHitState(m.replay);", npc_contact)

        # Native GetAni direction selection via SetCurrentAniDirection (never GetAniEx action-override misuse).
        self.assertNotIn("GetAniEx(", playback_hit)
        self.assertIn("SetCurrentAniDirection(", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeQuickStumbleAllowed", playback_hit)
        self.assertIn("npcplayback_hit_policy::NativeStumbleLeftFoot", playback_hit)
        self.assertIn("npcplayback_hit_policy::BuildRecoverStemFromHitMotion", playback_hit)
        self.assertIn("npcplayback_hit_policy::ParseMotionDisplacementCm", playback_hit)
        self.assertIn("npcplayback_hit_policy::EffectiveNativeKnockbackCm", playback_hit)
        self.assertIn("npcplayback_hit_policy::ClampKnockbackToCharacterObstacle", playback_hit)
        self.assertIn("npcplayback_hit_policy::WalkableKnockbackGroundDelta", playback_hit)
        self.assertIn("npcplayback_hit_policy::ConfirmHitKnockback", playback_hit)
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
        self.assertIn("npcplayback::EstimateHostHitKnockback(", claims)
        self.assertIn("npcplayback::ConfirmLocalHitKnockback(", npc_contact)
        self.assertIn("NPCHITSYNC stage=host-claim", claims)
        self.assertIn("npcplayback_hit_policy::HostHitStampExpired", npc_send)
        self.assertIn("NPCHITSYNC stage=host-ack", npc_contact)
        self.assertIn("NPCATTACK stage=local-miss", npc_contact)
        # StartLocalHitPresentation must work even when host slot 0 was inactive; Replay must abort before AdvanceHitKnockback.
        self.assertNotIn("!options.active", playback_hit)
        self.assertIn("state.localOptions = local;", playback_hit)
        self.assertIn("npcplayback_hit_policy::ShouldAcceptHitResult", npc_contact)
        self.assertLess(playback.index("if (!local && !holdEcho && !havePose) return false;"),
                        playback.index("npcplayback_hit_policy::AdvanceHitKnockback("))


if __name__ == "__main__":
    unittest.main()
