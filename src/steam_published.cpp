// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
// Published engine-thread state and lock-guarded readers/writers; see steam.h and steam_runtime.h.
#include "steam_runtime.h"

#include "config.h"
#include "log.h"
#include "relaywire.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace g3t::steam {
namespace {
PublishedPeer const* FindPublishedPeerLocked(unsigned short port)
{
    if (!store.published.ready || !rt.isHost || !port) return nullptr;
    for (auto const& peer : store.published.peers)
        if (peer.port == port && peer.account) return &peer;
    return nullptr;
}

bool ReadPeerIdentityLocked(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    PublishedPeer const* peer = FindPublishedPeerLocked(port);
    if (!peer) return false;
    account = peer->account;
    generation = peer->generation;
    strcpy_s(name, peer->name);
    return true;
}

bool ReadDirectPeerIdentityLocked(uint32_t hostIpv4, uint16_t port, uint64_t& account,
                                  char (&name)[24], uint64_t& generation, uint32_t* expectedTag)
{
    if (!store.published.ready || !rt.isHost || !hostIpv4 || !port) return false;
    for (auto const& peer : store.published.peers) {
        if (!peer.directPinned || !peer.account || !peer.generation) continue;
        if (peer.verifiedDirect.hostIpv4 != hostIpv4 || peer.verifiedDirect.port != port) continue;
        account = peer.account;
        generation = peer.generation;
        strcpy_s(name, peer.name);
        if (expectedTag) *expectedTag = CookieTag(peer.directOffer.cookie);
        return true;
    }
    return false;
}

void CancelPendingRelayCloseLocked(uint64_t account, uint64_t generation)
{
    store.relayCloses.erase(std::remove_if(store.relayCloses.begin(), store.relayCloses.end(),
        [account, generation](RelayClose const& request) {
            return request.account == account && request.generation == generation;
        }), store.relayCloses.end());
}

void FormatPublishedStatus(Published const& pub, bool hasPollTimer, char (&out)[900])
{
    out[0] = '\0';
    size_t pos = 0;
    auto append = [&](char const* fmt, ...) {
        if (pos + 1 >= sizeof(out)) return;
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(out + pos, sizeof(out) - pos, fmt, args);
        va_end(args);
        if (written > 0)
            pos = (std::min)(pos + static_cast<size_t>(written), sizeof(out) - 1);
    };
    auto const& d = pub.diag;
    append("STEAMSTATUS app=%u ready=%d role=%s lobby=%llu owner=%llu host=%s linked=%d ping=%d "
           "send_failures=%u offset=%d error=%s session_id=%s isolated_drops=%u "
           "poll_timer=%s poll_gap_avg_ms=%.2f poll_gap_max_ms=%u pump_work_max_ms=%u receive_caps=%u forward_caps=%u "
           "socket_failures=%u socket_error=%d receive_failures=%u invalid_pongs=%u",
           GothicAppId, pub.ready ? 1 : 0, d.isHost ? "host" : "client", pub.lobby, d.owner, pub.hostName,
           pub.linked, pub.ping, d.sendFailures, d.senderOffset, *pub.error ? pub.error : "-",
           d.sessionId, d.isolatedDrops, hasPollTimer ? "highres" : "legacy",
           d.pollGapAvgMs, d.pollGapMaxMs, d.pumpWorkMaxMs, d.receiveCaps, d.forwardCaps,
           d.socketFailures, d.socketError, d.receiveFailures, d.invalidPongs);
    append(" connect_mode=%s connect_state=%s why=%s",
           pub.connection.automatic ? "auto" : "invite",
           d.isHost ? connect::StateName(pub.connection.hostState) : connect::StateName(pub.connection.clientState),
           d.isHost ? "-" : connect::WhyName(pub.connection.why));
    for (auto const& p : d.peers) {
        if (!p.id) continue;
        append(" peer=%llu:%d:%u:%u:%d ping_age_ms=%u",
               p.id, p.linked ? 1 : 0, p.toEnet, p.toSteam, p.ping, p.pingAgeMs);
    }
}
}

void InvalidatePublishedLocked(PublishedStore& target, bool isHostRole, u64 account, uint64_t generation)
{
    for (auto& peer : target.published.peers) {
        if (peer.account != account || peer.generation != generation) continue;
        peer = PublishedPeer{};
        if (isHostRole && target.published.linked > 0) --target.published.linked;
    }
    if (!isHostRole && target.published.hostGeneration == generation) {
        target.published.hostLinked = false;
        target.published.hostGeneration = 0;
    }
}

void WorkerRuntime::Publish()
{
    Published next;
    const uint32_t now = Now();
    char const* session = GetLocalConfig().sessionId;
    next.ready = ready;
    next.lobby = lobby;
    next.linked = CountLinked();
    next.traffic = traffic;
    strcpy_s(next.playerName, playerName);
    next.traffic.envelopeBytes = static_cast<uint32_t>(RelayHeaderSize(session));
    for (auto const& r : remotes)
        if (r.linked && r.ping.Value(now) > next.ping) {
            next.ping = r.ping.Value(now);
            next.pingSampleAt = r.ping.sampledAt;
        }
    strcpy_s(next.hostName, hostName);
    strcpy_s(next.error, error);
    FillConnection(next.connection);

    auto& d = next.diag;
    d.isHost = isHost;
    d.owner = owner;
    strcpy_s(d.sessionId, session);
    d.sendFailures = sendFailures;
    d.senderOffset = senderOffset;
    d.isolatedDrops = isolatedDrops;
    d.pollGapAvgMs = pollGaps.Average();
    d.pollGapMaxMs = pollGaps.maximum;
    d.pumpWorkMaxMs = pumpWork.maximum;
    d.receiveCaps = receiveCaps;
    d.forwardCaps = forwardCaps;
    d.socketFailures = socketFailures;
    d.socketError = socketError;
    d.receiveFailures = receiveFailures;
    d.invalidPongs = invalidPongs;

    for (int i = 0; i < MaxRemotes; ++i) {
        auto const& r = remotes[i];
        if (!r.id) continue;
        if (r.linked) {
            auto& peer = next.peers[i];
            peer.port = r.relayPort;
            peer.account = r.id;
            peer.generation = r.generation;
            strcpy_s(peer.name, r.name);
            if (directUdpEnabled && r.haveDirectCookie) {
                auto& offer = peer.directOffer;
                offer.account = r.id;
                offer.generation = r.generation;
                std::memcpy(offer.cookie, r.directCookie, sizeof(offer.cookie));
                offer.candidateCount = r.directCandidateCount;
                for (unsigned c = 0; c < r.directCandidateCount && c < 8; ++c)
                    offer.candidates[c] = r.directCandidates[c];
                offer.ready = r.directReady && r.directCandidateCount != 0;
            }
        }
        if (!isHost && r.id == owner) {
            next.hostLinked = r.linked;
            if (r.linked) next.hostGeneration = r.generation;
        }
        d.peers[i] = {r.id, r.linked, r.toEnet, r.toSteam, r.ping.Value(now),
                      r.ping.milliseconds < 0 ? 0u : now - r.ping.sampledAt};
    }

    std::lock_guard<std::mutex> lock(store.mutex);
    for (auto& peer : next.peers) {
        if (!peer.account || !peer.generation) continue;
        for (auto const& prev : store.published.peers) {
            if (!prev.directPinned || prev.account != peer.account || prev.generation != peer.generation)
                continue;
            peer.verifiedDirect = prev.verifiedDirect;
            peer.directPinned = true;
            break;
        }
    }
    store.published = next;
}

unsigned short RelayPort() { return rt.relayPort; }

bool IsPeerRelay(unsigned short port)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return FindPublishedPeerLocked(port) != nullptr;
}

uint64_t PeerId(unsigned short port)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    PublishedPeer const* peer = FindPublishedPeerLocked(port);
    return peer ? peer->account : 0;
}

bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24])
{
    uint64_t generation = 0;
    return PeerIdentity(port, account, name, generation);
}

bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return ReadPeerIdentityLocked(port, account, name, generation);
}

bool ClaimPeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    if (!ReadPeerIdentityLocked(port, account, name, generation) || account == rt.me) return false;
    CancelPendingRelayCloseLocked(account, generation);
    return true;
}

std::vector<DirectOffer> DirectOffers()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    std::vector<DirectOffer> result;
    if (!store.published.ready) return result;
    for (auto const& peer : store.published.peers)
        if (peer.directOffer.account && peer.directOffer.generation)
            result.push_back(peer.directOffer);
    return result;
}

void RegisterDirectPeerEndpoint(uint64_t account, uint64_t generation, uint32_t hostIpv4, uint16_t port)
{
    if (!account || !generation || !IsValidDirectCandidateEndpoint(hostIpv4, port, AllowDirectLoopback()))
        return;
    std::lock_guard<std::mutex> lock(store.mutex);
    for (auto& peer : store.published.peers) {
        if (peer.account != account || peer.generation != generation) continue;
        if (peer.directPinned) {
            if (peer.verifiedDirect.hostIpv4 == hostIpv4 && peer.verifiedDirect.port == port) return;
            Logf("steam: refusing direct endpoint rebind for peer=%llu gen=%llu",
                 static_cast<unsigned long long>(account), static_cast<unsigned long long>(generation));
            return;
        }
        peer.verifiedDirect = DirectEndpoint{hostIpv4, port};
        peer.directPinned = true;
        return;
    }
}

bool DirectPeerIdentity(uint32_t hostIpv4, uint16_t port, uint64_t& account,
                        char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return ReadDirectPeerIdentityLocked(hostIpv4, port, account, name, generation, nullptr);
}

bool ClaimDirectPeerIdentity(uint32_t hostIpv4, uint16_t port, uint32_t connectTag,
                             uint64_t& account, char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    uint32_t expectedTag = 0;
    if (!ReadDirectPeerIdentityLocked(hostIpv4, port, account, name, generation, &expectedTag) ||
        account == rt.me || !expectedTag || connectTag != expectedTag)
        return false;
    CancelPendingRelayCloseLocked(account, generation);
    return true;
}

std::vector<RelayDrop> TakeRelayDrops()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    std::vector<RelayDrop> result;
    result.swap(store.relayDrops);
    return result;
}

void RequestHostRelayClose(uint64_t account, uint64_t generation, char const* reason)
{
    if (!account || !generation) return;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        RelayClose close{account, generation, {}};
        strncpy_s(close.reason, reason, _TRUNCATE);
        store.relayCloses.push_back(close);
    }
    if (store.wakeEvent) SetEvent(store.wakeEvent);
}

bool Ready() { std::lock_guard<std::mutex> lock(store.mutex); return store.published.ready; }
uint64_t LobbyId() { std::lock_guard<std::mutex> lock(store.mutex); return store.published.lobby; }
char const* PlayerName()
{
    thread_local char copy[24];
    std::lock_guard<std::mutex> lock(store.mutex);
    strcpy_s(copy, store.published.playerName);
    return copy;
}
uint64_t PlayerId() { return rt.me; }
int Ping()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.ping >= 0 && Now() - store.published.pingSampleAt < latency::PingExpiryMs
        ? store.published.ping : -1;
}
char const* Error()
{
    thread_local char copy[sizeof(rt.error)];
    std::lock_guard<std::mutex> lock(store.mutex);
    strcpy_s(copy, store.published.error);
    return copy;
}
char const* HostName()
{
    thread_local char copy[sizeof(rt.hostName)];
    std::lock_guard<std::mutex> lock(store.mutex);
    strcpy_s(copy, store.published.hostName);
    return copy;
}

bool HostLinked()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.hostLinked;
}

uint64_t HostRelayGeneration()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.hostGeneration;
}

bool RequestClientDisconnect()
{
    if (rt.isHost || rt.connectionMode != connect::Mode::Invite) return false;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.disconnectGeneration = store.published.hostGeneration;
    }
    store.disconnectRequested.store(true);
    if (store.wakeEvent) SetEvent(store.wakeEvent);
    return true;
}

void NoteClientEnetState(unsigned state, unsigned event, uint32_t data, uint32_t elapsedMs, char const* reason)
{
    std::lock_guard<std::mutex> lock(store.mutex);
    store.clientEnet.state = state;
    store.clientEnet.event = event;
    store.clientEnet.data = data;
    store.clientEnet.elapsedMs = elapsedMs;
    store.clientEnet.sampledAt = Now();
    strncpy_s(store.clientEnet.reason, reason, _TRUNCATE);
}

Connection Flow()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.connection;
}

int LinkedCount()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.linked;
}

TrafficCounters Traffic()
{
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.published.traffic;
}

void Status(void (*log)(char const*))
{
    char line[900];
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        FormatPublishedStatus(store.published, store.pollTimer != nullptr, line);
    }
    log(line);
}
}
