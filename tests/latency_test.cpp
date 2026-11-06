// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/latency.h"
#include "../src/snapshot.h"

#include <cassert>
#include <cstring>
#include <limits>

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
            assert(x == held && body::PlaybackDelay(npc) <= 250);
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
