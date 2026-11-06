// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::downed {
// Native animation, HP and loaded alive snapshots are not authority to release a downed life.
struct Life
{
    bool held = false;
    uint32_t sequence = 0;

    void Rebind()
    {
        if (++sequence == 0) ++sequence;
    }

    void Hold()
    {
        if (!held) Rebind();
        held = true;
    }

    void Restore(bool waiting)
    {
        if (waiting) Hold();
    }

    bool Revive(uint32_t request)
    {
        if (!held || !request || request != sequence) return false;
        held = false;
        return true;
    }
};
}
