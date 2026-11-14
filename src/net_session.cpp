// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "log.h"
#include "saving_policy.h"
#include "story_policy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3t::net::internal {

ENetHost* host = nullptr;
ENetPeer* server = nullptr;
ENetAddress target{};
uint32_t lastConnectAttempt = 0;
uint32_t startedAt = 0;
bool steamLinked = false;
uint64_t hostRelayGeneration = 0;
bool linkEnding = false;
bool connectLogged = false;
bool everConnected = false;
bool isHost = false;
uint8_t localId = 0;
bool connected = false;
bool refused = false;
char refusalReason[160] = {};
uint16_t helloVersion = 0;
char localName[24] = "Player";
int sendRate = 30;
uint32_t budgetRate = DefaultBandwidthBytesPerSecond;
uint32_t relayEnvelopeBytes = 0;
PeerBudget peerBudgets[MaxPlayers];
playerwire::Encoder playerSenders[MaxPlayers][MaxPlayers];
playerwire::Decoder playerReceivers[MaxPlayers];
uint32_t playerRequestAt[MaxPlayers] = {};
PlayerState currentLocal{};
bool haveCurrentLocal = false;
playerwire::Cadence playerCadence;
bool forcePlayerFull[MaxPlayers][MaxPlayers] = {};
Remote remotes[MaxPlayers];
uint32_t worldEpoch = 0;
detail::BandwidthCounters bandwidth;
Handlers handlers{};
WorldHandler& worldHandler = handlers.world;
PlayerStateHandler& playerStateHandler = handlers.playerState;
WorldStateHandler& worldStateHandler = handlers.worldState;
uint32_t hitSequence = 0;
replication::Sequence hitClaims[MaxPlayers];
ENetPeer* peers[MaxPlayers] = {};
uint64_t peerAccounts[MaxPlayers] = {};
uint64_t peerRelayGenerations[MaxPlayers] = {};
bool inWorld = false;
options::policy::Snapshot hostOptions;
bool clientInWorld[MaxPlayers] = {};
uint32_t generations[MaxPlayers] = {};
std::vector<uint8_t> goodsCache[MaxPlayers];
uint8_t goodsReplay[MaxPlayers] = {};
DirectPunchSession directSessions[MaxPlayers] = {};
latency::Samples enqueueDelays, serviceGaps;
unsigned serviceCaps = 0, serviceErrors = 0;
bool activitySent = false;
uint32_t lastActivitySend = 0;
char startupError[192] = {};
bool enetInitialized = false;
uint32_t welcomes = 0;
uint32_t joins = 0;
uint32_t worldEntries = 0;
uint32_t budgetLossAt[MaxPlayers] = {};
uint32_t budgetLastLost[MaxPlayers] = {};
std::unordered_map<uint64_t, uint8_t> rememberedSlots;
unsigned gatedIn = 0, gatedOut = 0;
bool haveQueued = false;
uint32_t queuedAt = 0, lastServiceAt = 0;
uint32_t peerConnectData[MaxPlayers] = {};
PoseRoute poseRoutes[MaxPlayers] = {};

namespace {
bool ConstantTimeCookieEqual(uint8_t const a[16], uint8_t const b[16])
{
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
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

uint32_t QueueBytes(ENetList const& list)
{
    uint64_t bytes = 0;
    for (auto* node = list.sentinel.next; node != &list.sentinel; node = node->next)
        bytes += reinterpret_cast<ENetOutgoingCommand const*>(node)->fragmentLength;
    return static_cast<uint32_t>((std::min)(bytes, uint64_t(UINT32_MAX)));
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
        if (data[25] != expectedPeerRole) return 1;
        const uint8_t flags = data[24];
        for (auto& session : directSessions) {
            if (!session.account || !session.generation || session.directFailed) continue;
            if (!ConstantTimeCookieEqual(data + 8, session.cookie)) continue;
            const bool sameEndpoint = session.inboundVerified &&
                session.verifiedAddress.host == from.host &&
                session.verifiedAddress.port == from.port;
            if (session.endpointLocked && !sameEndpoint) return 1;
            if (!sameEndpoint) {
                session.inboundVerified = true;
                session.verifiedAddress = from;
                steam::RegisterDirectPeerEndpoint(session.account, session.generation, from.host, from.port);
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
    for (auto& session : directSessions)
        if (session.account == account && session.generation == generation)
            session.endpointLocked = true;
    return true;
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

BudgetClass SendPriority(uint8_t kind, bool reliable)
{
    if (reliable) return BudgetClass::GameplayReliable;
    if (kind == MsgState) return BudgetClass::PlayerState;
    return BudgetClass::NpcActive;
}

void UpdateBudgets(uint32_t now)
{
    for (size_t i = 0; i < host->peerCount; ++i) {
        ENetPeer const& peer = host->peers[i];
        if (peer.state != ENET_PEER_STATE_CONNECTED) continue;
        BudgetTelemetry sample;
        const uint64_t queued = uint64_t(QueueBytes(peer.outgoingCommands)) + QueueBytes(peer.outgoingSendReliableCommands);
        sample.queuedPayloadBytes = static_cast<uint32_t>((std::min)(queued, uint64_t(UINT32_MAX)));
        sample.reliableInFlightBytes = peer.reliableDataInTransit;
        sample.reliableWindowBytes = (std::max)(peer.mtu, peer.packetThrottle * peer.windowSize / ENET_PEER_PACKET_THROTTLE_SCALE);
        sample.rttMs = peer.roundTripTime;
        sample.throttle = peer.packetThrottle;
        sample.validRtt = peer.lastReceiveTime != 0 && peer.packetsSent >= 32;
        if (peer.packetsLost != budgetLastLost[i]) {
            budgetLastLost[i] = peer.packetsLost;
            budgetLossAt[i] = now;
        }
        sample.packetsSent = peer.packetsSent;
        sample.packetsLost = peer.packetsLost;
        sample.lossPermille = peer.packetsSent >= 32
            ? static_cast<uint32_t>((std::min)(uint64_t(1000), uint64_t(peer.packetsLost) * 1000 / peer.packetsSent)) : 0;
        sample.validLoss = peer.packetsSent >= 32;
        sample.retrySampleAt = budgetLossAt[i];
        peerBudgets[i].Advance(now, sample);
    }
}

bool StoryHandshake(void const* data, size_t size)
{
    if (size < sizeof(StatesHeader) || static_cast<uint8_t const*>(data)[0] != MsgStates) return false;
    StatesHeader header{};
    std::memcpy(&header, data, sizeof(header));
    const uint8_t kind = header.kind & story::policy::KindMask;
    return (header.kind & story::policy::Snapshot) && kind >= StateQuests && kind <= StateWorldClock &&
           header.count <= MaxStatesPerMessage && size == sizeof(header) + header.count * sizeof(StateEntry) &&
           (!(header.kind & story::policy::End) || !header.count) &&
           (!(header.kind & story::policy::Catchup) || (header.count && !(header.kind & story::policy::End)));
}

bool DoorHandshake(void const* data, size_t size)
{
    if (size != sizeof(WorldStateMsg) || static_cast<uint8_t const*>(data)[0] != MsgWorldState) return false;
    WorldStateMsg message{};
    std::memcpy(&message, data, sizeof(message));
    return message.state.kind == 10 && (message.state.npcClass == 0x80 ||
           ((message.state.npcClass & 0x40) && !(message.state.npcClass & ~0x43)));
}

bool XpHandshake(void const* data, size_t size)
{
    if (size != sizeof(WorldStateMsg) || static_cast<uint8_t const*>(data)[0] != MsgWorldState) return false;
    WorldStateMsg message;
    std::memcpy(&message, data, sizeof(message));
    return message.state.kind == 19 && message.state.dialog >= 1 && message.state.dialog <= 3;
}

bool SendPlayerState(ENetPeer* peer, PlayerState const& state)
{
    const uint8_t to = DestinationId(peer);
    if (!to || to >= MaxPlayers || !state.id || state.id >= MaxPlayers) return false;
    auto& encoder = playerSenders[to][state.id];
    playerwire::Packet encoded;
    const uint32_t now = enet_time_get();
    if (encoder.Plan(state, now, forcePlayerFull[to][state.id], encoded) != playerwire::Status::Ok) {
        Logf("net: local player state refused by compact encoder player=%u", state.id);
        return false;
    }
    std::vector<uint8_t> message;
    message.reserve(1 + encoded.bytes.size());
    message.push_back(MsgState);
    message.insert(message.end(), encoded.bytes.begin(), encoded.bytes.end());
    if (!Send(peer, message.data(), message.size(), ChannelState, encoded.reliable)) return false;
    encoder.Commit(state, now, encoded);
    forcePlayerFull[to][state.id] = false;
    return true;
}

bool Sane(PlayerState& state)
{
    state.name[sizeof(state.name) - 1] = 0;
    state.animation[sizeof(state.animation) - 1] = 0;
    state.heldRight[sizeof(state.heldRight) - 1] = 0;
    state.heldLeft[sizeof(state.heldLeft) - 1] = 0;
    state.shape[sizeof(state.shape) - 1] = 0;
    state.armor[sizeof(state.armor) - 1] = 0;
    state.helmet[sizeof(state.helmet) - 1] = 0;
    state.look[sizeof(state.look) - 1] = 0;
    if (state.gold < 0) state.gold = 0;
    for (float v : state.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    for (float v : state.rotation) if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
    return true;
}

unsigned ClientCount()
{
    unsigned count = 0;
    for (uint8_t i = 2; i < MaxPlayers; ++i) if (peers[i]) ++count;
    return count;
}

void HandleHello(ENetPeer* peer, bool exactSize, uint16_t version, char const* name, uint64_t const* parityItems)
{
    if (peer->data) { Log("net: a second hello from a player who joined already; ignored"); return; }
    RefusedMsg refusal{MsgRefused, {}};
    if (version != saving::Protocol || !exactSize) {
        Format(refusal.reason, "different G3 Together version (protocol %u, host %u)", version, saving::Protocol);
    } else {
        uint64_t const* ours = parity::Local();
        for (int i = 0; i != parity::ItemCount; ++i)
            if (parityItems[i] != ours[i]) {
                Format(refusal.reason, "install differs from the host: %s; install the host's %s",
                       parity::ItemName(i), i == parity::Options ? "G3 Together build and unsupported [Game] values" :
                       i == parity::ScriptsFolder ? "script DLLs/mod package" :
                       i == parity::DataRules || i == parity::DataOther ? "data archives/mods" : "game version/patches");
                break;
            }
        if (!refusal.reason[0] && !options::policy::Valid(hostOptions))
            strcpy_s(refusal.reason, "host balancing is invalid; host must repair its game/mod options and restart");
    }
    if (refusal.reason[0]) {
        Logf("net: refused %s: %s", name, refusal.reason);
        Send(peer, &refusal, sizeof(refusal), ChannelReliable, true);
        enet_peer_disconnect_later(peer, 1);
        return;
    }
    uint8_t id = 0;
    uint64_t account = 0;
    uint64_t relayGeneration = 0;
    char displayName[24] = {};
    if (!ClaimAuthenticatedPeerIdentity(peer, account, displayName, relayGeneration) ||
        !account || account == steam::PlayerId()) {
        Logf("identity: refused invalid steamid=%llu source=transport-authenticated", account);
        strcpy_s(refusal.reason, "authenticated Steam identity unavailable");
        Send(peer, &refusal, sizeof(refusal), ChannelReliable, true);
        enet_peer_disconnect_later(peer, 1);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && peerAccounts[i] == account) {
            const unsigned count = ClientCount();
            Logf("peer: replace slot=%u steamid=%llu reason=new-authenticated-connection clients=%u->%u",
                 i, account, count, count - 1);
            LeavePeer(peers[i], "replaced", true);
        }
    const auto remembered = rememberedSlots.find(account);
    if (remembered != rememberedSlots.end() && !peers[remembered->second]) id = remembered->second;
    for (uint8_t i = 2; i < MaxPlayers && !id; ++i) if (!peers[i]) id = i;
    if (!id) { enet_peer_disconnect(peer, 2); return; }
    const unsigned before = ClientCount();
    peers[id] = peer;
    peerAccounts[id] = account;
    peerRelayGenerations[id] = relayGeneration;
    rememberedSlots[account] = id;
    for (auto& sender : playerSenders[id]) sender = {};
    for (auto& forced : forcePlayerFull[id]) forced = false;
    ++generations[id];
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
    remotes[id].connected = true;
    clientInWorld[id] = false;
    strncpy_s(remotes[id].latest.name, displayName, _TRUNCATE);
    const Welcome welcome{MsgWelcome, id, hostOptions};
    Send(peer, &welcome, sizeof(welcome), ChannelReliable, true);
    Logf("net: %s joined as player %u", name, id);
    Logf("identity: join steamid=%llu display=%s source=transport-authenticated slot=%u", account, displayName, id);
    Logf("peer: join slot=%u steamid=%llu reason=authenticated clients=%u->%u", id, account, before, ClientCount());
}

void HandleSaveMessage(ENetPeer* peer, ENetPacket const* packet)
{
    const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
    if ((isHost && (from < 2 || from >= MaxPlayers || peers[from] != peer)) || (!isHost && (!connected || peer != server))) return;
    if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
    SaveMsg message{};
    std::memcpy(&message, packet->data, sizeof(message));
    SaveNote const& note = message.note;
    if (note.world[32] || note.save[32] || note.name[48] || note.ok > 1) return;
    const bool clientOp = note.op == SaveRequest || note.op == SaveReady || note.op == SaveCancel;
    const bool hostOp = note.op == SavePrepare || note.op == SaveCancel || note.op == SaveResult;
    if ((isHost && !clientOp) || (!isHost && !hostOp)) return;
    const bool pendingCancel = isHost && note.op == SaveCancel && !*note.world;
    if (!note.epoch || !saving::Id(note.save) || (!pendingCancel && !saving::Id(note.world))) return;
    if (handlers.save) handlers.save(from, note);
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

bool StartFailed(char const* reason)
{
    strncpy_s(startupError, reason, _TRUNCATE);
    Logf("net: Steam-only connection failed: %s", startupError);
    if (!isHost)
        Logf("connect: client link ended: reason=%s stage=startup enet_state=%s elapsed_ms=0",
             startupError, EnetPeerStateName(server ? server->state : ENET_PEER_STATE_DISCONNECTED));
    Stop();
    return false;
}

void DropClientRemotes()
{
    for (uint8_t i = 1; i < MaxPlayers; ++i) {
        if (i != localId && remotes[i].connected && handlers.peerLeft)
            handlers.peerLeft(i, remotes[i].latest.name);
        Drop(i);
    }
}

void ProcessRelayDrops()
{
    for (auto const& drop : steam::TakeRelayDrops()) {
        if (!isHost) {
            if (hostRelayGeneration && hostRelayGeneration == drop.generation) {
                Logf("peer: leave slot=1 steamid=%llu reason=%s clients=%u->0",
                     drop.account, drop.reason, connected ? 1u : 0u);
                Logf("enet: relay closed role=client reason=%s state=%s elapsed_ms=%u",
                     drop.reason, EnetPeerStateName(server ? server->state : ENET_PEER_STATE_DISCONNECTED),
                     startedAt ? enet_time_get() - startedAt : 0);
                CloseClientLink();
            }
            continue;
        }
        for (uint8_t id = 2; id < MaxPlayers; ++id)
            if (peers[id] && peerAccounts[id] == drop.account && peerRelayGenerations[id] == drop.generation)
                LeavePeer(peers[id], drop.reason, true);
    }
}

size_t PeerIndex(ENetPeer const* peer) { return static_cast<size_t>(peer - host->peers); }

uint8_t PeerSlotOf(ENetPeer const* peer)
{
    if (!peer) return 0;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(peer->data);
    return slot >= 2 && slot < MaxPlayers && peers[slot] == peer ? static_cast<uint8_t>(slot) : 0;
}

uint8_t DestinationId(ENetPeer const* peer)
{
    return isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
}

void ResetBudget(ENetPeer* peer)
{
    BudgetConfig config;
    config.bytesPerSecond = budgetRate;
    config.mtuBytes = peer->mtu;
    config.envelopeBytes = relayEnvelopeBytes;
    config.checksumBytes = host->checksum ? 4 : 0;
    peerBudgets[PeerIndex(peer)].Reset(enet_time_get(), config);
    budgetLossAt[PeerIndex(peer)] = budgetLastLost[PeerIndex(peer)] = 0;
}

bool Open(ENetPeer* peer)
{
    if (!isHost) return inWorld;
    const uintptr_t id = reinterpret_cast<uintptr_t>(peer->data);
    return id > 1 && id < MaxPlayers && clientInWorld[id];
}

void SendToServer(void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (!isHost && server && connected) Send(server, data, size, channel, reliable);
}

void SendToPeer(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t to)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    if (to < MaxPlayers && peers[to]) Send(peers[to], data, size, channel, reliable);
}

void SendBroadcast(void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], data, size, channel, reliable);
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

void FinishClientConnectAttempt(DirectPunchSession* direct, bool useDirect, uint32_t now)
{
    lastConnectAttempt = now;
    ENetAddress connectTo = useDirect ? direct->verifiedAddress : target;
    if (useDirect && !server) {
        direct->directFailed = true;
        direct->connectStarted = false;
        direct->endpointLocked = false;
        if (!direct->fallbackLogged) {
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

void CloseClientLink()
{
    steamLinked = false;
    hostRelayGeneration = 0;
    startedAt = 0;
    if (server) {
        enet_peer_disconnect_now(server, 0);
        enet_host_flush(host);
        server = nullptr;
    }
    connected = false;
    inWorld = false;
    options::Restore();
    DropClientRemotes();
}

} // namespace g3t::net::internal

namespace g3t::net {

uint32_t HashWorld(char const* name)
{
    uint32_t hash = 2166136261u;
    for (; *name; ++name) hash = (hash ^ static_cast<uint8_t>(*name)) * 16777619u;
    return hash;
}

uint32_t Now() { return enet_time_get(); }
uint32_t WorldEpoch() { return worldEpoch; }
uint8_t LocalId() { return localId; }
bool IsHost() { return isHost; }
uint32_t JoinCount() { return joins; }
uint32_t WorldEntryCount() { return worldEntries; }
void RecordNpcSendDecision(NpcSendDecision decision, uint32_t count) { bandwidth.Npc(decision, count); }
bool InWorld(uint8_t id) { return isHost ? (id == 1 || (id < MaxPlayers && clientInWorld[id])) : inWorld; }
bool Joined() { return connected; }
char const* RefusalReason() { return refusalReason; }
uint32_t ConnectingFor() { return isHost || everConnected || !host || !steamLinked ? 0 : enet_time_get() - startedAt; }
void ClaimVersionForTest(uint16_t version) { helloVersion = version; }
void PrimePoseWrapForTest()
{
    if (!isHost) return;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) peers[i]->channels[ChannelPoses].outgoingUnreliableSequenceNumber = 0xfffe;
}
uint32_t WelcomeSerial() { return welcomes; }
void AdvanceWelcomeSerialForTest() { ++welcomes; }
bool PeerJoined(uint8_t id) { return isHost && id > 1 && id < MaxPlayers && peers[id]; }
uint32_t PeerGeneration(uint8_t id) { return id < MaxPlayers ? generations[id] : 0; }
char const* PeerName(uint8_t id) { return id < MaxPlayers ? remotes[id].latest.name : ""; }
void GateCounts(unsigned& in, unsigned& out) { in = gatedIn; out = gatedOut; }
uint32_t HashName(char const* name) { return HashWorld(name); }

uint32_t HashGuid(uint8_t const guid[16])
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; ++i) {
        hash ^= guid[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

bool SendWorldStateTo(uint8_t to, WorldState const& state)
{
    if (state.kind == 19) {
        if (state.dialog > 3 || state.npcClass != localId) return false;
    } else if (state.kind != 10 || (isHost ? !(state.npcClass & 0x40) : state.npcClass != 0x80)) return false;
    const WorldStateMsg message{MsgWorldState, state};
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to]) return false;
        return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    }
    return connected && server && to == 1 && Send(server, &message, sizeof(message), ChannelReliable, true);
}

bool Connected() { return isHost ? connected : connected && inWorld; }
Remote const* Remotes() { return remotes; }

void Stop()
{
    options::Restore();
    if (host) {
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
        } else if (server) {
            enet_peer_disconnect_now(server, 0);
        }
        enet_host_flush(host);
    }
    steam::Stop();
    steam::TakeRelayDrops();
    if (host) enet_host_destroy(host);
    host = nullptr;
    server = nullptr;
    haveQueued = false;
    lastServiceAt = 0;
    steamLinked = false;
    hostRelayGeneration = 0;
    linkEnding = false;
    connectLogged = false;
    everConnected = false;
    refused = false;
    refusalReason[0] = '\0';
    haveCurrentLocal = false;
    playerCadence = {};
    connected = inWorld = false;
    activitySent = false;
    lastActivitySend = 0;
    localId = 0;
    rememberedSlots.clear();
    for (auto& session : directSessions) session = DirectPunchSession{};
    for (auto& route : poseRoutes) route = PoseRoute{};
    for (auto& tag : peerConnectData) tag = 0;
    ResetNpcReplication();
    for (uint8_t i = 0; i < MaxPlayers; ++i) Drop(i);
    if (enetInitialized) {
        enet_deinitialize();
        enetInitialized = false;
    }
    handlers = {};
}

char const* StartupError() { return startupError; }

} // namespace g3t::net
