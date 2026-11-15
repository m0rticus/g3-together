// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "log.h"

#include <cstdlib>
#include <cstring>

namespace g3t::net::internal {

namespace {
bool ConstantTimeCookieEqual(uint8_t const a[16], uint8_t const b[16])
{
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool AllowDirectPunchLoopback()
{
#ifdef _WIN32
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, "G3T_DIRECT_UDP_ALLOW_LOOPBACK") != 0 || !raw) return false;
    const bool ok = raw[0] == '1';
    std::free(raw);
    return ok;
#else
    char const* raw = std::getenv("G3T_DIRECT_UDP_ALLOW_LOOPBACK");
    return raw && raw[0] == '1';
#endif
}

bool IsValidDirectPunchEndpoint(ENetAddress const& addr)
{
    if (!addr.host || addr.host == 0xffffffffu || !addr.port) return false;
    uint8_t octets[4] = {};
    std::memcpy(octets, &addr.host, sizeof(octets));
    if (octets[0] == 0 || octets[0] >= 224) return false;
    if (octets[0] == 169 && octets[1] == 254) return false;
    if (octets[0] == 127) {
        if (!AllowDirectPunchLoopback()) return false;
        if (octets[1] == 0 && octets[2] == 0 && octets[3] == 0) return false;
        if (octets[1] == 255 && octets[2] == 255 && octets[3] == 255) return false;
    }
    return true;
}

void SendDirectPunchPacket(ENetAddress const& to, uint8_t const cookie[16], uint8_t flags)
{
    if (!host || host->socket == ENET_SOCKET_NULL || !to.host || !to.port) return;
    uint8_t packet[DirectPunchBytes] = {};
    std::memcpy(packet, "G3TPUNCH", 8);
    std::memcpy(packet + 8, cookie, 16);
    packet[24] = flags;
    packet[25] = isHost ? 1u : 2u;
    ENetBuffer buf{sizeof(packet), packet};
    enet_socket_send(host->socket, &to, &buf, 1);
}
} // namespace

int ENET_CALLBACK InterceptTransportPacket(ENetHost* currentHost, ENetEvent*)
{
    if (!currentHost || !currentHost->receivedData) return 0;
    const ENetAddress from = currentHost->receivedAddress;
    const size_t length = currentHost->receivedDataLength;
    const uint8_t* data = currentHost->receivedData;
    const bool isRelayPort = from.host == target.host &&
        (isHost ? steam::IsPeerRelay(from.port) : from.port == target.port);
    if (length == DirectPunchBytes && std::memcmp(data, "G3TPUNCH", 8) == 0) {
        const uint8_t expectedPeerRole = isHost ? 2u : 1u;
        if (isRelayPort || data[25] != expectedPeerRole || !IsValidDirectPunchEndpoint(from)) return 1;
        const uint8_t flags = data[24];
        for (auto& session : directSessions) {
            if (!session.account || !session.generation || session.directFailed) continue;
            if (!ConstantTimeCookieEqual(data + 8, session.cookie)) continue;
            const bool sameEndpoint = session.inboundVerified &&
                session.verifiedAddress.host == from.host &&
                session.verifiedAddress.port == from.port;
            if ((session.inboundVerified || session.endpointLocked) && !sameEndpoint) return 1;
            if (!sameEndpoint) {
                if (isHost) {
                    steam::RegisterDirectPeerEndpoint(session.account, session.generation, from.host, from.port);
                    uint64_t regAccount = 0, regGen = 0;
                    char regName[24] = {};
                    if (!steam::DirectPeerIdentity(from.host, from.port, regAccount, regName, regGen) ||
                        regAccount != session.account || regGen != session.generation)
                        return 1;
                }
                session.inboundVerified = true;
                session.verifiedAddress = from;
            }
            if (flags & 0x02u) session.bidirectionalVerified = true;
            if (flags & 0x01u) SendDirectPunchPacket(from, session.cookie, 0x02u);
            return 1;
        }
        return 1;
    }
    if (isRelayPort) return 0;
    for (auto const& session : directSessions) {
        if (!session.account || !session.inboundVerified || session.directFailed) continue;
        if (session.verifiedAddress.host == from.host && session.verifiedAddress.port == from.port)
            return 0;
    }
    // Drop unauthenticated external UDP packets before ENet protocol handling.
    return 1;
}

bool PeerRelayOrDirect(ENetPeer const* peer)
{
    if (!peer) return false;
    if (peer->address.host == target.host && steam::IsPeerRelay(peer->address.port))
        return true;
    uint64_t account = 0, generation = 0;
    char name[24] = {};
    return steam::DirectPeerIdentity(peer->address.host, peer->address.port, account, name, generation);
}

bool ClaimAuthenticatedPeerIdentity(ENetPeer const* peer, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!peer) return false;
    if (peer->address.host == target.host && steam::ClaimPeerIdentity(peer->address.port, account, name, generation))
        return true;
    const size_t idx = PeerIndex(peer);
    const uint32_t connectTag = idx < MaxPlayers ? peerConnectData[idx] : 0;
    if (!steam::ClaimDirectPeerIdentity(peer->address.host, peer->address.port, connectTag, account, name, generation))
        return false;
    LockDirectSessionEndpoint(account, generation, true);
    return true;
}

void LockDirectSessionEndpoint(uint64_t account, uint64_t generation, bool locked)
{
    if (!account || !generation) return;
    if (!locked) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peerAccounts[i] == account && peerRelayGenerations[i] == generation)
                return;
    }
    for (auto& session : directSessions)
        if (session.account == account && session.generation == generation)
            session.endpointLocked = locked;
}

void SyncDirectSessions(uint32_t now)
{
    const auto offers = steam::DirectOffers();
    for (auto& s : directSessions) {
        bool stillPresent = false;
        for (auto const& o : offers)
            if (o.account == s.account && o.generation == s.generation) { stillPresent = true; break; }
        if (!stillPresent) s = DirectPunchSession{};
    }
    for (auto const& offer : offers) {
        DirectPunchSession* slot = nullptr;
        for (auto& s : directSessions)
            if (s.account == offer.account && s.generation == offer.generation) { slot = &s; break; }
        if (!slot) {
            for (auto& s : directSessions)
                if (!s.account) { slot = &s; break; }
        }
        if (!slot) continue;
        if (!slot->account) {
            slot->account = offer.account;
            slot->generation = offer.generation;
            slot->startedAt = now ? now : 1;
        }
        std::memcpy(slot->cookie, offer.cookie, 16);
        slot->candidateCount = offer.candidateCount;
        for (unsigned i = 0; i < offer.candidateCount && i < 8; ++i)
            slot->candidates[i] = offer.candidates[i];
        slot->ready = offer.ready;
    }
    for (auto& s : directSessions) {
        if (!s.account || !s.ready || s.bidirectionalVerified || s.directFailed) continue;
        if (now - s.startedAt >= DirectPunchTimeoutMs) continue;
        if (s.lastProbeAt && now - s.lastProbeAt < DirectPunchIntervalMs) continue;
        s.lastProbeAt = now ? now : 1;
        for (unsigned i = 0; i < s.candidateCount && i < 8; ++i) {
            ENetAddress to{s.candidates[i].hostIpv4, s.candidates[i].port};
            SendDirectPunchPacket(to, s.cookie, s.inboundVerified ? 0x03u : 0x01u);
        }
    }
}

void MarkDirectConnectFailed()
{
    for (auto& s : directSessions) {
        if (s.account && s.generation == hostRelayGeneration && s.connectStarted && !s.directFailed) {
            s.directFailed = true;
            s.connectStarted = false;
            s.endpointLocked = false;
            lastConnectAttempt = enet_time_get() - 3001;
            if (!s.fallbackLogged) {
                s.fallbackLogged = true;
                Log("net: direct UDP connection failed; falling back to Steam relay");
            }
            break;
        }
    }
}

DirectPunchSession* FindClientDirectSession()
{
    for (auto& s : directSessions)
        if (s.account && s.generation == hostRelayGeneration) return &s;
    return nullptr;
}

bool ClientPunchWaiting(DirectPunchSession const* direct, uint32_t now)
{
    return direct && !direct->bidirectionalVerified && !direct->directFailed &&
           (now - startedAt < DirectPunchTimeoutMs);
}

void ConnectClientWithDirectFallback(DirectPunchSession* direct, bool& useDirect, ENetAddress& connectTo, bool logFallback)
{
    useDirect = direct && direct->bidirectionalVerified && !direct->directFailed;
    connectTo = useDirect ? direct->verifiedAddress : target;
    server = useDirect
        ? ConnectTransportHost(host, &direct->verifiedAddress, DirectCookieTag(direct->cookie))
        : ConnectTransportHost(host, &target);
    if (useDirect && !server) {
        direct->directFailed = true;
        direct->connectStarted = false;
        direct->endpointLocked = false;
        if (logFallback && !direct->fallbackLogged) {
            direct->fallbackLogged = true;
            Log("net: direct UDP connect setup failed; falling back to Steam relay");
        }
        useDirect = false;
        connectTo = target;
        server = ConnectTransportHost(host, &target);
    } else if (direct) {
        direct->connectStarted = useDirect && server != nullptr;
        if (direct->connectStarted) direct->endpointLocked = true;
    }
}

void FinishClientConnectAttempt(DirectPunchSession* direct, bool useDirect, uint32_t now)
{
    lastConnectAttempt = now;
    ENetAddress connectTo = target;
    ConnectClientWithDirectFallback(direct, useDirect, connectTo, true);
    steam::NoteClientEnetState(server ? server->state : ENET_PEER_STATE_DISCONNECTED, ENET_EVENT_TYPE_NONE, 0,
                               now - startedAt, server ? "" : "ENet connect refused (no free peer)");
    Logf("enet: connect request role=client channels=%u compress=off route=%s port=%u result=%s state=%s elapsed_ms=%u",
         ChannelCount, useDirect ? "direct-udp" : "steam-relay", connectTo.port, server ? "pending" : "refused",
         EnetPeerStateName(server ? server->state : ENET_PEER_STATE_DISCONNECTED), now - startedAt);
    if (!connectLogged) {
        if (useDirect)
            Logf("net: direct UDP hole-punch verified (port %u); connecting natively (zero relay overhead)", connectTo.port);
        else
            Logf("net: connecting over Steam (relay 127.0.0.1:%u): %s", target.port, server ? "pending" : "failed");
    } else {
        Log(server ? "net: reconnecting" : "net: reconnect failed");
    }
    connectLogged = true;
}

void SyncClientSteamLink(uint32_t now)
{
    if (isHost) return;
    const uint64_t publishedGeneration = steam::HostRelayGeneration();
    const bool published = publishedGeneration != 0;
    if (!published) linkEnding = false;
    const bool linked = published && !linkEnding;
    if (linked && !steamLinked) {
        steamLinked = true;
        hostRelayGeneration = publishedGeneration;
        connectLogged = false;
        refused = false;
        refusalReason[0] = '\0';
        everConnected = false;
        startedAt = now;
        lastConnectAttempt = now - 3001;
        Log("net: the Steam link to the host is up; ENet connects through the relay");
    } else if (!linked && steamLinked) {
        const auto flow = steam::Flow();
        Logf("enet: relay closed role=client reason=%s state=%s elapsed_ms=%u",
             flow.endReason[0] ? flow.endReason : "relay no longer published",
             EnetPeerStateName(server ? server->state : ENET_PEER_STATE_DISCONNECTED), now - startedAt);
        CloseClientLink();
    }
}

} // namespace g3t::net::internal
