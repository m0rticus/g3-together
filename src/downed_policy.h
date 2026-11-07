// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::downed {
inline bool CombatUnavailable(bool player, bool held, bool terminal, bool nativeDown, bool nativeDead, uint8_t flags)
{
    return player && (held || terminal || nativeDown || nativeDead || (flags & 14) != 0);
}

enum TargetBinding : uint32_t {
    Current = 1, Alternative = 2, Attacker = 4, LastAttacker = 8, Spell = 16, Alignment = 32, Inflictor = 64
};

inline bool StopTargetInstruction(uint32_t bindings)
{
    return (bindings & (Current | Spell)) != 0;
}

// Native animation, HP and loaded alive snapshots are not authority to release a downed life.
struct Life
{
    bool held = false;
    bool terminal = false;
    uint32_t sequence = 0;

    void Rebind()
    {
        if (++sequence == 0) ++sequence;
    }

    void Hold()
    {
        if (terminal) return;
        if (!held) Rebind();
        held = true;
    }

    void Restore(bool waiting)
    {
        if (waiting) Hold();
    }

    bool Revive(uint32_t request)
    {
        if (terminal || !held || !request || request != sequence) return false;
        held = false;
        return true;
    }

    bool End()
    {
        if (terminal) return false;
        terminal = true;
        held = false;
        return true;
    }

    void OnWorldChange()
    {
        if (held) Rebind();
        terminal = false;
    }
};
}
