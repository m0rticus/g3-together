// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam_config.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace g3t::steam::diagnostics {
// Flat API layouts from Steamworks.NET 2025.164.1, steamnetworkingtypes.h.
#pragma pack(push, 8)
struct ConnectionInfo {
    uint8_t identity[136] = {};
    int64_t userData = 0;
    uint32_t listenSocket = 0;
    uint8_t remoteAddress[18] = {};
    uint16_t pad = 0;
    uint32_t popRemote = 0, popRelay = 0;
    int32_t state = 0, endReason = 0;
    char endDebug[128] = {}, description[128] = {};
    int32_t flags = 0;
    uint32_t reserved[63] = {};
};
struct SessionStatus {
    int32_t state = 0, ping = 0;
    float qualityLocal = 0, qualityRemote = 0;
    float outPackets = 0, outBytes = 0, inPackets = 0, inBytes = 0;
    int32_t sendRate = 0, pendingUnreliable = 0, pendingReliable = 0, sentUnackedReliable = 0;
    int64_t queueMicros = 0;
    int32_t maxJitterMicros = 0;
    uint32_t reserved[15] = {};
};
#pragma pack(pop)
static_assert(sizeof(ConnectionInfo) == 696 && offsetof(ConnectionInfo, userData) == 136 &&
              offsetof(ConnectionInfo, remoteAddress) == 148 && offsetof(ConnectionInfo, popRelay) == 172 &&
              offsetof(ConnectionInfo, state) == 176 && offsetof(ConnectionInfo, description) == 312 &&
              offsetof(ConnectionInfo, flags) == 440, "Steam connection info ABI");
static_assert(sizeof(SessionStatus) == 120 && offsetof(SessionStatus, queueMicros) == 48,
              "Steam realtime status ABI");

constexpr int Connected = 3, LoopbackBuffers = 4, Relayed = 16;
constexpr int StunServerList = 103, IceEnable = config::P2PTransportIceEnable, IcePenalty = 105, SdrPenalty = 106;
constexpr int TimeoutInitial = 24, TimeoutConnected = 25, MtuPacketSize = 32, Unencrypted = 34;

inline bool HasRemoteAddress(ConnectionInfo const& info)
{
    for (unsigned i = 0; i < 16; ++i)
        if (info.remoteAddress[i]) return true;
    return false;
}

inline bool Current(int state, ConnectionInfo const& info, SessionStatus const& status)
{
    return state == Connected && info.state == state && status.state == state;
}

// Only used for this mod's SteamID-addressed Messages sessions, not arbitrary IP connections.
inline char const* Transport(int state, ConnectionInfo const& info, SessionStatus const& status)
{
    if (!Current(state, info, status) || (info.flags & LoopbackBuffers)) return "unknown";
    if (info.flags & Relayed) return info.popRelay ? "relay-sdr" : "unknown";
    if (info.popRelay || !HasRemoteAddress(info)) return "unknown";
    return "direct-ice";
}

inline char const* Evidence(int state, ConnectionInfo const& info, SessionStatus const& status)
{
    if (!Current(state, info, status)) return "no-current-connected-sample";
    if (info.flags & LoopbackBuffers) return "internal-loopback";
    if (info.flags & Relayed) return info.popRelay ? "relayed-flag-and-pop" : "relayed-without-sdr-pop";
    return info.popRelay ? "inconsistent-relay-fields" :
        HasRemoteAddress(info) ? "nonrelayed-peer-address" : "peer-address-unavailable";
}

inline std::array<char, 129> Description(ConnectionInfo const& info)
{
    std::array<char, 129> result{};
    size_t out = 0, begin = 0;
    while (begin < sizeof(info.description) && info.description[begin]) {
        if (info.description[begin] == ' ') {
            if (out < result.size() - 1) result[out++] = ' ';
            ++begin;
            continue;
        }
        size_t end = begin;
        bool redact = false;
        while (end < sizeof(info.description) && info.description[end] && info.description[end] != ' ') {
            const unsigned char c = static_cast<unsigned char>(info.description[end++]);
            // Whole address-bearing tokens are suppressed, including IPv6 and IP=... forms.
            redact = redact || c == '.' || c == ':' || c < 32 || c > 126;
        }
        char const* text = redact ? "[redacted]" : info.description + begin;
        const size_t length = redact ? sizeof("[redacted]") - 1 : end - begin;
        for (size_t i = 0; i < length && out < result.size() - 1; ++i) result[out++] = text[i];
        begin = end;
    }
    return result;
}
}
