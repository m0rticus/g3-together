// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/netbudget.h"

#include <cassert>
#include <cstdint>
#include <limits>

using namespace g3t::net;

static BudgetConfig Config(uint32_t rate = 256u * 1024u)
{
    BudgetConfig config;
    config.bytesPerSecond = rate;
    return config;
}

static void TestChargeAndSaturation()
{
    assert(BudgetCharge(0) == 0);
    assert(BudgetCharge(1172, 1200, 16, 0) == 1220);
    assert(BudgetCharge(1173, 1200, 16, 0) == 1269);
    assert(BudgetCharge(1173, 1172, 16, 0) == 1269);
    assert(BudgetCharge(1169, 1200, 16, 4) == 1265);
    assert(BudgetCharge(std::numeric_limits<uint64_t>::max()) == std::numeric_limits<uint64_t>::max());
    assert(BudgetCharge(100, 28, 16, 0) >= 100);
}

static void TestAdmissionAndAccounting()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    for (uint32_t throttle : {32u, 24u, 16u}) {
        BudgetTelemetry telemetry;
        telemetry.throttle = throttle;
        budget.Advance(100, telemetry);
        assert(budget.Pressure() == (throttle == 32 ? BudgetPressure::Clear :
                                    throttle == 24 ? BudgetPressure::Constrained : BudgetPressure::Critical));
    }
    assert(budget.Snapshot().chargedBytes == 0);
    budget.CommitQueued(BudgetClass::NpcActive, BudgetCharge(1173));
    assert(budget.Snapshot().chargedBytes == 1269 && budget.Snapshot().queueBytes == 1269);
    budget.CommitQueued(BudgetClass::GameplayReliable, UINT64_MAX);
    assert(budget.Snapshot().chargedBytes == UINT64_MAX && budget.Snapshot().queueBytes == UINT64_MAX);
}

static void TestUncappedOfferedStream()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    for (uint32_t now = 20; now <= 60000; now += 20) {
        budget.CommitQueued(BudgetClass::NpcActive, 20000);
        budget.Advance(now);
        assert(budget.NpcStretchPermille() == 1000);
        assert(budget.Pressure() == BudgetPressure::Clear);
        assert(budget.Snapshot().signals == SignalNone);
    }
    assert(budget.Snapshot().chargedBytes == 60000000);
    assert(budget.Snapshot().offeredBytesPerSecond >= 999000);
    assert(budget.Snapshot().offeredBytesPerSecond <= 1000000);
}

static void TestClockWrapAndBackwards()
{
    PeerBudget budget;
    budget.Reset(UINT32_MAX - 10u, Config());
    budget.CommitQueued(20000);
    budget.Advance(9); // 20 ms across uint32 wrap => 1,000,000 B/s sample with alpha = 0.02 -> 20,000 B/s
    const auto wrapped = budget.Snapshot().offeredBytesPerSecond;
    assert(wrapped == 20000);
    budget.Advance(8); // backwards timestamp is ignored
    assert(budget.Snapshot().offeredBytesPerSecond == wrapped);
    budget.Advance(10); // 1 ms idle decays slightly
    assert(budget.Snapshot().offeredBytesPerSecond < wrapped);
}

static void TestThrottleAndDecay()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    BudgetTelemetry telemetry;
    telemetry.throttle = 16;
    budget.Advance(100, telemetry);
    assert(budget.NpcStretchPermille() == 2000 && budget.Pressure() == BudgetPressure::Critical);
    assert(budget.Snapshot().signals & SignalThrottle);
    telemetry.throttle = 32;
    budget.Advance(600, telemetry);
    assert(budget.NpcStretchPermille() == 1500 && budget.Pressure() == BudgetPressure::Constrained);
    budget.Advance(1100, telemetry);
    assert(budget.NpcStretchPermille() == 1000 && budget.Pressure() == BudgetPressure::Clear);
    telemetry.throttle = 0;
    budget.Advance(1200, telemetry);
    assert(budget.NpcStretchPermille() == 4000);
    telemetry.throttle = 32;
    budget.Advance(4200, telemetry);  // 3 s at 1000 permille/s from the 4000 maximum
    assert(budget.NpcStretchPermille() == 1000);
}

static void TestSustainedQueueAndWindow()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    BudgetTelemetry telemetry;
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 9) / 10;
    budget.Advance(100, telemetry);
    budget.Advance(349, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    budget.Advance(350, telemetry);
    assert(budget.NpcStretchPermille() == 1500);
    assert(budget.Snapshot().signals & SignalQueue);
    telemetry.queuedPayloadBytes = UINT64_MAX;
    budget.Advance(351, telemetry);
    assert(budget.NpcStretchPermille() <= 4000);

    PeerBudget interrupted;
    interrupted.Reset(0, Config());
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 9) / 10;
    interrupted.Advance(100, telemetry);
    telemetry.queuedPayloadBytes = 0;
    interrupted.Advance(300, telemetry);
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 9) / 10;
    interrupted.Advance(350, telemetry);
    interrupted.Advance(599, telemetry);
    assert(interrupted.NpcStretchPermille() == 1000);
    interrupted.Advance(600, telemetry);
    assert(interrupted.NpcStretchPermille() == 1500);

    PeerBudget offered;
    offered.Reset(0, Config());
    offered.CommitQueued(BudgetClass::NpcActive, 1000000);
    offered.Advance(1000);
    telemetry.queuedPayloadBytes = 60000;
    offered.Advance(1100, telemetry);
    offered.Advance(1350, telemetry);
    assert(offered.NpcStretchPermille() == 1000);

    PeerBudget window;
    window.Reset(0, Config());
    telemetry.queuedPayloadBytes = 1;
    telemetry.reliableInFlightBytes = telemetry.reliableWindowBytes = 1200;
    window.Advance(100, telemetry);
    assert(window.Pressure() == BudgetPressure::Constrained);
    assert(window.Snapshot().signals & SignalWindow);
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 4) / 5;
    window.Advance(101, telemetry);
    assert(window.Pressure() == BudgetPressure::Critical);
}

static void TestRttWithAndWithoutQueue()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    BudgetTelemetry telemetry;
    telemetry.validRtt = true;
    telemetry.rttMs = 100;
    budget.Advance(1, telemetry);
    assert(budget.Snapshot().baseRttMs == 100);
    telemetry.rttMs = 1400;
    budget.Advance(100, telemetry);
    assert(budget.NpcStretchPermille() == 1000 && budget.Pressure() == BudgetPressure::Clear);
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 39) / 40;
    telemetry.rttMs = 199;
    budget.Advance(101, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    telemetry.rttMs = 200;
    budget.Advance(102, telemetry);
    assert(budget.NpcStretchPermille() == 1400);
    assert(budget.Snapshot().signals & SignalRtt);
    telemetry.rttMs = 1400;
    budget.Advance(103, telemetry);
    assert(budget.NpcStretchPermille() == 4000);

    PeerBudget loading;
    loading.Reset(0, Config());
    telemetry.queuedPayloadBytes = 0;
    loading.Advance(100, telemetry);
    assert(loading.Snapshot().baseRttMs == 1400);
    assert(loading.NpcStretchPermille() == 1000 && loading.Pressure() == BudgetPressure::Clear);

    PeerBudget lowBase;
    lowBase.Reset(0, Config());
    telemetry.rttMs = 50;
    lowBase.Advance(1, telemetry);
    telemetry.queuedPayloadBytes = (PeerBudget::DrainFloorBytesPerSecond + 39) / 40;
    telemetry.rttMs = 124;
    lowBase.Advance(2, telemetry);
    assert(lowBase.NpcStretchPermille() == 1000);
    telemetry.rttMs = 125;
    lowBase.Advance(3, telemetry);
    assert(lowBase.NpcStretchPermille() == 1300 && (lowBase.Snapshot().signals & SignalRtt));

    // Steam's own send queue is invisible to ENet: RTT growth while a real stream is offered (no ENet queue) is congestion,
    // the same RTT growth on an idle/stalled peer is not.
    PeerBudget streaming, stalled;
    streaming.Reset(0, Config());
    stalled.Reset(0, Config());
    telemetry.queuedPayloadBytes = 0;
    telemetry.rttMs = 25;
    streaming.Advance(1, telemetry);
    stalled.Advance(1, telemetry);
    for (uint32_t now = 1; now <= 2000; ++now) {
        streaming.CommitQueued(BudgetClass::NpcActive, 250);  // 250 KB/s
        telemetry.rttMs = 25;
        streaming.Advance(now, telemetry);
    }
    telemetry.rttMs = 140;
    streaming.Advance(2001, telemetry);
    stalled.Advance(2001, telemetry);
    assert(streaming.NpcStretchPermille() == 1460 && (streaming.Snapshot().signals & SignalRtt));
    assert(stalled.NpcStretchPermille() == 1000);
}

static void TestLossSamplesAndFreshness()
{
    PeerBudget budget;
    budget.Reset(0, Config());
    BudgetTelemetry telemetry;
    telemetry.validLoss = true;
    telemetry.lossPermille = 50;
    telemetry.retrySampleAt = 100;
    telemetry.packetsSent = 199;
    telemetry.packetsLost = 4;
    budget.Advance(100, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    telemetry.packetsSent = 200;
    telemetry.packetsLost = 3;
    budget.Advance(101, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    telemetry.packetsLost = 4;
    telemetry.validLoss = false;
    budget.Advance(102, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    telemetry.validLoss = true;
    telemetry.lossPermille = 49;
    budget.Advance(103, telemetry);
    assert(budget.NpcStretchPermille() == 1000);
    telemetry.lossPermille = 50;
    budget.Advance(104, telemetry);
    assert(budget.NpcStretchPermille() == 1500 && (budget.Snapshot().signals & SignalLoss));
    telemetry.lossPermille = 150;
    budget.Advance(105, telemetry);
    assert(budget.NpcStretchPermille() == 2500);
    budget.Advance(2101, telemetry);
    assert(!(budget.Snapshot().signals & SignalLoss));
    budget.Advance(4101, telemetry);
    assert(budget.NpcStretchPermille() == 1000);

    // Zero retrySampleAt never counts as a fresh loss sample, even during early uptime (nowMs <= 2000).
    PeerBudget zeroSample;
    zeroSample.Reset(0, Config());
    telemetry.retrySampleAt = 0;
    telemetry.lossPermille = 200;
    telemetry.packetsSent = 300;
    telemetry.packetsLost = 60;
    zeroSample.Advance(500, telemetry);
    assert(!(zeroSample.Snapshot().signals & SignalLoss));
    assert(zeroSample.NpcStretchPermille() == 1000);

    // UpdateConfig refreshes config without wiping learned RTT baseline or active stretch.
    BudgetTelemetry rttSample{};
    rttSample.validRtt = true;
    rttSample.rttMs = 100;
    rttSample.throttle = 32;
    budget.Advance(4105, rttSample);
    const uint32_t learnedRtt = budget.Snapshot().baseRttMs;
    assert(learnedRtt > 0);
    budget.UpdateConfig(Config(1024u * 1024u));
    assert(budget.Snapshot().baseRttMs == learnedRtt);
}

int main()
{
    TestChargeAndSaturation();
    TestAdmissionAndAccounting();
    TestUncappedOfferedStream();
    TestClockWrapAndBackwards();
    TestThrottleAndDecay();
    TestSustainedQueueAndWindow();
    TestRttWithAndWithoutQueue();
    TestLossSamplesAndFreshness();
    return 0;
}
