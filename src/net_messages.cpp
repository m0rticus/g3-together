// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "downed.h"
#include "downed_policy.h"
#include "log.h"
#include "saving_policy.h"
#include "saving_store.h"
#include "story_policy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3t::net::internal {

namespace {
template <typename Msg>
bool UnpackFixed(ENetPacket const* packet, Msg& out)
{
    if (packet->dataLength != sizeof(Msg)) return false;
    std::memcpy(&out, packet->data, sizeof(Msg));
    return true;
}

template <typename Msg>
bool UnpackReliableFixed(ENetPacket const* packet, Msg& out)
{
    return (packet->flags & ENET_PACKET_FLAG_RELIABLE) && UnpackFixed(packet, out);
}

template <size_t N>
void ClampCString(char (&buffer)[N])
{
    buffer[N - 1] = '\0';
}

bool UnpackContentEntries(ENetPacket const* packet, size_t headerSize, uint8_t count, ContentEntry (&entries)[MaxContentEntries])
{
    if (count > MaxContentEntries || packet->dataLength != headerSize + count * sizeof(ContentEntry)) return false;
    if (count) std::memcpy(entries, packet->data + headerSize, count * sizeof(ContentEntry));
    for (uint8_t i = 0; i < count; ++i) ClampCString(entries[i].name);
    return true;
}

void SendClientOrTarget(void const* data, size_t size, uint8_t to)
{
    if (!isHost) SendToServer(data, size, ChannelReliable, true);
    else if (to == 0) SendBroadcast(data, size, ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(data, size, ChannelReliable, true, to);
}

template <typename Msg>
bool RelayClientUnicast(ENetPeer* peer, Msg& message, uint8_t to, uint8_t channel = ChannelReliable)
{
    message.from = PeerSlotOf(peer);
    if (!message.from) return true;
    if (to > 1 && to < MaxPlayers) {
        if (peers[to] && to != message.from) Send(peers[to], &message, sizeof(message), channel, true);
        return true;
    }
    return false;
}

template <typename Msg>
bool RelayClientBroadcast(ENetPeer* peer, Msg& message, uint8_t channel = ChannelReliable)
{
    message.from = PeerSlotOf(peer);
    if (!message.from) return false;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), channel, true);
    return true;
}

bool QueueStates(uint8_t to, uint8_t kind, StateEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxStatesPerMessage || (count && !entries) ||
        (count == 0 && isHost && !(kind & story::policy::Snapshot))) return false;
    constexpr int Batch = (1168 - sizeof(StatesHeader)) / sizeof(StateEntry);
    uint8_t buffer[sizeof(StatesHeader) + Batch * sizeof(StateEntry)];
    for (int at = 0; at < count || (at == 0 && count == 0); at += Batch) {
        const int chunk = (std::min)(Batch, count - at);
        const StatesHeader header{MsgStates, kind, static_cast<uint8_t>(chunk)};
        std::memcpy(buffer, &header, sizeof(header));
        if (chunk) std::memcpy(buffer + sizeof(header), entries + at, chunk * sizeof(StateEntry));
        const size_t size = sizeof(header) + chunk * sizeof(StateEntry);
        if (!isHost) {
            if (!connected || !server || (to && to != 1) || !Send(server, buffer, size, ChannelReliable, true)) return false;
        } else if (to) {
            if (to < 2 || to >= MaxPlayers || !peers[to] || !Send(peers[to], buffer, size, ChannelReliable, true)) return false;
        } else {
            for (uint8_t id = 2; id < MaxPlayers; ++id)
                if (peers[id] && Open(peers[id]) && !Send(peers[id], buffer, size, ChannelReliable, true)) return false;
        }
    }
    return true;
}
} // namespace

bool HandleGameplayMessage(ENetPeer* peer, ENetPacket* packet, uint8_t type)
{
    switch (type) {
    case MsgNpcLease: {
        const uint8_t from = AuthenticatedSender(peer);
        if (!from || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return true;
        npclease::Message note;
        if (!npclease::DecodeMessage(packet->data + 1, packet->dataLength - 1, note) ||
            note.world != worldEpoch) {
            Log("net: invalid NPC lease identity/state refused");
            return true;
        }
        if (handlers.npcLease) handlers.npcLease(from, note);
        return true;
    }
    case MsgNpcEquipment: {
        NpcEquipmentMsg message{};
        if (isHost || !connected || peer != server || !UnpackReliableFixed(packet, message)) return true;
        if (message.note.incarnation && handlers.npcEquipment) handlers.npcEquipment(message.note);
        return true;
    }
    case MsgSave:
        if (packet->dataLength == sizeof(SaveMsg)) HandleSaveMessage(peer, packet);
        return true;
    case MsgHitResult: {
        HitResultMsg message{};
        if (isHost || !connected || peer != server) return true;
        if (packet->dataLength == LegacyHitResultMsgSize) {
            std::memcpy(&message, packet->data, LegacyHitResultMsgSize);
            message.result.knockbackValid = 0;
        } else if (!UnpackFixed(packet, message)) {
            return true;
        }
        if (message.result.knockbackValid > 1) return true;
        if (message.result.knockbackValid) {
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(message.result.originPos[k]) || std::fabs(message.result.originPos[k]) > 1e7f ||
                    !std::isfinite(message.result.goalPos[k]) || std::fabs(message.result.goalPos[k]) > 1e7f)
                    return true;
            }
            if (!std::isfinite(message.result.stepCm) || message.result.stepCm < 0.0f || message.result.stepCm > 2000.0f ||
                message.result.hitDurationMs > 10000u || message.result.moveDurationMs > 10000u)
                return true;
        }
        if (message.result.epoch == worldEpoch && message.result.accepted <= 1 && handlers.hitResult)
            handlers.hitResult(message.result);
        return true;
    }
    case MsgStates: {
        if (packet->dataLength < sizeof(StatesHeader)) return true;
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || (!isHost && peer != server)) return true;
        StatesHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t domain = header.kind & story::policy::KindMask;
        const bool snapshot = (header.kind & story::policy::Snapshot) != 0;
        if (domain < StateQuests || domain > StateReputation ||
            ((header.kind & story::policy::End) && (!snapshot || header.count)) ||
            ((header.kind & story::policy::Catchup) && (!snapshot || !header.count || (header.kind & story::policy::End))) ||
            (isHost && snapshot && (header.count || (header.kind & (story::policy::End | story::policy::Catchup))))) return true;
        const uint8_t from = isHost ? PeerSlotOf(peer) : 1;
        if ((isHost && !from) || (!isHost && !connected)) return true;
        if (header.count <= MaxStatesPerMessage &&
            packet->dataLength == sizeof(header) + header.count * sizeof(StateEntry) &&
            handlers.state && from) {
            StateEntry entries[MaxStatesPerMessage];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(StateEntry));
            for (int i = 0; i < header.count; ++i) ClampCString(entries[i].name);
            handlers.state(from, header.kind, entries, header.count);
        }
        return true;
    }
    case MsgTalk: {
        TalkMsg message{};
        if (!isHost || !UnpackReliableFixed(packet, message)) return true;
        const uint8_t from = PeerSlotOf(peer);
        if (handlers.talk && from) handlers.talk(from, message.begin != 0, message.guid);
        return true;
    }
    case MsgSummon: {
        SummonMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.note.spellItem);
        ClampCString(message.note.spawn);
        for (float v : message.note.position) if (!std::isfinite(v)) return true;
        for (float v : message.note.rotation) if (!std::isfinite(v)) return true;
        if (message.note.epoch != worldEpoch || message.note.ownerPlayer >= MaxPlayers) return true;
        if (isHost) {
            message.from = PeerSlotOf(peer);
            if (!message.from) return true;
            if (message.note.op == SummonCreature) { Log("net: client NPC spawn publication refused"); return true; }
            if (message.note.op == SummonCreatureRequest) message.note.ownerPlayer = message.from;
            else RelayClientBroadcast(peer, message);
        } else if (!message.from || message.from >= MaxPlayers) {
            return true;
        }
        if (handlers.summon && message.from) handlers.summon(message.from, message.note);
        return true;
    }
    case MsgEnclave: {
        EnclaveMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.note.name);
        const uint8_t from = isHost ? PeerSlotOf(peer) : 1;
        if (handlers.enclave && from) handlers.enclave(from, message.note);
        return true;
    }
    case MsgPlayerSpell: {
        PlayerSpellMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.script);
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.playerSpell && message.from && message.from < MaxPlayers && message.to == localId)
            handlers.playerSpell(message.from, message.script);
        return true;
    }
    case MsgRansack: {
        RansackMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.ransack && message.from && message.from < MaxPlayers && message.to == localId)
            handlers.ransack(message.from, message.note);
        return true;
    }
    case MsgWitness: {
        WitnessMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        if (isHost) {
            message.from = PeerSlotOf(peer);
            if (!message.from) return true;
            if (message.to > 1 && message.to < MaxPlayers) {
                if (peers[message.to] && message.to != message.from)
                    Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return true;
            }
            if (message.to == 0) RelayClientBroadcast(peer, message);
        } else if (!message.from || message.from >= MaxPlayers) {
            return true;
        }
        if (handlers.witness && message.from && (message.to == 0 || message.to == localId))
            handlers.witness(message.from, message.note);
        return true;
    }
    case MsgNpcSpell: {
        NpcSpellMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.script);
        if (message.targetPlayer >= MaxPlayers) return true;
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.npcSpell && message.from && message.from < MaxPlayers && message.to == localId)
            handlers.npcSpell(message.from, message.script, message.target, message.args, message.targetPlayer);
        return true;
    }
    case MsgShot: {
        ShotMsg message{};
        if (!UnpackFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        static const uint8_t none[16] = {};
        if (isHost && std::memcmp(message.note.caster, none, 16)) {
            Log("net: client NPC projectile publication refused");
            return true;
        }
        ClampCString(message.note.arrow);
        ClampCString(message.note.spellItem);
        ClampCString(message.note.spell);
        for (float v : message.note.position) if (!std::isfinite(v)) return true;
        for (float v : message.note.rotation) if (!std::isfinite(v)) return true;
        for (float v : message.note.direction) if (!std::isfinite(v)) return true;
        if (!std::isfinite(message.note.velocity) || !std::isfinite(message.note.multiplier)) return true;
        if (message.note.epoch != worldEpoch || message.note.targetPlayer >= MaxPlayers) {
            Log("net: stale or invalid projectile refused");
            return true;
        }
        if (isHost) {
            if (!RelayClientBroadcast(peer, message, ChannelCombat)) return true;
        } else if (!message.from || message.from >= MaxPlayers) {
            return true;
        }
        if (handlers.shot && message.from) handlers.shot(message.from, message.note);
        return true;
    }
    case MsgRagdoll: {
        RagdollMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (isHost) { Log("net: client NPC force publication refused"); return true; }
        if (!connected || peer != server) return true;
        if (message.note.epoch != worldEpoch || !message.note.sequence || !message.note.incarnation) return true;
        for (float v : message.note.at) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return true;
        for (float v : message.note.force) if (!std::isfinite(v) || std::fabs(v) > 1e9f) return true;
        if (handlers.ragdoll) handlers.ragdoll(message.note);
        return true;
    }
    case MsgDialogLine: {
        DialogLineMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.note.sample);
        ClampCString(message.note.text);
        if (isHost) {
            if (!RelayClientBroadcast(peer, message)) return true;
        } else if (!message.from || message.from >= MaxPlayers) {
            return true;
        }
        if (handlers.dialogLine && message.from) handlers.dialogLine(message.from, message.note);
        return true;
    }
    case MsgOffer: {
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength < OfferHeaderSize || (!isHost && (!connected || peer != server))) return true;
        OfferMsg message{};
        std::memcpy(&message, packet->data, OfferHeaderSize);
        if (message.note.count > MaxOfferEntries ||
            packet->dataLength != OfferHeaderSize + message.note.count * sizeof(OfferEntry)) return true;
        std::memcpy(&message, packet->data, packet->dataLength);
        for (uint16_t i = 0; i < message.note.count; ++i) ClampCString(message.note.entries[i].name);
        if (isHost) {
            message.note.buyer = PeerSlotOf(peer);
            if (!message.note.buyer) return true;
            if (message.note.seller != 1) {
                const uint8_t to = message.note.seller;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.buyer)
                    Send(peers[to], &message, packet->dataLength, ChannelReliable, true);
                return true;
            }
        } else if (message.note.seller != localId || !message.note.buyer || message.note.buyer >= MaxPlayers) {
            return true;
        }
        if (handlers.offer && message.note.buyer) handlers.offer(message.note);
        return true;
    }
    case MsgOfferResult: {
        OfferResultMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        if (isHost) {
            message.note.seller = PeerSlotOf(peer);
            if (!message.note.seller) return true;
            if (message.note.buyer != 1) {
                const uint8_t to = message.note.buyer;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.seller)
                    Send(peers[to], &message, sizeof(message), ChannelReliable, true);
                return true;
            }
        } else if (message.note.buyer != localId || !message.note.seller || message.note.seller >= MaxPlayers) {
            return true;
        }
        if (handlers.offerResult && message.note.seller) handlers.offerResult(message.note);
        return true;
    }
    case MsgItem: {
        ItemMsg message{};
        if (!UnpackReliableFixed(packet, message) || (!isHost && (!connected || peer != server))) return true;
        ClampCString(message.note.name);
        const uint8_t from = isHost ? PeerSlotOf(peer) : 1;
        if (handlers.item && from) handlers.item(from, message.note);
        return true;
    }
    case MsgContents: {
        if (isHost || !connected || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength < sizeof(ContentsHeader)) return true;
        ContentsHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        ContentEntry entries[MaxContentEntries];
        if (UnpackContentEntries(packet, sizeof(header), header.count, entries) && handlers.contents)
            handlers.contents(header.guid, entries, header.count);
        return true;
    }
    case MsgContainerTx: {
        if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) || packet->dataLength < sizeof(TxHeader)) return true;
        TxHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t from = PeerSlotOf(peer);
        ContentEntry entries[MaxContentEntries];
        if (from && UnpackContentEntries(packet, sizeof(header), header.count, entries) && handlers.tx)
            handlers.tx(from, header.guid, header.seq, entries, header.count);
        return true;
    }
    case MsgTxResult: {
        TxResultMsg message{};
        if (isHost || !connected || peer != server || !UnpackReliableFixed(packet, message)) return true;
        if (handlers.txResult) handlers.txResult(message.guid, message.seq, message.ok != 0);
        return true;
    }
    case MsgWatch: {
        WatchMsg message{};
        if (!isHost || !UnpackReliableFixed(packet, message)) return true;
        const uint8_t from = PeerSlotOf(peer);
        if (handlers.watch && from) handlers.watch(from, message.guid);
        return true;
    }
    case MsgClock: {
        ClockMsg message{};
        if (!UnpackReliableFixed(packet, message) || isHost || !connected || peer != server) return true;
        if (handlers.clock) handlers.clock(message.state);
        return true;
    }
    case MsgDeath: {
        DeathMsg message{};
        if (isHost || !connected || peer != server || !UnpackReliableFixed(packet, message)) return true;
        if (handlers.death && message.by && message.by < MaxPlayers) handlers.death(message.by, message.guid);
        return true;
    }
    case MsgKilled: {
        KilledMsg message{};
        if (isHost || !connected || peer != server || !UnpackReliableFixed(packet, message)) return true;
        if (message.to == localId && message.epoch == worldEpoch && message.incarnation && handlers.killed)
            handlers.killed(message.guid, message.incarnation);
        return true;
    }
    case MsgLeave: {
        Leave leave{};
        if (isHost || !connected || peer != server || !UnpackReliableFixed(packet, leave)) return true;
        Logf("net: player %u left", leave.id);
        if (leave.id && leave.id < MaxPlayers && remotes[leave.id].connected && handlers.peerLeft)
            handlers.peerLeft(leave.id, remotes[leave.id].latest.name);
        Drop(leave.id);
        return true;
    }
    default:
        return false;
    }
}

namespace {
bool SendEquipmentPacket(ENetPeer* peer, PlayerEquipment const& equip)
{
    if (!peer || !equip.sequence) return false;
    std::vector<uint8_t> payload;
    if (!player_equip::Encode(equip, payload)) return false;
    std::vector<uint8_t> packet;
    packet.reserve(1 + payload.size());
    packet.push_back(MsgPlayerEquipment);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return Send(peer, packet.data(), packet.size(), ChannelState, true);
}
} // namespace

bool FlushPlayerEquipment(uint32_t now)
{
    bool changed = false;
    if (havePendingLocalEquip) {
        const bool dirty = !haveCurrentEquip || !pendingLocalEquip.SameSlots(currentLocalEquip) ||
                           pendingLocalEquip.world != currentLocalEquip.world ||
                           currentLocalEquip.epoch != worldEpoch;
        if (dirty) {
            currentLocalEquip = pendingLocalEquip;
            currentLocalEquip.id = localId;
            if (++equipSequence == 0) ++equipSequence;
            currentLocalEquip.sequence = equipSequence;
            currentLocalEquip.sent = now;
            currentLocalEquip.epoch = worldEpoch;
            haveCurrentEquip = true;
            changed = true;
        }
    }
    if (!host || !connected) return changed;
    if (isHost) {
        for (uint8_t to = 2; to < MaxPlayers; ++to) {
            if (!peers[to] || !Open(peers[to])) continue;
            for (uint8_t from = 1; from < MaxPlayers; ++from) {
                if (from == to) continue;
                PlayerEquipment const* src = nullptr;
                if (from == 1) {
                    if (haveCurrentEquip && currentLocalEquip.epoch == worldEpoch) src = &currentLocalEquip;
                } else if (clientInWorld[from] && remotes[from].hasEquip && remotes[from].latestEquip.epoch == worldEpoch) {
                    src = &remotes[from].latestEquip;
                }
                if (!src || !src->sequence || sentEquipSeq[to][from] == src->sequence) continue;
                if (SendEquipmentPacket(peers[to], *src)) sentEquipSeq[to][from] = src->sequence;
            }
        }
    } else if (server && Open(server) && haveCurrentEquip && currentLocalEquip.epoch == worldEpoch &&
               currentLocalEquip.sequence && sentEquipSeq[1][localId] != currentLocalEquip.sequence) {
        if (SendEquipmentPacket(server, currentLocalEquip)) sentEquipSeq[1][localId] = currentLocalEquip.sequence;
    }
    return changed;
}

bool HandlePlayerEquipmentMessage(ENetPeer* peer, ENetPacket const* packet)
{
    if (!isHost && (!connected || peer != server)) return true;
    if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || packet->dataLength < 1 + player_equip::FixedWireBytes)
        return true;
    uint8_t player = 0;
    if (!player_equip::PeekPlayerId(packet->data + 1, packet->dataLength - 1, player)) return true;
    if (isHost) player = PeerSlotOf(peer);
    if (!player || player >= MaxPlayers || player == localId) return true;
    PlayerEquipment equip;
    if (!player_equip::Decode(packet->data + 1, packet->dataLength - 1, equip, player)) {
        Logf("net: invalid player equipment packet player=%u", player);
        return true;
    }
    if (equip.epoch != worldEpoch || !equip.sequence) return true;
    Remote& r = remotes[player];
    if (r.hasEquip && static_cast<int32_t>(equip.sequence - r.latestEquip.sequence) <= 0) return true;
    const bool reset = !r.hasEquip || r.latestEquip.world != equip.world;
    r.latestEquip = equip;
    r.hasEquip = true;
    const uint32_t arrived = enet_time_get();
    if (handlers.playerEquipment) handlers.playerEquipment(equip, arrived, reset);
    if (isHost && clientInWorld[player]) {
        for (uint8_t to = 2; to < MaxPlayers; ++to) {
            if (to != player && peers[to] && Open(peers[to]) &&
                sentEquipSeq[to][player] != equip.sequence && SendEquipmentPacket(peers[to], equip)) {
                sentEquipSeq[to][player] = equip.sequence;
            }
        }
    }
    return true;
}

} // namespace g3t::net::internal

namespace g3t::net {

void SetPlayerEquipmentHandler(PlayerEquipmentHandler handler) { handlers.playerEquipment = handler; }

void UpdateLocalEquipment(PlayerEquipment const& equip, bool haveLocal)
{
    havePendingLocalEquip = haveLocal;
    if (haveLocal) pendingLocalEquip = equip;
    else pendingLocalEquip = PlayerEquipment{};
}

bool SendNpcLease(uint8_t to, npclease::Message const& note)
{
    if (note.world != worldEpoch || !Connected()) return false;
    ENetPeer* peer = isHost ? (to > 1 && to < MaxPlayers ? peers[to] : nullptr) : server;
    std::vector<uint8_t> bytes;
    if (!npclease::EncodeMessage(note, bytes)) return false;
    bytes.insert(bytes.begin(), MsgNpcLease);
    return Send(peer, bytes.data(), bytes.size(), ChannelReliable, true);
}

void SetNpcLeaseHandler(NpcLeaseHandler handler) { handlers.npcLease = handler; }
void SetWorldHandler(WorldHandler handler) { handlers.world = handler; }
void SetPlayerStateHandler(PlayerStateHandler handler) { handlers.playerState = handler; }
void SetPeerLeftHandler(PeerLeftHandler handler) { handlers.peerLeft = handler; }

bool SendWorld(WorldNote const& note, uint8_t to)
{
    if (!host) return false;
    if (isHost && note.op == WorldHost && worldEpoch != note.epoch) {
        ResetNpcReplication();
        worldEpoch = note.epoch;
        haveCurrentEquip = false;
        currentLocalEquip = {};
        ClearAllEquipTables();
        hostOptions = options::Capture();
    }
    const WorldMsg message{MsgWorld, note};
    if (isHost) {
        if (to > 1 && to < MaxPlayers && peers[to]) {
            if (note.op == WorldHost) {
                const HostWorldMsg announcement{MsgWorld, note, hostOptions};
                return Send(peers[to], &announcement, sizeof(announcement), ChannelReliable, true);
            }
            return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
        }
    } else if (server && connected) {
        return Send(server, &message, sizeof(message), ChannelReliable, true);
    }
    return false;
}

uint32_t NextDeedId()
{
    static uint32_t last = 0;
    return ++last;
}

void SetSendRate(int hz) { sendRate = hz < 20 ? 20 : hz > 60 ? 60 : hz; }

uint32_t RemoteStateAgeMs(uint8_t player)
{
    return player < MaxPlayers && remotes[player].connected && InWorld(player)
        ? enet_time_get() - remotes[player].latestTicks : UINT32_MAX;
}

uint32_t RemoteFocusNpc(uint8_t player)
{
    return RemoteStateAgeMs(player) < 1000 ? remotes[player].latest.focusNpc : 0;
}

uint32_t RemoteTargetNpc(uint8_t player)
{
    return RemoteStateAgeMs(player) < 1000 ? remotes[player].latest.targetNpc : 0;
}

void SetHitHandlers(HitNpcHandler npc, HitPlayerHandler player)
{
    handlers.hitNpc = npc;
    handlers.hitPlayer = player;
}

void SendHitResult(uint8_t to, HitResult const& result)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to]) return;
    HitResultMsg message{MsgHitResult, result};
    message.result.epoch = worldEpoch;
    message.result.processedAt = enet_time_get();
    SendToPeer(&message, sizeof(message), ChannelCombat, true, to);
}

void SetStateHandler(StateHandler handler) { handlers.state = handler; }
void SetTalkHandler(TalkHandler handler) { handlers.talk = handler; }
void SetItemHandler(ItemHandler handler) { handlers.item = handler; }
void SetKilledHandler(KilledHandler handler) { handlers.killed = handler; }
void SetDeathHandler(DeathHandler handler) { handlers.death = handler; }
void SetSaveHandler(SaveHandler handler) { handlers.save = handler; }
void SetCharacterHandler(CharacterHandler handler) { handlers.character = handler; }
void SetClockHandler(ClockHandler handler) { handlers.clock = handler; }
void SetWorldStateHandler(WorldStateHandler handler) { handlers.worldState = handler; }
void SetGoodsHandler(GoodsHandler goods) { handlers.goods = goods; }

void SetContainerHandlers(ContentsHandler contents, ContainerTxHandler tx, TxResultHandler result, WatchHandler watch)
{
    handlers.contents = contents;
    handlers.tx = tx;
    handlers.txResult = result;
    handlers.watch = watch;
}

void SetSummonHandler(SummonHandler handler) { handlers.summon = handler; }
void SetShotHandler(ShotHandler handler) { handlers.shot = handler; }
void SetRagdollHandler(RagdollHandler handler) { handlers.ragdoll = handler; }
void SetDialogLineHandler(DialogLineHandler handler) { handlers.dialogLine = handler; }

void SetOfferHandlers(OfferHandler offer, OfferResultHandler result)
{
    handlers.offer = offer;
    handlers.offerResult = result;
}

void SetPlayerSpellHandler(PlayerSpellHandler handler) { handlers.playerSpell = handler; }
void SetRansackHandler(RansackHandler handler) { handlers.ransack = handler; }
void SetWitnessHandler(WitnessHandler handler) { handlers.witness = handler; }
void SetNpcSpellHandler(NpcSpellHandler handler) { handlers.npcSpell = handler; }
void SetEnclaveHandler(EnclaveHandler handler) { handlers.enclave = handler; }

bool SendSave(uint8_t to, SaveNote const& note)
{
    if (!host) return false;
    const SaveMsg message{MsgSave, note};
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to]) return false;
        return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    }
    return connected && server && to == 1 && Send(server, &message, sizeof(message), ChannelReliable, true);
}

void SendClock(ClockState const& state)
{
    if (!host) return;
    const ClockMsg message{MsgClock, state};
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

void SendDeath(uint8_t const guid[16], uint8_t by, uint8_t except)
{
    if (!host || !isHost) return;
    DeathMsg message{MsgDeath, {}, by};
    std::memcpy(message.guid, guid, 16);
    SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
}

void SendKilled(uint8_t to, uint8_t const guid[16], uint32_t incarnation)
{
    if (!host || !isHost || to <= 1 || to >= MaxPlayers || !incarnation) return;
    KilledMsg message{MsgKilled, {}, to, worldEpoch, incarnation};
    std::memcpy(message.guid, guid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

bool SendGoods(uint64_t epoch, uint32_t serial, int32_t gold, GoodsEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxGoodsEntries || gold < 0) return false;
    uint8_t buffer[sizeof(GoodsHeader) + MaxGoodsEntries * sizeof(GoodsEntry)];
    const GoodsHeader header{MsgGoods, localId, epoch, serial, gold, static_cast<uint8_t>(count)};
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(GoodsEntry));
    const size_t size = sizeof(header) + count * sizeof(GoodsEntry);
    if (!isHost) return server && connected && Send(server, buffer, size, ChannelReliable, true);
    bool queued = true;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && clientInWorld[i] && !Send(peers[i], buffer, size, ChannelReliable, true)) queued = false;
    return queued;
}

bool SendContents(uint8_t const guid[16], ContentEntry const* entries, int count)
{
    if (!host || !isHost || count < 0 || count > MaxContentEntries || (count && !entries)) return false;
    uint8_t buffer[sizeof(ContentsHeader) + MaxContentEntries * sizeof(ContentEntry)];
    ContentsHeader header{MsgContents, {}, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    const size_t size = sizeof(header) + count * sizeof(ContentEntry);
    bool queued = true;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && !Send(peers[i], buffer, size, ChannelReliable, true)) queued = false;
    return queued;
}

bool SendContainerTx(uint8_t const guid[16], uint32_t seq, ContentEntry const* entries, int count)
{
    if (!host || isHost || !server || !connected || count < 0 || count > MaxContentEntries || (count && !entries)) return false;
    uint8_t buffer[sizeof(TxHeader) + MaxContentEntries * sizeof(ContentEntry)];
    TxHeader header{MsgContainerTx, {}, seq, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    return Send(server, buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
}

void SendContainerTxResult(uint8_t to, uint8_t const guid[16], uint32_t seq, bool ok)
{
    if (!host || !isHost || to >= MaxPlayers || !peers[to]) return;
    TxResultMsg message{MsgTxResult, {}, seq, static_cast<uint8_t>(ok ? 1 : 0)};
    std::memcpy(message.guid, guid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendWatch(uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    WatchMsg message{MsgWatch, {}};
    std::memcpy(message.guid, guid, 16);
    SendToServer(&message, sizeof(message), ChannelReliable, true);
}

void SendItem(ItemNote const& note, uint8_t to)
{
    if (!host) return;
    const ItemMsg message{MsgItem, note};
    SendClientOrTarget(&message, sizeof(message), to);
}

void SendNpcSpell(uint8_t to, char const* script, uint8_t const npcGuid[16], int32_t args, uint8_t targetPlayer)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    NpcSpellMsg message{MsgNpcSpell, localId, to, {}, {}, args, targetPlayer};
    strncpy_s(message.script, script, _TRUNCATE);
    if (npcGuid) std::memcpy(message.target, npcGuid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendEnclave(EnclaveNote const& note, uint8_t to)
{
    if (!host) return;
    const EnclaveMsg message{MsgEnclave, note};
    SendClientOrTarget(&message, sizeof(message), to);
}

void SendPlayerSpell(uint8_t to, char const* script)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    PlayerSpellMsg message{MsgPlayerSpell, localId, to, {}};
    strncpy_s(message.script, script, _TRUNCATE);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendRansack(uint8_t to, RansackNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const RansackMsg message{MsgRansack, localId, to, note};
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendWitness(uint8_t to, WitnessNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const WitnessMsg message{MsgWitness, localId, to, note};
    SendClientOrTarget(&message, sizeof(message), to);
}

void SendShot(ShotNote const& note)
{
    if (!host) return;
    ShotMsg message{MsgShot, localId, note};
    message.note.epoch = worldEpoch;
    if (!message.note.sent) message.note.sent = enet_time_get();
    SendBroadcast(&message, sizeof(message), ChannelCombat, true);
}

void SendRagdoll(RagdollNote const& note)
{
    if (!host || !isHost) return;
    RagdollMsg message{MsgRagdoll, localId, note};
    message.note.epoch = worldEpoch;
    SendBroadcast(&message, sizeof(message), ChannelCombat, true);
}

void SendDialogLine(DialogLineNote const& note)
{
    if (!host) return;
    const DialogLineMsg message{MsgDialogLine, localId, note};
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

bool SendOffer(OfferNote const& note)
{
    if (!host || note.seller == 0 || note.seller >= MaxPlayers || note.seller == localId ||
        note.count > MaxOfferEntries) return false;
    const size_t size = OfferHeaderSize + note.count * sizeof(OfferEntry);
    OfferMsg message{};
    message.type = MsgOffer;
    std::memcpy(&message.note, &note, size - offsetof(OfferMsg, note));
    message.note.buyer = localId;
    if (!isHost) {
        if (!server || !connected) return false;
        SendToServer(&message, size, ChannelReliable, true);
        return true;
    }
    if (!peers[note.seller] || !InWorld(note.seller)) return false;
    SendToPeer(&message, size, ChannelReliable, true, note.seller);
    return true;
}

void SendOfferResult(OfferResultNote const& note)
{
    if (!host) return;
    OfferResultMsg message{MsgOfferResult, note};
    message.note.seller = localId;
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (note.buyer > 1 && note.buyer < MaxPlayers && peers[note.buyer])
        SendToPeer(&message, sizeof(message), ChannelReliable, true, note.buyer);
}

void SendSummon(SummonNote const& note)
{
    if (!host) return;
    SummonMsg message{MsgSummon, localId, note};
    message.note.epoch = worldEpoch;
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

void SendTalk(bool begin, uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    TalkMsg message{MsgTalk, static_cast<uint8_t>(begin ? 1 : 0), {}};
    std::memcpy(message.guid, guid, 16);
    SendToServer(&message, sizeof(message), ChannelReliable, true);
}

bool SendStates(uint8_t kind, StateEntry const* entries, int count) { return QueueStates(0, kind, entries, count); }
bool SendStatesTo(uint8_t to, uint8_t kind, StateEntry const* entries, int count) { return QueueStates(to, kind, entries, count); }

} // namespace g3t::net
