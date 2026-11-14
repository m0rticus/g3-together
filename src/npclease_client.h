// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include "npcown_policy.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace g3t::npclease {
inline bool LeaseProcessingRange(bool permit, float distance)
{
    return permit && std::isfinite(distance) && distance >= 0.0f && distance <= ClientRadiusCm;
}

inline int FormatScanCensus(char* out, size_t capacity, npcown::OwnStats& snap, npcown::OwnStats const& totals,
                            std::unordered_map<std::string, unsigned> const& refused,
                            std::unordered_map<std::string, unsigned> const& waiting)
{
    const auto pick = [](auto const& values, char const*& label, unsigned& amount) {
        label = "-"; amount = 0;
        for (auto const& value : values)
            if (value.second > amount) { label = value.first.c_str(); amount = value.second; }
    };
    unsigned refusedCount = 0, waitCount = 0;
    char const* refusedTop; char const* waitingTop;
    pick(refused, refusedTop, refusedCount);
    pick(waiting, waitingTop, waitCount);
    snap.refusedTop = refusedTop; snap.refusedTopCount = refusedCount;
    snap.waitingTop = waitingTop; snap.waitingTopCount = waitCount;
    snap.frozenTop = waitingTop; snap.frozenTopCount = snap.frozen ? waitCount : 0;
    snap.visualOnly = snap.refused + snap.waiting;
    snap.visualTop = refusedCount >= waitCount ? refusedTop : waitingTop;
    snap.visualTopCount = (std::max)(refusedCount, waitCount);
    snap.hitsApplied = totals.hitsApplied; snap.deaths = totals.deaths; snap.handBacks = totals.handBacks;
    snap.yields = totals.yields; snap.takeovers = totals.takeovers; snap.reclaims = totals.reclaims;
    snap.registrationAlready = totals.registrationAlready; snap.registrationQueued = totals.registrationQueued;
    snap.registrationEnabled = totals.registrationEnabled; snap.registrationFailed = totals.registrationFailed;
    return npcown::FormatSummary(out, capacity, snap);
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
