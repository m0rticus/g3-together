// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::replication {
struct Cadence60
{
    uint32_t deadline = 0, remainder = 0;
    uint32_t missed = 0;
    bool started = false;

    void Advance(uint32_t count)
    {
        const uint64_t ticks = remainder + uint64_t(count) * 1000;
        deadline += static_cast<uint32_t>(ticks / 60);
        remainder = static_cast<uint32_t>(ticks % 60);
    }

    bool Due(uint32_t now)
    {
        if (!started) { started = true; deadline = now; }
        if (static_cast<int32_t>(now - deadline) < 0) return false;
        uint32_t count = static_cast<uint32_t>(uint64_t(now - deadline) * 60 / 1000) + 1;
        Advance(count);
        if (static_cast<int32_t>(now - deadline) >= 0) { Advance(1); ++count; }
        missed += count - 1;
        return true;
    }
};

struct Sequence
{
    uint32_t latest = 0;
    bool initialized = false;

    bool Accept(uint32_t next)
    {
        if (initialized && static_cast<int32_t>(next - latest) <= 0) return false;
        latest = next;
        initialized = true;
        return true;
    }
};

struct TerminalLife
{
    uint32_t incarnation = 0;
    bool dead = false;

    bool Observe(uint32_t life, bool terminal)
    {
        if (!life || (incarnation && static_cast<int32_t>(life - incarnation) < 0)) return false;
        if (life != incarnation) { incarnation = life; dead = terminal; return true; }
        if (dead && !terminal) return false;
        dead = dead || terminal;
        return true;
    }
};

struct ReplicaOrder
{
    Sequence publication;
    uint8_t owner = 0;
    bool released = false;

    bool Snapshot(uint8_t from, uint32_t next)
    {
        if (!publication.Accept(next)) return false;
        owner = from;
        released = false;
        return true;
    }

    bool Release(uint8_t from, uint32_t next)
    {
        if (!publication.Accept(next) || owner != from || released) return false;
        released = true;
        return true;
    }
};
}
