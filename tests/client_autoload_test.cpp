// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_autoload.h"
#include "../src/client_rejoin.h"

#include <cassert>
#include <cstdint>

using namespace g3t::autoload;

namespace {
Snapshot Ready()
{
    Snapshot s;
    s.enabled = s.joined = s.hostPlayable = s.routeAvailable = s.entryReady = s.recordKnown = true;
    s.epoch = 7;
    return s;
}
bool Enters(Decision const& d) { return d.action == Action::Enter; }

// Independent statement of the documented priority order for a fresh controller's first step.
Status Expected(Snapshot const& s)
{
    if (!s.enabled) return Status::Off;
    if (!s.joined) return Status::NotJoined;
    if (s.refused) return Status::Refused;
    if (!s.epoch || !s.hostPlayable) return Status::HostLoading;
    if (s.admitted) return Status::Idle;
    if (s.localWorld && !s.queued) return Status::InWorld;
    if (!s.routeAvailable) return Status::NoRoute;
    if (s.busy || !s.entryReady) return Status::Busy;
    return s.recordKnown ? Status::Countdown : Status::WaitRecord;
}

void ExhaustiveTable()
{
    for (unsigned mask = 0; mask < (1u << 12); ++mask) {
        Snapshot s;
        s.enabled = mask & 1; s.joined = mask & 2; s.refused = mask & 4; s.hostPlayable = mask & 8;
        s.localWorld = mask & 16; s.queued = mask & 32; s.admitted = mask & 64; s.busy = mask & 128;
        s.routeAvailable = mask & 256; s.entryReady = mask & 512; s.recordKnown = mask & 1024;
        for (uint32_t epoch : {0u, 9u}) {
            s.epoch = epoch;
            Controller c;
            const auto first = c.Step(s, 100);
            assert(first.action == Action::None); // the first eligible instant only starts a wait or the countdown
            assert(first.status == Expected(s));
            const bool eligible = Expected(s) == Status::Countdown || Expected(s) == Status::WaitRecord;
            const auto later = c.Step(s, 100 + CountdownMs);
            if (Expected(s) == Status::Countdown) {
                assert(Enters(later) && later.status == Status::Countdown && c.Entering());
                assert(first.remainingMs == CountdownMs);
            } else if (Expected(s) == Status::WaitRecord) {
                assert(!Enters(later) && later.status == Status::WaitRecord); // wording wait, bounded below
                const auto timedOut = c.Step(s, 100 + RecordWaitMs);
                assert(timedOut.status == Status::Countdown && !Enters(timedOut));
                assert(Enters(c.Step(s, 100 + RecordWaitMs + CountdownMs)));
            } else {
                assert(!Enters(later) && later.status == Expected(s) && !eligible);
            }
        }
    }
}

void JoinLaterAndCountdown()
{
    Controller c;
    Snapshot s = Ready();
    s.joined = false; // not yet connected: the same automatic entry follows the join
    assert(c.Step(s, 0).status == Status::NotJoined);
    assert(!Enters(c.Step(s, 50000)));
    s.joined = true;
    auto d = c.Step(s, 60000);
    assert(d.status == Status::Countdown && d.remainingMs == CountdownMs && !Enters(d));
    d = c.Step(s, 60000 + CountdownMs - 1);
    assert(d.remainingMs == 1 && !Enters(d));
    assert(Enters(c.Step(s, 60000 + CountdownMs)));
    // Asked again while the entry is in flight: nothing new until its outcome is known.
    assert(c.Step(s, 60000 + CountdownMs + 1).status == Status::Entering);
}

void MismatchNeverEnters()
{
    Controller c;
    Snapshot s = Ready();
    s.refused = true; // protocol/version/install mismatch
    for (uint32_t t = 0; t < 120000; t += 1000) assert(c.Step(s, t).status == Status::Refused && !Enters(c.Step(s, t)));
    s.refused = false;
    assert(c.Step(s, 120000).status == Status::Countdown && !Enters(c.Step(s, 120000)));
}

void HostLoadingAndMenu()
{
    Controller c;
    Snapshot s = Ready();
    s.hostPlayable = false; // host in the menu or still loading: wait without a countdown
    assert(c.Step(s, 0).status == Status::HostLoading);
    assert(!Enters(c.Step(s, 300000)));
    s.epoch = 0; // no announcement yet
    assert(c.Step(s, 300001).status == Status::HostLoading);
    s = Ready();
    assert(!Enters(c.Step(s, 400000)) && Enters(c.Step(s, 400000 + CountdownMs)));
    c.Entered(false, 400000 + CountdownMs);
    // The host quits to the menu during the wait: waiting again, and the full countdown restarts for the next world.
    s.hostPlayable = false;
    s.epoch = 8;
    assert(c.Step(s, 410000).status == Status::HostLoading);
    s.hostPlayable = true;
    assert(!Enters(c.Step(s, 420000)) && !Enters(c.Step(s, 420000 + CountdownMs - 1)) && Enters(c.Step(s, 420000 + CountdownMs)));
}

void LiveWorldIsNeverTouched()
{
    Controller c;
    Snapshot s = Ready();
    s.localWorld = true; // initial startup / same-epoch reconnect / user's own world / entry running: no queued entry
    for (uint32_t t = 0; t < 120000; t += 500) {
        const auto d = c.Step(s, t);
        assert(d.status == Status::InWorld && !Enters(d));
    }
    s.joined = false; // reconnect
    assert(c.Step(s, 120000).status == Status::NotJoined);
    s.joined = true;
    assert(c.Step(s, 120500).status == Status::InWorld);
    s.admitted = true;
    assert(c.Step(s, 121000).status == Status::Idle);
}

void QueuedInWorldEntry()
{
    // A host epoch change queued the entry: the stale world is frozen and waits for the countdown (or Esc/config).
    Controller c;
    Snapshot s = Ready();
    s.localWorld = s.queued = true;
    assert(c.Step(s, 0).status == Status::Countdown && !Enters(c.Step(s, 0)));
    assert(!Enters(c.Step(s, CountdownMs - 1)));
    assert(Enters(c.Step(s, CountdownMs)));
    // Mid-dialogue, trade, save or loading: the entry itself waits at its own boundary after approval; policy only
    // sees "busy" for native work, which restarts the countdown.
    Controller d;
    s.busy = true;
    assert(d.Step(s, 0).status == Status::Busy);
    s.busy = false;
    assert(!Enters(d.Step(s, 50000)) && !Enters(d.Step(s, 50000 + CountdownMs - 1)) && Enters(d.Step(s, 50000 + CountdownMs)));
    // Downed or dead heroes are not inputs: the queued entry behaves identically.
    static_assert(sizeof(Snapshot) >= 12 * sizeof(bool), "a new input must be justified and tested");
}

void BusyAndMenuNotReadyRestartTheCountdown()
{
    for (unsigned flag = 0; flag < 2; ++flag) { // loading screen/save exchange/nested frame (busy), or the menu is not ready
        Controller c;
        Snapshot s = Ready();
        assert(!Enters(c.Step(s, 0)));
        s.busy = flag == 0;
        s.entryReady = flag != 1;
        assert(c.Step(s, CountdownMs).status == Status::Busy && !Enters(c.Step(s, CountdownMs * 10)));
        s = Ready();
        assert(!Enters(c.Step(s, 50000)) && Enters(c.Step(s, 50000 + CountdownMs)));
    }
}

void RapidEpochsCoalesce()
{
    Controller c;
    Snapshot s = Ready();
    uint32_t now = 1000;
    for (uint32_t epoch = 10; epoch < 16; ++epoch, now += 700) { // host loads repeatedly; New Game also announces 3 epochs
        s.epoch = epoch;
        s.hostPlayable = (epoch & 1) != 0;
        assert(!Enters(c.Step(s, now)));
    }
    s.epoch = 20;
    s.hostPlayable = true;
    assert(!Enters(c.Step(s, now)));
    assert(!Enters(c.Step(s, now + CountdownMs - 1)));
    assert(Enters(c.Step(s, now + CountdownMs)));
    c.Entered(true, now + CountdownMs);
    s.localWorld = true;
    assert(c.Step(s, now + CountdownMs + 1).status == Status::Admitting);
    s.epoch = 21; // replaced while admitting: the queued entry gets its own countdown for the new epoch
    s.queued = true;
    assert(c.Step(s, now + CountdownMs + 2).status == Status::Countdown && c.Entries() == 0);
    // An entry in flight when the epoch changes is forgotten; its late outcome only counts for the new epoch.
    Controller d;
    s = Ready();
    d.Step(s, 0);
    assert(Enters(d.Step(s, CountdownMs)) && d.Entering());
    s.epoch = 8;
    assert(d.Step(s, CountdownMs + 1).status == Status::Countdown && !d.Entering());
}

void CancelIsPerEpoch()
{
    Controller c;
    Snapshot s = Ready();
    assert(!Enters(c.Step(s, 0)));
    assert(Cancelable(c.Step(s, 1000).status));
    c.Cancel(); // Esc during the countdown, manual New Game, or "No" in the native dialog
    assert(c.Held());
    for (uint32_t t = 2000; t < 200000; t += 1000) assert(c.Step(s, t).status == Status::Held && !Enters(c.Step(s, t)));
    s.hostPlayable = false; // host keeps loading the same epoch, then becomes playable: still the player's choice
    assert(c.Step(s, 200000).status == Status::HostLoading);
    s.hostPlayable = true;
    assert(c.Step(s, 201000).status == Status::Held);
    s.epoch = 8; // the next host load asks again
    assert(c.Step(s, 202000).status == Status::Countdown && !c.Held() && Enters(c.Step(s, 202000 + CountdownMs)));
    assert(!Cancelable(Status::Held) && !Cancelable(Status::Busy) && Cancelable(Status::Retry) && !Cancelable(Status::Entering));
    // Queued in a stale world: Esc holds the entry (the gate stays closed) until the player chooses New Game.
    Controller q;
    s = Ready();
    s.localWorld = s.queued = true;
    q.Step(s, 0);
    q.Cancel();
    assert(q.Step(s, CountdownMs * 5).status == Status::Held && !Enters(q.Step(s, CountdownMs * 5)));
}

void ConfigOffAndOn()
{
    Controller c;
    Snapshot s = Ready();
    s.enabled = false;
    for (uint32_t t = 0; t < 60000; t += 5000) assert(c.Step(s, t).status == Status::Off && !Enters(c.Step(s, t)));
    s.enabled = true;
    assert(!Enters(c.Step(s, 60000)) && Enters(c.Step(s, 60000 + CountdownMs)));
    s.enabled = false;
    assert(c.Step(s, 70000).status == Status::Off);
    s.enabled = true; // re-enabling restarts, no instant entry
    assert(!Enters(c.Step(s, 70001)));
}

void NoRouteAndRecordWait()
{
    Controller c;
    Snapshot s = Ready();
    s.routeAvailable = false; // no template and a world was already entered: restart Gothic 3
    for (uint32_t t = 0; t < 60000; t += 1000) assert(c.Step(s, t).status == Status::NoRoute && !Enters(c.Step(s, t)));
    s.routeAvailable = true;
    assert(!Enters(c.Step(s, 60000)) && Enters(c.Step(s, 60000 + CountdownMs)));
    Controller r; // the record has not arrived: wait for wording, but never forever
    s = Ready();
    s.recordKnown = false;
    assert(r.Step(s, 0).status == Status::WaitRecord);
    assert(r.Step(s, RecordWaitMs - 1).status == Status::WaitRecord);
    assert(r.Step(s, RecordWaitMs).status == Status::Countdown && !Enters(r.Step(s, RecordWaitMs)));
    s.recordKnown = true;
    assert(Enters(r.Step(s, RecordWaitMs + CountdownMs)));
}

void AdmissionAndStall()
{
    Controller c;
    Snapshot s = Ready();
    assert(!Enters(c.Step(s, 0)) && Enters(c.Step(s, CountdownMs)));
    c.Entered(true, 10000);
    s.localWorld = true;
    assert(c.Step(s, 10001).status == Status::Admitting);
    assert(c.Step(s, 10000 + AdmitStallMs - 1).status == Status::Admitting);
    assert(c.Step(s, 10000 + AdmitStallMs).status == Status::Stalled && !Enters(c.Step(s, 10000 + AdmitStallMs)));
    assert(c.Step(s, 999999).status == Status::Stalled);
    s.admitted = true;
    assert(c.Step(s, 1000000).status == Status::Idle && c.Entries() == 0);
    // The entry was undone (back at the title screen before admission): start over with a full countdown.
    Controller d;
    s = Ready();
    d.Step(s, 0);
    assert(Enters(d.Step(s, CountdownMs)));
    d.Entered(true, CountdownMs);
    s.localWorld = true;
    assert(d.Step(s, CountdownMs + 1).status == Status::Admitting);
    s.localWorld = false;
    assert(d.Step(s, CountdownMs + 2).status == Status::Countdown && d.Step(s, CountdownMs + 2).remainingMs == CountdownMs);
}

void RetriesAreBounded()
{
    Controller c;
    Snapshot s = Ready();
    assert(!Enters(c.Step(s, 0)) && Enters(c.Step(s, CountdownMs)));
    c.Entered(false, 5000); // the entry failed (native refusal, template removed, unavailable)
    auto d = c.Step(s, 5001);
    assert(d.status == Status::Retry && d.remainingMs == RetryMs[0] - 1 && !Enters(d));
    assert(Enters(c.Step(s, 5000 + RetryMs[0]))); // no second countdown
    c.Entered(false, 9000);
    assert(c.Step(s, 9001).status == Status::Retry && !Enters(c.Step(s, 9000 + RetryMs[1] - 1)));
    assert(Enters(c.Step(s, 9000 + RetryMs[1])));
    c.Entered(false, 20000);
    assert(c.Entries() == MaxEntries);
    for (uint32_t t = 20001; t < 500000; t += 7000) assert(c.Step(s, t).status == Status::GaveUp && !Enters(c.Step(s, t)));
    s.epoch = 8; // a new host load resets the budget
    assert(c.Step(s, 500000).status == Status::Countdown && c.Entries() == 0);
}

void ClockWrap()
{
    Controller c;
    Snapshot s = Ready();
    const uint32_t start = 0xffffffffu - 1000;
    assert(!Enters(c.Step(s, start)));
    assert(!Enters(c.Step(s, start + CountdownMs - 1)));
    assert(Enters(c.Step(s, start + CountdownMs))); // wraps past zero
    c.Entered(true, start + CountdownMs);
    s.localWorld = true;
    assert(c.Step(s, start + CountdownMs + AdmitStallMs - 1).status == Status::Admitting);
    assert(c.Step(s, start + CountdownMs + AdmitStallMs).status == Status::Stalled);
}

void RejoinBoundaryIgnoresHeroState()
{
    // Downed/dead/combat are not boundary inputs: only native work, nesting, a save exchange and dialogue/trade wait.
    g3t::rejoin::State state;
    state.Queue();
    assert(state.CanRetire({}));
    static_assert(sizeof(g3t::rejoin::Boundary) == 4 * sizeof(bool), "a new boundary input must be justified and tested");
}
}

int main()
{
    ExhaustiveTable();
    JoinLaterAndCountdown();
    MismatchNeverEnters();
    HostLoadingAndMenu();
    LiveWorldIsNeverTouched();
    QueuedInWorldEntry();
    BusyAndMenuNotReadyRestartTheCountdown();
    RapidEpochsCoalesce();
    CancelIsPerEpoch();
    ConfigOffAndOn();
    NoRouteAndRecordWait();
    AdmissionAndStall();
    RetriesAreBounded();
    ClockWrap();
    RejoinBoundaryIgnoresHeroState();
    return 0;
}
