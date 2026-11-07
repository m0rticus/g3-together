// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/movement_policy.h"
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
}
