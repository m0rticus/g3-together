// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/netbudget.h"

#include <cassert>
#include <cstdint>
#include <limits>

using g3t::net::BudgetCharge;
using g3t::net::BudgetClass;
using g3t::net::BudgetConfig;
using g3t::net::BudgetDecision;
using g3t::net::BudgetPressure;
using g3t::net::BudgetTelemetry;
using g3t::net::PeerBudget;

static BudgetConfig Config(uint32_t rate)
{
    BudgetConfig config;
    config.bytesPerSecond = rate;
    config.mtuBytes = 1200;
    config.envelopeBytes = 16;
    config.checksumBytes = 0;
    return config;
}

static void TestChargeAndSaturation()
{
    assert(BudgetCharge(0) == 0);
    assert(BudgetCharge(1172, 1200, 16, 0) == 1220);
    assert(BudgetCharge(1173, 1200, 16, 0) == 1269);
    assert(BudgetCharge(1173, 1172, 16, 0) == 1269);
    assert(BudgetCharge(1169, 1200, 16, 4) == 1265);
    assert(BudgetCharge(std::numeric_limits<uint64_t>::max(), 1200, 16, 0)
           == std::numeric_limits<uint64_t>::max());
    assert(BudgetCharge(100, 28, 16, 0) >= 100);
}

static void TestPreviewAndCommitAccounting()
{
    PeerBudget budget;
    budget.Reset(0, Config(256u * 1024u));

    assert(budget.Preview(BudgetClass::GameplayReliable, 1000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::PlayerState, 1000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::NpcFarIdle, 1000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::GameplayReliable, 1000) == BudgetDecision::Allow);
    auto before = budget.Snapshot();
    assert(before.chargedBytes == 0 && before.deferredCount == 0 && before.shedCount == 0);

    budget.RecordDecision(BudgetDecision::Defer);
    budget.RecordDecision(BudgetDecision::Shed);
    assert(budget.Snapshot().deferredCount == 1);
    assert(budget.Snapshot().shedCount == 1);

    budget.CommitQueued(BudgetClass::GameplayReliable, BudgetCharge(1173));
    assert(budget.Snapshot().chargedBytes == 1269);
    assert(budget.Snapshot().queueBytes == 1269);
    before = budget.Snapshot();
    budget.Preview(BudgetClass::NpcActive, 1269);
    assert(budget.Snapshot().chargedBytes == before.chargedBytes);

    budget.CommitQueued(BudgetClass::BulkReliable, 6000);
    assert(budget.Snapshot().chargedBytes == 7269);
}

static void TestRatesAndMilliRefill()
{
    for (uint32_t rate : {150u * 1024u, 256u * 1024u, 300u * 1024u}) {
        PeerBudget budget;
        budget.Reset(100, Config(rate));
        budget.CommitQueued(BudgetClass::GameplayReliable, 10000);
        const auto debited = budget.Snapshot().creditBytes;

        budget.Advance(101, BudgetTelemetry{});
        const auto oneMs = budget.Snapshot().creditBytes;
        budget.Advance(1100, BudgetTelemetry{});
        const auto oneSecond = budget.Snapshot().creditBytes;
        assert(oneMs >= debited);
        assert(oneSecond >= oneMs);

        PeerBudget refilled;
        refilled.Reset(100, Config(rate));
        refilled.CommitQueued(BudgetClass::GameplayReliable, 10000);
        refilled.Advance(1100, BudgetTelemetry{});
        assert(refilled.Snapshot().creditBytes >= static_cast<int64_t>(10u * 1024u));
    }

    PeerBudget milli;
    milli.Reset(0, Config(256u * 1024u));
    milli.CommitQueued(BudgetClass::GameplayReliable, 10000);
    milli.Advance(1, BudgetTelemetry{});
    milli.Advance(2, BudgetTelemetry{});
    milli.Advance(3, BudgetTelemetry{});
    milli.Advance(4, BudgetTelemetry{});
    auto snapshot = milli.Snapshot();
    assert(snapshot.creditBytes == 17262);

    PeerBudget exact;
    exact.Reset(0, Config(300u * 1024u));
    exact.CommitQueued(BudgetClass::GameplayReliable, 30720);
    exact.Advance(100, BudgetTelemetry{});
    assert(exact.Snapshot().creditBytes == 30720);
}

static void TestDebtWrapAndLongPause()
{
    PeerBudget debt;
    debt.Reset(0, Config(150u * 1024u));
    debt.CommitQueued(BudgetClass::GameplayReliable, 200000);
    auto snapshot = debt.Snapshot();
    assert(snapshot.creditBytes == -76800);
    assert(snapshot.overflowBytes >= 46400);
    assert(debt.Preview(BudgetClass::GameplayReliable, 1000) == BudgetDecision::Allow);
    assert(debt.Preview(BudgetClass::PlayerState, 1000) == BudgetDecision::Allow);

    PeerBudget wrap;
    wrap.Reset(UINT32_MAX - 10u, Config(256u * 1024u));
    const auto resetCredit = wrap.Snapshot().creditBytes;
    wrap.Advance(100, BudgetTelemetry{});
    assert(wrap.Snapshot().creditBytes >= resetCredit);
    wrap.Advance(UINT32_MAX - 9u, BudgetTelemetry{});
    assert(wrap.Snapshot().creditBytes >= resetCredit);

    PeerBudget backward;
    backward.Reset(1000, Config(256u * 1024u));
    backward.CommitQueued(BudgetClass::GameplayReliable, 10000);
    const auto before = backward.Snapshot().creditBytes;
    backward.Advance(900, BudgetTelemetry{});
    assert(backward.Snapshot().creditBytes == before);

    PeerBudget longPause;
    longPause.Reset(10, Config(256u * 1024u));
    longPause.CommitQueued(BudgetClass::GameplayReliable, 20000);
    longPause.Advance(10u + 3600000u, BudgetTelemetry{});
    assert(longPause.Snapshot().creditBytes >= 20000);
}

static void TestPressureClassesAndChunking()
{
    PeerBudget budget;
    budget.Reset(0, Config(256u * 1024u));

    assert(budget.Preview(BudgetClass::NpcFarIdle, 1000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Allow);

    BudgetTelemetry queue75{};
    queue75.queuedPayloadBytes = 256u * 1024u * 75u / 1000u;
    budget.Advance(1000, queue75);
    assert(budget.Snapshot().pressure == BudgetPressure::Constrained);
    assert(budget.Preview(BudgetClass::NpcFarIdle, 1000) == BudgetDecision::Shed);
    assert(budget.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Allow);

    BudgetTelemetry queue200{};
    queue200.queuedPayloadBytes = 256u * 1024u * 200u / 1000u;
    budget.Advance(2000, queue200);
    assert(budget.Snapshot().pressure == BudgetPressure::Critical);
    assert(budget.Preview(BudgetClass::NpcFarIdle, 1000) == BudgetDecision::Shed);
    assert(budget.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Shed);

    assert(budget.Preview(BudgetClass::GameplayReliable, 40000) == BudgetDecision::Allow);
    assert(budget.Preview(BudgetClass::BulkReliable, 7000) == BudgetDecision::NeedsChunking);

    PeerBudget bulk;
    bulk.Reset(0, Config(256u * 1024u));
    assert(bulk.Preview(BudgetClass::BulkReliable, 6000) == BudgetDecision::Allow);
    bulk.CommitQueued(BudgetClass::BulkReliable, 6000);
    assert(bulk.Preview(BudgetClass::BulkReliable, 1000) == BudgetDecision::Defer);
    assert(bulk.Preview(BudgetClass::BulkReliable, 7000) == BudgetDecision::NeedsChunking);

    BudgetTelemetry constrained{};
    constrained.queuedPayloadBytes = 256u * 1024u * 75u / 1000u;
    bulk.Advance(1000, constrained);
    assert(bulk.Preview(BudgetClass::BulkReliable, 1000) == BudgetDecision::Defer);

    PeerBudget window;
    window.Reset(0, Config(256u * 1024u));
    BudgetTelemetry blocked{};
    blocked.queuedPayloadBytes = 1;
    blocked.reliableInFlightBytes = 1200;
    blocked.reliableWindowBytes = 1200;
    window.Advance(1000, blocked);
    assert(window.Snapshot().pressure == BudgetPressure::Constrained);
}

static void TestRttLossRecoveryAndExpiry()
{
    PeerBudget startup;
    startup.Reset(0, Config(256u * 1024u));
    BudgetTelemetry startupRtt{};
    startupRtt.validRtt = true;
    startupRtt.rttMs = 500;
    startupRtt.referenceRttMs = 500;
    startup.Advance(100, startupRtt);
    assert(startup.Snapshot().pressure == BudgetPressure::Clear);

    PeerBudget stable;
    stable.Reset(0, Config(256u * 1024u));
    BudgetTelemetry high{};
    high.validRtt = true;
    high.rttMs = 900;
    high.referenceRttMs = 500;
    stable.Advance(100, high);
    assert(stable.Snapshot().pressure == BudgetPressure::Clear);
    high.queuedPayloadBytes = 256u * 1024u * 25u / 1000u;
    stable.Advance(200, high);
    assert(stable.Snapshot().pressure == BudgetPressure::Constrained);

    PeerBudget recovery;
    recovery.Reset(0, Config(256u * 1024u));
    BudgetTelemetry critical{};
    critical.queuedPayloadBytes = 256u * 1024u * 200u / 1000u;
    recovery.Advance(100, critical);
    assert(recovery.Snapshot().pressure == BudgetPressure::Critical);

    critical.queuedPayloadBytes = 256u * 1024u * 20u / 1000u;
    recovery.Advance(1100, critical);
    assert(recovery.Snapshot().pressure == BudgetPressure::Critical);
    recovery.Advance(2099, critical);
    assert(recovery.Snapshot().pressure == BudgetPressure::Critical);
    recovery.Advance(2100, critical);
    assert(recovery.Snapshot().pressure == BudgetPressure::Constrained);
    recovery.Advance(3100, critical);
    assert(recovery.Snapshot().pressure == BudgetPressure::Clear);

    PeerBudget interrupted;
    interrupted.Reset(0, Config(256u * 1024u));
    interrupted.Advance(100, critical);
    critical.queuedPayloadBytes = 0;
    interrupted.Advance(1100, critical);
    interrupted.Advance(1200, critical);
    critical.queuedPayloadBytes = 256u * 1024u * 200u / 1000u;
    interrupted.Advance(1300, critical);
    assert(interrupted.Snapshot().pressure == BudgetPressure::Critical);

    PeerBudget loss;
    loss.Reset(0, Config(256u * 1024u));
    BudgetTelemetry retry{};
    retry.validLoss = true;
    retry.lossPermille = 20;
    retry.retrySampleAt = 100;
    loss.Advance(100, retry);
    assert(loss.Snapshot().pressure == BudgetPressure::Constrained);

    retry.retrySampleAt = 100;
    loss.Advance(2101, retry);
    assert(loss.Snapshot().pressure == BudgetPressure::Constrained);
    loss.Advance(3101, retry);
    assert(loss.Snapshot().pressure == BudgetPressure::Clear);

    PeerBudget stableLoss;
    stableLoss.Reset(0, Config(256u * 1024u));
    BudgetTelemetry lossyDrained{};
    lossyDrained.validLoss = true;
    lossyDrained.lossPermille = 20;
    lossyDrained.retrySampleAt = 0;
    lossyDrained.queuedPayloadBytes = 0;
    stableLoss.Advance(100, lossyDrained);
    assert(stableLoss.Snapshot().pressure == BudgetPressure::Constrained);
    assert(stableLoss.Preview(BudgetClass::BulkReliable, 1000) == BudgetDecision::Allow);
    assert(stableLoss.Preview(BudgetClass::GameplayReliable, 1000) == BudgetDecision::Allow);

    uint64_t fileProgress = 0;
    for (uint32_t seconds = 1; seconds <= 180; ++seconds) {
        lossyDrained.retrySampleAt = 100;
        stableLoss.Advance(seconds * 1000u, lossyDrained);
        if (stableLoss.Preview(BudgetClass::BulkReliable, 1000) == BudgetDecision::Allow) {
            stableLoss.CommitQueued(BudgetClass::BulkReliable, 1000);
            fileProgress += 1000;
        }
    }
    assert(fileProgress > 0);
    assert(fileProgress <= 180u * 1000u);
    assert(stableLoss.Snapshot().pressure != BudgetPressure::Critical);
}

static void TestReserveAndFreshLossProgress()
{
    PeerBudget reserve;
    reserve.Reset(0, Config(256u * 1024u));
    reserve.CommitQueued(BudgetClass::GameplayReliable, 23000);
    assert(reserve.Snapshot().creditBytes == 3214);
    assert(reserve.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Defer);
    assert(reserve.Preview(BudgetClass::NpcActive, 800) == BudgetDecision::Allow);
    assert(reserve.Preview(BudgetClass::GameplayReliable, UINT64_MAX) == BudgetDecision::Allow);

    for (uint32_t rate : {64u * 1024u, 150u * 1024u, 256u * 1024u, 300u * 1024u}) {
        PeerBudget lossy;
        lossy.Reset(0, Config(rate));
        BudgetTelemetry sample;
        sample.validLoss = true;
        sample.lossPermille = 50;
        uint64_t transferred = 0;
        const uint64_t chunkCharge = BudgetCharge(1160);
        for (uint32_t now = 0; now < 10000; now += 10) {
            sample.retrySampleAt = now;
            lossy.Advance(now, sample);
            if (now % 30 == 0) lossy.CommitQueued(BudgetClass::PlayerState, BudgetCharge(66));
            while (lossy.Preview(BudgetClass::BulkReliable, chunkCharge) == BudgetDecision::Allow) {
                lossy.CommitQueued(BudgetClass::BulkReliable, chunkCharge);
                transferred += chunkCharge;
            }
            assert(lossy.Snapshot().pressure == BudgetPressure::Constrained);
            assert(lossy.Snapshot().creditBytes >= 2400);
        }
        assert(transferred > uint64_t(rate) * 9 / 8);
        assert(transferred <= uint64_t(rate) * 10 / 8 + 7500);
    }

    PeerBudget overflow;
    overflow.Reset(0, Config(150u * 1024u));
    overflow.CommitQueued(BudgetClass::GameplayReliable, 200000);
    overflow.Advance(500, BudgetTelemetry{});
    assert(overflow.Snapshot().pressure == BudgetPressure::Critical);
    assert(overflow.Preview(BudgetClass::NpcActive, 1000) == BudgetDecision::Shed);
    overflow.Advance(1001, BudgetTelemetry{});
    overflow.Advance(2001, BudgetTelemetry{});
    assert(overflow.Snapshot().pressure == BudgetPressure::Constrained);
    overflow.Advance(3001, BudgetTelemetry{});
    assert(overflow.Snapshot().pressure == BudgetPressure::Clear);

    PeerBudget lossyOverflow;
    lossyOverflow.Reset(0, Config(256u * 1024u));
    lossyOverflow.CommitQueued(BudgetClass::GameplayReliable, 300000);
    BudgetTelemetry freshLoss;
    freshLoss.validLoss = true;
    freshLoss.lossPermille = 50;
    for (uint32_t now : {500u, 1001u, 2001u}) {
        freshLoss.retrySampleAt = now;
        lossyOverflow.Advance(now, freshLoss);
    }
    assert(lossyOverflow.Snapshot().pressure == BudgetPressure::Constrained);
    assert(lossyOverflow.Preview(BudgetClass::BulkReliable, BudgetCharge(1160)) == BudgetDecision::Allow);
}

int main()
{
    TestChargeAndSaturation();
    TestPreviewAndCommitAccounting();
    TestRatesAndMilliRefill();
    TestDebtWrapAndLongPause();
    TestPressureClassesAndChunking();
    TestRttLossRecoveryAndExpiry();
    TestReserveAndFreshLossProgress();
    return 0;
}
