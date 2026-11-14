// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/pose.h"
#include "../src/snapshot.h"

#include <cassert>

int main()
{
    using namespace g3t::pose;
    History history;
    Bone bone;
    bone.name = 1;
    constexpr uint32_t start = 0xfffff000u;
    for (uint32_t i = 0; i < 400; ++i) {
        bone.values[9] = static_cast<int16_t>(i * 4);
        assert(history.Push(1, start + i * 16, &bone, 1));
    }
    assert(history.Size() == 400);
    std::vector<Transform> shown;
    assert(history.Sample(1, start, shown) && shown[0].values[9] == 0);
    assert(history.Sample(1, start + 16 * 10 + 8, shown));
    assert(shown[0].values[9] == 10.5f);
    assert(history.Sample(1, start + 16 * 399, shown));
    assert(shown[0].values[9] == 399 && history.Size() == HistorySize);
    // Retain a normal lookback window, but never extrapolate bones.
    assert(history.Sample(1, start + 16 * 350, shown));
    assert(!history.Sample(1, start + 16 * 400, shown));
    assert(!history.Push(1, start + 16 * 398, &bone, 1));
    assert(!history.Push(0, start + 16 * 400, &bone, 1));
    assert(history.Push(2, 100, &bone, 1));
    assert(history.Size() == 1 && !history.Sample(1, 100, shown));
    assert(history.Sample(2, 100, shown) && shown[0].values[9] == 399);

    g3t::body::Track track;
    track.predictive = false;
    History aligned;
    g3t::body::MotionState motion;
    motion.motion = 1;
    motion.motions[0].active = 1;
    motion.motions[0].name = 1;
    motion.motions[0].instance = 1;
    const float rotation[4] = {0, 0, 0, 1};
    auto feed = [&](uint32_t sent, uint32_t arrived) {
        const float x = static_cast<float>(sent - start) / 16.0f;
        const float position[3] = {x, 0, 0};
        bone.values[9] = static_cast<int16_t>(std::round(x * 4));
        g3t::body::Feed(track, position, rotation, sent, arrived, 2, motion);
        assert(aligned.Push(1, sent, &bone, 1));
    };
    auto sample = [&](uint32_t now) {
        float position[3], turn[4];
        g3t::body::Presentation presentation;
        assert(g3t::body::Sample(track, now, position, turn, -g3t::body::PlaybackDelay(track), true, &presentation));
        assert(aligned.Sample(1, presentation.sent, shown));
        assert(std::fabs(shown[0].values[9] - position[0]) < 0.01f);
    };
    for (uint32_t i = 0; i < 100; ++i) {
        feed(start + i * 16, start + i * 16 + 6000);
        sample(start + i * 16 + 6000);
    }
    uint32_t now = start + 99 * 16 + 6000;
    for (uint32_t i = 0; i < 320; ++i) { now += 16; sample(now); }
    for (uint32_t i = 100; i < 420; ++i) {
        if (i % 60 == 0) ++motion.motions[0].instance;
        feed(start + i * 16, now);
    }
    assert(aligned.Size() > HistorySize && g3t::body::PendingEvents(track) > 0);
    sample(now);
    for (uint32_t i = 420; i < 920; ++i) {
        now += 16;
        feed(start + i * 16, now);
        sample(now);
    }
    assert(track.backlogMs <= 50 && g3t::body::PendingEvents(track) == 0);
    assert(aligned.Size() == HistorySize);

    // Unsampled or stalled streams remain bounded instead of growing without limit.
    History unsampled;
    for (uint32_t i = 0; i < 1500; ++i) {
        bone.values[9] = static_cast<int16_t>(i);
        assert(unsampled.Push(1, 10000 + i * 16, &bone, 1));
        assert(unsampled.Size() <= MaxRetainedFrames);
    }
    assert(unsampled.Size() == MaxRetainedFrames);
    assert(unsampled.Sample(1, 10000 + 1499 * 16, shown));
    assert(unsampled.Size() == HistorySize);

    for (uint32_t i = 1500; i < 3000; ++i) {
        bone.values[9] = static_cast<int16_t>(i);
        assert(unsampled.Push(1, 10000 + i * 16, &bone, 1));
        assert(unsampled.Size() <= MaxRetainedFrames);
    }
    assert(unsampled.Size() == MaxRetainedFrames);
}
