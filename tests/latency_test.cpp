// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/latency.h"
#include "../src/snapshot.h"
#include "../src/npcown_policy.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace g3t;

void Feed(body::Track& track, uint32_t sent, uint32_t arrived, float x = 0.0f,
          body::MotionState const& motion = {})
{
    const float position[3] = {x, 0, 0}, rotation[4] = {0, 0, 0, 1};
    body::Feed(track, position, rotation, sent, arrived, 2, motion);
}

void Sample(body::Track& track, uint32_t now, float* x = nullptr, body::Presentation* presentation = nullptr)
{
    float position[3], rotation[4];
    assert(body::Sample(track, now, position, rotation, -body::PlaybackDelay(track), true, presentation));
    if (x) *x = position[0];
    for (float value : position) assert(std::isfinite(value));
    for (float value : rotation) assert(std::isfinite(value));
}

void Warm(body::Track& track, uint32_t start = 1000, unsigned count = 300)
{
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t sent = start + i * 1000 / 60;
        Feed(track, sent, sent + 6000, static_cast<float>(i));
        Sample(track, sent + 6000);
    }
}

    void TestTransitAndRecovery()
    {
        for (uint32_t transit : {15u, 30u}) for (uint32_t stallFrames : {125u, 320u}) {
            body::Track track;
            track.predictive = false;
            const uint32_t offset = 6000 + transit;
            for (uint32_t i = 0; i < 300; ++i) {
                const uint32_t sent = 1000 + i * 16;
                Feed(track, sent, sent + offset, static_cast<float>(i));
                Sample(track, sent + offset);
            }
            assert(track.offset == static_cast<int32_t>(offset));
            assert(track.transitExcessMs == 0 && body::PlaybackDelay(track) == body::NpcBufferFloor);
            assert(track.underruns == 0);

            const uint32_t lastSent = track.snaps[track.count - 1].sent;
            uint32_t now = track.lastArrived;
            float previousX = 0;
            Sample(track, now, &previousX);
            for (uint32_t i = 1; i <= stallFrames; ++i) {
                now += 16;
                const uint32_t previous = track.renderSent;
                float x;
                Sample(track, now, &x);
                assert(x >= previousX && x <= 299.0f && static_cast<int32_t>(track.renderSent - previous) >= 0);
                assert(track.backlogMs == 0); // Undelivered data is not a retained presentation backlog.
                previousX = x;
            }
            for (uint32_t i = 1; i <= stallFrames; ++i)
                Feed(track, lastSent + i * 16, now, 299.0f + i);
            assert(track.count == body::SnapshotCount && track.offset == static_cast<int32_t>(offset));
            assert(body::PendingEvents(track) == 0 && body::PlaybackDelay(track) == body::MaximumBufferDelay);
            assert(static_cast<int32_t>(body::OldestRetainedSent(track) - track.snaps[0].sent) < 0);
            Sample(track, now, &previousX);
            bool converged = false;
            uint32_t recoveredAt = 0;
            for (uint32_t i = 1; i <= 500; ++i) {
                now += 16;
                Feed(track, lastSent + (stallFrames + i) * 16, now, 299.0f + stallFrames + i);
                const uint32_t previous = track.renderSent;
                float x;
                Sample(track, now, &x);
                assert(static_cast<int32_t>(track.renderSent - previous) >= 0);
                assert(x >= previousX - 0.01f && x - previousX < 4.0f);
                previousX = x;
                if (!converged && track.backlogMs <= 50) { converged = true; recoveredAt = i; }
            }
            assert(converged && track.backlogMs <= 50 && track.catchUpFrames > 0);
            assert(recoveredAt <= stallFrames + 125); // two seconds recovers within four; five within eight
        }

        // A long, consistently queued path must not become the clock reference.
        for (uint32_t backlog : {2000u, 5000u}) {
            body::Track queued;
            queued.predictive = false;
            Feed(queued, 1000, 7000);
            for (uint32_t i = 1; i <= 400; ++i)
                Feed(queued, 1000 + i * 16, 7000 + i * 16 + backlog);
            assert(queued.offset == 6000 && queued.transitExcessMs == static_cast<int32_t>(backlog));
            assert(body::PlaybackDelay(queued) == body::MaximumBufferDelay);
            const uint32_t stateAge = queued.lastArrived - static_cast<uint32_t>(queued.offset) -
                queued.snaps[queued.count - 1].sent;
            assert(stateAge == backlog && !npcown::AcquisitionTimely(stateAge, 30));
        }

        body::Track wrapped;
        wrapped.predictive = false;
        Feed(wrapped, 0xfffffff0u, 0x7fffffe0u);
        Feed(wrapped, 0, 0x80000010u);
        assert(wrapped.offset == static_cast<int32_t>(0x7ffffff0u));
        assert(wrapped.transitExcessMs == 32);

        body::Track negativeControl;
        body::offsetWindow = false;
        Feed(negativeControl, 1000, 7000);
        Feed(negativeControl, 1016, 7016);
        assert(negativeControl.offset == 6001);
        body::offsetWindow = true;
    }

    void TestJitterAndTimedDecay()
    {
        body::Track npc;
        npc.predictive = false;
        Warm(npc);
        uint32_t previousArrival = npc.lastArrived;
        const uint32_t firstSent = npc.snaps[npc.count - 1].sent;
        bool sawExcess = false;
        for (uint32_t i = 1; i <= 450; ++i) {
            const uint32_t sent = firstSent + i * 16;
            const uint32_t extra = i % 120 == 1 ? 300 : (i % 40 == 1 ? 100 : 0);
            const uint32_t arrived = std::max(previousArrival, sent + 6000 + extra);
            Feed(npc, sent, arrived, 299.0f + i);
            const uint32_t previous = npc.renderSent;
            Sample(npc, arrived);
            assert(static_cast<int32_t>(npc.renderSent - previous) >= 0);
            assert(body::PlaybackDelay(npc) <= body::MaximumBufferDelay);
            sawExcess = sawExcess || npc.transitExcessMs >= 300;
            previousArrival = arrived;
        }
        assert(sawExcess && npc.offset == 6000 && body::PlaybackDelay(npc) > 100);
        const int32_t raised = body::PlaybackDelay(npc);
        const uint32_t lastSent = npc.snaps[npc.count - 1].sent;
        for (uint32_t i = 1; i <= 600; ++i) {
            const uint32_t sent = lastSent + i * 16;
            Feed(npc, sent, sent + 6000, 749.0f + i);
            Sample(npc, sent + 6000);
        }
        assert(body::PlaybackDelay(npc) < raised && body::PlaybackDelay(npc) == body::NpcBufferFloor);

        // Equal elapsed time, not the number of Feed calls, governs recovery.
        body::Track frequent, sparse;
        frequent.predictive = sparse.predictive = false;
        body::MotionState dead;
        dead.flags = 1;
        Feed(frequent, 1000, 7000, 0, dead);
        Feed(sparse, 1000, 7000, 0, dead);
        Feed(frequent, 1016, 7316, 0, dead);
        Feed(sparse, 1016, 7316, 0, dead);
        for (uint32_t now = 7332; now <= 11316; now += 16) Sample(frequent, now);
        Sample(sparse, 11316);
        assert(body::PlaybackDelay(frequent) == body::PlaybackDelay(sparse));
    }

    void TestEventBurst()
    {
        body::Track track;
        track.predictive = false;
        const uint32_t start = 0xfffffc00u, offset = 6000;
        body::MotionState motion;
        Feed(track, start, start + offset, 0, motion);
        Sample(track, start + offset);
        std::vector<uint32_t> edges;
        for (uint32_t i = 1; i <= 90; ++i) {
            motion.motions[0].active = 1;
            motion.motions[0].name = 99;
            motion.motions[0].instance = (i + 1) / 2;
            motion.motions[0].phase = i % 2 ? 0.1f : 0.2f;
            motion.hitSequence = i >= 20 ? 1 : 0;
            motion.contact = i >= 40 && i < 60 ? 1 : 0;
            motion.flags = i >= 70 ? 1 : 0;
            const uint32_t sent = start + i * 16;
            Feed(track, sent, start + offset + 3200, static_cast<float>(i), motion);
            if (i % 2 == 1 || i == 20 || i == 40 || i == 60 || i == 70) edges.push_back(sent);
        }
        motion.motions[0].active = 0;
        Feed(track, start + 91 * 16, start + offset + 3200, 91, motion);
        edges.push_back(start + 91 * 16);
        assert(track.count == body::SnapshotCount && body::PendingEvents(track) == edges.size());
        assert(body::OldestRetainedSent(track) == start);
        uint32_t now = start + offset + 3200;
        size_t shown = 0;
        for (unsigned frame = 0; frame < 300 && shown < edges.size(); ++frame, now += 16) {
            const uint32_t previous = track.renderSent;
            const size_t before = body::PendingEvents(track);
            body::Presentation presentation;
            Sample(track, now, nullptr, &presentation);
            assert(static_cast<int32_t>(track.renderSent - previous) >= 0);
            if (body::PendingEvents(track) != before) {
                assert(body::PendingEvents(track) + 1 == before);
                assert(presentation.stateSent == edges[shown]);
                if (shown + 1 < edges.size()) assert(presentation.motion.motions[0].active);
                ++shown;
            }
        }
        assert(shown == edges.size() && body::PendingEvents(track) == 0);

        body::Track player;
        body::MotionState action;
        action.flags = 1;
        action.motion = 42;
        action.actionSeq = 250;
        Feed(player, start, start + offset + 3000, 0, action);
        std::vector<uint32_t> playerEdges{start};
        for (uint32_t i = 1; i <= 40; ++i) {
            action.actionSeq = static_cast<uint8_t>(250 + i);
            action.motionFrac = static_cast<int>(i);
            Feed(player, start + i * 16, start + offset + 3000, static_cast<float>(i), action);
            playerEdges.push_back(start + i * 16);
        }
        action.motion = 43;
        action.locomotion = true;
        Feed(player, start + 41 * 16, start + offset + 3000, 41, action);
        playerEdges.push_back(start + 41 * 16);
        assert(player.count == body::SnapshotCount && body::PendingEvents(player) == playerEdges.size() - 1);
        now = start + offset + 3000;
        for (uint32_t expected : playerEdges) {
            body::Presentation presentation;
            Sample(player, now, nullptr, &presentation);
            assert(presentation.stateSent == expected);
            now += 16;
        }
        assert(body::PendingEvents(player) == 0);

        body::Track fractions;
        Feed(fractions, 1000, 7000, 0, action);
        action.motionFrac++;
        Feed(fractions, 1016, 7016, 1, action);
        assert(body::PendingEvents(fractions) == 0);

        body::Track duplicates;
        body::MotionState first;
        first.actionSeq = 10;
        Feed(duplicates, 1000, 7000, 0, first);
        first.actionSeq = 11;
        Feed(duplicates, 1000, 7000, 0, first);
        body::Presentation duplicateShown;
        Sample(duplicates, 7200, nullptr, &duplicateShown);
        assert(duplicateShown.motion.actionSeq == 10);
        Sample(duplicates, 7216, nullptr, &duplicateShown);
        assert(duplicateShown.motion.actionSeq == 11 && body::PendingEvents(duplicates) == 0);
    }

    void TestPrediction()
    {
        body::MotionState moving;
        moving.flags = 1;
        moving.locomotion = true;
        moving.motion = 1;
        body::Track player;
        Feed(player, 1000, 7000, 0, moving);
        Feed(player, 1016, 7016, 250, moving); // plausible position jump, excessive measured speed
        player.delayMs = body::PlayerBufferFloor;
        float x;
        body::Presentation shown;
        Sample(player, 7166, &x, &shown);
        assert(std::fabs(x - 550.0f) < 0.01f);
        assert(shown.sent == 1116 && player.underruns == 0);
        Sample(player, 7266, &x, &shown);
        assert(std::fabs(x - 550.0f) < 0.01f && player.underruns == 1);
        Sample(player, 7270, &x, &shown);
        assert(player.underruns == 1);
        const float previouslyPredicted = x;
        Feed(player, 1048, 7282, 282, moving);
        Sample(player, 7282, &x, &shown);
        assert(x < previouslyPredicted && x >= previouslyPredicted - 36.01f);
        assert(player.playerReconciling);
        for (uint32_t i = 1; i <= 90; ++i) {
            const float previous = x;
            Feed(player, 1048 + i * 16, 7282 + i * 16, 282.0f + i * 16, moving);
            Sample(player, 7282 + i * 16, &x, &shown);
            assert(std::fabs(x - previous) <= 48.01f);
        }
        assert(!player.playerReconciling);

        for (int kind = 0; kind < 4; ++kind) {
            body::Track terminal;
            Feed(terminal, 1000, 7000, 0, moving);
            body::MotionState next = moving;
            int mode = 2;
            if (kind == 0) mode = 1;
            if (kind == 1) next.flags = 3; // player knocked out; bit 0 means present, not NPC death
            if (kind == 2) next.flags = 5;
            if (kind == 3) { next.motion = 2; next.locomotion = false; }
            const float pos[3] = {10, 0, 0}, rot[4] = {0, 0, 0, 1};
            body::Feed(terminal, pos, rot, 1016, 7016, mode, next);
            for (uint32_t now : {7166u, 7182u}) Sample(terminal, now, &x, &shown);
            assert(x == 10 && shown.sent == 1016 && shown.speed == 0);
        }
    }

    void TestProtectedBacklog()
    {
        body::Track track;
        track.predictive = false;
        track.combatPlayer = 2;
        uint32_t sent = 1000, now = 7000;
        for (int i = 0; i < 20; ++i, sent += 16, now += 16) {
            Feed(track, sent, now);
            Sample(track, now);
        }
        sent -= 16;
        now -= 16;
        for (int i = 0; i < 200; ++i) {
            now += 16;
            Sample(track, now);
        }
        body::MotionState attack;
        attack.flags = 20;
        attack.motions[0].active = 1;
        attack.motions[0].name = 17;
        attack.motions[0].instance = 1;
        for (int i = 0; i < 200; ++i) {
            sent += 16;
            attack.motions[0].phase = static_cast<float>(i) / 200.0f;
            Feed(track, sent, now, 0, attack);
        }
        assert(body::PendingEvents(track) == 1);
        for (int i = 0; i < 300; ++i) {
            const uint32_t previous = track.renderSent;
            Sample(track, now);
            assert(track.combatClockProtected && !track.catchUp);
            assert(static_cast<int32_t>(track.renderSent - previous) >= 0);
            assert(static_cast<int32_t>(track.renderSent - previous) <= (i ? 16 : 0));
            assert(static_cast<int32_t>(track.renderSent - sent) <= 0);
            now += 16;
        }
        assert(track.renderSent == sent);
    }
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (!std::strcmp(argv[1], "floor")) {
        body::Track npc;
        npc.predictive = false;
        Feed(npc, 1000, 7000);
        assert(body::PlaybackDelay(npc) == 34);
        Warm(npc, 1017);
        assert(body::PlaybackDelay(npc) == 34);
        assert(npc.underruns == 0);
        body::Track player;
        assert(body::PlaybackDelay(player) == 100);
        Warm(player);
        assert(body::PlaybackDelay(player) == 50);
    } else if (!std::strcmp(argv[1], "cadence")) {
        for (const uint32_t interval : {33u, 50u, 100u}) {
            body::Track npc;
            npc.predictive = false;
            for (unsigned i = 0; i < 300; ++i) {
                const uint32_t sent = 1000 + i * interval;
                Feed(npc, sent, sent + 6000, static_cast<float>(i));
                Sample(npc, sent + 6000);
            }
            assert(body::PlaybackDelay(npc) == static_cast<int32_t>(2 * interval));
        }
    } else if (!std::strcmp(argv[1], "jitter")) {
        body::Track npc;
        npc.predictive = false;
        Warm(npc);
        for (unsigned i = 0; i < 300; ++i) {
            const uint32_t sent = 6000 + i * 1000 / 60;
            const uint32_t arrived = sent + 6000 + (i % 2 ? 8 : 0);
            Feed(npc, sent, arrived, static_cast<float>(300 + i));
            const uint32_t previous = npc.renderSent;
            Sample(npc, arrived);
            assert(static_cast<int32_t>(npc.renderSent - previous) >= 0);
        }
        assert(body::PlaybackDelay(npc) > 34 && body::PlaybackDelay(npc) <= 250);
        Warm(npc, 11000, 600);
        assert(body::PlaybackDelay(npc) == 34);
    } else if (!std::strcmp(argv[1], "loss")) {
        body::Track npc;
        npc.predictive = false;
        Warm(npc);
        const uint32_t lastSent = npc.snaps[npc.count - 1].sent;
        const uint32_t arrived = npc.lastArrived;
        float x = 0;
        Sample(npc, arrived + 33, &x);
        assert(npc.underruns == 0 && body::PlaybackDelay(npc) == 34);
        Sample(npc, arrived + 120, &x);
        const float held = npc.snaps[npc.count - 1].position[0];
        assert(x == held && npc.underruns == 1 && body::PlaybackDelay(npc) > 34);
        const int32_t raised = body::PlaybackDelay(npc);
        Sample(npc, arrived + 121, &x);
        assert(x == held && npc.underruns == 1 && body::PlaybackDelay(npc) == raised);
        for (uint32_t gap = 220; gap < 3000; gap += 100) {
            Sample(npc, arrived + gap, &x);
            assert(x == held && body::PlaybackDelay(npc) <= body::MaximumBufferDelay);
        }
        Feed(npc, lastSent + 3000, arrived + 3000, 400);
        assert(npc.serial == 2 && npc.count == 1);
        assert(!npc.predictive && body::PlaybackDelay(npc) == 34);
    } else if (!std::strcmp(argv[1], "timeline")) {
        body::Track npc;
        npc.predictive = false;
        body::MotionState alive, dead;
        alive.hitPoints = 100;
        dead.flags = 1;
        dead.hitPoints = 0;
        Feed(npc, 1000, 4000, 0, alive);
        Feed(npc, 1050, 4050, 50, dead);
        body::Presentation shown;
        float x = 0;
        Sample(npc, 4050, &x, &shown);
        assert(shown.motion.flags == 0 && shown.motion.hitPoints == 100);
        assert(x >= 0 && x < 50);
        Sample(npc, 4200, &x, &shown);
        assert(x == 50 && shown.motion.flags == 1 && shown.motion.hitPoints == 0);
        assert(npc.underruns == 0);
        body::Track wrap;
        wrap.predictive = false;
        Warm(wrap, 0xfffffe00u, 600);
        assert(wrap.count == body::SnapshotCount && wrap.serial == 1);
        assert(body::PlaybackDelay(wrap) == 34 && wrap.underruns == 0);
        const uint32_t serial = wrap.serial;
        Feed(wrap, wrap.snaps[wrap.count - 1].sent + 16, wrap.lastArrived + 16, 100000);
        assert(wrap.serial == serial + 1 && wrap.count == 1 && body::PlaybackDelay(wrap) == 34);
    } else if (!std::strcmp(argv[1], "transit")) {
        TestTransitAndRecovery();
    } else if (!std::strcmp(argv[1], "moderate-jitter")) {
        TestJitterAndTimedDecay();
    } else if (!std::strcmp(argv[1], "events")) {
        TestEventBurst();
    } else if (!std::strcmp(argv[1], "prediction")) {
        TestPrediction();
    } else if (!std::strcmp(argv[1], "protected-backlog")) {
        TestProtectedBacklog();
    } else if (!std::strcmp(argv[1], "ping")) {
        latency::Ping ping;
        assert(ping.Value(100) == -1);
        assert(ping.CanSend(100.0));
        ping.Sent(100.0);
        assert(!ping.CanSend(101.0) && ping.CanSend(105.0));
        assert(ping.Reply(100.0, 100.0026, 1000));
        assert(ping.Value(1000) == 3 && ping.Value(5999) == 3 && ping.Value(6000) == -1);
        assert(!ping.Reply(100.0, 100.003, 1001));
        ping.Sent(101.0);
        assert(!ping.Reply(100.0, 101.001, 1100));
        assert(!ping.Reply(std::numeric_limits<double>::quiet_NaN(), 101.001, 1100));
        assert(!ping.Reply(101.0, std::numeric_limits<double>::infinity(), 1100));
        assert(!ping.Reply(101.0, 100.0, 1100));
        assert(!ping.Reply(101.0, 106.0, 1100));
        assert(ping.Reply(101.0, 101.0004, 0xfffffff0u));
        assert(ping.Value(15) == 0 && ping.Value(0xfffffff0u + 5000u) == -1);
        ping.Sent(102.0);
        assert(ping.Reply(102.0, 102.1094, 1200) && ping.Value(1200) == 109);
        ping.Sent(103.0);
        assert(!ping.CanSend(104.0));
        assert(ping.Reply(103.0, 104.5, 2700) && ping.Value(2700) == 1500);
        latency::Samples samples;
        assert(samples.Average() == 0 && samples.maximum == 0);
        samples.Add(1); samples.Add(3); samples.Add(20);
        assert(samples.Average() == 8 && samples.maximum == 20 && samples.count == 3);
    } else {
        assert(false);
    }
}
