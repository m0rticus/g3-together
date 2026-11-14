// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <enet/enet.h>
#include <cstdint>
#include <cstring>

namespace g3t::net {
constexpr enet_uint32 Mtu = 1200;
enum : uint8_t {
    ChannelState = 0, ChannelReliable = 1, ChannelNpcs = 2, ChannelCombat = 3,
    ChannelPoses = 4, ChannelPosesAlt = 5, ChannelBulk = 6, ChannelDefinitions = 7, ChannelCount = 8
};

#ifdef _WIN32
inline bool DisableWindowsUdpConnReset(ENetSocket socket)
{
    if (socket == ENET_SOCKET_NULL) return false;
    const HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) return false;
    const FARPROC proc = GetProcAddress(ws2, "WSAIoctl");
    if (!proc) return false;
    using WsaIoctlFn = decltype(&::WSAIoctl);
    static_assert(sizeof(WsaIoctlFn) == sizeof(proc), "WSAIoctl pointer size mismatch");
    WsaIoctlFn wsaIoctl = nullptr;
    std::memcpy(&wsaIoctl, &proc, sizeof(wsaIoctl));
    BOOL connResetOff = FALSE;
    DWORD connResetBytes = 0;
    constexpr DWORD UdpConnReset = 0x9800000Cu; // SIO_UDP_CONNRESET
    return wsaIoctl(socket, UdpConnReset, &connResetOff, sizeof(connResetOff),
                    nullptr, 0, &connResetBytes, nullptr, nullptr) == 0;
}
#endif

inline ENetHost* CreateTransportHost(ENetAddress const* address, size_t peers, char const*& error)
{
    error = nullptr;
    ENetHost* host = enet_host_create(address, peers, ChannelCount, 0, 0);
    if (!host) {
        error = "loopback ENet socket could not be created";
        return nullptr;
    }
    if (enet_host_compress_with_range_coder(host) != 0)
        error = "ENet lossless compression context allocation failed";
    // Rice-packed bone deltas have ~7.6 bits/byte entropy (~6.7% range-coder reduction at heavy DIV/BST CPU cost).
    // Keep decompress allocated for backward-compatible test frames while disabling all outbound range coding.
    host->compressor.compress = nullptr;
    host->mtu = Mtu;
#ifdef _WIN32
    DisableWindowsUdpConnReset(host->socket);
#endif
    constexpr int PoseBurstBuffer = 4 * 1024 * 1024;
    if (!error && (enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, PoseBurstBuffer) < 0 ||
                  enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, PoseBurstBuffer) < 0))
        error = "cannot configure the owned ENet socket for canonical pose bursts";
    if (error) {
        enet_host_destroy(host);
        return nullptr;
    }
    return host;
}

inline ENetPeer* ConnectTransportHost(ENetHost* host, ENetAddress const* address, enet_uint32 data = 0)
{
    return enet_host_connect(host, address, ChannelCount, data);
}

inline char const* EnetPeerStateName(unsigned state)
{
    switch (state) {
    case ENET_PEER_STATE_DISCONNECTED: return "disconnected";
    case ENET_PEER_STATE_CONNECTING: return "connecting";
    case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT: return "acknowledging-connect";
    case ENET_PEER_STATE_CONNECTION_PENDING: return "connection-pending";
    case ENET_PEER_STATE_CONNECTION_SUCCEEDED: return "connection-succeeded";
    case ENET_PEER_STATE_CONNECTED: return "connected";
    case ENET_PEER_STATE_DISCONNECT_LATER: return "disconnect-later";
    case ENET_PEER_STATE_DISCONNECTING: return "disconnecting";
    case ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT: return "acknowledging-disconnect";
    case ENET_PEER_STATE_ZOMBIE: return "zombie";
    default: return "unknown";
    }
}

constexpr enet_uint32 PacketFlags(bool reliable)
{
    // Let ENet use the negotiated MTU/header budget; a payload below the MTU can still need fragments.
    return reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
}
}
