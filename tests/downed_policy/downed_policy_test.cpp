// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/downed_policy.h"
#include "../../src/replication.h"
#include <cassert>

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

int main()
{
    StickyAndRevive();
    TerminalAndWorldChanges();
    ReorderedSnapshotsAndRevives();
    HelperEligibilityAroundPendingRevive();
    return 0;
}
