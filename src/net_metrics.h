// SPDX-License-Identifier: GPL-3.0-or-later
// Local measurements only; none of these types are sent on the wire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3t::net {
enum class NpcSendDecision { Sent, SkippedUnchanged, SkippedFar, SkippedIrrelevant, SendFailed, SkippedPaced };

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
    uint64_t considered = 0, sent = 0, skippedUnchanged = 0, skippedFar = 0, skippedIrrelevant = 0, sendFailed = 0, skippedPaced = 0;
    uint64_t tierUpdates[3] = {}, tierBytes[3] = {};
    uint64_t poseBytes = 0, poseBones = 0, poseKeys = 0, poseDeltas = 0;
    uint64_t poseAcks = 0, poseAckMisses = 0, poseReferenceMisses = 0, poseDecodeFailures = 0;
    uint64_t poseHeaderBytes = 0, poseMaskBits = 0, poseRotationBits = 0, poseTranslationBits = 0;
    uint64_t poseControlBytes = 0, poseOuterBytes = 0, poseIncludedDeltaBones = 0, posePredictiveUpdates = 0;
    uint64_t poseRigidBones = 0, poseMaxBasisError = 0;
    uint64_t posePaddingBits = 0;

    void Pose(uint32_t interval, size_t bytes, size_t bonesBytes, int bones, bool key)
    {
        const unsigned tier = interval <= 16 ? 0 : interval < 100 ? 1 : 2;
        ++tierUpdates[tier]; tierBytes[tier] += bytes;
        poseBytes += bonesBytes; poseBones += static_cast<uint64_t>(bones);
        if (key) ++poseKeys; else ++poseDeltas;
    }

    void PoseComponents(size_t headerBytes, size_t maskBits, size_t rotationBits, size_t translationBits,
                        size_t includedBones, bool key, bool predictive, size_t controlBytes, size_t outerBytes,
                        size_t rigidBones = 0, size_t maxBasisError = 0, size_t paddingBits = 0)
    {
        poseHeaderBytes += headerBytes; poseMaskBits += maskBits;
        poseRotationBits += rotationBits; poseTranslationBits += translationBits;
        poseControlBytes += controlBytes; poseOuterBytes += outerBytes;
        if (!key) poseIncludedDeltaBones += includedBones;
        if (predictive) ++posePredictiveUpdates;
        poseRigidBones += rigidBones;
        posePaddingBits += paddingBits;
        if (maxBasisError > poseMaxBasisError) poseMaxBasisError = maxBasisError;
    }

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
        case NpcSendDecision::SkippedPaced: skippedPaced += count; break;
        default: return;
        }
        considered += count;
    }
};

template <typename FormatEntry, typename Emit>
void EmitKindLines(BandwidthCounters const& counters, char const* header, FormatEntry formatEntry, Emit emit)
{
    char line[760];
    std::snprintf(line, sizeof(line), "%s", header);
    size_t used = std::strlen(line);
    const size_t prefix = used;
    for (unsigned kind = 0; kind < 256; ++kind) {
        auto const& c = counters.kinds[kind];
        if (!c.txPackets && !c.rxPackets) continue;
        char entry[128];
        const int length = formatEntry(kind, c, entry, sizeof(entry));
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

template <typename Emit>
void ForEachKindLine(BandwidthCounters const& counters, Emit emit)
{
    EmitKindLines(counters, "net: bandwidth kinds id=tx_packets/bytes,rx_packets/bytes",
        [](unsigned kind, PacketCounts const& c, char* entry, size_t size) {
            return std::snprintf(entry, size, " %u=%llu/%llu,%llu/%llu", kind,
                static_cast<unsigned long long>(c.txPackets), static_cast<unsigned long long>(c.txBytes),
                static_cast<unsigned long long>(c.rxPackets), static_cast<unsigned long long>(c.rxBytes));
        }, emit);
}

template <typename Emit>
void ForEachKindRateLine(BandwidthCounters const& counters, double perSecond, Emit emit)
{
    const double scale = std::isfinite(perSecond) && perSecond >= 0.0 ? perSecond : 0.0;
    EmitKindLines(counters, "net: bandwidth kind_bps id=txBps/rxBps",
        [scale](unsigned kind, PacketCounts const& c, char* entry, size_t size) {
            return std::snprintf(entry, size, " %u=%.1f/%.1f", kind,
                static_cast<double>(c.txBytes) * scale, static_cast<double>(c.rxBytes) * scale);
        }, emit);
}
}
}
