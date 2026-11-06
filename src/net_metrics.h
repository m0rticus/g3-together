// SPDX-License-Identifier: GPL-3.0-or-later
// Local measurements only; none of these types are sent on the wire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace g3t::net {
enum class NpcSendDecision { Sent, SkippedUnchanged, SkippedFar, SkippedIrrelevant, SendFailed };

// Classify each NPC scheduling decision exactly once. "Sent" means queued successfully, not delivered;
// use SendFailed when a selected publication could not be queued. If decisions are made per recipient,
// report each recipient separately.
// Call on the same thread as net::Tick.
void RecordNpcSendDecision(NpcSendDecision decision, uint32_t count = 1);

namespace detail {
struct PacketCounts {
    uint64_t txPackets = 0, txBytes = 0, rxPackets = 0, rxBytes = 0;
};

struct BandwidthCounters {
    PacketCounts kinds[256] = {};
    uint64_t considered = 0, sent = 0, skippedUnchanged = 0, skippedFar = 0, skippedIrrelevant = 0, sendFailed = 0;

    void Tx(uint8_t kind, size_t bytes) { ++kinds[kind].txPackets; kinds[kind].txBytes += bytes; }
    void Rx(uint8_t kind, size_t bytes) { ++kinds[kind].rxPackets; kinds[kind].rxBytes += bytes; }
    void Npc(NpcSendDecision decision, uint32_t count)
    {
        switch (decision) {
        case NpcSendDecision::Sent: sent += count; break;
        case NpcSendDecision::SkippedUnchanged: skippedUnchanged += count; break;
        case NpcSendDecision::SkippedFar: skippedFar += count; break;
        case NpcSendDecision::SkippedIrrelevant: skippedIrrelevant += count; break;
        case NpcSendDecision::SendFailed: sendFailed += count; break;
        default: return;
        }
        considered += count;
    }
};

template <typename Emit>
void ForEachKindLine(BandwidthCounters const& counters, Emit emit)
{
    char line[760] = "net: bandwidth kinds id=tx_packets/bytes,rx_packets/bytes";
    size_t used = std::strlen(line);
    const size_t prefix = used;
    for (unsigned kind = 0; kind < 256; ++kind) {
        auto const& c = counters.kinds[kind];
        if (!c.txPackets && !c.rxPackets) continue;
        char entry[128];
        const int length = std::snprintf(entry, sizeof(entry), " %u=%llu/%llu,%llu/%llu", kind,
            static_cast<unsigned long long>(c.txPackets), static_cast<unsigned long long>(c.txBytes),
            static_cast<unsigned long long>(c.rxPackets), static_cast<unsigned long long>(c.rxBytes));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(entry)) continue;
        if (used + static_cast<size_t>(length) >= sizeof(line)) {
            emit(line);
            line[prefix] = '\0';
            used = prefix;
        }
        std::memcpy(line + used, entry, static_cast<size_t>(length) + 1);
        used += static_cast<size_t>(length);
    }
    if (used != prefix) emit(line);
}
}
}
