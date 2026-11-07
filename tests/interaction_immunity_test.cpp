// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/downed_policy.h"
#include "../src/playerwire.h"
#include <cassert>

using namespace g3t::downed;

void Windows()
{
    InteractionProtection state;
    state.Observe(true, false, false, 0);
    assert(!state.Flags(0));
    state.Observe(true, true, false, 10);
    assert(state.Flags(10) == InDialogue);
    state.Observe(true, true, false, 1000000);
    assert(state.Flags(1000000) == InDialogue); // a genuine open window is not time-limited
    state.Observe(true, true, true, 1000010);
    assert(state.Flags(1000010) == (InDialogue | InTrade));
    state.Observe(true, false, true, 1000020);
    assert(state.Flags(1000020) == InTrade && !state.grace);
    state.Observe(true, false, false, 1000030);
    assert(state.Flags(1000030) == InteractionGrace);
    const uint32_t closed = state.closedAt;
    state.Observe(true, false, false, closed + InteractionGraceMs - 1);
    assert(state.closedAt == closed && state.Flags(closed + InteractionGraceMs - 1) == InteractionGrace);
    state.Observe(true, false, false, closed + InteractionGraceMs);
    assert(!state.Flags(closed + InteractionGraceMs) && !state.grace);
    state.Observe(true, true, false, closed + InteractionGraceMs + 1);
    state.Observe(true, false, false, UINT32_MAX - 100);
    assert(state.Flags(uint32_t(UINT32_MAX - 100 + InteractionGraceMs - 1)) == InteractionGrace);
    assert(!state.Flags(uint32_t(UINT32_MAX - 100 + InteractionGraceMs)));
    state.Observe(true, false, true, 2000);
    assert(!state.grace && state.Flags(2000) == InTrade); // reopen cancels the previous close timer
    state.Observe(false, true, true, 2001);
    assert(!state.Flags(2001)); // absent/down/dead/loading/world-deactivated hero
    state.Observe(true, true, false, 2010);
    state.Clear();
    assert(!state.Flags(2010)); // new hero/world does not inherit an old window or grace

    for (int page = -1; page <= 17; ++page) {
        assert(InteractionPage(page, 5) == (page == 15 ? 5 : page));
        assert(InteractionPage(page, 6) == (page == 15 ? 6 : page));
        for (unsigned inputs = 0; inputs < 16; ++inputs) {
            const bool running = inputs & 1, ending = inputs & 2;
            const bool heroPartner = inputs & 4, livePartner = inputs & 8;
            assert(DialogueObserved(page, running, ending, heroPartner, livePartner) ==
                   (page == 6 || (running && !ending && heroPartner && livePartner)));
        }
    }
    assert(DialogueObserved(-1, true, false, true, true)); // first spoken line before option page
    assert(!DialogueObserved(-1, true, true, true, true)); // Esc/end request with no actual window
    assert(!DialogueObserved(-1, true, false, false, true)); // overheard NPC comment, not our dialogue
    assert(!DialogueObserved(-1, true, false, true, false)); // dying/removed/cleared partner
}

void OwnerAndPeer()
{
    // Same production policy on either HP owner, independent of the other hero and proximity.
    for (unsigned owner = 1; owner <= 2; ++owner) {
        InteractionProtection players[3];
        players[owner].Observe(true, owner == 1, owner == 2, 10);
        const unsigned other = owner == 1 ? 2 : 1;
        assert(!players[other].Flags(10));
        const uint8_t flags = players[owner].Flags(10);
        assert(HarmImmune(true, false, false, 0, false, flags != 0));
        assert(!HarmImmune(true, false, false, 0, false, players[other].Flags(10) != 0));
        assert(!HarmImmune(true, false, true, 0, false, true));
        assert(!HarmImmune(true, false, false, 4, false, true));
        assert(!HarmImmune(false, false, false, 0, false, true));

        g3t::net::PlayerActivity activity;
        assert(activity.Accept(100, 20, 7, 1, 1 | flags, 0));
        assert(PeerInteraction(true, activity.known, activity.ticks, activity.world, 7, activity.flags, 20) == flags);
        players[owner].Observe(true, false, false, 30); // close, Esc or native abort all remove observation
        assert(activity.Accept(101, 40, 7, 1, 1 | players[owner].Flags(30), 0));
        assert(PeerInteraction(true, true, activity.ticks, 7, 7, activity.flags, 40) == InteractionGrace);
        assert(!activity.Accept(100, 50, 7, 1, 1 | flags, 0)); // old pose cannot reopen a newer close
        assert(activity.flags == (1 | InteractionGrace));
        players[owner].Observe(true, false, false, 30 + InteractionGraceMs);
        assert(activity.Accept(102, 1540, 7, 1, 1 | players[owner].Flags(1530), 0));
        assert(!PeerInteraction(true, true, activity.ticks, 7, 7, activity.flags, 1540));

        assert(activity.Accept(103, 2000, 7, 1, 1 | flags, 0));
        assert(!PeerInteraction(false, true, 2000, 7, 7, activity.flags, 2000)); // disconnect
        assert(!PeerInteraction(true, false, 2000, 7, 7, activity.flags, 2000)); // unknown/rejoin
        assert(!PeerInteraction(true, true, 2000, 7, 8, activity.flags, 2000)); // another world
        assert(PeerInteraction(true, true, 2000, 7, 7, activity.flags, 6999) == flags);
        assert(!PeerInteraction(true, true, 2000, 7, 7, activity.flags, 7000)); // missing heartbeat safety expiry
        assert(!PeerInteraction(true, true, 2000, 7, 7, 32, 2000)); // loading is not dialogue
        assert(!PeerInteraction(true, true, 2000, 7, 7, 64, 2000)); // no-running-hero/menu is not trade
        activity = {};
        assert(!PeerInteraction(true, activity.known, 0, 7, 7, 1 | flags, 2000));
    }
    for (unsigned flags = 0; flags < 256; ++flags) {
        const bool valid = (flags & 1) ? (!(flags & 14) || !(flags & InteractionMask))
                                     : !(flags & InteractionGrace);
        assert(ValidActivityFlags(flags) == valid);
        const uint8_t expected = (flags & 1) && !(flags & 14) ? flags & InteractionMask : 0;
        assert(PeerInteraction(true, true, 100, 7, 7, flags, 100) == expected);
    }
    assert(ValidActivityFlags(32) && ValidActivityFlags(64));
    assert(!ValidActivityFlags(128));
}

void UnchangedWire()
{
    g3t::playerwire::Encoder encoder;
    g3t::playerwire::Decoder decoder;
    g3t::net::PlayerState state, decoded;
    state.id = 2;
    state.world = 7;
    state.rotation[3] = 1;
    for (uint8_t reason : {uint8_t(0), uint8_t(InDialogue), uint8_t(InTrade),
                           uint8_t(InDialogue | InTrade), uint8_t(InteractionGrace), uint8_t(0)}) {
        state.flags = 1 | reason;
        ++state.sequence;
        g3t::playerwire::Packet packet;
        assert(encoder.Plan(state, state.sequence * 40, false, packet) == g3t::playerwire::Status::Ok);
        encoder.Commit(state, state.sequence * 40, packet);
        assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), decoded) == g3t::playerwire::Status::Ok);
        assert(decoded.flags == state.flags && decoded.world == state.world);
    }
    static_assert(g3t::playerwire::FixedBytes == 65, "player codec size must not change");
}

int main()
{
    Windows();
    OwnerAndPeer();
    UnchangedWire();
}
