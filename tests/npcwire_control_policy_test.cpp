// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free executable tests for NPC control transaction policy.
#include "../src/npcwire_control_policy.h"

#include <cassert>

using namespace g3t::npcwire::control_policy;

int main()
{
    Reference first{1, 200, 7, 100};
    BaselinePin pin{first, 0x81, true};
    assert(SameLifeDefinition(pin, first, 7, 0x81));
    assert(!SameLifeDefinition(pin, first, 8, 0x81));

    // Same-life incompatible samples stay on the pinned definition until ACK.
    assert(!ShouldAllocateBaseline(true, false, false, true, true));
    assert(!ShouldAllocateBaseline(true, false, true, true, false));
    assert(ShouldAllocateBaseline(true, true, true, true, false));
    assert(ShouldAllocateBaseline(true, false, false, false, true));

    OfferTransaction offer{first, 9001, false, false, false};
    assert(RetryOffer(offer, first));
    assert(ShouldSendId(offer) && ShouldSendBaseline(offer));

    offer.idQueued = true;
    assert(RetryOffer(offer, first));
    assert(!ShouldSendId(offer) && ShouldSendBaseline(offer));

    offer.idQueued = false;
    offer.baselineQueued = true;
    assert(ShouldSendId(offer) && !ShouldSendBaseline(offer));

    offer.idQueued = true;
    offer.baselineQueued = true;
    assert(AcceptAck(offer, first, 9001));
    assert(!AcceptAck(offer, first, 9002));
    assert(!AcceptAck(offer, {1, 201, 7, 100}, 9001));

    // Duplicate and stale resync cursors remain idempotent.
    ResyncCursor cursor{first, ResyncReason::UnknownBaseline, true};
    assert(DuplicateResync(cursor, first, ResyncReason::UnknownBaseline));
    assert(!DuplicateResync(cursor, {1, 201, 7, 100}, ResyncReason::UnknownBaseline));
    assert(!DuplicateResync(cursor, first, ResyncReason::UnknownAnchor));
    assert(RetryAt(3100, 1000, 1000));
    assert(!RetryAt(1999, 1000, 1000));

    // BUG-3: Resync invalidation, queue-gated cursor commit, and fresh serials on forced resync.
    uint32_t nextSerial = 5000;
    OfferTransaction tx{};
    assert(BeginOffer(tx, first, false, nextSerial));
    assert(tx.serial == 5001 && nextSerial == 5001 && !tx.acknowledged && !tx.idQueued && !tx.baselineQueued);
    tx.idQueued = true;
    // Ordinary retry preserves queued ID progress and current serial.
    assert(BeginOffer(tx, first, false, nextSerial));
    assert(tx.serial == 5001 && nextSerial == 5001 && tx.idQueued && !tx.baselineQueued);
    tx.baselineQueued = true;
    assert(!AlreadySatisfied(tx, first, false));
    assert(CompleteAck(tx, first, 5001));
    assert(tx.acknowledged && AlreadySatisfied(tx, first, false));
    assert(!AlreadySatisfied(tx, first, true));

    // Forced resync offer clears acknowledged/queued flags and issues a fresh serial so a late old ACK fails.
    ResyncCursor resync{};
    assert(!CommitResync(resync, first, ResyncReason::UnknownBaseline, false));
    assert(!DuplicateResync(resync, first, ResyncReason::UnknownBaseline));
    assert(BeginOffer(tx, first, true, nextSerial));
    assert(tx.serial == 5002 && nextSerial == 5002 && !tx.acknowledged && !tx.idQueued && !tx.baselineQueued);
    tx.idQueued = true;
    tx.baselineQueued = true;
    assert(CommitResync(resync, first, ResyncReason::UnknownBaseline, true));
    assert(DuplicateResync(resync, first, ResyncReason::UnknownBaseline));
    assert(!CompleteAck(tx, first, 5001));
    assert(!tx.acknowledged);
    assert(CompleteAck(tx, first, 5002));
    assert(tx.acknowledged && AlreadySatisfied(tx, first, false));
    return 0;
}
