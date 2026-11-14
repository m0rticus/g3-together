// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/movement_policy.h"
#include "../src/playerwire.h"
#include "../src/snapshot.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
uint32_t Hash(char const* text)
{
    uint32_t h = 2166136261u;
    for (unsigned char const* p = reinterpret_cast<unsigned char const*>(text); *p; ++p)
        h = (h ^ *p) * 16777619u;
    return h ? h : 1u;
}

void FeedPuppetLikeRemote(g3t::body::Track& track, g3t::movement::PuppetMotionHistory& motions,
                          g3t::net::PlayerState const& state, uint32_t arrived)
{
    const uint32_t animHash = *state.animation ? Hash(state.animation) : 0u;
    const uint32_t rightHash = *state.heldRight ? Hash(state.heldRight) : 0u;
    const uint32_t leftHash = *state.heldLeft ? Hash(state.heldLeft) : 0u;
    g3t::body::MotionState motion;
    motion.motion = g3t::movement::IsContinuousPuppetMotion(state.animation) ? 0u : animHash;
    motion.heldRight = rightHash;
    motion.heldLeft = leftHash;
    motion.aniState = state.aniState;
    motion.actionSeq = state.actionSeq;
    motion.motionFrac = state.motionFrac;
    motion.flags = state.flags;
    motion.locomotion = g3t::body::IsLocomotionMotion(state.animation);
    g3t::body::Feed(track, state.position, state.rotation, state.sent, arrived, state.movementMode, motion);

    g3t::movement::PuppetMotionSample sample;
    sample.sent = state.sent;
    sample.motion = animHash;
    sample.heldRight = rightHash;
    sample.heldLeft = leftHash;
    sample.motionFrac = state.motionFrac;
    sample.movementMode = state.movementMode;
    sample.aniState = state.aniState;
    sample.actionSeq = state.actionSeq;
    sample.looping = g3t::movement::IsLoopingPuppetMotion(state.animation);
    motions.Push(track.serial, sample);
}

void TestContinuousLocomotionAndStrafeNeverStallTimeline()
{
    using namespace g3t;
    static_assert(playerwire::FixedBytes == 65);
    body::Track track;
    movement::PuppetMotionHistory motions;
    playerwire::Encoder encoder;
    playerwire::Decoder decoder;

    char const* const standClip = "Hero_1H_1H_1H_P0_Move_Stand_0_N_Fwd_00.xmot";
    char const* const strafeRightClip = "Hero_1H_1H_1H_P0_Move_Walk_N_Right_160.xmot";
    char const* const sprintClip = "Hero_1H_1H_1H_P0_Move_Sprint_N_Fwd_600.xmot";
    char const* const jumpUpClip = "Hero_1H_1H_1H_P0_Jump_Up_N_Fwd_00_%_00_0.xmot";
    char const* const jumpLoopClip = "Hero_1H_1H_1H_P0_Jump_Loop_N_Fwd_00_%_00_0.xmot";
    char const* const jumpEndClip = "Hero_1H_1H_1H_P0_Jump_End_N_Fwd_00_%_00_0.xmot";

    uint32_t sequence = 0;
    float x = 0.0f, z = 0.0f;
    float prevSampleX = 0.0f;
    bool sawStrafe = false, sawSprint = false, sawJumpUp = false, sawJumpLoop = false, sawJumpEnd = false;

    for (int tick = 0; tick < 90; ++tick) {
        const uint32_t sent = 1000u + static_cast<uint32_t>(tick) * 33u;
        const uint32_t arrived = sent + 16u;
        net::PlayerState out{};
        out.id = 2;
        out.sequence = ++sequence;
        out.sent = sent;
        out.world = 1;
        out.rotation[3] = 1.0f; // facing +Z throughout strafe and sprint
        out.aniState = 1;
        out.actionSeq = 3;
        std::strcpy(out.heldRight, "It_1H_BroadSword_01");

        if (tick < 10) {
            out.movementMode = 1;
            std::strcpy(out.animation, standClip);
            out.motionFrac = movement::EncodeRunningMotionFrac(std::fmod(tick * 0.07, 1.0));
        } else if (tick < 35) {
            // Strafe right at 160 cm/s while facing +Z
            x += 160.0f * 0.033f;
            out.movementMode = 3;
            std::strcpy(out.animation, strafeRightClip);
            out.motionFrac = movement::EncodeRunningMotionFrac(std::fmod((tick - 10) * 0.09, 1.0));
        } else if (tick < 65) {
            // Sprint forward at 600 cm/s
            z += 600.0f * 0.033f;
            out.movementMode = 5;
            std::strcpy(out.animation, sprintClip);
            out.motionFrac = movement::EncodeRunningMotionFrac(std::fmod((tick - 35) * 0.14, 1.0));
        } else if (tick < 70) {
            z += 500.0f * 0.033f;
            out.movementMode = 5;
            std::strcpy(out.animation, jumpUpClip);
            out.motionFrac = movement::EncodeRunningMotionFrac((tick - 65) * 0.22);
        } else if (tick < 78) {
            z += 500.0f * 0.033f;
            out.movementMode = 5;
            std::strcpy(out.animation, jumpLoopClip);
            out.motionFrac = movement::EncodeRunningMotionFrac(std::fmod((tick - 70) * 0.25, 1.0));
        } else {
            z += 420.0f * 0.033f;
            out.movementMode = 5;
            std::strcpy(out.animation, jumpEndClip);
            out.motionFrac = movement::EncodeRunningMotionFrac(std::min(1.0, (tick - 78) * 0.12));
        }
        out.position[0] = x;
        out.position[2] = z;

        playerwire::Packet packet;
        assert(encoder.Plan(out, sent, false, packet) == playerwire::Status::Ok);
        assert(packet.bytes.size() >= playerwire::FixedBytes);
        encoder.Commit(out, sent, packet);

        net::PlayerState decoded{};
        assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), decoded) == playerwire::Status::Ok);
        FeedPuppetLikeRemote(track, motions, decoded, arrived);
        assert(body::PendingEvents(track) == 0);

        // Render 4 frames (120 Hz) between 30 Hz packets
        for (int sub = 0; sub < 4; ++sub) {
            const uint32_t now = arrived + static_cast<uint32_t>(sub) * 8u;
            float pos[3]{}, rot[4]{};
            body::Presentation pres{};
            const int32_t delay = body::PlaybackDelay(track);
            if (!body::Sample(track, now, pos, rot, -delay, true, &pres)) continue;
            movement::PuppetMotionSample sampled{};
            uint32_t ageMs = 0;
            assert(motions.Sample(pres.sent, sampled, ageMs));
            if (sampled.motion == Hash(strafeRightClip)) {
                sawStrafe = true;
                assert(pos[0] + 0.01f >= prevSampleX);
                prevSampleX = pos[0];
                // Facing stays +Z (rot = identity) instead of turning +X toward velocity!
                assert(std::fabs(rot[3]) > 0.99f);
            }
            if (sampled.motion == Hash(sprintClip)) sawSprint = true;
            if (sampled.motion == Hash(jumpUpClip)) sawJumpUp = true;
            if (sampled.motion == Hash(jumpLoopClip)) sawJumpLoop = true;
            if (sampled.motion == Hash(jumpEndClip)) sawJumpEnd = true;
        }
    }
    assert(sawStrafe && sawSprint && sawJumpUp && sawJumpLoop && sawJumpEnd);
}

void TestLoopWrapAndRepeatActionBoundaries()
{
    using namespace g3t::movement;
    PuppetMotionHistory history;
    const uint32_t sprint = Hash("Hero_1H_1H_1H_P0_Move_Sprint_N_Fwd_600.xmot");
    const uint32_t attack = Hash("Hero_1H_1H_1H_P0_Attack_Hit_N_Left_00_%_00_P1_0_R.xmot");

    // Loop wrap across 0.90 -> 0.10 over 40 ms
    history.Push(1, PuppetMotionSample{1000, sprint, 0, 0, EncodeRunningMotionFrac(0.90), 5, 1, 0, true});
    history.Push(1, PuppetMotionSample{1040, sprint, 0, 0, EncodeRunningMotionFrac(0.10), 5, 1, 0, true});

    PuppetMotionSample out{};
    uint32_t ageMs = 999;
    assert(history.Sample(1010, out, ageMs)); // t = 0.25 -> 0.95
    assert(ageMs == 0);
    const double f25 = static_cast<double>(out.motionFrac) / static_cast<double>(MaxRunningMotionFrac);
    assert(f25 > 0.94 && f25 < 0.96);

    assert(history.Sample(1030, out, ageMs)); // t = 0.75 -> 0.05 (wrapped smoothly forward, not 0.30 backward!)
    assert(ageMs == 0);
    const double f75 = static_cast<double>(out.motionFrac) / static_cast<double>(MaxRunningMotionFrac);
    assert(f75 > 0.04 && f75 < 0.06);

    // Non-looping same-name repeat (actionSeq 1 -> actionSeq 2) must NOT blend across the boundary
    history.Push(1, PuppetMotionSample{1080, attack, 0, 0, EncodeRunningMotionFrac(0.85), 1, 1, 1, false});
    history.Push(1, PuppetMotionSample{1120, attack, 0, 0, EncodeRunningMotionFrac(0.10), 1, 1, 2, false});

    assert(history.Sample(1100, out, ageMs));
    assert(out.motion == attack && out.actionSeq == 1);
    assert(out.motionFrac == EncodeRunningMotionFrac(0.85));
    assert(ageMs == 20);

    assert(history.Sample(1120, out, ageMs));
    assert(out.motion == attack && out.actionSeq == 2);
    assert(out.motionFrac == EncodeRunningMotionFrac(0.10));
    assert(ageMs == 0);

    // Track serial bump resets stale entries cleanly
    history.Push(2, PuppetMotionSample{5000, sprint, 0, 0, EncodeRunningMotionFrac(0.25), 5, 1, 2, true});
    assert(history.count == 1 && history.serial == 2);
}

void TestPhaseLagAndPacingMath()
{
    using namespace g3t::movement;
    // Circular loop phase: local at 0.78s, expected wrapped to 0.02s on a 0.80s stride -> +40 ms behind, NOT -760 ms!
    const double expected = ExpectedPuppetPlayTime(0.80, EncodeRunningMotionFrac(0.95), 60, 1000.0, true);
    assert(std::fabs(expected - 0.02) < 0.002);
    const float lagMs = PuppetPhaseLagMs(0.78, expected, 0.80, 1000.0, true);
    assert(std::fabs(lagMs - 40.0f) < 2.0f);
    const float speed = PuppetPaceSpeed(lagMs);
    assert(speed > 1.15f && speed <= 1.30f);

    // Within deadzone -> exact 1.0f play speed (preserves natural footstep cadence)
    assert(PuppetPaceSpeed(0.0f) == 1.0f);
    assert(PuppetPaceSpeed(6.5f) == 1.0f);
    assert(PuppetPaceSpeed(-7.5f) == 1.0f);

    // Ahead of sender -> gently slows down, bounded at >= 0.75f
    assert(PuppetPaceSpeed(-36.0f) < 1.0f && PuppetPaceSpeed(-36.0f) >= 0.75f);
    assert(PuppetPaceSpeed(-500.0f) == 0.75f);
    assert(PuppetPaceSpeed(500.0f) == 1.30f);

    // Non-looping action does NOT wrap past maxSec
    const double oneShot = ExpectedPuppetPlayTime(0.90, EncodeRunningMotionFrac(0.90), 200, 1000.0, false);
    assert(oneShot > 0.90);
}
}

int main()
{
    TestContinuousLocomotionAndStrafeNeverStallTimeline();
    TestLoopWrapAndRepeatActionBoundaries();
    TestPhaseLagAndPacingMath();
    return 0;
}
