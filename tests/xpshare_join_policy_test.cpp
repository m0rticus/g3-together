#include "../src/xpshare.h"

#include <cassert>
#include <cstdint>

int main()
{
    using g3t::xpshare::counter::Ack;
    using g3t::xpshare::counter::Baseline;
    using g3t::xpshare::counter::BaselineEarned;
    using g3t::xpshare::counter::Live;
    using g3t::xpshare::counter::Receiver;
    using g3t::xpshare::counter::Sender;

    constexpr uint32_t maximum = 10000000;
    Sender sender;
    Receiver receiver;
    uint32_t earned = 900;

    // Admission excludes historical earnings, even when the first reward precedes the ACK.
    sender.Begin(17, earned);
    assert(sender.Total(earned) == 0);
    earned += 100;
    assert(sender.Total(earned) == 100);
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 0);
    assert(!receiver.known);
    assert(receiver.Snapshot(sender.nonce, BaselineEarned));
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 100);
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 0);

    // Lost ACKs/repeated baselines must never reset an already applied counter.
    assert(receiver.Snapshot(sender.nonce, BaselineEarned));
    assert(receiver.applied == 100);
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 0);
    earned += 50;
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 50);
    assert(receiver.Take(sender.nonce, 100, maximum) == 0);
    assert(receiver.applied == 150);

    // Rejoin/rewind creates a new stream at the current origin, not at the old total.
    sender.Begin(18, earned);
    assert(receiver.Snapshot(sender.nonce, BaselineEarned));
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 0);
    earned += 25;
    assert(receiver.Take(sender.nonce, sender.Total(earned), maximum) == 25);
    assert(receiver.Take(17, 200, maximum) == 0);
    assert(receiver.nonce == 18 && receiver.applied == 25);
    assert(!receiver.Snapshot(0, 0) && !receiver.Snapshot(19, 900));
    assert(receiver.nonce == 18 && receiver.applied == 25);
    assert(receiver.Take(18, maximum + 26, maximum) == 0);
    assert(receiver.applied == 25);

    Sender anotherPeer;
    anotherPeer.Begin(19, earned);
    earned += 10;
    assert(sender.Total(earned) == 35 && anotherPeer.Total(earned) == 10);
    assert(BaselineEarned == 0);
    assert(Live == 0);
    assert(Baseline == 1);
    assert(Ack == 2);
    return 0;
}
