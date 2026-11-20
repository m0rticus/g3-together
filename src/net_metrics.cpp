// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "log.h"

#include <algorithm>

namespace g3t::net::internal {

namespace {
bool rttLog = true;
enet_uint32 lastRttLog = 0;

uint32_t QueueBytes(ENetList const& list)
{
    uint64_t bytes = 0;
    for (auto* node = list.sentinel.next; node != &list.sentinel; node = node->next)
        bytes += reinterpret_cast<ENetOutgoingCommand const*>(node)->fragmentLength;
    return static_cast<uint32_t>((std::min)(bytes, uint64_t(UINT32_MAX)));
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
} // namespace

BudgetClass SendPriority(uint8_t kind, bool reliable)
{
    if (reliable) return BudgetClass::GameplayReliable;
    if (kind == MsgState) return BudgetClass::PlayerState;
    return BudgetClass::NpcActive;
}

void UpdateBudgets(uint32_t now)
{
    if (!host) return;
    for (size_t i = 0; i < host->peerCount && i < MaxPlayers; ++i) {
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
            budgetLossAt[i] = now ? now : 1;
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

void ResetBudget(ENetPeer* peer)
{
    const size_t idx = PeerIndex(peer);
    if (!host || !peer || idx >= MaxPlayers) return;
    BudgetConfig config;
    config.bytesPerSecond = budgetRate;
    config.mtuBytes = peer->mtu;
    config.envelopeBytes = relayEnvelopeBytes;
    config.checksumBytes = host->checksum ? 4 : 0;
    peerBudgets[idx].Reset(enet_time_get(), config);
    budgetLossAt[idx] = budgetLastLost[idx] = 0;
}

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
    for (size_t i = 0; host && i < host->peerCount && i < MaxPlayers; ++i) {
        ENetPeer& peer = host->peers[i];
        if (peer.state != ENET_PEER_STATE_CONNECTED) continue;
        auto const snapshot = peerBudgets[i].Snapshot();
        const unsigned peerSlot = isHost ? static_cast<unsigned>(PeerSlotOf(&peer)) : 1u;
        Logf("net: valve peer=%u npc_stretch_permille=%u level=%u signals=%u queue_bytes=%u offered_bytes_per_s=%llu "
             "reliable_inflight=%u rtt_ms=%u base_rtt_ms=%u throttle=%u transitions=%u",
             peerSlot, snapshot.stretchPermille,
             static_cast<unsigned>(snapshot.pressure), static_cast<unsigned>(snapshot.signals),
             static_cast<unsigned>(snapshot.queueBytes), static_cast<unsigned long long>(snapshot.offeredBytesPerSecond),
             peer.reliableDataInTransit, peer.roundTripTime, snapshot.baseRttMs, peer.packetThrottle,
             snapshot.pressureTransitions);
        Logf("steam-send: enet peer=%u throttle=%u/%u rtt_ms=%u reliable_inflight_bytes=%u "
             "queued_commands=%u reliable_queued_commands=%u reliable_sent_commands=%u "
             "reliability_owner=ENet steam_channels_are_not_lanes=1",
             peerSlot, peer.packetThrottle, ENET_PEER_PACKET_THROTTLE_SCALE, peer.roundTripTime, peer.reliableDataInTransit,
             static_cast<unsigned>(enet_list_size(&peer.outgoingCommands)),
             static_cast<unsigned>(enet_list_size(&peer.outgoingSendReliableCommands)),
             static_cast<unsigned>(enet_list_size(&peer.sentReliableCommands)));
    }
}

} // namespace g3t::net::internal

namespace g3t::net {

void SetBandwidthBudget(uint32_t bytesPerSecond)
{
    budgetRate = (std::clamp)(bytesPerSecond, 64u * 1024, 4096u * 1024);
    if (host) {
        for (size_t i = 0; i < host->peerCount && i < MaxPlayers; ++i) {
            BudgetConfig config;
            config.bytesPerSecond = budgetRate;
            config.mtuBytes = host->peers[i].mtu;
            config.envelopeBytes = relayEnvelopeBytes;
            config.checksumBytes = host->checksum ? 4 : 0;
            peerBudgets[i].UpdateConfig(config);
        }
    }
}

BudgetSnapshot PeerBandwidth(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    const size_t idx = PeerIndex(peer);
    return host && peer && idx < MaxPlayers ? peerBudgets[idx].Snapshot() : BudgetSnapshot{};
}

uint32_t NpcStretchPermille(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    const size_t idx = PeerIndex(peer);
    return host && peer && peer->state == ENET_PEER_STATE_CONNECTED && idx < MaxPlayers
        ? peerBudgets[idx].NpcStretchPermille() : PeerBudget::StretchNormal;
}

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
