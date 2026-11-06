// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/downed_policy.h"
#include "../../src/replication.h"
#include <cassert>

int main()
{
    g3t::downed::Life life;
    assert(!life.held);
    assert(!life.Revive(0));
    life.Hold();
    const uint32_t first = life.sequence;
    assert(first && life.held);
    // Repeated native knock-outs, healing/standing snapshots and reconnect restores do not release or replace this life.
    for (int i = 0; i < 10000; ++i) {
        life.Hold();
        life.Restore(false);
        assert(life.held && life.sequence == first);
    }
    assert(!life.Revive(0));
    assert(!life.Revive(first + 1));
    assert(life.held);
    assert(life.Revive(first));
    assert(!life.held);
    assert(!life.Revive(first));
    life.Hold();
    assert(life.sequence != first);
    assert(!life.Revive(first)); // a delayed paid revive from before this knock-out
    const uint32_t beforeLoad = life.sequence;
    life.Rebind();
    assert(life.held && !life.Revive(beforeLoad));
    assert(life.Revive(life.sequence));
    life.Restore(true); // a downed save loaded into a fresh/alive hero
    assert(life.held);
    life.sequence = UINT32_MAX;
    life.Rebind();
    assert(life.sequence == 1 && life.held); // zero is reserved for not waiting

    g3t::replication::Sequence order;
    assert(order.Accept(100));
    assert(order.Accept(101));
    assert(!order.Accept(100)); // an old alive state must not replace the newer downed one
    assert(!order.Accept(101));
    assert(order.Accept(102));
    order = {};
    assert(order.Accept(UINT32_MAX));
    assert(order.Accept(0));
    assert(!order.Accept(UINT32_MAX));
    return 0;
}
