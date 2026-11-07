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

constexpr uint32_t ActivityStaleMs = 5000;
constexpr uint32_t AbsenceGraceMs = 5000;

enum class Availability { Able, Down, Disconnected, Unknown, Stale, NotPlaying };

inline Availability HelperAvailability(bool connected, bool known, uint32_t arrived, uint8_t flags, uint32_t now)
{
    if (!connected) return Availability::Disconnected;
    if (!known) return Availability::Unknown;
    if (now - arrived >= ActivityStaleMs) return Availability::Stale;
    if (!(flags & 1)) return Availability::NotPlaying;
    return flags & 14 ? Availability::Down : Availability::Able;
}

inline bool Playing(Availability availability)
{
    return availability == Availability::Able || availability == Availability::Down;
}

enum class EndDecision { Wait, AllDown, AloneGrace };

inline EndDecision DecideEnd(bool helperAble, bool allOthersDown, uint32_t now, uint32_t& aloneSince)
{
    if (helperAble) {
        aloneSince = 0;
        return EndDecision::Wait;
    }
    if (allOthersDown) return EndDecision::AllDown;
    if (!aloneSince) aloneSince = now ? now : 1;
    return static_cast<int32_t>(now - aloneSince) >= static_cast<int32_t>(AbsenceGraceMs)
        ? EndDecision::AloneGrace : EndDecision::Wait;
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
