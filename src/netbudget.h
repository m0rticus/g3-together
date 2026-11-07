// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure per-peer transport policy. It is an emergency valve plus bulk pacing, NOT a bandwidth cap:
//  * NPC pose/control, player state and gameplay reliable traffic are never refused or charged against a credit.
//    Preview() returns Allow for them in every state, so the normal NPC stream is not gated by this module.
//  * Real congestion signals (ENet's own packet throttle, a standing outgoing queue, RTT growth while data is queued,
//    fresh loss with a meaningful sample) raise NpcStretchPermille() from 1000 (normal) to 4000. The NPC scheduler multiplies
//    every tier interval and staleness limit by that one factor: all NPCs slow down together and proportionally, none is selected,
//    shed or starved. Normal LAN-like conditions keep the factor at exactly 1000.
//  * BulkReliable (reserved bulk traffic on channel 6) has its own token bucket: bytesPerSecond/4 when clear,
//    /8 when constrained, deferred while critical. It is decoupled from the NPC stream.
// Parent-owned Send commits one conservative BudgetCharge once only after the common successful send.

#include <algorithm>
#include <cstdint>
#include <limits>

namespace g3t::net {

enum class BudgetClass : uint8_t {
    GameplayReliable,
    PlayerState,
    NpcActive,
    BulkReliable,
    NpcFarIdle
};

enum class BudgetDecision : uint8_t {
    Allow,
    Defer,
    Shed,
    NeedsChunking
};

enum class BudgetPressure : uint8_t {
    Clear,
    Constrained,
    Critical
};

// Which real congestion signal raised the stretch, for diagnostics.
enum BudgetSignal : uint8_t {
    SignalNone = 0,
    SignalThrottle = 1,
    SignalQueue = 2,
    SignalRtt = 4,
    SignalLoss = 8,
    SignalWindow = 16
};

struct BudgetConfig {
    // Reserved bulk-channel pacing; never applied to NPC, player, character-save or gameplay traffic.
    uint32_t bytesPerSecond = 256u * 1024u;
    uint32_t mtuBytes = 1200u;
    uint32_t envelopeBytes = 16u;
    uint32_t checksumBytes = 0u;
};

struct BudgetTelemetry {
    uint64_t queuedPayloadBytes = 0;
    uint64_t reliableInFlightBytes = 0;
    uint64_t reliableWindowBytes = 0;
    uint32_t rttMs = 0;
    uint32_t throttle = 32u;          // ENet packetThrottle, 32 = unthrottled
    uint32_t lossPermille = 0;
    uint32_t retrySampleAt = 0;
    uint32_t packetsSent = 0;
    uint32_t packetsLost = 0;
    bool validRtt = false;
    bool validLoss = false;
};

struct BudgetSnapshot {
    BudgetPressure pressure = BudgetPressure::Clear;
    uint32_t stretchPermille = 1000;
    uint8_t signals = SignalNone;
    uint64_t queueBytes = 0;
    uint64_t chargedBytes = 0;
    uint64_t offeredBytesPerSecond = 0;
    uint64_t deferredCount = 0;
    uint64_t shedCount = 0;
    uint32_t pressureTransitions = 0;
    uint32_t baseRttMs = 0;
    int64_t bulkCreditBytes = 0;
};

inline uint64_t BudgetCharge(uint64_t payload, uint32_t mtu = 1200u,
                             uint32_t envelope = 16u, uint32_t checksum = 0u)
{
    static const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    if (!payload) return 0;

    uint64_t capacity = 0;
    if (static_cast<uint64_t>(mtu) > 28ull + checksum) capacity = static_cast<uint64_t>(mtu) - 28u - checksum;
    if (!capacity) capacity = 1;

    const uint64_t fragments = payload / capacity + (payload % capacity ? 1u : 0u);
    const uint64_t overheadPerFragment = 32u + static_cast<uint64_t>(envelope);
    uint64_t overhead = fragments;
    if (overheadPerFragment != 0 && overhead > maximum / overheadPerFragment)
        overhead = maximum;
    else
        overhead *= overheadPerFragment;

    return payload > maximum - overhead ? maximum : payload + overhead;
}

class PeerBudget {
public:
    static constexpr uint32_t StretchNormal = 1000u;
    static constexpr uint32_t StretchMax = 4000u;
    static constexpr uint32_t ThrottleScale = 32u;
    // The outgoing queue is judged against what this peer is actually being offered, never below this drain rate,
    // so one frame-end flush or a reliable burst on a healthy link is not congestion.
    static constexpr uint64_t DrainFloorBytesPerSecond = 512u * 1024u;
    static constexpr uint32_t QueueSignalMs = 100u;
    static constexpr uint32_t QueueSustainMs = 250u;
    static constexpr uint32_t DecayPermillePerSecond = 1000u;
    static constexpr uint32_t RttWindowMs = 10000u;
    static constexpr uint32_t MinLossSamples = 200u;
    static constexpr uint32_t MinLostPackets = 4u;
    static constexpr double RttOfferedFloorBytesPerSecond = 65536.0;

    PeerBudget() = default;

    void Reset(uint32_t nowMs, const BudgetConfig& config = BudgetConfig{})
    {
        config_ = config;
        if (!config_.mtuBytes) config_.mtuBytes = 1200u;
        bulkCapBytes_ = (std::max)(uint64_t(2) * config_.mtuBytes, uint64_t(config_.bytesPerSecond) / 40u);
        lastNowMs_ = nowMs;
        bulkCreditMilli_ = static_cast<int64_t>(bulkCapBytes_) * 1000;
        telemetry_ = BudgetTelemetry{};
        pressure_ = BudgetPressure::Clear;
        stretch_ = StretchNormal;
        signals_ = SignalNone;
        queueBytes_ = chargedBytes_ = deferredCount_ = shedCount_ = 0;
        offeredBytesPerSecond_ = 0.0;
        offeredSinceAdvance_ = 0;
        pressureTransitions_ = 0;
        queueSinceMs_ = UINT32_MAX;
        rttMinCurrent_ = rttMinPrevious_ = UINT32_MAX;
        rttWindowAtMs_ = nowMs;
        baseRttMs_ = 0;
    }

    void Advance(uint32_t nowMs, const BudgetTelemetry& telemetry = BudgetTelemetry{})
    {
        const uint32_t elapsed = Elapsed(lastNowMs_, nowMs);
        if (elapsed) {
            const double window = elapsed > 1000u ? 1000.0 : static_cast<double>(elapsed);
            const double alpha = window / 1000.0;
            const double sample = static_cast<double>(offeredSinceAdvance_) * 1000.0 / static_cast<double>(elapsed);
            offeredBytesPerSecond_ += (sample - offeredBytesPerSecond_) * alpha;
            offeredSinceAdvance_ = 0;
            RefillBulk(elapsed);
            lastNowMs_ = nowMs;
        }
        telemetry_ = telemetry;
        queueBytes_ = telemetry.queuedPayloadBytes;
        TrackBaseRtt(nowMs, telemetry);

        const uint64_t queueMs = QueueMilliseconds(telemetry.queuedPayloadBytes);
        uint8_t mask = SignalNone;
        const uint32_t target = StretchTarget(nowMs, queueMs, telemetry, mask);
        if (target >= stretch_) {
            stretch_ = target;
        } else {
            const uint64_t decay = static_cast<uint64_t>(elapsed) * DecayPermillePerSecond / 1000u;
            stretch_ = (std::max)(target, static_cast<uint32_t>(stretch_ > decay ? stretch_ - decay : StretchNormal));
        }
        if (stretch_ < StretchNormal) stretch_ = StretchNormal;
        if (stretch_ > StretchMax) stretch_ = StretchMax;
        signals_ = mask;

        BudgetPressure next = BudgetPressure::Clear;
        const bool windowBlocked = (mask & SignalWindow) != 0;
        if (stretch_ >= 2000u || (windowBlocked && queueMs >= 200u)) next = BudgetPressure::Critical;
        else if (stretch_ > StretchNormal || windowBlocked) next = BudgetPressure::Constrained;
        if (next != pressure_) {
            pressure_ = next;
            ++pressureTransitions_;
        }
    }

    // Only reserved bulk traffic is ever refused. Gameplay and character saves are always admitted.
    BudgetDecision Preview(BudgetClass cls, uint64_t chargedBytes) const
    {
        if (cls != BudgetClass::BulkReliable) return BudgetDecision::Allow;
        if (chargedBytes > bulkCapBytes_) return BudgetDecision::NeedsChunking;
        if (pressure_ == BudgetPressure::Critical) return BudgetDecision::Defer;
        return chargedBytes <= BulkAllowance() ? BudgetDecision::Allow : BudgetDecision::Defer;
    }

    uint64_t BulkAllowance() const
    {
        if (pressure_ == BudgetPressure::Critical) return 0;
        return (std::min)(BulkCreditBytes(), BulkLimit());
    }

    void CommitQueued(BudgetClass cls, uint64_t chargedBytes)
    {
        if (!chargedBytes) return;
        chargedBytes_ = SaturatingAdd(chargedBytes_, chargedBytes);
        queueBytes_ = SaturatingAdd(queueBytes_, chargedBytes);
        offeredSinceAdvance_ = SaturatingAdd(offeredSinceAdvance_, chargedBytes);
        if (cls != BudgetClass::BulkReliable) return;
        const int64_t charge = chargedBytes <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) / 1000u
            ? static_cast<int64_t>(chargedBytes) * 1000 : (std::numeric_limits<int64_t>::max)();
        bulkCreditMilli_ = bulkCreditMilli_ > charge ? bulkCreditMilli_ - charge : 0;
    }

    void RecordDecision(BudgetDecision decision)
    {
        if (decision == BudgetDecision::Defer) deferredCount_ = SaturatingAdd(deferredCount_, 1u);
        else if (decision == BudgetDecision::Shed) shedCount_ = SaturatingAdd(shedCount_, 1u);
    }

    // 1000 = unthrottled. Every NPC interval and staleness limit is multiplied by this single factor.
    uint32_t NpcStretchPermille() const { return stretch_; }
    BudgetPressure Pressure() const { return pressure_; }

    BudgetSnapshot Snapshot() const
    {
        BudgetSnapshot snapshot;
        snapshot.pressure = pressure_;
        snapshot.stretchPermille = stretch_;
        snapshot.signals = signals_;
        snapshot.queueBytes = queueBytes_;
        snapshot.chargedBytes = chargedBytes_;
        snapshot.offeredBytesPerSecond = static_cast<uint64_t>(offeredBytesPerSecond_);
        snapshot.deferredCount = deferredCount_;
        snapshot.shedCount = shedCount_;
        snapshot.pressureTransitions = pressureTransitions_;
        snapshot.baseRttMs = baseRttMs_;
        snapshot.bulkCreditBytes = bulkCreditMilli_ / 1000;
        return snapshot;
    }

private:
    static uint64_t SaturatingAdd(uint64_t left, uint64_t right)
    {
        return left > (std::numeric_limits<uint64_t>::max)() - right
            ? (std::numeric_limits<uint64_t>::max)() : left + right;
    }

    static uint32_t Elapsed(uint32_t from, uint32_t to)
    {
        const int32_t difference = static_cast<int32_t>(to - from);
        return difference > 0 ? static_cast<uint32_t>(difference) : 0u;
    }

    uint64_t QueueMilliseconds(uint64_t queued) const
    {
        if (!queued) return 0;
        const uint64_t drain = (std::max)(DrainFloorBytesPerSecond, static_cast<uint64_t>(offeredBytesPerSecond_));
        if (queued > (std::numeric_limits<uint64_t>::max)() / 1000u) return (std::numeric_limits<uint64_t>::max)();
        return queued * 1000u / drain;
    }

    uint64_t BulkCreditBytes() const
    {
        return bulkCreditMilli_ > 0 ? static_cast<uint64_t>(bulkCreditMilli_ / 1000) : 0u;
    }

    uint64_t BulkLimit() const
    {
        if (pressure_ != BudgetPressure::Constrained) return bulkCapBytes_;
        // A constrained link must still fit one complete unfragmented chunk.
        return (std::max)(uint64_t(config_.mtuBytes) + 32 + config_.envelopeBytes, bulkCapBytes_ / 2);
    }

    void RefillBulk(uint32_t elapsedMs)
    {
        if (pressure_ == BudgetPressure::Critical) return;
        const uint64_t perSecond = static_cast<uint64_t>(config_.bytesPerSecond) /
                                   (pressure_ == BudgetPressure::Constrained ? 8u : 4u);
        const int64_t cap = static_cast<int64_t>(BulkLimit()) * 1000;
        const uint64_t add = perSecond * elapsedMs;
        bulkCreditMilli_ = add >= static_cast<uint64_t>(cap) ? cap : (std::min)(cap, bulkCreditMilli_ + static_cast<int64_t>(add));
    }

    void TrackBaseRtt(uint32_t nowMs, const BudgetTelemetry& telemetry)
    {
        if (Elapsed(rttWindowAtMs_, nowMs) >= RttWindowMs) {
            rttMinPrevious_ = rttMinCurrent_;
            rttMinCurrent_ = UINT32_MAX;
            rttWindowAtMs_ = nowMs;
        }
        if (telemetry.validRtt && telemetry.rttMs && telemetry.rttMs < rttMinCurrent_) rttMinCurrent_ = telemetry.rttMs;
        const uint32_t base = (std::min)(rttMinCurrent_, rttMinPrevious_);
        baseRttMs_ = base == UINT32_MAX ? 0u : base;
    }

    // The target never exceeds StretchMax and is exactly StretchNormal without a real congestion signal.
    uint32_t StretchTarget(uint32_t nowMs, uint64_t queueMs, const BudgetTelemetry& telemetry, uint8_t& mask)
    {
        uint32_t target = StretchNormal;
        auto raise = [&](uint32_t value, BudgetSignal signal) {
            mask |= signal;
            if (value > target) target = value;
        };

        // ENet lowers its throttle itself when round trips outgrow their mean; follow it proportionally.
        if (telemetry.throttle < ThrottleScale)
            raise(ThrottleScale * 1000u / (std::max)(telemetry.throttle, 8u), SignalThrottle);

        // A queue deeper than QueueSignalMs of the offered rate that persists is congestion. A momentary
        // frame-end backlog or a reliable burst that drains is not.
        if (queueMs >= QueueSignalMs) {
            if (queueSinceMs_ == UINT32_MAX) queueSinceMs_ = nowMs;
            if (Elapsed(queueSinceMs_, nowMs) >= QueueSustainMs)
                raise(StretchNormal + static_cast<uint32_t>((std::min<uint64_t>)(queueMs * 5u, 3000u)), SignalQueue);
        } else {
            queueSinceMs_ = UINT32_MAX;
        }

        // RTT growth counts only while this peer is really being fed (a standing queue, or a stream of at least
        // RttOfferedFloorBytesPerSecond): a stalled (loading) peer receives nothing and measures a large ENet RTT without any
        // congestion. Steam's own send queue sits behind the loopback socket where ENet cannot see it; it shows up as
        // this RTT growth while the stream is on.
        if (telemetry.validRtt && baseRttMs_ && telemetry.rttMs > baseRttMs_ &&
            (queueMs >= 25u || offeredBytesPerSecond_ >= RttOfferedFloorBytesPerSecond)) {
            const uint32_t excess = telemetry.rttMs - baseRttMs_;
            if (excess >= (std::max)(75u, baseRttMs_))
                raise(StretchNormal + (std::min)(excess * 4u, 3000u), SignalRtt);
        }

        if (telemetry.validLoss && telemetry.packetsSent >= MinLossSamples &&
            telemetry.packetsLost >= MinLostPackets && RetryFresh(nowMs, telemetry.retrySampleAt)) {
            if (telemetry.lossPermille >= 150u) raise(2500u, SignalLoss);
            else if (telemetry.lossPermille >= 50u) raise(1500u, SignalLoss);
        }

        const bool windowBlocked = telemetry.reliableWindowBytes != 0 &&
            telemetry.reliableInFlightBytes >= telemetry.reliableWindowBytes && telemetry.queuedPayloadBytes != 0;
        if (windowBlocked) mask |= SignalWindow;
        return target;
    }

    static bool RetryFresh(uint32_t nowMs, uint32_t retryMs)
    {
        const uint32_t age = Elapsed(retryMs, nowMs);
        return age != 0u ? age <= 2000u : nowMs == retryMs;
    }

    BudgetConfig config_{};
    BudgetTelemetry telemetry_{};
    BudgetPressure pressure_ = BudgetPressure::Clear;
    uint32_t lastNowMs_ = 0;
    uint32_t stretch_ = StretchNormal;
    uint8_t signals_ = SignalNone;
    uint64_t bulkCapBytes_ = 2u * 1200u;
    int64_t bulkCreditMilli_ = 0;
    uint64_t queueBytes_ = 0;
    uint64_t chargedBytes_ = 0;
    uint64_t deferredCount_ = 0;
    uint64_t shedCount_ = 0;
    uint64_t offeredSinceAdvance_ = 0;
    double offeredBytesPerSecond_ = 0.0;
    uint32_t pressureTransitions_ = 0;
    uint32_t queueSinceMs_ = UINT32_MAX;
    uint32_t rttMinCurrent_ = UINT32_MAX, rttMinPrevious_ = UINT32_MAX, rttWindowAtMs_ = 0, baseRttMs_ = 0;
};

}
