// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace g3t::movement {
inline unsigned GapBucket(uint64_t us)
{
    return us <= 16000 ? 0 : us <= 33000 ? 1 : us <= 66000 ? 2 : us <= 100000 ? 3 : 4;
}

// A candidate, not a verdict: game collisions and native tasks can stop held-key movement.
struct StopWatch {
    bool armed = false, reported = false;
    uint64_t stillSince = 0;
    void Reset() { *this = {}; }
    bool Observe(uint64_t now, float speed, bool held, bool eligible)
    {
        if (!held || !eligible) { Reset(); return false; }
        if (speed >= 40.0f) { armed = true; reported = false; stillSince = 0; return false; }
        if (!armed || speed > 5.0f) { stillSince = 0; return false; }
        if (!stillSince) { stillSince = now; return false; }
        if (reported || now - stillSince < 150000) return false;
        reported = true;
        return true;
    }
};
}
