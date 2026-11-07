// SPDX-License-Identifier: GPL-3.0-or-later
#define far SyntheticFarMacro
#include "../src/npclease.h"
#include "../src/npclease_client.h"
#include "../src/npclease_save.h"
#undef far
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace g3t::npclease;

static Checkpoint ValidCheckpoint(uint8_t seed = 1)
{
    Checkpoint c{};
    c.templateId[0] = seed;
    c.templateId[15] = seed;
    c.position[0] = 100.0f;
    c.position[1] = 200.0f;
    c.position[2] = 300.0f;
    c.rotation[3] = 1.0f;
    c.hp = 10;
    c.maxHp = 10;
    c.mana = 5;
    c.maxMana = 5;
    c.stamina = 7;
    c.maxStamina = 7;
    c.level = 3;
    c.attitude = 3;
    c.aiMode = 2;
    c.action = 4;
    c.attackReason = 2;
    c.targetPlayer = 1;
    std::memcpy(c.name, "Wolf", 5);
    std::memcpy(c.routine, "Routine", 8);
    c.statusAge[0] = 100;
    c.statusAge[1] = 200;
    return c;
}

static_assert(sizeof(Checkpoint) == 233, "Unexpected packed Checkpoint size");
static_assert(sizeof(Message) == 278, "Unexpected packed Message size");
static_assert(detail::LegacySavedWireSize == 261, "Unexpected legacy overlay row size");
static_assert(detail::SavedWireSize == 262, "Unexpected overlay payload record size");
static_assert(FullMessageWireSize == 278 && StateMessageWireSize == 150 &&
              RenewMessageWireSize == 46, "Unexpected message wire sizes");
static_assert(static_cast<uint8_t>(Kind::Reject) == 11 && static_cast<uint8_t>(Kind::Adopt) == 12 &&
              static_cast<uint8_t>(Kind::StateFull) == 13,
              "Existing operation numbers must remain stable");

static void TestValidityAndWire()
{
    Checkpoint c = ValidCheckpoint();
    assert(Valid(c));
    c.routine[0] = '\0';
    assert(Valid(c));
    c.templateId[0] = c.templateId[15] = 0;
    assert(!Valid(c));
    c = ValidCheckpoint();
    c.position[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!Valid(c));
    c = ValidCheckpoint();
    c.rotation[0] = 2.0f;
    c.rotation[3] = 0.0f;
    assert(!Valid(c));
    c = ValidCheckpoint();
    c.hp = c.maxHp + 1;
    assert(!Valid(c));
    c = ValidCheckpoint();
    c.effects = 4;
    assert(!Valid(c));

    Message m{};
    m.world = 7;
    m.guid[0] = 9;
    m.state = ValidCheckpoint();
    assert(Valid(m));
    m.version = 1;
    assert(!Valid(m));
    m.version = WireVersion;
    m.owner = 8;
    assert(!Valid(m));
    m.owner = 2;
    m.duration = 3001;
    assert(!Valid(m));
    m.duration = LeaseMs;
    m.kind = Kind::Ack;
    m.state = Checkpoint{};
    assert(Valid(m));
    assert(sizeof(Checkpoint) <= 512);
    assert(sizeof(Message) < 1000);
}

static void TestMessageCodec()
{
    Message sent{};
    sent.world = 0x10203040;
    sent.epoch = 8;
    sent.incarnation = 9;
    sent.generation = 10;
    sent.revision = 11;
    sent.echo = 12;
    sent.duration = 0x345;
    sent.owner = 2;
    sent.guid[0] = 9;
    sent.state = ValidCheckpoint();
    sent.state.statusAge[4] = 0x10203040;
    sent.state.effects = NativeDeath | QuestDeath;
    Message received{};
    std::vector<uint8_t> bytes;

    for (uint8_t kind = 1; kind <= 13; ++kind) {
        sent.kind = static_cast<Kind>(kind);
        assert(EncodeMessage(sent, bytes));
        const size_t expected = sent.kind == Kind::State ? StateMessageWireSize :
                                sent.kind == Kind::Renew ? RenewMessageWireSize : FullMessageWireSize;
        assert(bytes.size() == expected);
        assert(bytes[0] == 2 && bytes[1] == kind && bytes[3] == 0x40 && bytes[6] == 0x10);
        assert(bytes[27] == 0x45 && bytes[28] == 0x03);
        assert(DecodeMessage(bytes.data(), bytes.size(), received));
        assert(received.version == sent.version && received.kind == sent.kind && received.owner == sent.owner);
        assert(received.world == sent.world && received.epoch == sent.epoch &&
               received.incarnation == sent.incarnation && received.generation == sent.generation &&
               received.revision == sent.revision && received.echo == sent.echo &&
               received.duration == sent.duration);
        assert(!std::memcmp(received.guid, sent.guid, sizeof(sent.guid)));
        if (sent.kind == Kind::State) {
            assert(ValidStateFields(received.state));
            assert(!Valid(received.state));
            assert(received.state.name[0] == 0 && received.state.routine[0] == 0);
            assert(received.state.statusAge[4] == sent.state.statusAge[4]);
            std::memcpy(received.state.name, sent.state.name, sizeof(sent.state.name));
            std::memcpy(received.state.routine, sent.state.routine, sizeof(sent.state.routine));
            assert(detail::ExactState(received.state, sent.state));
        } else if (sent.kind == Kind::Renew) {
            assert(received.state.effects == sent.state.effects);
            assert(!detail::GuidNonzero(received.state.templateId));
            assert(received.state.name[0] == 0 && received.state.routine[0] == 0);
        } else {
            assert(detail::ExactState(received.state, sent.state));
        }

        Message unchanged = received;
        assert(!DecodeMessage(bytes.data(), bytes.size() - 1, received));
        assert(!std::memcmp(&received, &unchanged, sizeof(received)));
        bytes.push_back(0);
        assert(!DecodeMessage(bytes.data(), bytes.size(), received));
        assert(!std::memcmp(&received, &unchanged, sizeof(received)));
    }

    sent.kind = Kind::State;
    assert(EncodeMessage(sent, bytes));
    const auto validState = bytes;
    auto reject = [&](size_t offset, uint8_t value) {
        auto bad = validState;
        bad[offset] = value;
        Message unchanged = received;
        assert(!DecodeMessage(bad.data(), bad.size(), received));
        assert(!std::memcmp(&received, &unchanged, sizeof(received)));
    };
    reject(0, 1);
    reject(0, 3);
    reject(1, 0);
    reject(1, 14);
    reject(1, static_cast<uint8_t>(Kind::Offer)); // compact payload cannot masquerade as full
    reject(2, 8);
    reject(11, 0); // incarnation has no other set bytes
    reject(MessageHeaderWireSize + 76, 2); // dead must be boolean
    reject(MessageHeaderWireSize + 82, 4); // effect mask
    reject(MessageHeaderWireSize + 84, 8); // target player
    std::vector<uint8_t> bad = validState;
    bad[MessageHeaderWireSize + 16] = 0;
    bad[MessageHeaderWireSize + 17] = 0;
    bad[MessageHeaderWireSize + 18] = 0xc0;
    bad[MessageHeaderWireSize + 19] = 0x7f; // NaN
    assert(!DecodeMessage(bad.data(), bad.size(), received));
    bad = validState;
    std::memset(bad.data() + 29, 0, 16);
    assert(!DecodeMessage(bad.data(), bad.size(), received));
    assert(!DecodeMessage(nullptr, 0, received));

    sent.kind = Kind::StateFull;
    std::strcpy(sent.state.routine, "NewRoutine");
    assert(EncodeMessage(sent, bytes));
    assert(bytes.size() == FullMessageWireSize);
    assert(DecodeMessage(bytes.data(), bytes.size(), received));
    assert(Valid(received) && detail::ExactState(received.state, sent.state));
    const Checkpoint updatedCanonical = received.state;
    bad = bytes;
    std::memset(bad.data() + MessageHeaderWireSize + 84, 'x', 64);
    assert(!DecodeMessage(bad.data(), bad.size(), received));
    bad = bytes;
    bad[MessageHeaderWireSize + 82] = 4;
    assert(!DecodeMessage(bad.data(), bad.size(), received));
    sent.kind = Kind::State;
    sent.state.hp = 8;
    assert(EncodeMessage(sent, bytes));
    assert(DecodeMessage(bytes.data(), bytes.size(), received));
    assert(received.state.routine[0] == 0);
    std::memcpy(received.state.name, updatedCanonical.name, sizeof(updatedCanonical.name));
    std::memcpy(received.state.routine, updatedCanonical.routine, sizeof(updatedCanonical.routine));
    assert(Valid(received) && detail::ExactState(received.state, sent.state));

    sent.kind = Kind::Renew;
    assert(EncodeMessage(sent, bytes));
    bytes.back() = 4;
    assert(!DecodeMessage(bytes.data(), bytes.size(), received));
    sent.state.effects = 4;
    const auto savedBytes = bytes;
    assert(!EncodeMessage(sent, bytes));
    assert(bytes == savedBytes);
    sent.state.effects = 0;
    sent.kind = Kind::Adopt;
    sent.state.name[0] = 0;
    assert(!EncodeMessage(sent, bytes));
    sent.kind = Kind::StateFull;
    assert(!EncodeMessage(sent, bytes));
    sent.kind = Kind::Commit;
    assert(!EncodeMessage(sent, bytes));
    sent.kind = Kind::Ack;
    std::memset(sent.state.name, 'x', sizeof(sent.state.name));
    assert(!EncodeMessage(sent, bytes));
}

static void TestNativeDomainActions()
{
    Checkpoint c = ValidCheckpoint();
    for (uint32_t action : {26u, 35u, 36u, 56u, 117u, 143u, 144u}) {
        c.action = action;
        assert(Valid(c));
    }
    c.action = 145;
    assert(!Valid(c));
    c.action = 255;
    assert(!Valid(c));

    c = ValidCheckpoint();
    for (uint32_t attitude = 0; attitude <= 6; ++attitude) {
        c.attitude = static_cast<uint8_t>(attitude);
        assert(Valid(c));
    }
    c.attitude = 7;
    assert(!Valid(c));
    c.attitude = 8;
    assert(!Valid(c));

    c = ValidCheckpoint();
    c.action = 35;
    c.dead = 1;
    c.hp = 0;
    assert(CommitEffect(c, NativeDeath));
    assert(Valid(c));

    Lease lease;
    Checkpoint baseline = ValidCheckpoint();
    baseline.action = 36;
    assert(lease.Offer(2, 3, 1, 100, baseline));
    assert(lease.Prepared(1, 100));
    c.effects = 0;
    c.action = 35;
    c.dead = 1;
    c.hp = 0;
    assert(CommitEffect(c, NativeDeath));
    assert(Valid(c));
    assert(lease.AcceptState(2, 3, 1, 1, 1, c));
    assert(lease.Cut(2, 3, 1, 1, 1, c));

}

static void TestEligibilityAndWrap()
{
    Eligibility eligibility{};
    Interest interest{};
    interest.hostRoi = 8000;
    interest.hostDistance = 12000;
    interest.clientDistance = ClientRadiusCm;
    assert(!eligibility.Delegate(interest, 100));
    assert(!eligibility.Delegate(interest, 3099));
    assert(eligibility.Delegate(interest, 3100));
    assert(eligibility.farHeld);

    interest.hostDistance = 11000;
    assert(eligibility.Delegate(interest, 3200));
    assert(eligibility.farHeld);
    interest.hostDistance = 9999;
    assert(!eligibility.Delegate(interest, 3300));
    assert(!eligibility.farHeld);

    eligibility.farHeld = true;
    eligibility.farSince = 100;
    interest.ready = false;
    assert(!eligibility.Delegate(interest, 1000));
    assert(!eligibility.farHeld && eligibility.farSince == 0);

    interest.ready = true;
    interest.hostDistance = std::numeric_limits<float>::quiet_NaN();
    assert(!eligibility.Delegate(interest, 1100));
    assert(!eligibility.farHeld);

    interest.hostDistance = 9999;
    interest.clientDistance = 100000;
    assert(eligibility.Recall(interest));
    interest.ready = false;
    assert(eligibility.Recall(interest));

    Interest farAway{};
    farAway.hostDistance = 12000;
    farAway.clientDistance = 4000;
    assert(!eligibility.Recall(farAway));

    assert(Reached(0, 0xffffffffu));
    assert(!Reached(0, 1));
    assert(Newer(0, 0xffffffffu));
    assert(!Newer(0xffffffffu, 0));
}

static void TestHostPresenceEligibility()
{
    Eligibility unavailable{};
    Interest interest{};
    interest.hostDistance = 1000;
    interest.clientDistance = 3000;
    interest.hostPresent = false;

    assert(unavailable.Delegate(interest, 100));
    assert(unavailable.farHeld);
    assert(!unavailable.Recall(interest));

    interest.hostDistance = 9999;
    interest.hostPresent = false;
    assert(unavailable.Delegate(interest, 3200));
    assert(unavailable.farHeld);
    assert(!unavailable.Recall(interest));

    interest.hostPresent = true;
    assert(!unavailable.Delegate(interest, 3300));
    assert(!unavailable.farHeld);
    assert(unavailable.Recall(interest));

    unavailable = Eligibility{};
    interest.hostDistance = 20000;
    interest.hostPresent = false;
    interest.clientDistance = ClientRadiusCm;
    assert(unavailable.Delegate(interest, 500));
    assert(unavailable.farHeld);
    assert(!unavailable.Recall(interest));

    Interest gates = interest;
    gates.ready = false;
    assert(!unavailable.Delegate(gates, 3600));
    gates.ready = true;
    gates.eligible = false;
    assert(!unavailable.Delegate(gates, 3700));
    gates.eligible = true;
    gates.sameWorld = false;
    assert(!unavailable.Delegate(gates, 3800));
    gates.sameWorld = true;
    gates.conflict = true;
    assert(!unavailable.Delegate(gates, 3900));

    Eligibility jitter{};
    interest.hostDistance = 1000;
    interest.clientDistance = 3000;
    interest.hostPresent = false;
    assert(jitter.Delegate(interest, 100));
    interest.hostPresent = true;
    assert(!jitter.Delegate(interest, 2000));
    assert(!jitter.farHeld && jitter.farSince == 0);
    interest.hostPresent = false;
    assert(jitter.Delegate(interest, 2500));
    assert(jitter.farHeld);

    Eligibility range{};
    interest.clientDistance = ClientRadiusCm + 1;
    assert(!range.Delegate(interest, 100));
    assert(!range.farHeld && !range.dwellActive);
    interest.clientDistance = ClientRadiusCm;
    assert(range.Delegate(interest, 101));
    interest.clientDistance = ClientRadiusCm + 1;
    assert(!range.Delegate(interest, 102));

    Eligibility bypass{};
    interest.hostPresent = true;
    interest.hostDistance = 12000;
    interest.clientDistance = ClientRadiusCm;
    assert(!bypass.Delegate(interest, 100));
    assert(bypass.dwellActive);
    interest.hostPresent = false;
    assert(bypass.Delegate(interest, 101));
    assert(!bypass.dwellActive);
}

static void TestLeaseGrantAndExpiry()
{
    Lease lease{};
    Checkpoint baseline = ValidCheckpoint();
    assert(lease.Offer(2, 5, 1, 100, baseline));
    assert(lease.phase == Phase::Offering && lease.owner == 2);
    const uint32_t offeredDeadline = lease.deadline;
    assert(!lease.Offer(3, 6, 2, 101, baseline));
    assert(lease.owner == 2 && lease.deadline == offeredDeadline);

    assert(!lease.Prepared(2, 200));
    assert(!lease.Prepared(1, 3100));
    assert(lease.Prepared(1, 3099));
    assert(lease.phase == Phase::Client);
    const uint32_t leaseDeadline = lease.deadline;
    assert(!lease.Prepared(1, 3100));
    assert(lease.deadline == leaseDeadline);

    Checkpoint value = baseline;
    value.hp = 9;
    assert(!lease.AcceptState(3, 5, 1, 1, 1, value));
    assert(!lease.AcceptState(2, 6, 1, 1, 1, value));
    assert(!lease.AcceptState(2, 5, 2, 1, 1, value));
    assert(!lease.AcceptState(2, 5, 1, 2, 1, value));
    assert(lease.AcceptState(2, 5, 1, 1, 1, value));
    assert(lease.revision == 1 && lease.state.hp == 9);
    assert(!lease.AcceptState(2, 5, 1, 1, 1, value));
    value.hp = 8;
    assert(!lease.AcceptState(2, 5, 1, 1, 0, value));
    assert(lease.revision == 1 && lease.state.hp == 9);
    assert(!lease.AcceptState(2, 5, 1, 1, 1, baseline));

    assert(lease.Recall(4000));
    assert(lease.phase == Phase::Recalling && lease.deadline == leaseDeadline);
    assert(!lease.Recall(4001));
    assert(lease.AcceptState(2, 5, 1, 1, 2, value));

    Lease offerTimeout{};
    assert(offerTimeout.Offer(2, 1, 1, 100, baseline));
    assert(!offerTimeout.Recover(3099));
    assert(offerTimeout.Recover(3100));
    assert(offerTimeout.phase == Phase::Host);

    Lease clientTimeout{};
    assert(clientTimeout.Offer(2, 1, 1, 100, baseline));
    assert(clientTimeout.Prepared(1, 100));
    assert(!clientTimeout.Recover(clientTimeout.deadline - 1));
    assert(clientTimeout.Recover(clientTimeout.deadline));
    assert(clientTimeout.phase == Phase::Dormant);
    assert(!clientTimeout.ActiveLeases());
}

static void TestCutRecallAndHandoff()
{
    Lease lease{};
    Checkpoint baseline = ValidCheckpoint();
    assert(lease.Offer(4, 9, 10, 1000, baseline));
    assert(lease.Prepared(10, 1001));

    Checkpoint finalState = baseline;
    finalState.hp = 6;
    assert(!lease.Cut(5, 9, 10, 1, 1, finalState));
    assert(!lease.Cut(4, 8, 10, 1, 1, finalState));
    assert(!lease.Cut(4, 9, 11, 1, 1, finalState));
    assert(!lease.Cut(4, 9, 10, 2, 1, finalState));
    assert(lease.Cut(4, 9, 10, 1, 1, finalState));
    assert(lease.phase == Phase::Frozen && lease.revision == 1 && lease.state.hp == 6);
    assert(!lease.Cut(4, 9, 10, 1, 2, finalState));
    assert(!lease.HandBack(10, 5000));
    assert(lease.HandBack(11, 5000));
    assert(lease.phase == Phase::Host && lease.epoch == 11 && lease.owner == 1);
    assert(lease.owner == 1 && lease.generation == 0);
    assert(!lease.ActiveLeases());

    Lease recallCut{};
    assert(recallCut.Offer(3, 4, 3, 100, baseline));
    assert(recallCut.Prepared(3, 101));
    assert(recallCut.Recall(400));
    const uint32_t recallDeadline = recallCut.deadline;
    assert(recallCut.AcceptState(3, 4, 3, 1, 2, finalState));
    assert(recallCut.Cut(3, 4, 3, 1, 2, finalState));
    assert(recallCut.phase == Phase::Frozen && recallCut.deadline == recallDeadline);
    assert(recallCut.HandBack(4, 500));
}

static void TestDeadEffectsAndCycles()
{
    Checkpoint ledger = ValidCheckpoint();
    assert(CommitEffect(ledger, NativeDeath));
    assert(!CommitEffect(ledger, NativeDeath));
    assert(CommitEffect(ledger, QuestDeath));
    assert(!CommitEffect(ledger, QuestDeath));
    assert((ledger.effects & (NativeDeath | QuestDeath)) == (NativeDeath | QuestDeath));
    assert(!CommitEffect(ledger, static_cast<Effect>(4)));

    Lease lease{};
    Checkpoint baseline = ValidCheckpoint();
    assert(lease.Offer(2, 1, 1, 100, baseline));
    assert(lease.Prepared(1, 101));

    Checkpoint terminal = baseline;
    terminal.dead = 1;
    assert(CommitEffect(terminal, NativeDeath));
    assert(lease.AcceptState(2, 1, 1, 1, 1, terminal));

    Checkpoint resurrected = baseline;
    resurrected.dead = 0;
    resurrected.hp = 10;
    assert(!lease.AcceptState(2, 1, 1, 1, 2, resurrected));

    Checkpoint missingEffect = terminal;
    missingEffect.effects = 0;
    assert(!lease.AcceptState(2, 1, 1, 1, 2, missingEffect));

    Checkpoint withBoth = terminal;
    assert(CommitEffect(withBoth, QuestDeath));
    assert(lease.AcceptState(2, 1, 1, 1, 3, withBoth));
    Checkpoint missingQuest = withBoth;
    missingQuest.effects = NativeDeath;
    assert(!lease.AcceptState(2, 1, 1, 1, 4, missingQuest));

    uint32_t nativeCount = 0, questCount = 0;
    for (uint32_t i = 0; i < 120; ++i) {
        Lease cycle{};
        Checkpoint c = ValidCheckpoint(static_cast<uint8_t>((i % 15) + 1));
        const uint32_t epoch = i + 1;
        assert(cycle.Offer(static_cast<uint8_t>(i % 7 + 1), i + 1, epoch, i * 100, c));
        assert(cycle.Prepared(epoch, i * 100));
        Checkpoint final = c;
        final.hp = i % 11;
        if (i % 2 == 0) {
            assert(CommitEffect(final, NativeDeath));
            ++nativeCount;
        }
        if (i % 3 == 0) {
            assert(CommitEffect(final, QuestDeath));
            ++questCount;
        }
        assert(cycle.Cut(static_cast<uint8_t>(i % 7 + 1), i + 1, epoch, 1, 1, final));
        assert(cycle.phase == Phase::Frozen);
        assert(cycle.HandBack(epoch + 1, i * 100 + 50));
        assert(cycle.phase == Phase::Host);
    }
    assert(nativeCount == 60);
    assert(questCount == 40);
}

static void TestMultipleOffersAndReorderedControl()
{
    Lease lease{};
    Checkpoint c = ValidCheckpoint();
    assert(lease.Offer(1, 1, 1, 0, c));
    assert(!lease.Offer(2, 2, 2, 1, c));
    assert(!lease.Prepared(0, 1));
    assert(lease.Prepared(1, 2999));
    Checkpoint current = c;
    current.hp = 9;
    assert(!lease.AcceptState(1, 1, 1, 1, 0, current)); // same revision differs
    assert(lease.AcceptState(1, 1, 1, 1, 5, current));
    assert(!lease.AcceptState(1, 1, 1, 1, 4, c));
    assert(!lease.AcceptState(1, 1, 1, 1, 5, c));
}

static void TestRecoverAndClientComposition()
{
    Checkpoint baseline = ValidCheckpoint();

    Lease offering{};
    assert(offering.Offer(3, 5, 10, 100, baseline));
    const uint32_t offeringEpoch = offering.epoch;
    const Checkpoint offeringState = offering.state;
    assert(!offering.Recover(offering.deadline - 1));
    assert(offering.Recover(offering.deadline));
    assert(offering.phase == Phase::Host);
    assert(offering.owner == 1 && offering.generation == 0);
    assert(offering.epoch == offeringEpoch);
    assert(offering.deadline == 0 && offering.started == 0);
    assert(detail::ExactState(offering.state, offeringState));

    Lease clientLease{};
    assert(clientLease.Offer(3, 5, 10, 100, baseline));
    assert(clientLease.Prepared(10, 100));
    const uint32_t clientDeadline = clientLease.deadline;
    const uint32_t clientEpoch = clientLease.epoch;
    Checkpoint injury = baseline;
    injury.hp = 7;
    assert(clientLease.AcceptState(3, 5, 10, 1, 1, injury));
    assert(!clientLease.Recover(clientDeadline - 1));
    assert(clientLease.Recover(clientDeadline));
    assert(clientLease.phase == Phase::Dormant);
    assert(clientLease.owner == 1 && clientLease.generation == 0);
    assert(clientLease.epoch == clientEpoch);
    assert(clientLease.deadline == 0 && clientLease.started == 0);
    assert(clientLease.revision == 1);
    assert(detail::ExactState(clientLease.state, injury));

    ClientWindow window;
    assert(window.Prepare(10, 100, LeaseMs, 100));
    const uint32_t windowDeadline = window.deadline;
    assert(!window.Prepare(9, 99, LeaseMs, 101));
    assert(window.deadline == windowDeadline);
    assert(window.Prepare(10, 150, LeaseMs, 170));
    assert(window.deadline == windowDeadline);
    assert(!window.Commit(9, 200));
    assert(window.Commit(10, 200));
    assert(window.Commit(10, 200));
    assert(window.Active(10, 201));
    assert(!window.Active(9, 201));
    assert(window.Renew(10, 400, LeaseMs, 400));
    const uint32_t renewedDeadline = window.deadline;
    assert(Newer(renewedDeadline, windowDeadline));
    assert(!window.Renew(10, 250, LeaseMs, 450));
    assert(window.deadline == renewedDeadline);
    assert(!window.Renew(9, 500, LeaseMs, 500));
    assert(!window.Renew(10, 500, LeaseMs + 1, 500));
    assert(window.Active(10, renewedDeadline - 1));
    assert(!window.Active(10, renewedDeadline));

    Lease recalling{};
    assert(recalling.Offer(3, 5, 20, 100, baseline));
    assert(recalling.Prepared(20, 100));
    const uint32_t recallLeaseDeadline = recalling.deadline;
    assert(recalling.Recall(500));
    assert(recalling.phase == Phase::Recalling);
    assert(recalling.deadline == recallLeaseDeadline);

    Checkpoint recallState = baseline;
    recallState.hp = 6;
    assert(recalling.AcceptState(3, 5, 20, 1, 2, recallState));
    assert(!recalling.AcceptState(3, 4, 20, 1, 3, recallState));
    assert(!recalling.AcceptState(3, 5, 19, 1, 3, recallState));
    assert(!recalling.AcceptState(3, 5, 20, 2, 3, recallState));
    assert(!recalling.AcceptState(3, 5, 20, 1, 1, recallState));
    assert(recalling.Cut(3, 5, 20, 1, 2, recallState));
    assert(recalling.phase == Phase::Frozen);
    window.Freeze();
    assert(!window.Renew(10, 600, LeaseMs, 600));
    assert(recalling.HandBack(21, 700));
    assert(recalling.phase == Phase::Host);
    assert(recalling.owner == 1 && recalling.generation == 0);
    assert(recalling.epoch == 21);
    assert(detail::ExactState(recalling.state, recallState));

    Lease lostCut{};
    assert(lostCut.Offer(4, 7, 30, 100, baseline));
    assert(lostCut.Prepared(30, 100));
    Checkpoint lostState = baseline;
    lostState.hp = 5;
    assert(lostCut.AcceptState(4, 7, 30, 1, 1, lostState));
    const uint32_t lostDeadline = lostCut.deadline;
    assert(!lostCut.Recover(lostDeadline - 1));
    assert(lostCut.Recover(lostDeadline));
    assert(lostCut.phase == Phase::Dormant);
    assert(lostCut.owner == 1 && lostCut.generation == 0);
    assert(lostCut.epoch == 30 && lostCut.revision == 1);
    assert(detail::ExactState(lostCut.state, lostState));

    Lease recallingLostCut{};
    assert(recallingLostCut.Offer(4, 8, 35, 100, baseline));
    assert(recallingLostCut.Prepared(35, 100));
    assert(recallingLostCut.Recall(200));
    const uint32_t recallingDeadline = recallingLostCut.deadline;
    Checkpoint recallingLostState = baseline;
    recallingLostState.hp = 4;
    assert(recallingLostCut.AcceptState(4, 8, 35, 1, 1, recallingLostState));
    assert(!recallingLostCut.Cut(4, 8, 34, 1, 2, recallingLostState));
    assert(recallingLostCut.Recover(recallingDeadline));
    assert(recallingLostCut.phase == Phase::Dormant);
    assert(recallingLostCut.owner == 1 && recallingLostCut.generation == 0);
    assert(recallingLostCut.epoch == 35 && recallingLostCut.revision == 1);
    assert(detail::ExactState(recallingLostCut.state, recallingLostState));

    ClientWindow lostWindow;
    assert(lostWindow.Prepare(30, 100, LeaseMs, 100));
    assert(lostWindow.Commit(30, 110));
    assert(!lostWindow.Active(30, lostDeadline));
    lostWindow.Reset(30);
    assert(lostWindow.mode == ClientWindow::Mode::Idle);
    assert(!lostWindow.Prepare(29, 200, LeaseMs, 200));
    assert(!lostWindow.Commit(29, 210));
}

static void TestTerminalRecoveryWatermark()
{
    Lease lease{};
    Checkpoint baseline = ValidCheckpoint();
    assert(lease.Offer(6, 11, 40, 100, baseline));
    assert(lease.Prepared(40, 100));

    Checkpoint terminal = baseline;
    terminal.hp = 0;
    assert(CommitEffect(terminal, NativeDeath));
    assert(CommitEffect(terminal, QuestDeath));
    assert(lease.AcceptState(6, 11, 40, 1, 1, terminal));
    assert(lease.Cut(6, 11, 40, 1, 1, terminal));
    assert(lease.HandBack(41, 500));
    assert(lease.phase == Phase::Host);
    assert(lease.owner == 1 && lease.generation == 0);
    assert(lease.epoch == 41 && lease.revision == 1);
    assert(lease.state.dead == 1);
    assert((lease.state.effects & (NativeDeath | QuestDeath)) == (NativeDeath | QuestDeath));

    Lease recovery{};
    assert(recovery.Offer(6, 12, 50, 100, terminal));
    assert(recovery.Prepared(50, 100));
    assert(recovery.AcceptState(6, 12, 50, 1, 2, terminal));
    const uint32_t recoveryDeadline = recovery.deadline;
    assert(recovery.Recover(recoveryDeadline));
    assert(recovery.phase == Phase::Dormant);
    assert(recovery.owner == 1 && recovery.generation == 0);
    assert(recovery.epoch == 50 && recovery.revision == 2);
    assert(recovery.state.dead == 1);
    assert((recovery.state.effects & (NativeDeath | QuestDeath)) == (NativeDeath | QuestDeath));

    Checkpoint resurrected = terminal;
    resurrected.dead = 0;
    resurrected.hp = 10;
    resurrected.effects = NativeDeath;
    assert(!recovery.Offer(6, 12, 51, recoveryDeadline, resurrected));

}

static void TestDisconnectLedger()
{
    Lease lease;
    Checkpoint baseline = ValidCheckpoint();
    assert(lease.Offer(2, 77, 42, 100, baseline));
    assert(lease.Prepared(42, 101));
    Checkpoint accepted = baseline;
    accepted.hp = 0;
    assert(CommitEffect(accepted, NativeDeath));
    assert(CommitEffect(accepted, QuestDeath));
    assert(lease.AcceptState(2, 77, 42, 1, 3, accepted));
    Checkpoint unacknowledged = accepted;
    unacknowledged.position[0] += 10;
    assert(!lease.AcceptState(2, 76, 42, 1, 4, unacknowledged));
    assert(lease.Recover(lease.deadline));
    assert(lease.phase == Phase::Dormant && lease.revision == 3);
    assert(detail::ExactState(lease.state, accepted));
    assert(!CommitEffect(lease.state, NativeDeath));
    assert(!CommitEffect(lease.state, QuestDeath));
    assert(!lease.AcceptState(2, 77, 42, 1, 4, unacknowledged));
    Checkpoint erased = accepted;
    erased.effects = NativeDeath;
    assert(!lease.Offer(3, 78, 43, 4000, erased));
    assert(lease.Offer(3, 78, 43, 4000, accepted));
    assert(lease.state.dead && lease.state.effects == (NativeDeath | QuestDeath));
}

static void RecalculateOverlayCrc(std::vector<uint8_t>& bytes)
{
    std::vector<uint8_t> checked(bytes.begin(), bytes.begin() + 16);
    checked.insert(checked.end(), bytes.begin() + 20, bytes.end());
    detail::Put32(bytes, 16, detail::Crc32(checked.data(), checked.size()));
}

static void TestLegacyOverlayDiscard()
{
    SaveOverlay overlay;
    for (uint32_t i = 0; i < 512; ++i) {
        Saved saved;
        saved.guid[0] = static_cast<uint8_t>(i + 1);
        saved.guid[15] = static_cast<uint8_t>((i >> 8) + 1);
        saved.incarnation = i + 1;
        saved.epoch = i + 10;
        saved.revision = i + 20;
        saved.state = ValidCheckpoint(static_cast<uint8_t>((i % 15) + 1));
        saved.adopted = static_cast<uint8_t>(i % 2);
        overlay.push_back(saved);
    }

    std::vector<uint8_t> encoded;
    assert(EncodeOverlay(overlay, encoded));
    assert(detail::Get32(encoded.data()) == detail::OverlayMagic);
    assert(detail::Get32(encoded.data() + 4) == detail::OverlayVersion);
    assert(detail::Get32(encoded.data() + 8) == 512);
    assert(detail::Get32(encoded.data() + 12) == 512 * detail::SavedWireSize);
    assert(encoded.size() == 134164);

    static constexpr uint8_t crcSample[] = {'1','2','3','4','5','6','7','8','9'};
    assert(detail::Crc32(crcSample, sizeof(crcSample)) == 0xcbf43926u);
    std::vector<uint8_t> legacy(encoded.begin(), encoded.begin() + 20);
    for (size_t i = 0; i < overlay.size(); ++i) {
        const auto row = encoded.begin() + 20 + i * detail::SavedWireSize;
        legacy.insert(legacy.end(), row, row + detail::LegacySavedWireSize);
    }
    detail::Put32(legacy, 4, detail::LegacyOverlayVersion);
    detail::Put32(legacy, 12, static_cast<uint32_t>(overlay.size() * detail::LegacySavedWireSize));
    RecalculateOverlayCrc(legacy);
    assert(legacy.size() == 133652);
    for (auto const& fixture : {legacy, encoded}) {
        std::string text = SavePrefix;
        constexpr char hex[] = "0123456789abcdef";
        for (uint8_t byte : fixture) {
            text += hex[byte >> 4];
            text += hex[byte & 15];
        }
        text += '\n';
        SaveOverlay discarded = overlay;
        assert(DecodeSaveText(text, discarded) && discarded.empty());
        text.pop_back(); // Invalid embedded legacy payloads cannot block restore either.
        discarded = overlay;
        assert(DecodeSaveText(text, discarded) && discarded.empty());
    }
    std::string capture = "old content";
    assert(EncodeSaveText(overlay, capture) && capture.empty());
}

int main()
{
    TestValidityAndWire();
    TestMessageCodec();
    TestNativeDomainActions();
    TestEligibilityAndWrap();
    TestHostPresenceEligibility();
    TestLeaseGrantAndExpiry();
    TestCutRecallAndHandoff();
    TestDeadEffectsAndCycles();
    TestMultipleOffersAndReorderedControl();
    TestRecoverAndClientComposition();
    TestTerminalRecoveryWatermark();
    TestDisconnectLedger();
    TestLegacyOverlayDiscard();
    std::fprintf(stdout, "sizeof(Checkpoint)=%zu sizeof(Message)=%zu\n",
                 sizeof(Checkpoint), sizeof(Message));
    std::puts("npclease_test: PASS");
    return 0;
}
