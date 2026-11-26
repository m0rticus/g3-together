// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_tips_policy.h"

#include <cassert>
#include <cstdint>

using namespace g3t::tips::policy;

void HostNeverTouched()
{
    assert(!JoinDefaultTracker::ShouldPrepareEntry(true, false));
    assert(!JoinDefaultTracker::ShouldPrepareEntry(true, true));
    JoinDefaultTracker tracker;
    State host{true, true, true, 1, 1, 0x1234};
    assert(!tracker.ShouldApply(host));
    assert(!tracker.Applied());
    tracker.MarkApplied(host);
    assert(!tracker.Applied());
}

void InitialJoinNewGameAndReconnects()
{
    assert(!JoinDefaultTracker::ShouldPrepareEntry(false, false));
    assert(JoinDefaultTracker::ShouldPrepareEntry(false, true));

    JoinDefaultTracker tracker;
    // Before hero is running in world, wait without marking applied.
    State loading{false, true, false, 1, 1, 0};
    assert(!tracker.ShouldApply(loading));
    assert(!tracker.Applied());

    // Initial join / New Game completes: apply once, stay quiet on later ticks.
    State s{false, true, true, 1, 1, 0x1000};
    assert(tracker.ShouldApply(s));
    tracker.MarkApplied(s);
    assert(tracker.Applied());
    assert(!tracker.ShouldApply(s));

    // Host-load reconnect (template load / new world activation): apply again once.
    s.worldEntrySeq = 2;
    s.heroToken = 0x2000;
    assert(tracker.ShouldApply(s));
    tracker.MarkApplied(s);
    assert(!tracker.ShouldApply(s));

    // Same worldEntrySeq but hero instance changed: apply again once.
    s.heroToken = 0x3000;
    assert(tracker.ShouldApply(s));
    tracker.MarkApplied(s);
    assert(!tracker.ShouldApply(s));

    // Disconnect resets state so a same-world reconnect re-enables default.
    State disconnected{false, false, true, 1, 2, 0x3000};
    assert(!tracker.ShouldApply(disconnected));
    assert(!tracker.Applied());

    s.welcomeSerial = 2;
    assert(tracker.ShouldApply(s));
    tracker.MarkApplied(s);
    assert(!tracker.ShouldApply(s));

    // Welcome serial advance without an observed disconnected tick also re-applies.
    s.welcomeSerial = 3;
    assert(tracker.ShouldApply(s));
    tracker.MarkApplied(s);
    assert(!tracker.ShouldApply(s));

    // Role switch to host resets immediately.
    s.isHost = true;
    assert(!tracker.ShouldApply(s));
    assert(!tracker.Applied());
}

void OpenTutorialLayoutContract()
{
    uint8_t valid[OpenTutorialProbeBytes] = {
        0x56, 0x8b, 0x71, 0x04, 0x85, 0xf6, 0x74, 0x36,
        0x57, 0x8b, 0x7e, 0x74, 0x85, 0xff, 0x74, 0x27,
        0x8d, 0x4f, 0x58, 0xff, 0x15,
        0x9c, 0xd4, 0x24, 0x10,
        0x84, 0xc0, 0x75, 0x1a
    };
    uint32_t iat = 0;
    assert(VerifyOpenTutorialBytes(valid, sizeof(valid), iat));
    assert(iat == 0x1024d49c);

    // Rebased (ASLR) IAT address is still accepted.
    valid[24] = 0x20;
    assert(VerifyOpenTutorialBytes(valid, sizeof(valid), iat));
    assert(iat == 0x2024d49c);

    // Zero IAT slot, wrong size, wrong offset byte, or null buffer are rejected.
    assert(!VerifyOpenTutorialBytes(nullptr, sizeof(valid), iat));
    assert(!VerifyOpenTutorialBytes(valid, sizeof(valid) - 1, iat));
    uint8_t zeroIat[OpenTutorialProbeBytes];
    for (size_t i = 0; i < sizeof(valid); ++i) zeroIat[i] = valid[i];
    zeroIat[21] = zeroIat[22] = zeroIat[23] = zeroIat[24] = 0;
    assert(!VerifyOpenTutorialBytes(zeroIat, sizeof(zeroIat), iat));
    uint8_t wrongOffset[OpenTutorialProbeBytes];
    for (size_t i = 0; i < sizeof(valid); ++i) wrongOffset[i] = valid[i];
    wrongOffset[18] = 0x5c;
    assert(!VerifyOpenTutorialBytes(wrongOffset, sizeof(wrongOffset), iat));
}

int main()
{
    HostNeverTouched();
    InitialJoinNewGameAndReconnects();
    OpenTutorialLayoutContract();
    return 0;
}
