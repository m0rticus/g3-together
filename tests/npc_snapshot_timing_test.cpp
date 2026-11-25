// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/snapshot.h"

#include <cassert>
#include <cmath>

static void TestDeathPelvisDiscontinuityHold()
{
    using namespace g3t::body;
    Track npc;
    npc.predictive = false;
    SetPoseInterval(npc, 16);
    const float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    MotionState alive{};
    alive.flags = 8; // fighting, alive on ground
    uint32_t sent = 2000;
    for (int i = 0; i < 6; ++i, sent += 16) {
        const float ground[3] = {91200.0f + i * 2.0f, 5141.1f, -12700.0f};
        Feed(npc, ground, rotation, sent, sent + 4, 2, alive);
    }
    const float lastGround[3] = {91210.0f, 5141.1f, -12700.0f};
    float shown[3] = {}, turn[4] = {};
    Presentation presentation{};
    // Anchor render clock at the last alive sample (sent - 16 = 2080).
    uint32_t now = (sent - 16) + 4 + 65;
    assert(Sample(npc, now, shown, turn, -65, true, &presentation));
    assert((presentation.motion.flags & 1) == 0);

    // Host ragdoll death jumps WorldPosition +90 cm from feet (5141.1) to Bip01 Pelvis (5231.1).
    MotionState deadRagdoll{};
    deadRagdoll.flags = 129; // dead | ragdoll
    const float pelvis[3] = {91211.0f, 5231.1f, -12699.0f};
    Feed(npc, pelvis, rotation, sent, sent + 4, 14, deadRagdoll);

    // Advance 8 ms (halfway between alive at 2080 and dead+ragdoll at 2096, fraction = 0.5):
    // must keep alive ground Y=5141.1 instead of floating +45 cm into the air while still alive.
    now += 8;
    assert(Sample(npc, now, shown, turn, -65, true, &presentation));
    assert((presentation.motion.flags & 1) == 0);
    assert(std::fabs(shown[0] - lastGround[0]) < 0.01f);
    assert(std::fabs(shown[1] - 5141.1f) < 0.01f);
    assert(presentation.speed == 0.0f);

    // Advance another 8 ms to reach the death snapshot (2096): death flags take effect cleanly.
    now += 8;
    assert(Sample(npc, now, shown, turn, -65, true, &presentation));
    assert((presentation.motion.flags & 129) == 129);
    assert(std::fabs(shown[1] - 5231.1f) < 0.01f);
}

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
    TestDeathPelvisDiscontinuityHold();
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

    // 10 Hz predictive track initializes cadenceWindowAt on its first interval instead of underestimating 50 ms.
    Track pred10Hz;
    Feed(pred10Hz, combatPos, rotation, 10000, 10010, 2, motion);
    Feed(pred10Hz, combatPos, rotation, 10100, 10110, 2, motion);
    assert(PlaybackDelay(pred10Hz) >= 200);

    // Sample with nullptr Presentation safely omits motion interpolation without dereferencing null.
    assert(Sample(combatNpc, combatNow + 15, shown, turn, -PlaybackDelay(combatNpc), false, nullptr));

    // Position teleport soft-resets ring/render state while preserving clock offset and counters;
    // clock stalls (> RestartAfter) drop stale offset while preserving cumulative counters.
    Track teleportTrack;
    teleportTrack.predictive = false;
    SetPoseInterval(teleportTrack, 16);
    float tpPos[3] = {100.0f, 0.0f, 0.0f};
    Feed(teleportTrack, tpPos, rotation, 1000, 1020, 2, motion);
    tpPos[0] = 102.0f;
    Feed(teleportTrack, tpPos, rotation, 1016, 1036, 2, motion);
    assert(teleportTrack.haveOffset && teleportTrack.offset == 20);
    const uint32_t gapsBefore = teleportTrack.gapCount;
    tpPos[0] = 2500.0f;
    Feed(teleportTrack, tpPos, rotation, 1016, 1036, 2, motion); // !span teleport
    assert(teleportTrack.serial == 2 && teleportTrack.count == 1 && teleportTrack.haveOffset && teleportTrack.offset == 20);
    assert(teleportTrack.gapCount == gapsBefore);
    tpPos[0] = 2502.0f;
    Feed(teleportTrack, tpPos, rotation, 3500, 3580, 2, motion); // > RestartAfter clock stall
    assert(teleportTrack.serial == 3 && teleportTrack.count == 1 && teleportTrack.offset == 80);
    assert(teleportTrack.gapCount == gapsBefore);

    // Transit excess holds escalated delayMs steady through OffsetWindow instead of pumping down between packets,
    // and slow-host idle cadence (>100 ms, <=150 ms) raises PlaybackDelay to cover the observed capture period.
    Track idleTrack;
    idleTrack.predictive = false;
    SetPoseInterval(idleTrack, 100);
    for (int i = 0; i < 25; ++i) {
        const uint32_t s = 10000 + i * 125;
        Feed(idleTrack, tpPos, rotation, s, s + 10 + (i % 3 == 0 ? 15 : 0), 1, motion);
        Sample(idleTrack, s + 10, shown, turn, -PlaybackDelay(idleTrack), true, &presentation);
    }
    const int32_t escalated = PlaybackDelay(idleTrack);
    assert(escalated >= 250);
    Sample(idleTrack, 10000 + 24 * 125 + 110, shown, turn, -PlaybackDelay(idleTrack), true, &presentation);
    assert(PlaybackDelay(idleTrack) == escalated);
}
