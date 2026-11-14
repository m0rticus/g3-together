// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/snapshot.h"

#include <cassert>

static void TestArrivalGapDiagnostics()
{
    using namespace g3t::body;
    Track track;
    const float position[3] = {};
    const float rotation[4] = {0, 0, 0, 1};
    Feed(track, position, rotation, 1000, 1100);
    assert(track.gapCount == 0 && track.gapMinMs == UINT32_MAX && track.gapMaxMs == 0);
    Feed(track, position, rotation, 1016, 1116);
    Feed(track, position, rotation, 1032, 1156);
    Feed(track, position, rotation, 1032, 1300); // duplicate
    Feed(track, position, rotation, 1031, 1300); // older snapshot
    Feed(track, position, rotation, 1048, 1156);
    assert(track.gapCount == 3 && track.gapMinMs == 0 && track.gapMaxMs == 40 && track.gapSumMs == 56);

    Track wrapped;
    Feed(wrapped, position, rotation, UINT32_MAX - 10, UINT32_MAX - 5);
    Feed(wrapped, position, rotation, 4, 14);
    assert(wrapped.gapCount == 1 && wrapped.gapMinMs == 20 && wrapped.gapMaxMs == 20 && wrapped.gapSumMs == 20);
}

int main()
{
    using namespace g3t::body;
    TestArrivalGapDiagnostics();
    Track npc;
    npc.predictive = false;
    float position[3] = {};
    const float rotation[4] = {0, 0, 0, 1};
    MotionState motion;
    uint32_t now = 1000;
    for (int i = 0; i < 10; ++i, now += 16)
        Feed(npc, position, rotation, now, now + 5, 1, motion);
    const int32_t before = PlaybackDelay(npc);
    for (int i = 0; i < 5; ++i) {
        now += 1000;
        Feed(npc, position, rotation, now, now + 5, 1, motion);
    }
    assert(PlaybackDelay(npc) == before);
    assert(npc.jitterMs == 0.0f);

    for (int i = 0; i < 30; ++i) {
        now += 16;
        position[0] += 1.0f;
        Feed(npc, position, rotation, now, now + 5, 2, motion);
    }
    assert(PlaybackDelay(npc) <= before);
    float shown[3], turn[4];
    Presentation presentation;
    assert(Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation));
    assert(shown[0] > 0.0f && shown[0] < position[0]);

    for (uint32_t interval : {33u, 100u}) {
        SetPoseInterval(npc, interval);
        assert(PlaybackDelay(npc) >= static_cast<int32_t>(interval * 2));
        for (int tick = 0; tick < 50; ++tick) {
            now += interval;
            position[0] += 5;
            // One lost snapshot every ten updates; presentation remains between
            // received samples, without extrapolation or adaptive-floor collapse.
            if (tick % 10 != 5) Feed(npc, position, rotation, now, now + 5, 2, motion);
            assert(Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation));
            assert(shown[0] <= position[0]);
        }
        assert(PlaybackDelay(npc) >= static_cast<int32_t>(interval * 2));
    }
    SetPoseInterval(npc, 16);
    for (int i = 0; i < 250; ++i) {
        now += 16; position[0] += 1;
        Feed(npc, position, rotation, now, now + 5, 2, motion);
    }
    assert(PlaybackDelay(npc) == NpcBufferFloor);
    // Slower tier announced while fast snapshots still arrive: the clock
    // catches its buffer before the first sparse interval, without a hold.
    SetPoseInterval(npc, 100);
    for (int i = 0; i < 125; ++i) {
        now += 16; position[0] += 1;
        Feed(npc, position, rotation, now, now + 5, 2, motion);
        Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation);
    }
    now += 100; position[0] += 5;
    Feed(npc, position, rotation, now, now + 5, 2, motion);
    Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation);
    assert(npc.lastCase == 1);

    SetPoseInterval(npc, 33);
    for (int i = 0; i < 250; ++i) {
        now += 33; position[0] += 1;
        Feed(npc, position, rotation, now, now + 5, 2, motion);
        Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation);
    }
    assert(PlaybackDelay(npc) == 66);
    SetPoseInterval(npc, 100);
    for (int i = 0; i < 61; ++i) {
        now += 33; position[0] += 1;
        Feed(npc, position, rotation, now, now + 5, 2, motion);
        Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation);
    }
    now += 100; position[0] += 1;
    Feed(npc, position, rotation, now, now + 5, 2, motion);
    Sample(npc, now + 5, shown, turn, -PlaybackDelay(npc), true, &presentation);
    assert(npc.lastCase == 1); // ordinary-to-idle hold covers the new sparse interval

    Track slowHost;
    slowHost.predictive = false;
    SetPoseInterval(slowHost, 33);
    for (int i = 0; i < 60; ++i) {
        const uint32_t sent = 10000 + i * 50; // native frame rate cannot meet the 33 ms target
        position[0] += 1;
        Feed(slowHost, position, rotation, sent, sent + 5, 2, motion);
        Sample(slowHost, sent + 5, shown, turn, -PlaybackDelay(slowHost), true, &presentation);
    }
    assert(PlaybackDelay(slowHost) >= 100); // the hint is a floor, not a replacement for observed cadence

    Track player;
    now = 1000;
    Feed(player, position, rotation, now, now, 1, motion);
    for (int i = 0; i < 4; ++i) {
        now += 1000;
        Feed(player, position, rotation, now, now, 1, motion);
    }
    assert(PlaybackDelay(player) > BufferDelay);

    // Combat 16 ms NPC pausing stationary for KeepaliveMs (180 ms) emits QuietCadenceMs (250 ms)
    // keyframes; those heartbeats must not escalate combat playback delay to 200 ms or trigger underruns.
    Track combatNpc;
    combatNpc.predictive = false;
    SetPoseInterval(combatNpc, 16);
    float combatPos[3] = {500.0f, 0.0f, 500.0f};
    uint32_t combatNow = 5000;
    for (int i = 0; i < 40; ++i) {
        combatNow += 16;
        combatPos[0] += 1.5f;
        Feed(combatNpc, combatPos, rotation, combatNow, combatNow + 15, 2, motion);
        Sample(combatNpc, combatNow + 15, shown, turn, -PlaybackDelay(combatNpc), true, &presentation);
    }
    assert(PlaybackDelay(combatNpc) == NpcBufferFloor);
    const unsigned underrunsBeforeHold = combatNpc.underruns;
    // Stationary 5.4 s pause across multiple OffsetWindows emitting QuietCadenceMs (250 ms) heartbeats:
    combatNow += 16;
    Feed(combatNpc, combatPos, rotation, combatNow, combatNow + 15, 1, motion);
    for (int holdBeat = 0; holdBeat < 30; ++holdBeat) {
        for (uint32_t wait = 16; wait < 180; wait += 16)
            Sample(combatNpc, combatNow + 15 + wait, shown, turn, -PlaybackDelay(combatNpc), true, &presentation);
        combatNow += 180;
        SetPoseInterval(combatNpc, 250);
        Feed(combatNpc, combatPos, rotation, combatNow, combatNow + 15, 1, motion);
        Sample(combatNpc, combatNow + 15, shown, turn, -PlaybackDelay(combatNpc), true, &presentation);
    }
    assert(PlaybackDelay(combatNpc) == NpcBufferFloor);
    assert(combatNpc.underruns == underrunsBeforeHold);
    // Waking 16 ms combat packet 170 ms after last heartbeat keeps crisp 34 ms buffer immediately:
    combatNow += 170;
    combatPos[0] += 2.0f;
    SetPoseInterval(combatNpc, 16);
    Feed(combatNpc, combatPos, rotation, combatNow, combatNow + 15, 2, motion);
    Sample(combatNpc, combatNow + 15, shown, turn, -PlaybackDelay(combatNpc), true, &presentation);
    assert(PlaybackDelay(combatNpc) == NpcBufferFloor);
    combatNow += 16;
    combatPos[0] += 2.0f;
    Feed(combatNpc, combatPos, rotation, combatNow, combatNow + 15, 2, motion);
    Sample(combatNpc, combatNow + 15, shown, turn, -PlaybackDelay(combatNpc), true, &presentation);
    assert(PlaybackDelay(combatNpc) == NpcBufferFloor);
}
