// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <cmath>

namespace g3t::npclease {
inline bool LeaseProcessingRange(bool permit, float distance)
{
    return permit && std::isfinite(distance) && distance >= 0.0f && distance <= ClientRadiusCm;
}

// Observations about a client-owned NPC. Only the first two can end ownership (LossReason); every other field merely
// explains why native processing is suspended right now (Suspension) and never freezes, fences or cuts the NPC.
struct ClientReadiness {
    bool running = true, enabled = true, checkpointBusy = false;
    bool processed = true, actor = true, eligible = true;
    bool processingEnabled = true, processable = true, insideRange = true;

    char const* LossReason() const
    {
        if (!enabled) return "authority fallback disabled";
        if (!eligible) return "native eligibility changed";
        return nullptr;
    }
    char const* Suspension() const
    {
        if (!running) return "session/world paused";
        if (checkpointBusy) return "checkpoint in progress";
        if (!processingEnabled) return "native entity disabled";
        if (!insideRange) return "outside client processing ring";
        if (!processed) return "not in native range list";
        if (!processable) return "not in native processable list";
        if (!actor) return "native animation actor unavailable";
        return nullptr;
    }
};

// The host handshake window is telemetry for a client-owned NPC: it never expires ownership.
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
