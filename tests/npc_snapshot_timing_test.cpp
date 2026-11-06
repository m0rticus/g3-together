// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/snapshot.h"

#include <cassert>

int main()
{
    using namespace g3t::body;
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

    Track player;
    now = 1000;
    Feed(player, position, rotation, now, now, 1, motion);
    for (int i = 0; i < 4; ++i) {
        now += 1000;
        Feed(player, position, rotation, now, now, 1, motion);
    }
    assert(PlaybackDelay(player) > BufferDelay);
}
