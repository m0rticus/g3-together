// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"

namespace g3t::npclease {
inline bool PortableCreatureSlot(bool occupied, bool hand, bool natural, bool templateDefault)
{
    return !occupied || (hand && natural && templateDefault);
}

struct ClientReadiness {
    bool running = true, enabled = true, checkpointBusy = false;
    bool processed = true, actor = true, eligible = true, expired = false;

    char const* LossReason() const
    {
        if (!running) return "session/world unavailable";
        if (!enabled) return "authority fallback disabled";
        if (checkpointBusy) return "checkpoint barrier";
        if (!processed) return "native processing unavailable";
        if (!actor) return "native animation actor unavailable";
        if (!eligible) return "native eligibility changed";
        if (expired) return "conservative lease expired";
        return nullptr;
    }
};

struct ClientWindow {
    enum class Mode { Idle, Prepared, Active, Frozen };
    Mode mode = Mode::Idle;
    uint32_t epoch = 0, deadline = 0;

    bool Prepare(uint32_t next, uint32_t echo, uint32_t duration, uint32_t now)
    {
        if (!next || !duration || duration > LeaseMs || duration <= LeaseSafetyMs ||
            Reached(now + LeaseSafetyMs, echo + duration)) return false;
        if (epoch && !Newer(next, epoch)) return next == epoch && mode == Mode::Prepared && !Reached(now, deadline);
        epoch = next;
        deadline = echo + duration - LeaseSafetyMs;
        mode = Mode::Prepared;
        return true;
    }
    bool Commit(uint32_t authority, uint32_t now)
    {
        if (authority != epoch || Reached(now, deadline) ||
            (mode != Mode::Prepared && mode != Mode::Active)) return false;
        mode = Mode::Active;
        return true;
    }
    bool Renew(uint32_t authority, uint32_t echo, uint32_t duration, uint32_t now)
    {
        if (!Active(authority, now) || !duration || duration > LeaseMs || duration <= LeaseSafetyMs ||
            Reached(now + LeaseSafetyMs, echo + duration)) return false;
        const uint32_t next = echo + duration - LeaseSafetyMs;
        if (!Newer(next, deadline)) return false;
        deadline = next;
        return true;
    }
    bool Active(uint32_t authority, uint32_t now) const
    {
        return mode == Mode::Active && authority == epoch && !Reached(now, deadline);
    }
    void Freeze() { mode = Mode::Frozen; }
    void Reset(uint32_t authority) { epoch = authority; deadline = 0; mode = Mode::Idle; }
};
}
