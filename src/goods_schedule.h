// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::trade {
// A successful publication commits the observed join/entry counters; a failed one stays due.
struct GoodsSchedule {
    static constexpr uint32_t KeyframeMs = 30000;
    uint32_t lastSent = 0, joins = 0, entries = 0;
    bool published = false;

    bool Due(uint32_t now, bool changed, uint32_t currentJoins, uint32_t currentEntries) const
    {
        return !published || changed || currentJoins != joins || currentEntries != entries ||
               now - lastSent >= KeyframeMs;
    }

    void Published(uint32_t now, uint32_t currentJoins, uint32_t currentEntries)
    {
        lastSent = now;
        joins = currentJoins;
        entries = currentEntries;
        published = true;
    }
};
}
