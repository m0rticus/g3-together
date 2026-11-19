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

void ClearPeerPlayerTables(uint8_t id)
{
    if (id >= MaxPlayers) return;
    for (auto& sender : playerSenders[id]) sender = {};
    for (auto& senders : playerSenders) senders[id] = {};
    for (auto& forced : forcePlayerFull[id]) forced = false;
    for (auto& forced : forcePlayerFull) forced[id] = false;
}

void ClearAllPlayerTables()
{
    for (auto& senders : playerSenders) for (auto& sender : senders) sender = {};
    for (auto& forced : forcePlayerFull) for (auto& flag : forced) flag = false;
}

bool StoryHandshake(void const* data, size_t size)
{
    if (size < sizeof(StatesHeader) || static_cast<uint8_t const*>(data)[0] != MsgStates) return false;
    StatesHeader header{};
    std::memcpy(&header, data, sizeof(header));
    const uint8_t kind = header.kind & story::policy::KindMask;
    return (header.kind & story::policy::Snapshot) && kind >= StateQuests && kind <= StateReputation &&
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
    return playerwire::detail::finiteCoordsAndRotation(state);
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
    if (peer->address.host != target.host)
        LockDirectSessionEndpoint(account, relayGeneration, true);
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
    const uint8_t from = isHost ? PeerSlotOf(peer) : 1;
    if ((isHost && !from) || (!isHost && (!connected || peer != server))) return;
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
    if (!isHost) ResetNpcReplication();
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

size_t PeerIndex(ENetPeer const* peer)
{
    if (!host || !peer || !host->peers || peer < host->peers) return MaxPlayers;
    const size_t limit = host->peerCount ? (std::min)(host->peerCount, size_t(MaxPlayers)) : size_t(MaxPlayers);
    const size_t index = static_cast<size_t>(peer - host->peers);
    return index < limit ? index : MaxPlayers;
}

uint8_t PeerSlotOf(ENetPeer const* peer)
{
    if (!peer) return 0;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(peer->data);
    return slot >= 2 && slot < MaxPlayers && peers[slot] == peer ? static_cast<uint8_t>(slot) : 0;
}

uint8_t DestinationId(ENetPeer const* peer)
{
    return isHost ? PeerSlotOf(peer) : 1;
}

bool Open(ENetPeer* peer)
{
    if (!isHost) return inWorld;
    const uint8_t id = PeerSlotOf(peer);
    return id != 0 && clientInWorld[id];
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

void SendBroadcastExcept(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t except)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != except) Send(peers[i], data, size, channel, reliable);
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
