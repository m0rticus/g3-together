// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/movement_policy.h"
#include "../src/world_processing_policy.h"
#include <array>
#include <cassert>

void TestMovementPolicy()
{
    using namespace g3t::movement;
    assert(GapBucket(0) == 0);
    assert(GapBucket(16000) == 0);
    assert(GapBucket(16001) == 1);
    assert(GapBucket(33000) == 1);
    assert(GapBucket(33001) == 2);
    assert(GapBucket(66000) == 2);
    assert(GapBucket(66001) == 3);
    assert(GapBucket(100000) == 3);
    assert(GapBucket(100001) == 4);
    StopWatch stop;
    assert(!stop.Observe(1000000, 0, true, true)); // Never moving: not a stop.
    assert(!stop.Observe(1010000, 40, true, true));
    assert(!stop.Observe(1020000, 5, true, true));
    assert(!stop.Observe(1169999, 0, true, true));
    assert(stop.Observe(1170000, 0, true, true));
    assert(!stop.Observe(2000000, 0, true, true)); // One report per stopped episode.
    assert(!stop.Observe(2010000, 100, true, true));
    assert(!stop.Observe(2020000, 0, true, true));
    assert(!stop.Observe(2150000, 6, true, true)); // Slow continuous motion cancels the timer.
    assert(!stop.Observe(2160000, 0, true, true));
    assert(stop.Observe(2310000, 0, true, true));
    assert(!stop.Observe(2320000, 0, false, true)); // Key release disarms.
    assert(!stop.Observe(2500000, 0, true, true));
    assert(!stop.Observe(2510000, 100, true, true));
    assert(!stop.Observe(2520000, 0, true, false)); // Menu/focus/downed/loading gap disarms.
    assert(!stop.Observe(3000000, 0, true, true));
    stop.Reset();
    assert(!stop.armed && !stop.reported && stop.stillSince == 0);

    const std::array<unsigned char, 17> refresh = {
        0x56, 0x8b, 0xf1, 0xe8, 0xf8, 0xfb, 0xff, 0xff,
        0xc6, 0x86, 0xfd, 0x00, 0x00, 0x00, 0x01, 0x5e, 0xc3
    };
    std::array<unsigned char, 512> admin;
    admin.fill(0xa5);
    const auto original = admin;
    assert(!g3t::world::QueueProcessingRebuild(nullptr, refresh.data()));
    assert(!g3t::world::QueueProcessingRebuild(admin.data(), nullptr));
    for (size_t byte = 0; byte < refresh.size(); ++byte) {
        auto unsupported = refresh;
        unsupported[byte] ^= 1;
        assert(!g3t::world::QueueProcessingRebuild(admin.data(), unsupported.data()));
        assert(admin == original);
    }
    for (unsigned request = 0; request < 100; ++request) {
        assert(g3t::world::QueueProcessingRebuild(admin.data(), refresh.data()));
        for (size_t byte = 0; byte < admin.size(); ++byte)
            assert(admin[byte] == (byte == 0xfd ? 1 : original[byte]));
    }

    assert(IsContinuousPuppetMotion(""));
    assert(IsContinuousPuppetMotion("Hero_None_None_None_P0_Move_Sprint_N_Fwd_600.xmot"));
    assert(IsContinuousPuppetMotion("Hero_1H_1H_1H_P0_Move_Walk_N_Right_160.xmot"));
    assert(IsContinuousPuppetMotion("Hero_None_None_None_P0_Jump_Up_N_Fwd_00_%_00_0.xmot"));
    assert(IsContinuousPuppetMotion("Hero_None_None_None_P0_Fall_Loop_N_Fwd_00_%_00_0.xmot"));
    assert(!IsContinuousPuppetMotion("Hero_1H_1H_1H_P0_Attack_Hit_N_Left_00_%_00_P1_0_R.xmot"));
    assert(IsTrackedActionMotion("Hero_1H_1H_1H_P0_Attack_Hit_N_Left_00_%_00_P1_0_R.xmot"));
    assert(!IsTrackedActionMotion("Hero_None_None_None_P0_Jump_Up_N_Fwd_00_%_00_0.xmot"));

    assert(IsLoopingPuppetMotion("Hero_None_None_None_P0_Move_Sprint_N_Fwd_600.xmot"));
    assert(IsLoopingPuppetMotion("Hero_1H_1H_1H_P0_Parade_Loop_N_Fwd_00_%_00_0.xmot"));
    assert(IsLoopingPuppetMotion("Hero_None_None_None_P0_Jump_Loop_N_Fwd_00_%_00_0.xmot"));
    assert(!IsLoopingPuppetMotion("Hero_None_None_None_P0_Jump_Up_N_Fwd_00_%_00_0.xmot"));
    assert(!IsLoopingPuppetMotion("Hero_None_None_None_P0_Jump_End_N_Fwd_00_%_00_0.xmot"));
    assert(!IsLoopingPuppetMotion("Hero_1H_1H_1H_P0_Attack_Hit_N_Left_00_%_00_P1_0_R.xmot"));

    assert(EncodeRunningMotionFrac(1.0) == MaxRunningMotionFrac);
    assert(EncodeRunningMotionFrac(1.0) < NoMotionFrac);
    const uint16_t nearEnd = EncodeRunningMotionFrac(0.92);
    const uint16_t afterWrap = EncodeRunningMotionFrac(0.08);
    const uint16_t midWrap = InterpolateMotionFrac(nearEnd, afterWrap, 0.5f, true);
    assert(midWrap <= EncodeRunningMotionFrac(0.02) || midWrap >= EncodeRunningMotionFrac(0.98));
}
