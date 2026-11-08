// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/snapshot.h"

#include <cassert>
#include <cstring>

namespace {
using namespace g3t::body;
constexpr float Position[3] = {0, 0, 0};
constexpr float Rotation[4] = {0, 0, 0, 1};

Presentation At(Track& track, uint32_t when, bool counted = true)
{
    float position[3], rotation[4];
    Presentation result;
    assert(Sample(track, when, position, rotation, 0, counted, &result));
    return result;
}

MotionState Fighting(uint32_t opponent, uint8_t playerFlags)
{
    MotionState result;
    result.aiMode = 3;
    result.combatTarget = opponent;
    result.flags = playerFlags;
    result.hitPoints = 100;
    return result;
}

void Timeline()
{
    Track track;
    track.predictive = false;
    Feed(track, Position, Rotation, 1000, 1000, 1, Fighting(0x12345678, 4));
    Feed(track, Position, Rotation, 1100, 1100, 1, Fighting(0x87654321, 4 | (2 << 3)));
    auto shown = At(track, 1050);
    assert(shown.motion.combatTarget == 0x12345678 && shown.motion.flags == 4 && shown.motion.aiMode == 3);
    assert(shown.stateSent == 1000);
    shown = At(track, 1100, false);
    assert(shown.motion.combatTarget == 0x87654321 && PendingEvents(track) == 1);
    shown = At(track, 1100);
    assert(shown.motion.combatTarget == 0x87654321 && shown.motion.flags == (4 | (2 << 3)));
    assert(shown.stateSent == 1100);
    assert(PendingEvents(track) == 0);
    Feed(track, Position, Rotation, 1200, 1200, 1, Fighting(0, 4 | (2 << 3)));
    shown = At(track, 1150);
    assert(shown.motion.combatTarget == 0x87654321);
    shown = At(track, 1200);
    assert(shown.motion.combatTarget == 0 && shown.motion.flags == (4 | (2 << 3)));
    assert(PendingEvents(track) == 0);
}

void Order()
{
    Track track;
    track.predictive = false;
    Feed(track, Position, Rotation, 1000, 1000, 1, Fighting(0x12345678, 4));
    MotionState dead;
    dead.aiMode = 9;
    dead.flags = 1;
    Feed(track, Position, Rotation, 1100, 1100, 1, dead);
    Feed(track, Position, Rotation, 1050, 1150, 1, Fighting(0x87654321, 4));
    assert(track.count == 2);
    auto shown = At(track, 1099);
    assert(shown.motion.combatTarget == 0x12345678 && shown.motion.aiMode == 3);
    shown = At(track, 1100);
    assert(shown.motion.combatTarget == 0 && shown.motion.aiMode == 9 && shown.motion.flags == 1);
    assert(shown.motion.hitPoints == 0);
    shown = At(track, 5000);
    assert(shown.motion.combatTarget == 0 && shown.motion.aiMode == 9 && shown.motion.flags == 1);
}
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (!std::strcmp(argv[1], "timeline")) Timeline();
    else if (!std::strcmp(argv[1], "order")) Order();
    else return 2;
}
