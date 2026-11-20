// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "ai.h"
#include "downed.h"
#include "downed_policy.h"
#include "log.h"
#include "movement.h"
#include "saving_policy.h"
#include "saving_store.h"
#include "story_policy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3t::net::internal {

constexpr uint16_t ProtocolVersion = saving::Protocol;

#pragma pack(push, 1)
struct Hello { uint8_t type; uint16_t version; char name[24]; uint64_t parity[parity::ItemCount]; };
#pragma pack(pop)
static_assert(sizeof(Hello) == 83, "hello wire layout changed");
static_assert(sizeof(ActivityMsg) == 19, "player activity wire layout changed");
static_assert(MaxPlayers <= 8, "goods replay bitset must cover every player slot");

namespace {
uint32_t sequence = 0;
uint8_t lastActivityFlags = 0;
uint32_t lastActivityDowned = 0;
uint32_t bandwidthSince = 0;
uint32_t lastSentData = 0, lastSentPackets = 0, lastReceivedData = 0, lastReceivedPackets = 0;
steam::TrafficCounters lastSteamTraffic;
constexpr enet_uint16 PoseEpochLimit = 0xff00;
constexpr enet_uint32 PoseRestMs = 3000;

[[maybe_unused]] size_t ENET_CALLBACK CompressDatagram(void* context, ENetBuffer const* buffers, size_t count,
                                                       size_t inputBytes, enet_uint8* output, size_t outputBytes)
{
    if (!context || !buffers || !count || !inputBytes || !output || !outputBytes) {
        Log("guard: ENet compression missing context/input/output; codec refused");
        return 0;
    }
    size_t empty = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!buffers[i].dataLength) ++empty;
        else if (!buffers[i].data) {
            Logf("guard: ENet compression null payload buffer=%zu bytes=%zu; codec refused",
                 i, buffers[i].dataLength);
            return 0;
        }
    }
    if (empty)
        Logf("guard: ENet compression empty_buffers=%zu input_bytes=%zu action=skip-empty-preserve-checkpoint",
             empty, inputBytes);
    return enet_range_coder_compress(context, buffers, count, inputBytes, output, outputBytes);
}

size_t ENET_CALLBACK DecompressDatagram(void* context, enet_uint8 const* input, size_t inputBytes,
                                        enet_uint8* output, size_t outputBytes)
{
    if (!context || !input || !inputBytes || !output || !outputBytes) {
        Log("guard: ENet decompression missing context/input/output; codec refused");
        return 0;
    }
    return enet_range_coder_decompress(context, input, inputBytes, output, outputBytes);
}

bool ReadAuthenticatedPeerIdentity(ENetPeer const* peer, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!peer) return false;
    if (peer->address.host == target.host && steam::PeerIdentity(peer->address.port, account, name, generation))
        return true;
    return steam::DirectPeerIdentity(peer->address.host, peer->address.port, account, name, generation);
}

bool Gated(uint8_t type)
{
    switch (type) {
    case MsgHello: case MsgWelcome: case MsgRefused: case MsgLeave:
    case MsgWorld: case MsgSave: case MsgCharacter:
    case MsgPlayerActivity:
        return false;
    default:
        return true;
    }
}

void FlushGoodsReplays()
{
    if (!isHost) return;
    for (uint8_t to = 2; to < MaxPlayers; ++to) {
        if (!peers[to] || !clientInWorld[to]) { goodsReplay[to] = 0; continue; }
        for (uint8_t from = 2; from < MaxPlayers; ++from) {
            const uint8_t bit = static_cast<uint8_t>(1u << from);
            if (!(goodsReplay[to] & bit)) continue;
            auto const& snapshot = goodsCache[from];
            if (!clientInWorld[from] || snapshot.empty() ||
                Send(peers[to], snapshot.data(), snapshot.size(), ChannelReliable, true))
                goodsReplay[to] &= static_cast<uint8_t>(~bit);
        }
    }
}

void Store(PlayerState const& state)
{
    if (state.id == 0 || state.id >= MaxPlayers || state.id == localId) return;
    Remote& r = remotes[state.id];
    r.activity.Accept(state.sequence, enet_time_get(), state.world, worldEpoch, state.flags, state.downedSequence);
    if (r.latestTicks && static_cast<int32_t>(state.sequence - r.latest.sequence) <= 0) {
        if ((r.latest.flags & 14) != (state.flags & 14))
            Logf("downed: remote stale player=%u packet=%u current=%u flags=%u current_flags=%u source=player-state",
                 state.id, state.sequence, r.latest.sequence, state.flags, r.latest.flags);
        return;
    }
    if (!r.latestTicks || (r.latest.flags & 14) != (state.flags & 14) || r.latest.downedSequence != state.downedSequence)
        Logf("downed: remote player=%u packet=%u downed_sequence=%u flags=%u previous=%u source=player-state",
             state.id, state.sequence, state.downedSequence, state.flags, r.latest.flags);
    const bool reset = !r.latestTicks || r.latest.world != state.world || std::strcmp(r.latest.shape, state.shape) ||
                       std::strcmp(r.latest.look, state.look);
    r.previous = r.latestTicks ? r.latest : state;
    r.previousTicks = r.latestTicks ? r.latestTicks : enet_time_get();
    r.latest = state;
    r.latestTicks = enet_time_get();
    r.connected = true;
    if (playerStateHandler) playerStateHandler(state, r.latestTicks, reset);
}

void ClearPresentation(uint8_t id)
{
    Remote cleared{};
    cleared.activity = remotes[id].activity;
    cleared.connected = remotes[id].connected;
    strncpy_s(cleared.latest.name, remotes[id].latest.name, _TRUNCATE);
    remotes[id] = cleared;
}
} // namespace

void LeavePeer(ENetPeer* peer, char const* reason, bool disconnect)
{
    if (!peer) return;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(peer->data);
    if (slot < 2 || slot >= MaxPlayers || peers[slot] != peer) return;
    const auto id = static_cast<uint8_t>(slot);
    const uint64_t account = peerAccounts[id];
    const uint64_t relayGeneration = peerRelayGenerations[id];
    const unsigned before = ClientCount();
    char name[24];
    strncpy_s(name, remotes[id].latest.name, _TRUNCATE);
    peer->data = nullptr;
    if (disconnect) enet_peer_disconnect_now(peer, 0);
    Drop(id);
    LockDirectSessionEndpoint(account, relayGeneration, false);
    if (!disconnect) steam::RequestHostRelayClose(account, relayGeneration, reason);
    const Leave leave{MsgLeave, id};
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &leave, sizeof(leave), ChannelState, true);
    Logf("peer: leave slot=%u steamid=%llu reason=%s clients=%u->%u",
         id, account, reason, before, ClientCount());
    if (handlers.peerLeft) handlers.peerLeft(id, name);
}

bool Send(ENetPeer* peer, void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (!host || !peer || (size && !data)) return false;
    if (size && Gated(static_cast<uint8_t const*>(data)[0]) && !XpHandshake(data, size) &&
        !(reliable && (StoryHandshake(data, size) || DoorHandshake(data, size))) && !Open(peer)) { ++gatedOut; return false; }
    const size_t peerIdx = PeerIndex(peer);
    if (peerIdx >= MaxPlayers) return false;
    const BudgetClass priority = SendPriority(size ? static_cast<uint8_t const*>(data)[0] : 0, reliable);
    const uint64_t charge = BudgetCharge(size, peer->mtu, relayEnvelopeBytes, host->checksum ? 4 : 0);
    PeerBudget& budget = peerBudgets[peerIdx];
    if (channel == ChannelPoses && !reliable) {
        PoseRoute& route = poseRoutes[peerIdx];
        uint8_t active = route.alt ? ChannelPosesAlt : ChannelPoses;
        if (peer->channels[active].outgoingUnreliableSequenceNumber >= PoseEpochLimit) {
            const uint8_t next = route.alt ? ChannelPoses : ChannelPosesAlt;
            const enet_uint32 now = enet_time_get() | 1u;
            if (route.checkpointed[next - ChannelPoses] && now - route.checkpointed[next - ChannelPoses] < PoseRestMs) return false;
            // Only an empty reliable packet resets the 16-bit unreliable epoch, and the receiver holds every later packet
            // of that channel until it arrives. Reset the exhausted channel while the stream already runs on the other.
            ENetPacket* checkpoint = enet_packet_create(nullptr, 0, ENET_PACKET_FLAG_RELIABLE);
            if (!checkpoint) { Log("net: pose sequence checkpoint allocation failed"); return false; }
            if (enet_peer_send(peer, active, checkpoint) < 0) {
                enet_packet_destroy(checkpoint);
                Log("net: pose sequence checkpoint could not be queued");
                return false;
            }
            route.checkpointed[active - ChannelPoses] = now;
            route.alt = !route.alt;
            active = next;
            Logf("net: pose sequence checkpoint queued; the pose stream moved to channel %u", static_cast<unsigned>(active));
        }
        channel = active;
    }
    ENetPacket* packet = enet_packet_create(data, size, PacketFlags(reliable));
    if (!packet) { Log("net: outgoing packet allocation failed"); return false; }
    const int queued = enet_peer_send(peer, channel, packet);
    if (queued < 0) {
        enet_packet_destroy(packet);
        Log("net: outgoing packet could not be queued");
        return false;
    }
    budget.CommitQueued(priority, charge);
    if (!haveQueued) { haveQueued = true; queuedAt = enet_time_get(); }
    if (size) bandwidth.Tx(static_cast<uint8_t const*>(data)[0], size);
    return true;
}

void Drop(uint8_t id)
{
    if (id == 0 || id >= MaxPlayers) return;
    ForgetBonePeer(id);
    ForgetControlPeer(id);
    remotes[id] = Remote{};
    playerReceivers[id] = {};
    playerRequestAt[id] = 0;
    ClearPeerPlayerTables(id);
    peers[id] = nullptr;
    peerAccounts[id] = 0;
    peerRelayGenerations[id] = 0;
    clientInWorld[id] = false;
    goodsCache[id].clear();
    goodsReplay[id] = 0;
    for (auto& pending : goodsReplay) pending &= static_cast<uint8_t>(~(1u << id));
    npcOrigins[id].clear();
    hitClaims[id] = replication::Sequence{};
}

namespace {
void OnReceive(ENetPeer* peer, ENetPacket* packet)
{
    if (packet->dataLength) bandwidth.Rx(packet->data[0], packet->dataLength);
    if (isHost && !PeerRelayOrDirect(peer)) return;
    if (isHost && peer->data) {
        const uintptr_t slot = reinterpret_cast<uintptr_t>(peer->data);
        uint64_t account = 0, relayGeneration = 0;
        char name[24] = {};
        if (slot < 2 || slot >= MaxPlayers || peers[slot] != peer ||
            !ReadAuthenticatedPeerIdentity(peer, account, name, relayGeneration) ||
            !peerAccounts[slot] || peerAccounts[slot] != account ||
            peerRelayGenerations[slot] != relayGeneration) {
            Log("identity: refused relay/account mismatch");
            enet_peer_disconnect_later(peer, 1);
            return;
        }
    }
    if (packet->dataLength < 1) return;
    const uint8_t type = packet->data[0];
    if (type == MsgPosedNpc || type == MsgCompactPosedNpc || type == MsgControlPose) ++poseReceived;
    const bool xpHandshake = XpHandshake(packet->data, packet->dataLength);
    const bool storyHandshake = (packet->flags & ENET_PACKET_FLAG_RELIABLE) &&
                               (StoryHandshake(packet->data, packet->dataLength) || DoorHandshake(packet->data, packet->dataLength));
    // Channel 0 can deliver the player's keyframe before channel 1 finishes entry.
    // Cache authenticated state now; puppet/world writes still wait for admission.
    const bool playerEntryCache = type == MsgState && !isHost && connected && peer == server;
    if (Gated(type) && !xpHandshake && !storyHandshake && !playerEntryCache && !Open(peer)) { ++gatedIn; return; }
    if (type == MsgHello && isHost && packet->dataLength >= 3) {
        Hello hello{};
        std::memcpy(&hello, packet->data, packet->dataLength < sizeof(hello) ? packet->dataLength : sizeof(hello));
        hello.name[sizeof(hello.name) - 1] = '\0';
        HandleHello(peer, packet->dataLength == sizeof(Hello), hello.version, hello.name, hello.parity);
    } else if (type == MsgRefused && !isHost && packet->dataLength == sizeof(RefusedMsg)) {
        RefusedMsg refusal;
        std::memcpy(&refusal, packet->data, sizeof(refusal));
        refusal.reason[sizeof(refusal.reason) - 1] = '\0';
        refused = true;
        options::Restore();
        strncpy_s(refusalReason, refusal.reason, _TRUNCATE);
        Logf("net: REFUSED by host: %s", refusal.reason);
    } else if (type == MsgWelcome && !isHost && packet->dataLength == sizeof(Welcome)) {
        Welcome welcome;
        std::memcpy(&welcome, packet->data, sizeof(welcome));
        if (welcome.id < 2 || welcome.id >= MaxPlayers) { Logf("net: welcome with player id %u; ignored", welcome.id); return; }
        if (peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        if (!options::Adopt(welcome.options, 0)) {
            refused = true;
            strcpy_s(refusalReason, "invalid host balancing or unavailable native option readers; install the same supported patches/build");
            options::Restore();
            Logf("net: REFUSED host options: %s", refusalReason);
            enet_peer_disconnect_later(peer, 1);
            return;
        }
        localId = welcome.id;
        connected = true;
        inWorld = false;
        activitySent = false;
        ++welcomes;
        ResetNpcReplication();
        Logf("net: welcomed as player %u (welcome %u)", localId, welcomes);
        Logf("identity: join steamid=%llu display=%s source=transport-authenticated slot=%u",
             steam::PlayerId(), steam::PlayerName(), localId);
    } else if (type == MsgPlayerActivity && packet->dataLength == sizeof(ActivityMsg)) {
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        ActivityMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        const uint8_t from = AuthenticatedSender(peer);
        if (!from) return;
        if (isHost) message.id = from;
        if (!message.id || message.id >= MaxPlayers || message.id == localId ||
            !downed::ValidActivityFlags(message.flags)) return;
        Remote& r = remotes[message.id];
        r.activity.Accept(message.sequence, enet_time_get(), message.world, message.epoch,
                          message.flags, message.downedSequence);
        r.connected = true;
        if (isHost)
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.id &&
                    !Send(peers[i], &message, sizeof(message), ChannelState, true))
                    Logf("net: player activity relay to player %u failed", i);
    } else if (type == MsgState) {
        if (!isHost && (!connected || peer != server)) return;
        if (packet->dataLength >= 1 + playerwire::FixedBytes &&
            (packet->data[2] || BoneWire16(packet->data + 1 + playerwire::FixedBytes - 2)) &&
            !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) {
            Log("net: unreliable player metadata refused");
            return;
        }
        uint8_t player = 0;
        if (!playerwire::PeekPlayerId(packet->data + 1, packet->dataLength - 1, player)) {
            Log("net: compact player state has an invalid header");
            return;
        }
        if (isHost) player = PeerSlotOf(peer);
        if (!player || player >= MaxPlayers || player == localId) return;
        PlayerState state;
        const auto status = playerReceivers[player].Decode(packet->data + 1, packet->dataLength - 1, state, player);
        if (status == playerwire::Status::NeedKeyframe) {
            ++remotes[player].metadataWaits;
            const uint32_t now = enet_time_get();
            if (!playerRequestAt[player] || now - playerRequestAt[player] >= 1000) {
                const PlayerResyncMsg request{MsgPlayerResync, player};
                if (Send(peer, &request, sizeof(request), ChannelReliable, true)) {
                    playerRequestAt[player] = now | 1u;
                    Logf("net: player metadata keyframe requested player=%u", player);
                }
            }
            return;
        }
        if (status == playerwire::Status::Stale) { ++remotes[player].staleStates; return; }
        if (status != playerwire::Status::Ok || !Sane(state)) {
            Logf("net: invalid compact player state player=%u", player);
            return;
        }
        if (isHost) strncpy_s(state.name, remotes[player].latest.name, _TRUNCATE);
        ++remotes[player].acceptedStates;
        if (!isHost && !inWorld && ++remotes[player].preEntryStates == 1)
            Logf("player-stream: player=%u action=cache-before-entry packet=%u world=%08X metadata_revision=%u",
                 player, state.sequence, state.world, playerReceivers[player].Revision());
        Store(state);
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != player) SendPlayerState(peers[i], state);
        }
    } else if (type == MsgPlayerResync && packet->dataLength == sizeof(PlayerResyncMsg) &&
               (packet->flags & ENET_PACKET_FLAG_RELIABLE)) {
        PlayerResyncMsg request{};
        std::memcpy(&request, packet->data, sizeof(request));
        const uint8_t to = isHost ? PeerSlotOf(peer) : 1;
        if (!to || to >= MaxPlayers || !request.player || request.player >= MaxPlayers ||
            (!isHost && (!connected || peer != server || request.player != localId))) return;
        forcePlayerFull[to][request.player] = true;
    } else if (type == MsgControlId) {
        HandleControlId(peer, packet);
    } else if (type == MsgControlSignal) {
        uint8_t peerId = 0;
        NpcRecord latest{};
        if (HandleControlSignal(peer, packet, peerId, latest))
            SendFullNpc(peers[peerId], latest, true);
    } else if (HandleNpcStreamMessage(peer, packet, type)) {
        return;
    } else if (type == MsgHitNpc && packet->dataLength == sizeof(HitNpcMsg)) {
        if (!isHost) { Log("net: client NPC hit publication refused"); return; }
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        HitNpcMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.from = PeerSlotOf(peer);
        static const uint8_t zero[16] = {};
        if (message.from < 2 || message.from >= MaxPlayers || message.to != 1 ||
            message.epoch != worldEpoch || !message.sequence ||
            !ValidHitContact(message.delta, message.incarnation, message.seen, message.outcome, message.reaction, message.contact) ||
            std::memcmp(message.npc, zero, 16)) { Log("net: invalid or stale NPC hit rejected"); return; }
        if (!hitClaims[message.from].Accept(message.sequence)) {
            Log("net: repeated NPC hit ignored; its original reliable result remains authoritative");
            return;
        }
        if (handlers.hitNpc && message.from)
            handlers.hitNpc(message.from, message.guid, message.delta, message.weapon, message.npc, message.outcome, message.seen,
                            message.reaction, message.sequence, message.incarnation, message.contact);
    } else if (type == MsgHitPlayer && packet->dataLength == sizeof(HitPlayerMsg)) {
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        HitPlayerMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.delta < -1000000 || message.delta > 0) return;
        if (isHost) {
            const uint8_t from = PeerSlotOf(peer);
            if (!from || !message.to || message.to >= MaxPlayers || message.to == from) return;
            if (message.attackerPlayer) message.attackerPlayer = from;
        } else if (!connected || peer != server || message.to != localId) {
            return;
        }
        if (isHost && downed::PlayerInteractionFlags(message.to)) {
            Logf("net: player hit refused target=%u reason=%s", message.to,
                 downed::InteractionReason(downed::PlayerInteractionFlags(message.to)));
            return;
        }
        if (isHost && message.to > 1 && message.to < MaxPlayers) {
            if (peers[message.to]) Send(peers[message.to], &message, sizeof(message), ChannelCombat, true);
            return;
        }
        if (handlers.hitPlayer)
            handlers.hitPlayer(message.delta, message.attacker, message.attackerPlayer, message.meansToKill != 0);
    } else if (type == MsgGoods && packet->dataLength >= sizeof(GoodsHeader)) {
        const uint8_t from = AuthenticatedSender(peer);
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || (!isHost && !from)) return;
        GoodsHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        if (header.count > MaxGoodsEntries || packet->dataLength != sizeof(header) + header.count * sizeof(GoodsEntry) || header.gold < 0)
            return;
        if (isHost) {
            if (!from) return;
            header.player = from;
            std::memcpy(packet->data, &header, sizeof(header));
        }
        if (header.player == 0 || header.player >= MaxPlayers || header.player == localId) return;
        GoodsEntry entries[MaxGoodsEntries];
        if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(GoodsEntry));
        for (int i = 0; i < header.count; ++i) {
            entries[i].name[sizeof(entries[i].name) - 1] = 0;
            if (entries[i].amount <= 0 || entries[i].amount > 1000000 || entries[i].quality >= 1024) return;
        }
        if (header.count) std::memcpy(packet->data + sizeof(header), entries, header.count * sizeof(GoodsEntry));
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != header.player) Send(peers[i], packet->data, packet->dataLength, ChannelReliable, true);
            if (header.epoch) {
                auto& cache = goodsCache[header.player];
                GoodsHeader previous{};
                if (!cache.empty()) std::memcpy(&previous, cache.data(), sizeof(previous));
                if (cache.empty() || header.epoch > previous.epoch ||
                    (header.epoch == previous.epoch && header.serial > previous.serial))
                    cache.assign(packet->data, packet->data + packet->dataLength);
            }
        }
        if (handlers.goods) handlers.goods(header.player, header.epoch, header.serial, header.gold, entries, header.count);
    } else if (type == MsgWorldState && packet->dataLength == sizeof(WorldStateMsg)) {
        const uint8_t from = AuthenticatedSender(peer);
        if (!from || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        WorldStateMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.state.routine[sizeof(message.state.routine) - 1] = 0;
        if (message.state.moved) {
            for (float v : message.state.position) if (!std::isfinite(v)) return;
            for (float v : message.state.rotation) if (!std::isfinite(v)) return;
        }
        if (isHost && message.state.kind != 19 && !ClientWorldStateKind(message.state.kind)) return;
        if (isHost && (message.state.kind == 8 || message.state.kind == 9))
            message.state.npcClass = from;
        if (message.state.kind == 19) {
            if (message.state.dialog > 3) return;
            if (isHost) {
                if (message.state.npcClass != from) return;
            } else if (from != 1 || message.state.npcClass != 1) {
                return;
            }
        }
        if (worldStateHandler) worldStateHandler(from, message.state);
    } else if (type == MsgCharacter && packet->dataLength >= sizeof(CharacterMsg)) {
        const uint8_t from = AuthenticatedSender(peer);
        if (!from || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        CharacterMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        auto const& note = message.note;
        const uint64_t authenticated = isHost ? PeerAccount(from) : steam::PlayerId();
        if (!authenticated || note.account != authenticated) {
            Logf("identity: character refused claimed=%llu authenticated=%llu slot=%u", note.account, authenticated, from);
            return;
        }
        if (note.world[32] || note.save[32] || !saving::Id(note.world) || !saving::Id(note.save) ||
            !note.epoch || !note.account || note.length > saving::store::PlayerLimit ||
            packet->dataLength - sizeof(message) != note.length ||
            (isHost ? note.purpose != CharacterCapture : note.purpose != CharacterRestore)) return;
        std::vector<uint8_t> bytes(packet->data + sizeof(message), packet->data + packet->dataLength);
        if (note.crc != saving::store::Crc(bytes)) {
            Logf("net: character record CRC refused player=%u save=%s bytes=%u", from, note.save, note.length);
            return;
        }
        if (handlers.character) handlers.character(from, note, bytes);
    } else if (HandleGameplayMessage(peer, packet, type)) {
        return;
    } else if (type == MsgWorld && (packet->dataLength == sizeof(WorldMsg) || packet->dataLength == sizeof(HostWorldMsg))) {
        WorldMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.campaign[sizeof(message.note.campaign) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || !message.note.epoch || message.note.kind > KindSaved ||
            (isHost ? from < 2 || from >= MaxPlayers || peers[from] != peer ||
                      (message.note.op != WorldReady && message.note.op != WorldWaiting && message.note.op != WorldOther)
                    : peer != server || !connected || (message.note.op != WorldHost && message.note.op != WorldReady))) return;
        if (!isHost && message.note.op == WorldHost) {
            HostWorldMsg announced{};
            if (packet->dataLength == sizeof(announced)) std::memcpy(&announced, packet->data, sizeof(announced));
            if (packet->dataLength != sizeof(announced) || !options::Adopt(announced.options, message.note.epoch)) {
                refused = true;
                connected = inWorld = false;
                strcpy_s(refusalReason, "invalid/missing host world balancing; install the same G3 Together build");
                options::Restore();
                Logf("net: REFUSED host options: %s", refusalReason);
                enet_peer_disconnect_later(peer, 1);
                return;
            }
        } else if (packet->dataLength != sizeof(WorldMsg)) return;
        if (!isHost && message.note.op == WorldHost && worldEpoch != message.note.epoch) {
            ResetNpcReplication();
            worldEpoch = message.note.epoch;
        }
        if (worldHandler && from) worldHandler(from, message.note);
        if (!isHost && message.note.op == WorldHost && !options::WorldAnnounced()) {
            refused = true;
            connected = inWorld = false;
            strcpy_s(refusalReason, "host balancing needs a fresh world entry; restart Gothic 3 and create the client rejoin template");
            options::Restore();
            Logf("net: REFUSED host entry: %s", refusalReason);
            enet_peer_disconnect_later(peer, 1);
        }
    }
}
} // namespace

} // namespace g3t::net::internal

namespace g3t::net {

void SetInWorld(uint8_t id, bool in)
{
    if (!isHost) {
        if (inWorld == in) return;
        inWorld = in;
        if (!in) for (auto& receiver : playerReceivers) receiver = {};
        ClearAllPlayerTables();
        if (!in) {
            for (uint8_t i = 0; i < MaxPlayers; ++i) ClearPresentation(i);
            ClearClientNpcTables();
        }
        if (in) ++worldEntries;
        Logf("net: this player is %s the host's world", in ? "in" : "out of");
        return;
    }
    if (id < 2 || id >= MaxPlayers || !peers[id] || clientInWorld[id] == in) return;
    if (in) {
        bool anyOtherInWorld = false;
        for (uint8_t other = 2; other < MaxPlayers; ++other)
            if (other != id && clientInWorld[other]) { anyOtherInWorld = true; break; }
        if (!anyOtherInWorld) {
            ResetNpcReplication();
        } else {
            ForgetBonePeer(id);
            ForgetControlPeer(id);
        }
    } else {
        ForgetBonePeer(id);
        ForgetControlPeer(id);
    }
    clientInWorld[id] = in;
    playerReceivers[id] = {};
    ClearPeerPlayerTables(id);
    if (in) {
        ++joins;
        for (uint8_t from = 2; from < MaxPlayers; ++from)
            if (from != id && clientInWorld[from] && !goodsCache[from].empty())
                goodsReplay[id] |= static_cast<uint8_t>(1u << from);
    } else {
        goodsCache[id].clear();
        goodsReplay[id] = 0;
        for (auto& pending : goodsReplay) pending &= static_cast<uint8_t>(~(1u << id));
        ClearPresentation(id);
        npcOrigins[id].clear();
    }
    Logf("net: player %u (%s) is %s the host's world", id, remotes[id].latest.name, in ? "in" : "out of");
}

uint64_t PeerAccount(uint8_t id)
{
    if (!isHost || id < 2 || id >= MaxPlayers || !peers[id]) return 0;
    uint64_t account = 0, generation = 0;
    char name[24] = {};
    return ReadAuthenticatedPeerIdentity(peers[id], account, name, generation) &&
        account == peerAccounts[id] && generation == peerRelayGenerations[id] ? account : 0;
}

uint32_t SendHitNpc(uint8_t to, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16], uint8_t outcome,
                    uint32_t seen, uint8_t reaction, uint32_t incarnation, MeleeContact const& contact)
{
    if (!host || isHost || !server || !connected || !inWorld || to != 1) {
        Log("net: NPC hit not sent: no ready authoritative host");
        return 0;
    }
    if (!ValidHitContact(delta, incarnation, seen, outcome, reaction, contact)) {
        Log("net: NPC hit not sent: invalid contact judgement");
        return 0;
    }
    if (++hitSequence == 0) ++hitSequence;
    HitNpcMsg message{MsgHitNpc, {}, delta, {}, to, localId, {}, outcome, seen, reaction, worldEpoch, hitSequence, incarnation, contact};
    std::memcpy(message.guid, guid, 16);
    std::memcpy(message.weapon, weapon, 16);
    if (npc) std::memcpy(message.npc, npc, 16);
    if (!Send(server, &message, sizeof(message), ChannelCombat, true)) {
        Log("net: NPC hit could not be queued");
        return 0;
    }
    return hitSequence;
}

void SetHitResultHandler(HitResultHandler handler) { handlers.hitResult = handler; }

void SendHitPlayer(uint8_t playerId, int32_t delta, uint8_t const attacker[16], uint8_t attackerPlayer, bool meansToKill)
{
    if (!host || !playerId || playerId == localId || playerId >= MaxPlayers) return;
    if (downed::PlayerInteractionFlags(playerId)) {
        Logf("net: player hit not sent target=%u reason=%s", playerId,
             downed::InteractionReason(downed::PlayerInteractionFlags(playerId)));
        return;
    }
    HitPlayerMsg message{MsgHitPlayer, delta, {}, attackerPlayer, playerId, static_cast<uint8_t>(meansToKill ? 1 : 0)};
    std::memcpy(message.attacker, attacker, 16);
    SendToPeer(&message, sizeof(message), ChannelCombat, true, playerId);
}

bool SendCharacter(uint8_t to, CharacterNote const& note, std::vector<uint8_t> const& bytes)
{
    if (!host || note.length != bytes.size() || bytes.size() > saving::store::PlayerLimit ||
        note.crc != saving::store::Crc(bytes)) return false;
    const CharacterMsg message{MsgCharacter, note};
    std::vector<uint8_t> packet(sizeof(message) + bytes.size());
    std::memcpy(packet.data(), &message, sizeof(message));
    if (!bytes.empty()) std::memcpy(packet.data() + sizeof(message), bytes.data(), bytes.size());
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to] || note.purpose != CharacterRestore) return false;
        return Send(peers[to], packet.data(), packet.size(), ChannelReliable, true);
    }
    return connected && server && to == 1 && note.purpose == CharacterCapture &&
           Send(server, packet.data(), packet.size(), ChannelReliable, true);
}

void SendWorldState(WorldState const& state, uint8_t except)
{
    const WorldStateMsg message{MsgWorldState, state};
    if (state.kind == 19) {
        if (isHost) {
            if (state.dialog == 0) SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
            return;
        }
        if (connected && server) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    if (!host || (!isHost && !ClientWorldStateKind(state.kind))) return;
    SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
}

bool Start(bool asHost)
{
    isHost = asHost;
    if (!options::Install()) return StartFailed("Game.dll/Engine.dll balancing readers unsupported; install the same CP 1.75.14 patches/build");
    hostOptions = options::Capture();
    haveCurrentLocal = false;
    currentLocal = {};
    playerCadence = {};
    ResetNpcReplication();
    ResetNpcCountersOnStart();
    startupError[0] = '\0';
    if (enet_initialize() != 0) return StartFailed("ENet initialization failed");
    enetInitialized = true;
    target = ENetAddress{};
    if (enet_address_set_host_ip(&target, "127.0.0.1") != 0) return StartFailed("loopback address unavailable");
    ENetAddress bindAddress{};
    bindAddress.host = ENET_HOST_ANY;
    bindAddress.port = 0;
    for (auto& session : directSessions) session = DirectPunchSession{};
    for (auto& tag : peerConnectData) tag = 0;
    char const* createError = nullptr;
    host = CreateTransportHost(&bindAddress, isHost ? MaxPlayers - 1 : 1, createError);
    if (!host) return StartFailed(createError);
    ENetAddress bound{};
    if (enet_socket_get_address(host->socket, &bound) == 0 && bound.port)
        host->address.port = bound.port;
    host->compressor.compress = nullptr;
    host->compressor.decompress = &DecompressDatagram;
    host->intercept = &InterceptTransportPacket;
    Logf("enet: host created role=%s channels=%u compress=off mtu=%u port=%u",
         isHost ? "host" : "client", ChannelCount, host->mtu, host->address.port);
    Log("enet: range coder outbound compression disabled; decompression guard retained");
    bandwidth = detail::BandwidthCounters{};
    bandwidthSince = enet_time_get();
    lastSentData = host->totalSentData;
    lastSentPackets = host->totalSentPackets;
    lastReceivedData = host->totalReceivedData;
    lastReceivedPackets = host->totalReceivedPackets;
    if (!steam::Start(isHost, host->address.port)) return StartFailed(steam::Error());
    lastSteamTraffic = steam::Traffic();
    relayEnvelopeBytes = lastSteamTraffic.envelopeBytes;
    strcpy_s(localName, steam::PlayerName());
    Logf("net: player name from Steam: %s", localName);
    if (isHost) {
        localId = 1;
        Logf("identity: join steamid=%llu display=%s source=transport-authenticated slot=1", steam::PlayerId(), localName);
        connected = true;
        Logf("net: Steam-only host relay on 127.0.0.1:%u", host->address.port);
    } else {
        target.port = steam::RelayPort();
        if (!target.port) return StartFailed("Steam relay socket is unavailable");
        steamLinked = false;
        connectLogged = false;
        startedAt = 0;
        Logf("net: waiting for a Steam invite (ENet connects through the relay 127.0.0.1:%u once the Steam link to the host is up)", target.port);
    }
    return true;
}

namespace {
void Service(PlayerState const& local, bool haveLocal)
{
    strncpy_s(localName, steam::PlayerName(), _TRUNCATE);
    ProcessRelayDrops();
    SyncDirectSessions(enet_time_get());
    if (isHost) for (uint8_t id = 2; id < MaxPlayers; ++id) {
        uint64_t account = 0, generation = 0;
        char name[24] = {};
        if (peers[id] && ReadAuthenticatedPeerIdentity(peers[id], account, name, generation) &&
            account == peerAccounts[id] && generation == peerRelayGenerations[id])
            strncpy_s(remotes[id].latest.name, name, _TRUNCATE);
    }
    ENetEvent event;
    int events = 0;
    int result = 0;
    for (; events < 512; ++events) {
        result = enet_host_service(host, &event, 0);
        if (result <= 0) break;
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            Logf("enet: event role=%s type=connect(%u) data=%u state=%s(%u) channels=%zu elapsed_ms=%u",
                 isHost ? "host" : "client", static_cast<unsigned>(event.type), event.data,
                 EnetPeerStateName(event.peer->state), static_cast<unsigned>(event.peer->state), event.peer->channelCount,
                 !isHost && startedAt ? enet_time_get() - startedAt : 0);
            if (!isHost)
                steam::NoteClientEnetState(event.peer->state, event.type, event.data,
                                          startedAt ? enet_time_get() - startedAt : 0, "");
            if (isHost && !PeerRelayOrDirect(event.peer)) {
                Log("net: rejected an ENet connection outside the linked Steam/direct peers");
                enet_peer_reset(event.peer);
                break;
            }
            if (const size_t peerIdx = PeerIndex(event.peer); peerIdx < MaxPlayers) {
                peerConnectData[peerIdx] = event.data;
                poseRoutes[peerIdx] = PoseRoute{};
            }
            enet_peer_timeout(event.peer, G3T_PEER_TIMEOUT_LIMIT,
                              G3T_PEER_TIMEOUT_MINIMUM_MS, G3T_PEER_TIMEOUT_MAXIMUM_MS);
            enet_peer_throttle_configure(event.peer, 5000, 0, 0);
            event.peer->data = nullptr;
            ResetBudget(event.peer);
            if (!isHost) for (auto& sender : playerSenders[1]) sender = {};
            if (!isHost) {
                everConnected = true;
                Hello hello{MsgHello, helloVersion ? helloVersion : ProtocolVersion, {}, {}};
                strncpy_s(hello.name, localName, _TRUNCATE);
                std::memcpy(hello.parity, parity::Local(), sizeof(hello.parity));
                Send(event.peer, &hello, sizeof(hello), ChannelReliable, true);
                Log("net: connected, hello sent");
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            {
                movement::Stage stage("ENet receive");
                OnReceive(event.peer, event.packet);
            }
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            Logf("enet: event role=%s type=disconnect(%u) data=%u state=%s(%u) channels=%zu elapsed_ms=%u",
                 isHost ? "host" : "client", static_cast<unsigned>(event.type), event.data,
                 EnetPeerStateName(event.peer->state), static_cast<unsigned>(event.peer->state), event.peer->channelCount,
                 !isHost && startedAt ? enet_time_get() - startedAt : 0);
            if (isHost) {
                char reason[160];
                Format(reason, "%s event_data=%u", event.data ? "disconnect" : "timeout-or-disconnect", event.data);
                LeavePeer(event.peer, reason, false);
            } else {
                char reason[160];
                if (refused)
                    Format(reason, "ENet connect refused: %.110s (disconnect event data=%u)", refusalReason, event.data);
                else
                    Format(reason, event.data ? "ENet disconnect event data=%u" : "ENet timeout or disconnect event data=%u",
                           event.data);
                steam::NoteClientEnetState(event.peer->state, event.type, event.data,
                                          startedAt ? enet_time_get() - startedAt : 0, reason);
                const bool endLink = everConnected && steam::RequestClientDisconnect();
                if (!endLink && !refused) MarkDirectConnectFailed();
                Log(endLink ? "net: disconnected from host; closing the Steam link (accept the invite again to reconnect)"
                            : "net: disconnected from host; will reconnect");
                connected = false;
                inWorld = false;
                options::Restore();
                server = nullptr;
                DropClientRemotes();
                if (endLink) linkEnding = true;
            }
            break;
        default:
            break;
        }
    }
    if (events == 512) ++serviceCaps;
    if (result < 0 && ++serviceErrors == 1)
        Logf("enet: service failed role=%s result=%d state=%s elapsed_ms=%u; subsequent failures counted in RTT diagnostics",
             isHost ? "host" : "client", result, EnetPeerStateName(server ? server->state : ENET_PEER_STATE_DISCONNECTED),
             !isHost && startedAt ? enet_time_get() - startedAt : 0);
    FlushGoodsReplays();
    const uint32_t now = enet_time_get();
    SyncClientSteamLink(now);
    if (!isHost && !server && !refused && steamLinked && now - lastConnectAttempt > 3000) {
        DirectPunchSession* direct = FindClientDirectSession();
        if (!ClientPunchWaiting(direct, now))
            FinishClientConnectAttempt(direct, now);
    }
    UpdateBudgets(now);
    if (connected) {
        if (!activitySent || local.flags != lastActivityFlags || local.downedSequence != lastActivityDowned ||
            now - lastActivitySend >= 1000) {
            const ActivityMsg activity{MsgPlayerActivity, localId, local.flags, ++sequence,
                                       local.world, worldEpoch, local.downedSequence};
            bool queued = true;
            if (isHost) {
                for (uint8_t i = 2; i < MaxPlayers; ++i)
                    if (peers[i] && !Send(peers[i], &activity, sizeof(activity), ChannelState, true)) queued = false;
            } else if (server) {
                queued = Send(server, &activity, sizeof(activity), ChannelState, true);
            }
            if (!queued) Log("net: player activity heartbeat queue failed; retrying at the next heartbeat");
            activitySent = true;
            lastActivitySend = now;
            lastActivityFlags = local.flags;
            lastActivityDowned = local.downedSequence;
        }
        if (!haveLocal) return;
        const bool due = playerCadence.Due(now, static_cast<uint32_t>(sendRate));
        const bool immediate = !haveCurrentLocal || local.flags != currentLocal.flags ||
            local.downedSequence != currentLocal.downedSequence || local.world != currentLocal.world ||
            local.actionSeq != currentLocal.actionSeq;
        if (!due && !immediate) return;
        currentLocal = local;
        currentLocal.id = localId;
        currentLocal.sequence = ++sequence;
        currentLocal.sent = now;
        strncpy_s(currentLocal.name, localName, _TRUNCATE);
        haveCurrentLocal = true;
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i) if (peers[i]) SendPlayerState(peers[i], currentLocal);
        } else if (server) {
            SendPlayerState(server, currentLocal);
        }
    }
}

void BandwidthLog()
{
    const uint32_t now = enet_time_get();
    const uint32_t elapsed = now - bandwidthSince;
    if (elapsed < 5000) return;
    const uint32_t sentData = host->totalSentData, sentPackets = host->totalSentPackets;
    const uint32_t receivedData = host->totalReceivedData, receivedPackets = host->totalReceivedPackets;
    const double perSecond = 1000.0 / elapsed;
    const auto traffic = steam::Traffic();
    EmitBandwidthDetails(elapsed, sentData, sentPackets, receivedData, receivedPackets,
                         lastSentData, lastSentPackets, lastReceivedData, lastReceivedPackets,
                         traffic, lastSteamTraffic);
    detail::ForEachKindLine(bandwidth, [](char const* line) { Logf("%s", line); });
    detail::ForEachKindRateLine(bandwidth, perSecond, [](char const* line) { Logf("%s", line); });
    bandwidth = detail::BandwidthCounters{};
    bandwidthSince = now;
    lastSentPackets = sentPackets;
    lastSentData = sentData;
    lastReceivedPackets = receivedPackets;
    lastReceivedData = receivedData;
    lastSteamTraffic = traffic;
}
} // namespace

void Flush()
{
    if (!host || g3t::ai::ShuttingDown()) return;
    if (haveQueued) {
        enqueueDelays.Add(enet_time_get() - queuedAt);
        haveQueued = false;
    }
    enet_host_flush(host);
    steam::Pump();
}

void Tick(PlayerState const& local, bool haveLocal)
{
    if (g3t::ai::ShuttingDown()) return;
    if (!host) return;
    const uint32_t now = enet_time_get();
    if (lastServiceAt) serviceGaps.Add(now - lastServiceAt);
    lastServiceAt = now;
    Service(local, haveLocal);
    RttLog();
    BandwidthLog();
}

void Reconnect(void (*log)(char const*))
{
    if (isHost || !server || !connected) { if (log) log("REFUSED reconnect: a connected client only"); return; }
    ResetClientLinkState(true);
    DirectPunchSession* direct = FindClientDirectSession();
    bool useDirect = false;
    ENetAddress connectTo = target;
    ConnectClientWithDirectFallback(direct, useDirect, connectTo, false);
    lastConnectAttempt = enet_time_get();
    steam::NoteClientEnetState(server ? server->state : ENET_PEER_STATE_DISCONNECTED, ENET_EVENT_TYPE_NONE, 0,
                               startedAt ? lastConnectAttempt - startedAt : 0, server ? "" : "ENet QA reconnect refused");
    Logf("enet: connect request role=client channels=%u compress=off route=%s port=%u result=%s source=QA-reconnect",
         ChannelCount, useDirect ? "direct-udp" : "steam-relay", connectTo.port, server ? "pending" : "refused");
    Log("net: QA reconnect: left the host and connecting again");
    if (log) log(server ? "RECONNECT ok=1" : "RECONNECT ok=0");
}

} // namespace g3t::net

#ifndef G3T_NET_SEPARATE_TUS
#include "net_direct.cpp"
#include "net_session.cpp"
#include "net_metrics.cpp"
#include "net_npc.cpp"
#include "net_npc_pose.cpp"
#include "net_messages.cpp"
#endif
