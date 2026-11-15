// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
#include "steam_runtime.h"

#include "log.h"

#include <algorithm>
#include <cstring>

namespace g3t::steam {
config::Read WorkerRuntime::ReadNetworkingConfig(int id, int scope, intptr_t handle) const
{
    config::Read read;
    if (!networkingUtils || !api.GetConfigValue) return read;
    size_t size = sizeof(read.value);
    read.result = api.GetConfigValue(networkingUtils, id, scope, handle, &read.type, &read.value, &size);
    if (size != sizeof(read.value)) {
        read.type = 0;
        read.result = -1;
    }
    return read;
}

bool WorkerRuntime::ConfigureNetworking()
{
    if (!networkingUtils || !api.SetConfigValue || !api.GetConfigValue) {
        Log("steam-send: config unavailable action=keep-Steam-defaults; capacity not verified (older stand-in/API)");
        return true;
    }
    bool verified = true;
    for (unsigned i = 0; i < 4; ++i) {
        auto const& setting = config::Settings[i];
        auto& applied = networkingConfig[i];
        applied = config::Apply(
            setting,
            [this](int id) { return ReadNetworkingConfig(id); },
            [this](int id, int32_t value) {
                return api.SetConfigValue(networkingUtils, id, config::GlobalScope, 0, config::Int32, &value) != 0;
            });
        Logf("steam-send: config scope=global name=%s before=%d requested=%d effective=%d "
             "read_result=%d/%d set=%s verified=%u action=%s",
             setting.name, applied.before.value, applied.requested, applied.after.value,
             applied.before.result, applied.after.result,
             !applied.attempted ? "unchanged" : applied.accepted ? "accepted" : "rejected",
             applied.Verified() ? 1u : 0u,
             setting.id == config::SendBufferSize ? "bounded-buffer" :
                 setting.id == config::SendRateMin ? "manual-send-rate-floor" :
                 setting.capacity ? "capacity-headroom" : "preserve-default");
        verified = verified && applied.Verified();
    }
    int32_t iceAll = config::IceEnableAll;
    if (api.SetConfigValue(networkingUtils, config::P2PTransportIceEnable, config::GlobalScope, 0, config::Int32, &iceAll))
        Log("steam-send: config scope=global name=P2P_Transport_ICE_Enable requested=7 action=prefer-direct-ice");
    networkingConfigured = verified;
    Log("steam-send: rate_policy=manual-headroom adaptive_bandwidth_probe=not-assumed "
        "queued-data-only=1 warning=offered-traffic-must-fit-real-upload; no-NPC-cap-or-shedding");
    return verified;
}

void WorkerRuntime::TransportConfigDiagnostics() const
{
    const auto ice = ReadNetworkingConfig(diagnostics::IceEnable);
    const auto icePenalty = ReadNetworkingConfig(diagnostics::IcePenalty);
    const auto sdrPenalty = ReadNetworkingConfig(diagnostics::SdrPenalty);
    size_t stunBytes = 0;
    int stunType = 0, stunResult = 0;
    if (networkingUtils && api.GetConfigValue)
        stunResult = api.GetConfigValue(networkingUtils, diagnostics::StunServerList,
                                       config::GlobalScope, 0, &stunType, nullptr, &stunBytes);
    Logf("steam: transport-config scope=global action=read-only ice_enable=%d ice_valid=%u ice_read_result=%d "
         "ice_penalty_ms=%d ice_penalty_valid=%u sdr_penalty_ms=%d sdr_penalty_valid=%u "
         "stun_list_bytes=%zu stun_type=%d stun_read_result=%d "
         "ice_default_minus_one=Steam-user-policy peer_resolved_config_available=0",
         ice.value, ice.Valid() ? 1u : 0u, ice.result, icePenalty.value, icePenalty.Valid() ? 1u : 0u,
         sdrPenalty.value, sdrPenalty.Valid() ? 1u : 0u, stunBytes, stunType, stunResult);
    const auto initial = ReadNetworkingConfig(diagnostics::TimeoutInitial);
    const auto connected = ReadNetworkingConfig(diagnostics::TimeoutConnected);
    const auto mtu = ReadNetworkingConfig(diagnostics::MtuPacketSize);
    const auto unencrypted = ReadNetworkingConfig(diagnostics::Unencrypted);
    Logf("steam: transport-config scope=global action=read-only timeout_initial_ms=%d timeout_initial_valid=%u "
         "timeout_connected_ms=%d timeout_connected_valid=%u mtu_packet_bytes=%d mtu_valid=%u "
         "unencrypted=%d unencrypted_valid=%u",
         initial.value, initial.Valid() ? 1u : 0u, connected.value, connected.Valid() ? 1u : 0u,
         mtu.value, mtu.Valid() ? 1u : 0u, unencrypted.value, unencrypted.Valid() ? 1u : 0u);
}

void WorkerRuntime::SendDiagnostics(uint32_t now)
{
    if (CountLinked()) {
        config::Read current[4];
        bool verified = networkingConfigured;
        for (unsigned i = 0; i < 4; ++i) {
            current[i] = ReadNetworkingConfig(config::Settings[i].id);
            const bool ok = config::Settings[i].exact
                ? current[i].value == networkingConfig[i].requested
                : current[i].value >= networkingConfig[i].requested;
            verified = verified && current[i].Valid() && ok;
        }
        Logf("steam-send: config-current scope=global verified=%u connection_config_verified=0 "
             "send_buffer=%d send_buffer_valid=%u send_rate_min=%d send_rate_min_valid=%u "
             "send_rate_max=%d send_rate_max_valid=%u nagle_us=%d nagle_valid=%u action=read-only",
             verified ? 1u : 0u, current[0].value, current[0].Valid() ? 1u : 0u,
             current[1].value, current[1].Valid() ? 1u : 0u, current[2].value, current[2].Valid() ? 1u : 0u,
             current[3].value, current[3].Valid() ? 1u : 0u);
        TransportConfigDiagnostics();
    }
    for (auto& r : remotes) {
        if (!r.linked) continue;
        auto const& c = r.pending.counters;
        Logf("steam-send: peer=%llu accepted_datagrams=%llu accepted_bytes=%llu "
             "critical=%llu/%llu definition=%llu/%llu npc_pose=%llu/%llu bulk=%llu/%llu "
             "enet_reliable_ok=%llu enet_unreliable_ok=%llu player_state_commands=%llu reliable_failed=%llu unreliable_failed=%llu "
             "retained_retries=%llu dedup_retransmit_commands=%llu",
             r.id, c.reliableAccepted + c.unreliableAccepted, c.bytes[0] + c.bytes[1] + c.bytes[2] + c.bytes[3],
             c.accepted[0], c.bytes[0], c.accepted[1], c.bytes[1], c.accepted[2], c.bytes[2], c.accepted[3], c.bytes[3],
             c.reliableAccepted, c.unreliableAccepted, c.playerStates, c.reliableFailed, c.unreliableFailed,
             c.retries, c.duplicates);
        Logf("steam-send: peer=%llu queue_datagrams=%zu queue_bytes=%zu oldest_ms=%u "
             "peak_datagrams=%zu peak_bytes=%zu peak_oldest_ms=%u plain_bytes=%llu wire_bytes=%llu "
             "unclassified=%llu action=retry-retained-independent-priority npc_updates_dropped=%llu",
             r.id, r.pending.Depth(), r.pending.Bytes(), r.pending.OldestAge(now),
             c.peakDepth, c.peakBytes, (std::max)(c.peakAge, r.pending.OldestAge(now)),
             c.plainBytes, c.wireBytes, c.unclassified, c.npcUpdatesDropped);
        Logf("steam-send: peer=%llu steam_reliable_ok=%llu steam_unreliable_ok=%llu "
             "data_flags=%d data_reliability=ENet-only config_verified=%u send_buffer=%d send_rate_min=%d "
             "send_rate_max=%d nagle_us=%d channels=data:0,control:1 channel_priority_lanes=%u",
             r.id, r.steamReliableOk, r.steamUnreliableOk, send::AutoRestart | send::NoDelay | send::NoNagle,
             networkingConfigured ? 1u : 0u, networkingConfig[0].after.value, networkingConfig[1].after.value,
             networkingConfig[2].after.value, networkingConfig[3].after.value,
             (socketsLaneTransport && r.p2pConn && r.p2pLanesConfigured) ? LaneCount : 0u);

        diagnostics::ConnectionInfo info{};
        diagnostics::SessionStatus status{};
        int state = 0;
        char const* source = "none";
        const bool preferSockets = socketsLaneTransport && r.p2pConn && r.p2pConnected &&
                                   api.GetConnectionInfo && api.GetConnectionRealTimeStatus;
        if (preferSockets) {
            const bool haveInfo = api.GetConnectionInfo(sockets, r.p2pConn, &info) != 0;
            const int statusResult = api.GetConnectionRealTimeStatus(sockets, r.p2pConn, &status, 0, nullptr);
            if (haveInfo && statusResult == ResultOk) {
                state = info.state;
                source = "sockets-v012";
            } else {
                info = {};
                status = {};
            }
        }
        if (state == 0 && api.GetSessionConnectionInfo && messages) {
            char identity[160];
            Identity(identity, r.id);
            state = api.GetSessionConnectionInfo(messages, identity, &info, &status);
            source = "messages";
        }

        if (std::strcmp(source, "none") != 0) {
            const bool current = diagnostics::Current(state, info, status);
            const auto description = diagnostics::Description(info);
            Logf("steam: path peer=%llu source=%s transport=%s evidence=%s available=%u state=%d info_state=%d status_state=%d "
                 "flags=0x%x pop_relay=0x%08x pop_remote=0x%08x peer_address_present=%u "
                 "ping_ms=%d est_bandwidth_bytes_per_s=%d estimate_kind=Steam-send-rate-not-link-speed "
                 "pending_unreliable_bytes=%d pending_reliable_bytes=%d queue_us=%lld "
                 "description=\"%s\" address_tokens=redacted",
                 r.id, source, diagnostics::Transport(state, info, status), diagnostics::Evidence(state, info, status),
                 current ? 1u : 0u, state, info.state, status.state, static_cast<unsigned>(info.flags),
                 info.popRelay, info.popRemote, diagnostics::HasRemoteAddress(info) ? 1u : 0u,
                 status.ping, status.sendRate, status.pendingUnreliable, status.pendingReliable,
                 static_cast<long long>(status.queueMicros), description.data());
            Logf("steam-send: session peer=%llu source=%s available=%u state=%d ping_ms=%d "
                 "send_rate_bytes_per_s=%d pending_unreliable_bytes=%d pending_reliable_bytes=%d "
                 "sent_unacked_reliable_bytes=%d queue_us=%lld out_packets_per_s=%.1f out_bytes_per_s=%.1f "
                 "quality_local=%.3f quality_remote=%.3f",
                 r.id, source, current ? 1u : 0u, state, status.ping, status.sendRate,
                 status.pendingUnreliable, status.pendingReliable, status.sentUnackedReliable,
                 static_cast<long long>(status.queueMicros), status.outPackets, status.outBytes,
                 status.qualityLocal, status.qualityRemote);
        } else {
            Logf("steam: path peer=%llu transport=unknown available=0 reason=status-export-unavailable", r.id);
            Logf("steam-send: session peer=%llu available=0 reason=status-export-unavailable", r.id);
        }
        for (unsigned result = 0; result < 128; ++result) {
            const uint64_t dataFailed = (std::max)(c.failed[result], r.steamDataFailed[result]);
            const uint64_t controlFailed = r.steamControlFailed[result];
            const uint64_t totalFailed = dataFailed + controlFailed;
            if (totalFailed)
                Logf("steam-send: peer=%llu result=%u failed_datagrams=%llu data_failed=%llu control_failed=%llu "
                     "data_action=%s control_action=control-flow-retry",
                     r.id, result, totalFailed, dataFailed, controlFailed,
                     send::Retryable(static_cast<int>(result)) ? "retained-for-retry" : "fallback-messages-or-close-session");
        }
        r.pending.counters = {};
        r.pending.counters.peakDepth = r.pending.Depth();
        r.pending.counters.peakBytes = r.pending.Bytes();
        r.pending.counters.peakAge = r.pending.OldestAge(now);
        r.steamReliableOk = r.steamUnreliableOk = 0;
        std::memset(r.steamDataFailed, 0, sizeof(r.steamDataFailed));
        std::memset(r.steamControlFailed, 0, sizeof(r.steamControlFailed));
    }
}
}
