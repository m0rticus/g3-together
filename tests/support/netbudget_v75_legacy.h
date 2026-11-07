// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Frozen copy of the protocol-75 build-18 per-peer budget (commit 2bf870d), namespace renamed, behaviour identical (only the
// int64 overflow clamps in Reset were dropped). It is used ONLY by tests/npc_stream_simulation.cpp to reproduce the starvation
// regression: the 256 KiB/s credit bucket that gated the NPC pose stream. Do not include it from src/.

#include <algorithm>
#include <cstdint>
#include <limits>

namespace g3t::net_legacy_v75 {

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

struct BudgetConfig {
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
    uint32_t referenceRttMs = 500u;
    uint32_t rttVarianceMs = 0;
    uint32_t throttle = 32u;
    uint32_t lossPermille = 0;
    uint32_t retrySampleAt = 0;
    bool validRtt = false;
    bool validLoss = false;
};

struct BudgetSnapshot {
    BudgetPressure pressure = BudgetPressure::Clear;
    int64_t creditBytes = 0;
    uint64_t queueBytes = 0;
    uint64_t chargedBytes = 0;
    uint64_t deferredCount = 0;
    uint64_t shedCount = 0;
    uint64_t overflowBytes = 0;
    uint32_t pressureTransitions = 0;

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
    PeerBudget() = default;

    void Reset(uint32_t nowMs, const BudgetConfig& config = BudgetConfig{})
    {
        config_ = config;
        if (!config_.mtuBytes) config_.mtuBytes = 1200u;

        const uint64_t mtu = config_.mtuBytes;
        const uint64_t rate = config_.bytesPerSecond;
        burstBytes_ = (std::max)(uint64_t(4) * mtu, rate / 10u);
        debtFloorBytes_ = (std::max)(uint64_t(4) * mtu, rate / 2u);
        reserveBytes_ = 2ull * mtu;
        bulkCapBytes_ = (std::max)(uint64_t(2) * mtu, rate / 40u);

        lastNowMs_ = nowMs;
        mainCreditMilli_ = static_cast<int64_t>(burstBytes_) * 1000;
        bulkCreditMilli_ = static_cast<int64_t>(bulkCapBytes_) * 1000;

        pressure_ = BudgetPressure::Clear;
        queueBytes_ = 0;
        chargedBytes_ = 0;
        deferredCount_ = 0;
        shedCount_ = 0;
        overflowBytes_ = 0;
        pressureTransitions_ = 0;
        lastOverflowMs_ = UINT32_MAX;
        recoverySinceMs_ = UINT32_MAX;
    }

    void Advance(uint32_t nowMs, const BudgetTelemetry& telemetry = BudgetTelemetry{})
    {
        const uint32_t elapsed = Elapsed(lastNowMs_, nowMs);
        if (elapsed) {
            Refill(elapsed);
            lastNowMs_ = nowMs;
        }
        telemetry_ = telemetry;
        queueBytes_ = telemetry.queuedPayloadBytes;

        const uint64_t queueMs = QueueMilliseconds(telemetry.queuedPayloadBytes);
        BudgetPressure candidate = Candidate(queueMs, telemetry);

        if (candidate > pressure_) {
            pressure_ = candidate;
            ++pressureTransitions_;
            recoverySinceMs_ = UINT32_MAX;
            return;
        }

        if (pressure_ == BudgetPressure::Clear) {
            recoverySinceMs_ = UINT32_MAX;
            return;
        }

        if (!RecoveryEligible(nowMs, queueMs, telemetry)) {
            recoverySinceMs_ = UINT32_MAX;
            return;
        }

        if (recoverySinceMs_ == UINT32_MAX) {
            recoverySinceMs_ = nowMs;
            return;
        }
        if (Elapsed(recoverySinceMs_, nowMs) < 1000u) return;

        pressure_ = pressure_ == BudgetPressure::Critical
            ? BudgetPressure::Constrained : BudgetPressure::Clear;
        ++pressureTransitions_;
        recoverySinceMs_ = nowMs;
    }

    BudgetDecision Preview(BudgetClass cls, uint64_t chargedBytes) const
    {
        if (cls == BudgetClass::GameplayReliable || cls == BudgetClass::PlayerState)
            return BudgetDecision::Allow;
        if (OverCapacity(chargedBytes)) return BudgetDecision::NeedsChunking;
        if (cls == BudgetClass::BulkReliable && chargedBytes > bulkCapBytes_)
            return BudgetDecision::NeedsChunking;

        switch (cls) {
        case BudgetClass::GameplayReliable:
        case BudgetClass::PlayerState:
            return BudgetDecision::Allow;

        case BudgetClass::NpcActive:
            if (pressure_ == BudgetPressure::Critical) return BudgetDecision::Shed;
            if (chargedBytes <= Allowance(cls))
                return BudgetDecision::Allow;
            return BudgetDecision::Defer;

        case BudgetClass::NpcFarIdle:
            if (pressure_ != BudgetPressure::Clear) return BudgetDecision::Shed;
            if (chargedBytes <= Allowance(cls))
                return BudgetDecision::Allow;
            return BudgetDecision::Shed;

        case BudgetClass::BulkReliable:
            if (pressure_ == BudgetPressure::Critical) return BudgetDecision::Defer;
            if (chargedBytes <= Allowance(cls))
                return BudgetDecision::Allow;
            return BudgetDecision::Defer;

        default:
            return BudgetDecision::Shed;
        }
    }

    uint64_t Allowance(BudgetClass cls) const
    {
        if (cls == BudgetClass::BulkReliable) {
            if (pressure_ == BudgetPressure::Critical ||
                (pressure_ == BudgetPressure::Constrained && QueueMilliseconds(queueBytes_) >= 25u)) return 0;
            const int64_t mainSigned = CreditBytes();
            const uint64_t main = mainSigned > static_cast<int64_t>(reserveBytes_)
                ? static_cast<uint64_t>(mainSigned) - reserveBytes_ : 0u;
            const uint64_t bulk = (std::min)(BulkCreditBytes(), BulkLimit());
            return main < bulk ? main : bulk;
        }
        if (cls == BudgetClass::NpcActive || cls == BudgetClass::NpcFarIdle) {
            if (pressure_ == BudgetPressure::Critical ||
                (cls == BudgetClass::NpcFarIdle && pressure_ != BudgetPressure::Clear)) return 0;
            const int64_t mainSigned = CreditBytes();
            const uint64_t credit = mainSigned > 0 ? static_cast<uint64_t>(mainSigned) : 0u;
            return credit > reserveBytes_ ? credit - reserveBytes_ : 0u;
        }
        if (mainCreditMilli_ >= 0)
            return static_cast<uint64_t>(mainCreditMilli_ / 1000);
        const uint64_t debtAvailable = static_cast<uint64_t>(
            static_cast<int64_t>(debtFloorBytes_) * 1000 + mainCreditMilli_) / 1000;
        return debtAvailable;
    }

    void CommitQueued(BudgetClass cls, uint64_t chargedBytes)
    {
        if (!chargedBytes) return;

        const int64_t floorMilli = -static_cast<int64_t>(debtFloorBytes_) * 1000;
        const int64_t availableMilli = SaturatingSubtract(mainCreditMilli_, floorMilli);
        const uint64_t availableBytes = availableMilli > 0
            ? static_cast<uint64_t>(availableMilli / 1000) : 0u;
        if (chargedBytes > availableBytes) {
            overflowBytes_ = SaturatingAdd(overflowBytes_, chargedBytes - availableBytes);
            mainCreditMilli_ = floorMilli;
            lastOverflowMs_ = lastNowMs_;
        } else {
            const int64_t charge = static_cast<int64_t>(chargedBytes) * 1000;
            mainCreditMilli_ = SaturatingSubtract(mainCreditMilli_, charge);
            if (mainCreditMilli_ < floorMilli) mainCreditMilli_ = floorMilli;
        }

        if (cls == BudgetClass::BulkReliable) {
            const int64_t charge = chargedBytes <=
                static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) / 1000u
                ? static_cast<int64_t>(chargedBytes) * 1000
                    : (std::numeric_limits<int64_t>::max)();
            bulkCreditMilli_ = SaturatingSubtract(bulkCreditMilli_, charge);
            if (bulkCreditMilli_ < 0) bulkCreditMilli_ = 0;
        }

        chargedBytes_ = SaturatingAdd(chargedBytes_, chargedBytes);
        queueBytes_ = SaturatingAdd(queueBytes_, chargedBytes);
    }

    void RecordDecision(BudgetDecision decision)
    {
        if (decision == BudgetDecision::Defer)
            deferredCount_ = SaturatingAdd(deferredCount_, 1u);
        else if (decision == BudgetDecision::Shed)
            shedCount_ = SaturatingAdd(shedCount_, 1u);
    }

    BudgetSnapshot Snapshot() const
    {
        BudgetSnapshot snapshot;
        snapshot.pressure = pressure_;
        snapshot.creditBytes = mainCreditMilli_ / 1000;
        snapshot.queueBytes = queueBytes_;
        snapshot.chargedBytes = chargedBytes_;
        snapshot.deferredCount = deferredCount_;
        snapshot.shedCount = shedCount_;
        snapshot.overflowBytes = overflowBytes_;
        snapshot.pressureTransitions = pressureTransitions_;
        snapshot.bulkCreditBytes = bulkCreditMilli_ / 1000;
        return snapshot;
    }

private:
    static uint64_t SaturatingAdd(uint64_t left, uint64_t right)
    {
        return left > (std::numeric_limits<uint64_t>::max)() - right
            ? (std::numeric_limits<uint64_t>::max)() : left + right;
    }

    static int64_t SaturatingAdd(int64_t left, int64_t right)
    {
        if (right > 0 && left > (std::numeric_limits<int64_t>::max)() - right)
            return (std::numeric_limits<int64_t>::max)();
        if (right < 0 && left < (std::numeric_limits<int64_t>::min)() - right)
            return (std::numeric_limits<int64_t>::min)();
        return left + right;
    }

    static int64_t SaturatingSubtract(int64_t left, int64_t right)
    {
        if (right > 0 && left < (std::numeric_limits<int64_t>::min)() + right)
            return (std::numeric_limits<int64_t>::min)();
        if (right < 0 && left > (std::numeric_limits<int64_t>::max)() + right)
            return (std::numeric_limits<int64_t>::max)();
        return left - right;
    }

    static uint32_t Elapsed(uint32_t from, uint32_t to)
    {
        const int32_t difference = static_cast<int32_t>(to - from);
        return difference > 0 ? static_cast<uint32_t>(difference) : 0u;
    }

    uint64_t QueueMilliseconds(uint64_t queued) const
    {
        if (!queued) return 0;
        const uint64_t rate = config_.bytesPerSecond;
        if (!rate) return (std::numeric_limits<uint64_t>::max)();
        if (queued > (std::numeric_limits<uint64_t>::max)() / 1000u)
            return (std::numeric_limits<uint64_t>::max)();
        const uint64_t numerator = queued * 1000u;
        return numerator / rate + (numerator % rate ? 1u : 0u);
    }

    int64_t CreditBytes() const
    {
        return mainCreditMilli_ / 1000;
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

    bool OverCapacity(uint64_t charged) const
    {
        if (!charged) return false;
        const uint64_t mtuLimit = 4u * config_.mtuBytes;
        uint64_t limit = burstBytes_ > mtuLimit ? burstBytes_ : mtuLimit;
        if (config_.bytesPerSecond == 0) limit = mtuLimit;
        if (charged > limit) return true;
        return false;
    }

    void Refill(uint32_t elapsedMs)
    {
        const uint64_t mainAdd = static_cast<uint64_t>(config_.bytesPerSecond) *
                                 static_cast<uint64_t>(elapsedMs);
        const uint64_t bulkAdd = (static_cast<uint64_t>(config_.bytesPerSecond) /
                                 (pressure_ == BudgetPressure::Constrained ? 8u : 4u)) *
                                 static_cast<uint64_t>(elapsedMs);

        const int64_t mainCap = static_cast<int64_t>(burstBytes_) * 1000;
        const int64_t bulkCap = static_cast<int64_t>(BulkLimit()) * 1000;

        if (mainAdd <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
            const int64_t addMilli = static_cast<int64_t>(mainAdd);
            mainCreditMilli_ = SaturatingAdd(mainCreditMilli_, addMilli);
        } else {
            mainCreditMilli_ = mainCap;
        }
        if (mainCreditMilli_ > mainCap) mainCreditMilli_ = mainCap;

        if (bulkAdd <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
            const int64_t addMilli = static_cast<int64_t>(bulkAdd);
            bulkCreditMilli_ = SaturatingAdd(bulkCreditMilli_, addMilli);
        } else {
            bulkCreditMilli_ = bulkCap;
        }
        if (bulkCreditMilli_ > bulkCap) bulkCreditMilli_ = bulkCap;
    }

    static uint64_t Maximum(uint64_t left, uint64_t right) { return left > right ? left : right; }

    BudgetPressure Candidate(uint64_t queueMs, const BudgetTelemetry& telemetry) const
    {
        BudgetPressure candidate = BudgetPressure::Clear;
        if (mainCreditMilli_ <= -static_cast<int64_t>(debtFloorBytes_) * 1000 ||
            (lastOverflowMs_ != UINT32_MAX && Elapsed(lastOverflowMs_, lastNowMs_) < 1000))
            candidate = BudgetPressure::Critical;

        if (queueMs >= 200u) candidate = BudgetPressure::Critical;
        else if (queueMs >= 75u && candidate < BudgetPressure::Constrained) candidate = BudgetPressure::Constrained;

        const bool windowBlocked = telemetry.reliableWindowBytes != 0 &&
            telemetry.reliableInFlightBytes >= telemetry.reliableWindowBytes;
        if (windowBlocked && telemetry.queuedPayloadBytes != 0) {
            const uint64_t effectiveMs = queueMs > 25u ? queueMs : 25u;
            if (effectiveMs >= 200u) candidate = BudgetPressure::Critical;
            else if (effectiveMs >= 75u || effectiveMs >= 25u) {
                if (candidate < BudgetPressure::Constrained)
                    candidate = BudgetPressure::Constrained;
            }
        }

        if (telemetry.validLoss) {
            const bool fresh = RetryFresh(lastNowMs_, telemetry.retrySampleAt);
            if (fresh) {
                if (telemetry.lossPermille >= 100u) {
                    if (candidate < BudgetPressure::Critical)
                        candidate = BudgetPressure::Critical;
                } else if (telemetry.lossPermille >= 20u &&
                           candidate < BudgetPressure::Constrained) {
                    candidate = BudgetPressure::Constrained;
                }
            }
        }

        if (telemetry.validRtt && telemetry.referenceRttMs && queueMs >= 25u &&
            telemetry.rttMs >= telemetry.referenceRttMs) {
            const uint64_t halfReference = telemetry.referenceRttMs / 2u;
            const uint64_t threshold = Maximum(25u, halfReference);
            if (static_cast<uint64_t>(telemetry.rttMs) >=
                static_cast<uint64_t>(telemetry.referenceRttMs) + threshold) {
                if (queueMs >= 75u) {
                    if (candidate < BudgetPressure::Critical)
                        candidate = BudgetPressure::Critical;
                } else if (candidate < BudgetPressure::Constrained) {
                    candidate = BudgetPressure::Constrained;
                }
            }
        }

        if (telemetry.throttle < 32u && queueMs >= 25u &&
            candidate < BudgetPressure::Constrained)
            candidate = BudgetPressure::Constrained;

        return candidate;
    }

    bool RetryFresh(uint32_t nowMs, uint32_t retryMs) const
    {
        const uint32_t age = Elapsed(retryMs, nowMs);
        return age != 0u ? age <= 2000u : nowMs == retryMs;
    }

    bool RecoveryEligible(uint32_t nowMs, uint64_t queueMs,
                          const BudgetTelemetry& telemetry) const
    {
        if (queueMs >= 25u) return false;
        if (mainCreditMilli_ / 1000 <= static_cast<int64_t>(reserveBytes_)) return false;
        if (lastOverflowMs_ != UINT32_MAX &&
            Elapsed(lastOverflowMs_, nowMs) < 1000u) return false;

        if (!telemetry.validLoss) return true;
        if (telemetry.lossPermille < 10u) return true;
        if (pressure_ == BudgetPressure::Critical && telemetry.lossPermille < 100u) return true;
        return !RetryFresh(nowMs, telemetry.retrySampleAt);
    }

    BudgetConfig config_{};
    BudgetTelemetry telemetry_{};
    BudgetPressure pressure_ = BudgetPressure::Clear;
    uint32_t lastNowMs_ = 0;
    uint64_t burstBytes_ = 4u * 1200u;
    uint64_t debtFloorBytes_ = 4u * 1200u;
    uint64_t reserveBytes_ = 2u * 1200u;
    uint64_t bulkCapBytes_ = 2u * 1200u;
    int64_t mainCreditMilli_ = 0;
    int64_t bulkCreditMilli_ = 0;
    uint64_t queueBytes_ = 0;
    uint64_t chargedBytes_ = 0;
    uint64_t deferredCount_ = 0;
    uint64_t shedCount_ = 0;
    uint64_t overflowBytes_ = 0;
    uint32_t pressureTransitions_ = 0;
    uint32_t lastOverflowMs_ = UINT32_MAX;
    uint32_t recoverySinceMs_ = UINT32_MAX;
};

}
