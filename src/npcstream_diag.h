// SPDX-License-Identifier: GPL-3.0-or-later
// The host's `npc-stream:` line, one per remote player every 5 s: what the host considered for that player, which rule kept
// the rest from being streamed, and what the host's engine holds around that player. Local measurements only: nothing here
// is sent, and nothing here limits, delays or drops any NPC traffic.
#pragma once

#include "host_frame_diag.h"
#include "hostsim_status.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace g3t::npcstream_diag {
// Candidate rules count once per candidate and tick, recipient rules once per recipient, candidate and tick.
enum class Drop : uint8_t {
    Unresolved, Hero, Removed, Lease, Pending, Mirror, GivenBack, Range,
    Away, Outside, Unchanged, Paced, Invalid, Failed,
    Count
};
constexpr unsigned DropCount = static_cast<unsigned>(Drop::Count);
constexpr unsigned MaxRecipients = 8;
constexpr uint32_t PeriodMs = 5000;

inline char const* Name(Drop drop)
{
    static char const* const names[DropCount] = {"unresolved", "hero", "removed", "lease", "pending", "mirror", "givenback",
                                                 "range", "away", "outside", "unchanged", "paced", "invalid", "failed"};
    const unsigned index = static_cast<unsigned>(drop);
    return index < DropCount ? names[index] : "?";
}

struct Context {
    bool hostValid = false;
    float hostAt[3] = {};
    char const* mode = "spheres";
    float roiCm = 0.0f;
    world::HostSimStatus sim;
    host_frame_diag::Summary perf{};
};

class Window {
    struct Recipient {
        bool connected = false, inWorld = false, positionValid = false;
        uint32_t stateAgeMs = 0;
        float at[3] = {};
        unsigned inRoi = 0, processedInRoi = 0, interest = 0;  // last tick
        float nearest = -1.0f;
        unsigned sent = 0, released = 0;                      // window
        unsigned drops[DropCount] = {};
    };
    bool started = false;
    uint32_t startedAt = 0, ticks = 0;
    unsigned interaction = 0, processedNpc = 0, candidates = 0, processedTotal = 0;
    unsigned shared[DropCount] = {};
    Recipient recipients[MaxRecipients];

    static double PerSecond(unsigned count, uint32_t windowMs) { return windowMs ? count * 1000.0 / windowMs : 0.0; }

  public:
    void BeginTick(uint32_t now)
    {
        if (!started) { started = true; startedAt = now; }
        ++ticks;
        interaction = processedNpc = candidates = processedTotal = 0;
        for (Recipient& r : recipients) {
            r.connected = r.inWorld = r.positionValid = false;
            r.inRoi = r.processedInRoi = r.interest = 0;
            r.nearest = -1.0f;
        }
    }
    void Candidates(unsigned fromInteraction, unsigned fromProcessing, unsigned unique, unsigned processingList)
    {
        interaction = fromInteraction; processedNpc = fromProcessing; candidates = unique; processedTotal = processingList;
    }
    void Candidate(Drop drop) { if (static_cast<unsigned>(drop) < DropCount) ++shared[static_cast<unsigned>(drop)]; }
    void Player(uint8_t id, bool inWorld, bool positionValid, uint32_t stateAgeMs, float const at[3])
    {
        if (id >= MaxRecipients) return;
        Recipient& r = recipients[id];
        r.connected = true; r.inWorld = inWorld; r.positionValid = positionValid; r.stateAgeMs = stateAgeMs;
        if (positionValid) std::memcpy(r.at, at, sizeof(r.at));
    }
    void Reach(uint8_t id, float distanceCm, bool fromProcessing, float roiCm, float interestCm)
    {
        if (id >= MaxRecipients || !recipients[id].positionValid) return;
        Recipient& r = recipients[id];
        if (r.nearest < 0.0f || distanceCm < r.nearest) r.nearest = distanceCm;
        if (distanceCm <= roiCm) { ++r.inRoi; if (fromProcessing) ++r.processedInRoi; }
        if (distanceCm <= interestCm) ++r.interest;
    }
    void Result(uint8_t id, Drop drop)
    {
        if (id < MaxRecipients && static_cast<unsigned>(drop) < DropCount) ++recipients[id].drops[static_cast<unsigned>(drop)];
    }
    void Sent(uint8_t id) { if (id < MaxRecipients) ++recipients[id].sent; }
    void Released(uint8_t id) { if (id < MaxRecipients) ++recipients[id].released; }

    bool Due(uint32_t now) const { return started && static_cast<int32_t>(now - startedAt) >= static_cast<int32_t>(PeriodMs); }

    // One line per connected recipient; resets the window counters.
    template <class Emit>
    void Flush(uint32_t now, Context const& context, Emit emit)
    {
        const uint32_t windowMs = now - startedAt;
        for (unsigned id = 0; id < MaxRecipients; ++id) {
            Recipient const& r = recipients[id];
            if (!r.connected) continue;
            char line[1536];
            Format(line, sizeof(line), static_cast<uint8_t>(id), windowMs, context, r);
            emit(static_cast<char const*>(line));
        }
        for (Recipient& r : recipients) {
            r.sent = r.released = 0;
            std::memset(r.drops, 0, sizeof(r.drops));
        }
        std::memset(shared, 0, sizeof(shared));
        startedAt = now;
        ticks = 0;
    }

  private:
    void Format(char* out, size_t size, uint8_t id, uint32_t windowMs, Context const& context, Recipient const& r) const
    {
        unsigned total[DropCount];
        unsigned dropped = 0;
        unsigned top = DropCount;
        for (unsigned i = 0; i < DropCount; ++i) {
            total[i] = shared[i] + r.drops[i];
            dropped += total[i];
            if (total[i] && (top == DropCount || total[i] > total[top])) top = i;
        }
        char client[64] = "-", distance[32] = "-", nearest[32] = "-", host[64] = "-";
        if (r.positionValid) std::snprintf(client, sizeof(client), "%.0f,%.0f,%.0f", r.at[0], r.at[1], r.at[2]);
        if (context.hostValid) std::snprintf(host, sizeof(host), "%.0f,%.0f,%.0f", context.hostAt[0], context.hostAt[1], context.hostAt[2]);
        if (r.positionValid && context.hostValid) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) { const double d = static_cast<double>(r.at[k]) - context.hostAt[k]; sum += d * d; }
            std::snprintf(distance, sizeof(distance), "%.0f", std::sqrt(sum));
        }
        if (r.nearest >= 0.0f) std::snprintf(nearest, sizeof(nearest), "%.0f", static_cast<double>(r.nearest));
        world::HostSimStatus const& s = context.sim;
        const unsigned considered = r.sent + r.released + dropped;
        int used = std::snprintf(out, size,
            "npc-stream: recipient=%u window_ms=%u ticks=%u host=%s client=%s distance_cm=%s state_age_ms=%u in_world=%d "
            "sim=%s roi_cm=%.0f gate=%s resumable=%d stops=%u resumes=%u spheres=%u anchors=%u regions=%u/%u region_cells=%u "
            "region_static=%u region_dynamic=%u native_roi_cm=%.0f processed=%u processed_npc=%u interaction_npc=%u "
            "candidates=%u near_roi=%u processed_near_roi=%u nearest_cm=%s in_interest=%u "
            "per_s considered=%.1f sent=%.1f released=%.1f top_drop=",
            static_cast<unsigned>(id), windowMs, ticks, host, client, distance,
            r.stateAgeMs == UINT32_MAX ? 0u : r.stateAgeMs, r.inWorld ? 1 : 0,
            context.mode, static_cast<double>(context.roiCm), s.paused ? "paused" : s.hooked ? "open" : "unhooked",
            s.resumable ? 1 : 0, s.stops, s.resumes, s.spheres, s.anchors, s.regions, s.regionsFailed, s.regionCells,
            s.regionStatics, s.regionDynamics, static_cast<double>(s.nativeRoiCm), s.processed, processedNpc, interaction,
            candidates, r.inRoi, r.processedInRoi, nearest, r.interest,
            PerSecond(considered, windowMs), PerSecond(r.sent, windowMs), PerSecond(r.released, windowMs));
        if (used < 0 || static_cast<size_t>(used) >= size) return;
        if (top == DropCount) {
            used += std::snprintf(out + used, size - static_cast<size_t>(used), "-");
        } else {
            used += std::snprintf(out + used, size - static_cast<size_t>(used), "%s:%.1f", Name(static_cast<Drop>(top)),
                                  PerSecond(total[top], windowMs));
        }
        if (used < 0 || static_cast<size_t>(used) >= size) return;
        used += std::snprintf(out + used, size - static_cast<size_t>(used), " drops=");
        bool any = false;
        for (unsigned i = 0; i < DropCount && used > 0 && static_cast<size_t>(used) < size; ++i) {
            if (!total[i]) continue;
            used += std::snprintf(out + used, size - static_cast<size_t>(used), "%s%s:%.1f", any ? "," : "",
                                  Name(static_cast<Drop>(i)), PerSecond(total[i], windowMs));
            any = true;
        }
        if (!any && used > 0 && static_cast<size_t>(used) < size)
            used += std::snprintf(out + used, size - static_cast<size_t>(used), "-");
        if (context.perf.frames && used > 0 && static_cast<size_t>(used) < size)
            host_frame_diag::FormatFields(out + used, size - static_cast<size_t>(used), context.perf);
    }
};
}
