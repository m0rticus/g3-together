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
bool rttLog = true;
enet_uint32 lastRttLog = 0;

template <typename Msg>
bool UnpackFixed(ENetPacket const* packet, Msg& out)
{
    if (packet->dataLength != sizeof(Msg)) return false;
    std::memcpy(&out, packet->data, sizeof(Msg));
    return true;
}

template <size_t N>
void ClampCString(char (&buffer)[N])
{
    buffer[N - 1] = '\0';
}

template <typename Msg>
bool RelayClientUnicast(ENetPeer* peer, Msg& message, uint8_t to, uint8_t channel = ChannelReliable)
{
    message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
    if (to > 1 && to < MaxPlayers && peers[to]) {
        Send(peers[to], &message, sizeof(message), channel, true);
        return true;
    }
    return false;
}

template <typename Msg>
void RelayClientBroadcast(ENetPeer* peer, Msg& message, uint8_t channel = ChannelReliable)
{
    message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), channel, true);
}

void RttLine(ENetPeer* peer, char const* who, char* out, size_t size)
{
    auto const& poses = peer->channels[ChannelPoses];
    auto const& alt = peer->channels[ChannelPosesAlt];
    Format(out, size, "net: rtt=%u ms (+-%u) loss=%.1f%% sent=%u lost=%u throttle=%u/%u to %s "
                     "pose_send=%u/%u,%u/%u pose_recv=%u/%u,%u/%u waiting_bytes=%u "
                     "transport_max_rtt_ms=%d service_gap_avg_ms=%.2f service_gap_max_ms=%u "
                     "enqueue_flush_avg_ms=%.2f enqueue_flush_max_ms=%u service_caps=%u service_errors=%u "
                     "queued_commands=%u reliable_queued=%u reliable_inflight=%u reliable_bytes=%u",
           peer->roundTripTime, peer->roundTripTimeVariance,
           peer->packetLoss * 100.0 / ENET_PEER_PACKET_LOSS_SCALE, peer->packetsSent, peer->packetsLost,
           peer->packetThrottle, ENET_PEER_PACKET_THROTTLE_SCALE, who,
           poses.outgoingReliableSequenceNumber, poses.outgoingUnreliableSequenceNumber,
           alt.outgoingReliableSequenceNumber, alt.outgoingUnreliableSequenceNumber,
           poses.incomingReliableSequenceNumber, poses.incomingUnreliableSequenceNumber,
           alt.incomingReliableSequenceNumber, alt.incomingUnreliableSequenceNumber,
           static_cast<unsigned>(peer->totalWaitingData), steam::Ping(),
           serviceGaps.Average(), serviceGaps.maximum, enqueueDelays.Average(), enqueueDelays.maximum, serviceCaps, serviceErrors,
           static_cast<unsigned>(enet_list_size(&peer->outgoingCommands)),
           static_cast<unsigned>(enet_list_size(&peer->outgoingSendReliableCommands)),
           static_cast<unsigned>(enet_list_size(&peer->sentReliableCommands)), peer->reliableDataInTransit);
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

void RttLog()
{
    if (!rttLog) return;
    const enet_uint32 now = enet_time_get();
    if (now - lastRttLog < 5000) return;
    lastRttLog = now;
    char line[900];
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peers[i]->state == ENET_PEER_STATE_CONNECTED) {
                RttLine(peers[i], PeerName(i), line, sizeof(line));
                Log(line);
            }
    } else if (server && connected) {
        RttLine(server, "host", line, sizeof(line));
        Log(line);
    }
    serviceGaps = {};
    enqueueDelays = {};
}

void EmitBandwidthDetails(uint32_t elapsed, uint32_t sentData, uint32_t sentPackets,
                          uint32_t receivedData, uint32_t receivedPackets,
                          uint32_t lastSentData, uint32_t lastSentPackets,
                          uint32_t lastReceivedData, uint32_t lastReceivedPackets,
                          steam::TrafficCounters const& traffic,
                          steam::TrafficCounters const& lastSteamTraffic)
{
    const double perSecond = 1000.0 / elapsed;
    const uint64_t steamPackets = traffic.txDatagrams - lastSteamTraffic.txDatagrams;
    const uint64_t steamBytes = traffic.txBytes - lastSteamTraffic.txBytes;
    const uint64_t framingBytes = traffic.txEnvelopeBytes - lastSteamTraffic.txEnvelopeBytes;
    const uint64_t enetHeaderBytes = traffic.txEnetProtocolBytes - lastSteamTraffic.txEnetProtocolBytes;
    const uint64_t enetAckBytes = traffic.txEnetAckBytes - lastSteamTraffic.txEnetAckBytes;
    Logf("net: bandwidth role=%s window_ms=%u enet_loopback_tx_packets_bytes=%u/%u enet_loopback_rx_packets_bytes=%u/%u "
         "npc_per_s=considered:%.1f,sent:%.1f,unchanged:%.1f,far:%.1f,irrelevant:%.1f,send_failed:%.1f,paced:%.1f",
         isHost ? "host" : "client", elapsed, sentPackets - lastSentPackets, sentData - lastSentData,
         receivedPackets - lastReceivedPackets, receivedData - lastReceivedData,
         bandwidth.considered * perSecond, bandwidth.sent * perSecond, bandwidth.skippedUnchanged * perSecond,
         bandwidth.skippedFar * perSecond, bandwidth.skippedIrrelevant * perSecond, bandwidth.sendFailed * perSecond,
         bandwidth.skippedPaced * perSecond);
    const uint64_t updates = bandwidth.poseKeys + bandwidth.poseDeltas;
    if (isHost)
        Logf("npc-converge: ordered_setup_poses=%llu ordered_setup_controls=%llu reliable_anchors=%llu "
             "control_baselines=%u control_acks=%u "
             "baseline_policy=life-only pose_ack_policy=anchor-only",
             static_cast<unsigned long long>(npcOrderedSetupPoses),
             static_cast<unsigned long long>(npcOrderedSetupControls),
             static_cast<unsigned long long>(npcAnchorsSent), npcControlCreated, npcControlAckAccepted);
    else
        Logf("npc-converge: received_anchors=%llu pose_ack_policy=anchor-only",
             static_cast<unsigned long long>(npcAnchorsReceived));
    Logf("net: pose bandwidth tier_per_s=fast:%.1f,ordinary:%.1f,slow:%.1f tier_bytes_per_s=%.0f,%.0f,%.0f "
         "bone_bytes_per_update=%.1f bones_per_update=%.1f keys=%llu deltas=%llu acks=%llu ack_misses=%llu "
         "reference_misses=%llu decode_failures=%llu",
         bandwidth.tierUpdates[0] * perSecond, bandwidth.tierUpdates[1] * perSecond, bandwidth.tierUpdates[2] * perSecond,
         bandwidth.tierBytes[0] * perSecond, bandwidth.tierBytes[1] * perSecond, bandwidth.tierBytes[2] * perSecond,
         updates ? double(bandwidth.poseBytes) / updates : 0.0, updates ? double(bandwidth.poseBones) / updates : 0.0,
         static_cast<unsigned long long>(bandwidth.poseKeys), static_cast<unsigned long long>(bandwidth.poseDeltas),
         static_cast<unsigned long long>(bandwidth.poseAcks), static_cast<unsigned long long>(bandwidth.poseAckMisses),
         static_cast<unsigned long long>(bandwidth.poseReferenceMisses), static_cast<unsigned long long>(bandwidth.poseDecodeFailures));
    const double perUpdate = updates ? 1.0 / updates : 0.0;
    const double basisError = isHost ? double(bandwidth.poseMaxBasisError) : bandwidth.poseRigidBones ? 2.0 : 0.0;
    const double rotationBound = bandwidth.poseRigidBones ? bonewire::PoseRotationBoundDegrees(basisError) : 0;
    Logf("net: pose components bytes_per_update=header:%.1f,mask:%.1f,rotation:%.1f,translation:%.1f,padding:%.1f,control:%.1f,outer:%.1f "
         "included_bones_per_delta=%.1f predictive=%llu rigid_bones_per_update=%.1f "
         "host_observed_basis_error_q12=%d rotation_error_bound_deg=%.4f origin_error_cm=0 skin_200cm_error_bound_cm=%.4f",
         bandwidth.poseHeaderBytes * perUpdate, bandwidth.poseMaskBits * perUpdate / 8.0,
         bandwidth.poseRotationBits * perUpdate / 8.0, bandwidth.poseTranslationBits * perUpdate / 8.0,
         bandwidth.posePaddingBits * perUpdate / 8.0,
         bandwidth.poseControlBytes * perUpdate, bandwidth.poseOuterBytes * perUpdate,
         bandwidth.poseDeltas ? double(bandwidth.poseIncludedDeltaBones) / bandwidth.poseDeltas : 0.0,
         static_cast<unsigned long long>(bandwidth.posePredictiveUpdates), bandwidth.poseRigidBones * perUpdate,
         isHost ? static_cast<int>(bandwidth.poseMaxBasisError) : -1, rotationBound, basisError * 600.0 / 4096.0);
    Logf("net: bandwidth rates enet_tx_datagrams_per_s=%.1f enet_tx_bytes_per_s=%.1f "
         "steam_api_tx_datagrams_per_s=%.1f steam_api_tx_bytes_per_s=%.1f tx_total_bytes_per_s=%.1f "
         "tx_enet_relay_MB_per_s=%.3f relay_header_bytes_per_s=%.1f relay_header_pct=%.2f",
         (sentPackets - lastSentPackets) * perSecond, (sentData - lastSentData) * perSecond,
         steamPackets * perSecond, steamBytes * perSecond, framingBytes * perSecond,
         ((sentData - lastSentData) + (framingBytes - steamBytes)) * perSecond / 1000000.0,
         (framingBytes - steamBytes) * perSecond,
         steamBytes ? 100.0 * (framingBytes - steamBytes) / steamBytes : 0.0);
    Logf("net: bandwidth overhead enet_header_bytes_per_s=%.1f enet_ack_bytes_per_s=%.1f "
         "protocol_overhead_pct=%.2f enet_parse_failures=%llu enet_unparsed_datagrams=%llu",
         enetHeaderBytes * perSecond, enetAckBytes * perSecond,
         steamBytes ? 100.0 * ((framingBytes - steamBytes) + enetHeaderBytes) / steamBytes : 0.0,
         static_cast<unsigned long long>(traffic.txEnetParseFailures - lastSteamTraffic.txEnetParseFailures),
         static_cast<unsigned long long>(traffic.txEnetUnparsedDatagrams - lastSteamTraffic.txEnetUnparsedDatagrams));
    for (size_t i = 0; i < host->peerCount; ++i) {
        ENetPeer& peer = host->peers[i];
        if (peer.state != ENET_PEER_STATE_CONNECTED) continue;
        auto const snapshot = peerBudgets[i].Snapshot();
        Logf("net: valve peer=%u npc_stretch_permille=%u level=%u signals=%u queue_bytes=%u offered_bytes_per_s=%llu "
             "reliable_inflight=%u rtt_ms=%u base_rtt_ms=%u throttle=%u transitions=%u",
             isHost ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(peer.data)) : 1u, snapshot.stretchPermille,
             static_cast<unsigned>(snapshot.pressure), static_cast<unsigned>(snapshot.signals),
             static_cast<unsigned>(snapshot.queueBytes), static_cast<unsigned long long>(snapshot.offeredBytesPerSecond),
             peer.reliableDataInTransit, peer.roundTripTime, snapshot.baseRttMs, peer.packetThrottle,
             snapshot.pressureTransitions);
        Logf("steam-send: enet peer=%u throttle=%u/%u rtt_ms=%u reliable_inflight_bytes=%u "
             "queued_commands=%u reliable_queued_commands=%u reliable_sent_commands=%u "
             "reliability_owner=ENet steam_channels_are_not_lanes=1",
             isHost ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(peer.data)) : 1u,
             peer.packetThrottle, ENET_PEER_PACKET_THROTTLE_SCALE, peer.roundTripTime, peer.reliableDataInTransit,
             static_cast<unsigned>(enet_list_size(&peer.outgoingCommands)),
             static_cast<unsigned>(enet_list_size(&peer.outgoingSendReliableCommands)),
             static_cast<unsigned>(enet_list_size(&peer.sentReliableCommands)));
    }
}

bool HandleGameplayMessage(ENetPeer* peer, ENetPacket* packet, uint8_t type)
{
    switch (type) {
    case MsgNpcLease: {
        const uint8_t from = isHost ? PeerSlotOf(peer) : 1;
        if ((isHost && !from) || (!isHost && (!connected || peer != server)) ||
            !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return true;
        npclease::Message note;
        if (!npclease::DecodeMessage(packet->data + 1, packet->dataLength - 1, note) ||
            note.world != worldEpoch) {
            Log("net: invalid NPC lease identity/state refused");
            return true;
        }
        if (handlers.npcLease) handlers.npcLease(from, note);
        return true;
    }
    case MsgHitResult: {
        HitResultMsg message{};
        if (isHost) return true;
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
        if (!isHost || !UnpackFixed(packet, message)) return true;
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (handlers.talk && from) handlers.talk(from, message.begin != 0, message.guid);
        return true;
    }
    case MsgSummon: {
        SummonMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.note.spellItem);
        ClampCString(message.note.spawn);
        if (message.note.epoch != worldEpoch || message.note.ownerPlayer >= MaxPlayers) return true;
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.note.op == SummonCreature) { Log("net: client NPC spawn publication refused"); return true; }
            if (message.note.op == SummonCreatureRequest) message.note.ownerPlayer = message.from;
            else RelayClientBroadcast(peer, message);
        }
        if (handlers.summon && message.from) handlers.summon(message.from, message.note);
        return true;
    }
    case MsgEnclave: {
        EnclaveMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.note.name);
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (handlers.enclave && from) handlers.enclave(from, message.note);
        return true;
    }
    case MsgPlayerSpell: {
        PlayerSpellMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.script);
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.playerSpell && message.from && message.to == localId)
            handlers.playerSpell(message.from, message.script);
        return true;
    }
    case MsgRansack: {
        RansackMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.ransack && message.from && message.to == localId)
            handlers.ransack(message.from, message.note);
        return true;
    }
    case MsgWitness: {
        WitnessMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.to > 1 && message.to < MaxPlayers) {
                if (peers[message.to]) Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return true;
            }
            if (message.to == 0) RelayClientBroadcast(peer, message);
        }
        if (handlers.witness && message.from && (message.to == 0 || message.to == localId))
            handlers.witness(message.from, message.note);
        return true;
    }
    case MsgNpcSpell: {
        NpcSpellMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.script);
        if (isHost && RelayClientUnicast(peer, message, message.to)) return true;
        if (handlers.npcSpell && message.from && message.to == localId)
            handlers.npcSpell(message.from, message.script, message.target, message.args);
        return true;
    }
    case MsgShot: {
        ShotMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        static const uint8_t none[16] = {};
        if (isHost && std::memcmp(message.note.caster, none, 16)) {
            Log("net: client NPC projectile publication refused");
            return true;
        }
        ClampCString(message.note.arrow);
        ClampCString(message.note.spellItem);
        ClampCString(message.note.spell);
        if (message.note.epoch != worldEpoch || message.note.targetPlayer >= MaxPlayers) {
            Log("net: stale or invalid projectile refused");
            return true;
        }
        if (isHost) RelayClientBroadcast(peer, message, ChannelCombat);
        if (handlers.shot && message.from) handlers.shot(message.from, message.note);
        return true;
    }
    case MsgRagdoll: {
        RagdollMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (isHost) { Log("net: client NPC force publication refused"); return true; }
        if (message.note.epoch != worldEpoch || !message.note.sequence || !message.note.incarnation) return true;
        for (float v : message.note.at) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return true;
        for (float v : message.note.force) if (!std::isfinite(v) || std::fabs(v) > 1e9f) return true;
        if (handlers.ragdoll) handlers.ragdoll(message.note);
        return true;
    }
    case MsgDialogLine: {
        DialogLineMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.note.sample);
        ClampCString(message.note.text);
        if (isHost) RelayClientBroadcast(peer, message);
        if (handlers.dialogLine && message.from) handlers.dialogLine(message.from, message.note);
        return true;
    }
    case MsgOffer: {
        if (packet->dataLength < OfferHeaderSize) return true;
        OfferMsg message{};
        std::memcpy(&message, packet->data, OfferHeaderSize);
        if (message.note.count > MaxOfferEntries ||
            packet->dataLength != OfferHeaderSize + message.note.count * sizeof(OfferEntry)) return true;
        std::memcpy(&message, packet->data, packet->dataLength);
        for (auto& entry : message.note.entries) ClampCString(entry.name);
        if (isHost) {
            message.note.buyer = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.note.seller != 1) {
                const uint8_t to = message.note.seller;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.buyer)
                    Send(peers[to], &message, packet->dataLength, ChannelReliable, true);
                return true;
            }
        } else if (message.note.seller != localId) {
            return true;
        }
        if (handlers.offer && message.note.buyer) handlers.offer(message.note);
        return true;
    }
    case MsgOfferResult: {
        OfferResultMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (isHost) {
            message.note.seller = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.note.buyer != 1) {
                const uint8_t to = message.note.buyer;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.seller)
                    Send(peers[to], &message, sizeof(message), ChannelReliable, true);
                return true;
            }
        } else if (message.note.buyer != localId) {
            return true;
        }
        if (handlers.offerResult && message.note.seller) handlers.offerResult(message.note);
        return true;
    }
    case MsgItem: {
        ItemMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        ClampCString(message.note.name);
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (handlers.item && from) handlers.item(from, message.note);
        return true;
    }
    case MsgContents: {
        if (isHost || packet->dataLength < sizeof(ContentsHeader)) return true;
        ContentsHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        if (header.count <= MaxContentEntries &&
            packet->dataLength == sizeof(header) + header.count * sizeof(ContentEntry) &&
            handlers.contents) {
            ContentEntry entries[MaxContentEntries];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(ContentEntry));
            for (int i = 0; i < header.count; ++i) ClampCString(entries[i].name);
            handlers.contents(header.guid, entries, header.count);
        }
        return true;
    }
    case MsgContainerTx: {
        if (!isHost || packet->dataLength < sizeof(TxHeader)) return true;
        TxHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (header.count <= MaxContentEntries &&
            packet->dataLength == sizeof(header) + header.count * sizeof(ContentEntry) &&
            handlers.tx && from) {
            ContentEntry entries[MaxContentEntries];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(ContentEntry));
            for (int i = 0; i < header.count; ++i) ClampCString(entries[i].name);
            handlers.tx(from, header.guid, header.seq, entries, header.count);
        }
        return true;
    }
    case MsgTxResult: {
        TxResultMsg message{};
        if (isHost || !UnpackFixed(packet, message)) return true;
        if (handlers.txResult) handlers.txResult(message.guid, message.seq, message.ok != 0);
        return true;
    }
    case MsgWatch: {
        WatchMsg message{};
        if (!isHost || !UnpackFixed(packet, message)) return true;
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (handlers.watch && from) handlers.watch(from, message.guid);
        return true;
    }
    case MsgClock: {
        ClockMsg message{};
        if (!UnpackFixed(packet, message)) return true;
        if (handlers.clock) handlers.clock(message.state);
        return true;
    }
    case MsgDeath: {
        DeathMsg message{};
        if (isHost || !UnpackFixed(packet, message)) return true;
        if (handlers.death && message.by && message.by < MaxPlayers) handlers.death(message.by, message.guid);
        return true;
    }
    case MsgKilled: {
        KilledMsg message{};
        if (isHost || !UnpackFixed(packet, message)) return true;
        if (message.to == localId && message.epoch == worldEpoch && message.incarnation && handlers.killed)
            handlers.killed(message.guid, message.incarnation);
        return true;
    }
    case MsgLeave: {
        Leave leave{};
        if (isHost || !UnpackFixed(packet, leave)) return true;
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

} // namespace g3t::net::internal

namespace g3t::net {

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

void SetBandwidthBudget(uint32_t bytesPerSecond)
{
    budgetRate = (std::clamp)(bytesPerSecond, 64u * 1024, 4096u * 1024);
    if (host) for (size_t i = 0; i < host->peerCount; ++i) ResetBudget(&host->peers[i]);
}

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

BudgetSnapshot PeerBandwidth(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    return host && peer && PeerIndex(peer) < MaxPlayers ? peerBudgets[PeerIndex(peer)].Snapshot() : BudgetSnapshot{};
}

uint32_t NpcStretchPermille(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    return host && peer && peer->state == ENET_PEER_STATE_CONNECTED && PeerIndex(peer) < MaxPlayers
        ? peerBudgets[PeerIndex(peer)].NpcStretchPermille() : PeerBudget::StretchNormal;
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
    if (!host || !isHost || count < 0 || count > MaxContentEntries) return false;
    uint8_t buffer[sizeof(ContentsHeader) + MaxContentEntries * sizeof(ContentEntry)];
    ContentsHeader header{MsgContents, {}, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    SendBroadcast(buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
    return true;
}

bool SendContainerTx(uint8_t const guid[16], uint32_t seq, ContentEntry const* entries, int count)
{
    if (!host || isHost || !server || !connected || count < 0 || count > MaxContentEntries) return false;
    uint8_t buffer[sizeof(TxHeader) + MaxContentEntries * sizeof(ContentEntry)];
    TxHeader header{MsgContainerTx, {}, seq, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    SendToServer(buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
    return true;
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
    ItemMsg message{MsgItem, note};
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendNpcSpell(uint8_t to, char const* script, uint8_t const npcGuid[16], int32_t args)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    NpcSpellMsg message{MsgNpcSpell, localId, to, {}, {}, args};
    strncpy_s(message.script, script, _TRUNCATE);
    std::memcpy(message.target, npcGuid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendEnclave(EnclaveNote const& note, uint8_t to)
{
    if (!host) return;
    const EnclaveMsg message{MsgEnclave, note};
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
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
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
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
    if (!host || note.seller == 0 || note.seller >= MaxPlayers || note.seller == localId) return false;
    OfferMsg message{MsgOffer, note};
    message.note.buyer = localId;
    if (note.count > MaxOfferEntries) return false;
    const size_t size = OfferHeaderSize + note.count * sizeof(OfferEntry);
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

void Stats(void (*log)(char const*))
{
    char line[900];
    int n = 0;
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peers[i]->state == ENET_PEER_STATE_CONNECTED) {
                RttLine(peers[i], PeerName(i), line, sizeof(line));
                log(line + 5);
                ++n;
            }
    } else if (server && connected) {
        RttLine(server, "host", line, sizeof(line));
        log(line + 5);
        ++n;
    }
    Format(line, "NETSTATS peers=%d rtt_log=%d", n, rttLog ? 1 : 0);
    log(line);
}

void RttLogOff(void (*log)(char const*))
{
    rttLog = false;
    log("SWITCH rttlogoff ok=1");
}

uint32_t RoundTrip(uint8_t player)
{
    ENetPeer* peer = isHost ? (player < MaxPlayers ? peers[player] : nullptr) : (connected ? server : nullptr);
    return peer && peer->state == ENET_PEER_STATE_CONNECTED ? peer->roundTripTime : 0;
}

} // namespace g3t::net
