// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/downed_policy.h"
#include "../../src/replication.h"
#include "../../src/net.h"
#include <cassert>
#include <initializer_list>

using g3t::downed::Life;

// Model only the two helper-eligibility checks around the production Life policy.
struct PendingRevive
{
    Life& life;
    uint32_t sequence = 0;

    void Receive(uint32_t request, bool helperAble)
    {
        if (helperAble && life.held && !life.terminal && request && request == life.sequence)
            sequence = request;
    }

    bool Apply(bool helperAble)
    {
        if (!helperAble) sequence = 0;
        const uint32_t request = sequence;
        sequence = 0;
        return life.Revive(request);
    }
};

void StickyAndRevive()
{
    Life life;
    assert(!life.held && !life.terminal);
    assert(!life.Revive(0));
    life.Hold();
    const uint32_t first = life.sequence;
    assert(first && life.held && !life.terminal);
    // Native knock-outs, healing/standing snapshots and reconnect restores cannot release or replace this life.
    for (int i = 0; i < 10000; ++i) {
        life.Hold();
        life.Restore(false);
        assert(life.held && life.sequence == first);
    }
    assert(!life.Revive(0));
    assert(!life.Revive(first + 1));
    assert(life.held);
    assert(life.Revive(first));
    assert(!life.held && !life.terminal);
    assert(!life.Revive(first));
    life.Hold();
    assert(life.sequence != first);
    assert(!life.Revive(first)); // delayed paid revive from before this knock-out
    const uint32_t beforeLoad = life.sequence;
    life.OnWorldChange();
    assert(life.held && !life.terminal && life.sequence != beforeLoad);
    assert(!life.Revive(beforeLoad));
    assert(life.Revive(life.sequence));
    life.Restore(true); // a downed save loaded into a fresh/alive hero
    assert(life.held);
    life.sequence = UINT32_MAX;
    life.OnWorldChange();
    assert(life.sequence == 1 && life.held); // zero is reserved for not waiting
}

void TerminalAndWorldChanges()
{
    Life life;
    assert(life.End()); // ending an otherwise unheld life is still one-shot
    assert(life.terminal && !life.held);
    assert(!life.End());
    life.Hold();
    life.Restore(true);
    life.Restore(false);
    assert(life.terminal && !life.held && !life.Revive(life.sequence));
    const uint32_t unheldSequence = life.sequence;
    life.OnWorldChange();
    assert(!life.terminal && !life.held && life.sequence == unheldSequence);

    for (int i = 0; i < 100; ++i) {
        life.Hold();
        const uint32_t ended = life.sequence;
        assert(life.End());
        assert(life.terminal && !life.held);
        assert(!life.End()); // reentry cannot send another native Kill
        assert(!life.Revive(ended));
        life.Hold();
        life.Restore(true); // a delayed downed snapshot must not re-hold terminal life
        assert(life.terminal && !life.held && life.sequence == ended);
        life.OnWorldChange();
        assert(!life.terminal && !life.held && life.sequence == ended);
        life.Restore(true);
        assert(life.held && life.sequence != ended);
        assert(!life.Revive(ended)); // old-world or old-episode revive
        assert(life.Revive(life.sequence));
        assert(!life.Revive(life.sequence)); // duplicate
    }
}

void ReorderedSnapshotsAndRevives()
{
    Life life;
    g3t::replication::Sequence snapshots;
    assert(snapshots.Accept(100));
    life.Restore(true);
    const uint32_t first = life.sequence;
    assert(snapshots.Accept(102));
    life.Restore(false);
    assert(!snapshots.Accept(101)); // older alive snapshot after a newer downed one
    assert(!snapshots.Accept(102)); // duplicate
    assert(life.held && life.sequence == first);
    assert(life.Revive(first));
    life.Hold();
    const uint32_t second = life.sequence;
    assert(second != first && !life.Revive(first));
    assert(life.End());
    assert(snapshots.Accept(103));
    life.Restore(true); // even a newer downed snapshot cannot resurrect a terminal life
    assert(!life.held && life.terminal && !life.Revive(second));
    assert(!snapshots.Accept(102));
    life.OnWorldChange();
    life.Hold();
    assert(!life.Revive(second));
    assert(life.Revive(life.sequence));

    snapshots = {};
    assert(snapshots.Accept(UINT32_MAX));
    assert(snapshots.Accept(0));
    assert(!snapshots.Accept(UINT32_MAX));
}

void HelperEligibilityAroundPendingRevive()
{
    Life life;
    PendingRevive pending{life};
    life.Hold();
    const uint32_t first = life.sequence;
    pending.Receive(first, false); // helper down before the message arrives
    assert(!pending.Apply(true) && life.held);
    pending.Receive(first, true);
    assert(!pending.Apply(false) && life.held); // helper down after acceptance, before application
    assert(!pending.Apply(true) && life.held); // discarded message cannot revive later
    pending.Receive(first, true);
    assert(pending.Apply(true) && !life.held);
    pending.Receive(first, true);
    assert(!pending.Apply(true)); // duplicate after successful application

    life.Hold();
    const uint32_t second = life.sequence;
    pending.Receive(first, true);
    assert(!pending.Apply(true) && life.held);
    pending.Receive(second, true);
    pending.Receive(first, true); // late old request cannot replace the accepted current one
    pending.Receive(second, true); // duplicate while still pending
    assert(pending.Apply(true));
    assert(!pending.Apply(true));
    life.Hold();
    const uint32_t beforeLoad = life.sequence;
    pending.Receive(beforeLoad, true);
    life.OnWorldChange(); // accepted before loading, applied after rebind
    assert(!pending.Apply(true) && life.held);
    pending.Receive(life.sequence, true);
    assert(life.End()); // accepted before terminal decision, applied after it
    assert(!pending.Apply(true) && life.terminal && !life.held);
}

g3t::downed::Availability AvailabilityOf(g3t::net::Remote const& r, uint32_t now)
{
    return g3t::downed::HelperAvailability(r.connected, r.activity.known, r.activity.ticks, r.activity.flags, now);
}

void LocationAndPresentationDoNotDecideAvailability()
{
    using namespace g3t::downed;
    g3t::net::Remote partner;
    partner.connected = true;
    partner.latestTicks = 1; // pose is stale, off-screen, and in another world
    partner.latest.world = 42;
    partner.latest.position[0] = 1000000.0f;
    assert(partner.activity.Accept(100, 10000, 99, 17, 1, 0));
    uint32_t alone = 0;
    for (uint32_t now = 10000; now < 190000; now += 1000) {
        assert(partner.activity.Accept(101 + (now - 10000) / 1000, now, 99, 17, 1, 0));
        assert(AvailabilityOf(partner, now) == Availability::Able);
        assert(DecideEnd(true, false, now, alone) == EndDecision::Wait && !alone);
    }
    // World/zone/interior and rendering are deliberately not inputs to helper classification.
    partner.latest.world = 0;
    partner.latestTicks = 0;
    assert(AvailabilityOf(partner, 189000) == Availability::Able);
    assert(AvailabilityOf(partner, 193999) == Availability::Able);
    assert(AvailabilityOf(partner, 194000) == Availability::Stale);
    assert(DecideEnd(false, false, 194000, alone) == EndDecision::Wait);
    assert(DecideEnd(false, false, 198999, alone) == EndDecision::Wait);
    assert(DecideEnd(false, false, 199000, alone) == EndDecision::AloneGrace);
}

void RealUnavailableAndAllDown()
{
    using namespace g3t::downed;
    assert(HelperAvailability(false, true, 100, 1, 100) == Availability::Disconnected);
    assert(HelperAvailability(true, false, 0, 1, 100) == Availability::Unknown);
    assert(HelperAvailability(true, true, 100, 1, 5100) == Availability::Stale);
    for (uint8_t flags : {uint8_t(0), uint8_t(32), uint8_t(64)}) {
        const auto unavailable = HelperAvailability(true, true, 100, flags, 100);
        assert(unavailable == Availability::NotPlaying && !Playing(unavailable));
        uint32_t alone = 0;
        assert(DecideEnd(false, false, 100, alone) == EndDecision::Wait && alone == 100);
        assert(DecideEnd(false, false, 5099, alone) == EndDecision::Wait);
        assert(DecideEnd(false, false, 5100, alone) == EndDecision::AloneGrace);
    }
    for (uint8_t flags : {uint8_t(3), uint8_t(5), uint8_t(9), uint8_t(11), uint8_t(19)}) {
        const auto down = HelperAvailability(true, true, 100, flags, 100);
        assert(down == Availability::Down && Playing(down));
        uint32_t alone = 0;
        assert(DecideEnd(false, true, 100, alone) == EndDecision::AllDown && !alone);
        assert(DecideEnd(true, true, 100, alone) == EndDecision::Wait); // another alive candidate wins
    }
    uint32_t alone = 200;
    assert(DecideEnd(false, true, 201, alone) == EndDecision::AllDown); // helper goes down during grace
    assert(DecideEnd(true, false, 5199, alone) == EndDecision::Wait && alone == 0); // just before expiry
    assert(DecideEnd(false, false, 5200, alone) == EndDecision::Wait && alone == 5200); // new continuous absence
    assert(DecideEnd(false, false, 10199, alone) == EndDecision::Wait);
    assert(DecideEnd(false, false, 10200, alone) == EndDecision::AloneGrace);
}

void ActivityOrderingAndTimerWrap()
{
    using namespace g3t::downed;
    g3t::net::PlayerActivity activity;
    assert(activity.Accept(100, 1000, 42, 7, 11, 3));
    assert(!activity.Accept(99, 1001, 42, 7, 1, 0)); // delayed alive heartbeat cannot conceal downed state
    assert(!activity.Accept(100, 1002, 42, 7, 1, 0));
    assert(activity.flags == 11 && activity.ticks == 1000 && activity.downedSequence == 3);
    assert(activity.Accept(101, 1003, 77, 8, 1, 0)); // alive in another world is still alive
    activity = {};
    assert(activity.Accept(UINT32_MAX, UINT32_MAX - 100, 42, 7, 1, 0));
    assert(activity.Accept(0, 50, 42, 7, 11, 4));
    assert(!activity.Accept(UINT32_MAX, 51, 42, 7, 1, 0));
    assert(HelperAvailability(true, true, UINT32_MAX - 100, 1, 50) == Availability::Able);
    uint32_t alone = 0;
    assert(DecideEnd(false, false, UINT32_MAX - 100, alone) == EndDecision::Wait);
    assert(DecideEnd(false, false, 4898, alone) == EndDecision::Wait);
    assert(DecideEnd(false, false, 4899, alone) == EndDecision::AloneGrace);
    alone = 0;
    assert(DecideEnd(false, false, 0, alone) == EndDecision::Wait); // zero sentinel must not underflow into expiry
}

int main()
{
    StickyAndRevive();
    TerminalAndWorldChanges();
    ReorderedSnapshotsAndRevives();
    HelperEligibilityAroundPendingRevive();
    LocationAndPresentationDoNotDecideAvailability();
    RealUnavailableAndAllDown();
    ActivityOrderingAndTimerWrap();
    return 0;
}
