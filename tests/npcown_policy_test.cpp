// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcown_policy.h"
#include "../src/npclease.h"
#include "../src/npclease_client.h"
#include "../src/npclease_admission.h"
#include "../src/npclease_presentation.h"
#include "../src/replication.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <type_traits>

namespace own = g3t::npcown;
namespace lease = g3t::npclease;

enum RoleFlag : unsigned {
    Incomplete = 1u << 0, Hero = 1u << 1, Puppet = 1u << 2,
    Summon = 1u << 3, Item = 1u << 4, Duplicate = 1u << 5, Removed = 1u << 6
};

static own::RoleFacts Role(unsigned flags)
{
    own::RoleFacts role;
    role.complete = (flags & Incomplete) == 0;
    role.hero = (flags & Hero) != 0;
    role.remotePuppet = (flags & Puppet) != 0;
    role.summonOrFollower = (flags & Summon) != 0;
    role.itemOrSpell = (flags & Item) != 0;
    role.duplicateIdentity = (flags & Duplicate) != 0;
    role.removed = (flags & Removed) != 0;
    return role;
}

static char const* ExpectedExclusion(unsigned flags)
{
    if (flags & Removed) return "authored removal";
    if (flags & (Hero | Puppet)) return "hero or remote player";
    if (flags & Duplicate) return "duplicate identity quarantined";
    if (flags & Incomplete) return "incomplete native NPC/template";
    if (flags & Item) return "item or spell actor";
    if (flags & Summon) return "summon/party/owned actor";
    return nullptr;
}

static void SameReason(char const* actual, char const* expected)
{
    if (!expected) {
        assert(actual == nullptr);
    } else {
        assert(actual != nullptr);
        assert(std::strcmp(actual, expected) == 0);
    }
}

static void TestRoles()
{
    for (unsigned flags = 0; flags < 128; ++flags) {
        SameReason(own::Exclusion(Role(flags)), ExpectedExclusion(flags));
        own::Facts facts;
        facts.role = Role(flags);
        facts.hostPresenting = true;
        const auto decision = own::Decide(facts);
        assert(decision.verdict == (flags ? own::Verdict::Excluded : own::Verdict::HostStreamed));
        SameReason(decision.reason, flags ? ExpectedExclusion(flags) : "host presenting");
    }
}

static void TestDecisions()
{
    // These are descriptive test traits, not RoleFacts fields or ownership restrictions.
    enum Trait : unsigned {
        Human = 1u << 0, Orc = 1u << 1, Goblin = 1u << 2, Slave = 1u << 3,
        Guard = 1u << 4, Trader = 1u << 5, Teacher = 1u << 6, Named = 1u << 7,
        Quest = 1u << 8, Dialogue = 1u << 9, Immortal = 1u << 10,
        Wolf = 1u << 11, Scavenger = 1u << 12, Deer = 1u << 13, Equipment = 1u << 14
    };
    struct NpcClass { char const* name; unsigned traits; };
    const NpcClass classes[] = {
        {"plain human", Human}, {"orc", Orc}, {"goblin", Goblin},
        {"slave", Human | Slave}, {"guard", Human | Guard},
        {"trader", Human | Trader}, {"teacher", Human | Teacher},
        {"named quest/dialogue actor", Human | Named | Quest | Dialogue},
        {"immortal actor", Human | Immortal},
        {"wolf", Wolf}, {"scavenger", Scavenger}, {"deer", Deer},
        {"creature with non-default equipment", Wolf | Equipment},
        {"named creature", Wolf | Named}
    };
    static_assert(std::is_same<decltype(&own::Decide), own::Decision (*)(own::Facts const&)>::value,
                  "Decide has no dwell or time argument");
    for (auto const& npc : classes) {
        assert(npc.name[0] != '\0' && npc.traits != 0);
        own::Facts facts;
        assert(own::Exclusion(facts.role) == nullptr);
        auto decision = own::Decide(facts);
        assert(decision.verdict == own::Verdict::ClientNative);
        SameReason(decision.reason, "host not presenting");
        facts.hostPresenting = true;
        decision = own::Decide(facts);
        assert(decision.verdict == own::Verdict::HostStreamed);
        SameReason(decision.reason, "host presenting");
    }

    // Exhaust the readiness flags and both overriding states, including their priority.
    for (unsigned bits = 0; bits < 512; ++bits) {
        own::Facts facts;
        facts.installed = (bits & 1u) != 0;
        facts.nativeReady = (bits & 2u) != 0;
        facts.settled = (bits & 4u) != 0;
        facts.cooldownHeld = (bits & 8u) != 0;
        facts.alreadyOwned = (bits & 16u) != 0;
        facts.hostPresenting = (bits & 32u) != 0;
        facts.configEnabled = (bits & 64u) != 0;
        facts.hostStreamUncertain = (bits & 128u) != 0;
        facts.hostReleasePending = (bits & 256u) != 0;
        own::Verdict verdict;
        char const* reason;
        if (!facts.configEnabled) {
            verdict = own::Verdict::Excluded; reason = "npc_authority_fallback off";
        } else if (facts.hostPresenting) {
            verdict = own::Verdict::HostStreamed; reason = "host presenting";
        } else if (facts.alreadyOwned) {
            verdict = own::Verdict::ClientNative; reason = "owned";
        } else if (!facts.installed) {
            verdict = own::Verdict::Wait; reason = "native authority guards initializing";
        } else if (!facts.nativeReady) {
            verdict = own::Verdict::Wait; reason = "native processing/actor not ready";
        } else if (!facts.settled) {
            verdict = own::Verdict::Wait; reason = "host stream settling";
        } else if (facts.hostReleasePending) {
            verdict = own::Verdict::Wait; reason = "final host state awaiting presentation";
        } else if (facts.hostStreamUncertain) {
            verdict = own::Verdict::Wait; reason = "host stream stale; authority unknown";
        } else if (facts.cooldownHeld) {
            verdict = own::Verdict::Wait; reason = "host yield cool-down";
        } else {
            verdict = own::Verdict::ClientNative; reason = "host not presenting";
        }
        const auto decision = own::Decide(facts);
        assert(decision.verdict == verdict);
        SameReason(decision.reason, reason);
    }
    own::Facts excluded;
    excluded.role.removed = true;
    excluded.configEnabled = false;
    excluded.hostPresenting = true;
    excluded.alreadyOwned = true;
    SameReason(own::Decide(excluded).reason, "authored removal");
    assert(own::Decide(excluded).verdict == own::Verdict::Excluded);
}

static void TestTiming()
{
    assert(own::StreamSettleMs == 2000u);
    assert(own::YieldCooldownMs == 1500u);
    const uint32_t start = 100u;
    assert(!own::StreamSettled(start + own::StreamSettleMs - 1u, start, false));
    assert(own::StreamSettled(start + own::StreamSettleMs, start, false));
    assert(own::StreamSettled(start + own::StreamSettleMs + 1u, start, false));
    assert(own::StreamSettled(start, start, true));
    assert(own::StreamSettled(0, 0, true));
    assert(!own::StreamSettled(UINT32_MAX, 0, false));
    assert(own::CooldownHeld(start + own::YieldCooldownMs - 1u, start));
    assert(!own::CooldownHeld(start + own::YieldCooldownMs, start));
    assert(!own::CooldownHeld(start + own::YieldCooldownMs + 1u, start));
    assert(!own::CooldownHeld(0, 0));
    assert(!own::CooldownHeld(UINT32_MAX, 0));

    const uint32_t wrapStart = UINT32_MAX - 1000u;
    assert(!own::StreamSettled(wrapStart + own::StreamSettleMs - 1u, wrapStart, false));
    assert(own::StreamSettled(wrapStart + own::StreamSettleMs, wrapStart, false));
    assert(own::StreamSettled(wrapStart + own::StreamSettleMs + 1u, wrapStart, false));
    assert(own::CooldownHeld(wrapStart + own::YieldCooldownMs - 1u, wrapStart));
    assert(!own::CooldownHeld(wrapStart + own::YieldCooldownMs, wrapStart));
    assert(!own::CooldownHeld(wrapStart + own::YieldCooldownMs + 1u, wrapStart));
}

static void TestRelease()
{
    own::OwnerFacts facts;
    assert(own::Release(facts) == nullptr);
    bool own::OwnerFacts::* const nonFatal[] = {
        &own::OwnerFacts::leaseExpired, &own::OwnerFacts::hostSilent,
        &own::OwnerFacts::checkpointBusy, &own::OwnerFacts::paused,
        &own::OwnerFacts::registrationFailed, &own::OwnerFacts::processingListsMissing,
        &own::OwnerFacts::outsideRing, &own::OwnerFacts::disconnected
    };
    for (auto field : nonFatal) {
        own::OwnerFacts single;
        single.*field = true;
        assert(own::Release(single) == nullptr);
        facts.*field = true;
    }
    assert(own::Release(facts) == nullptr);
    facts.hostPresenting = true;
    SameReason(own::Release(facts), "host presenting");
    facts.hostPresenting = false;
    facts.hostCommit = true;
    SameReason(own::Release(facts), "host took over");
    facts.hostCommit = false;
    facts.hostRecallDue = true;
    SameReason(own::Release(facts), "host recall");
    facts.hostPresenting = facts.hostCommit = true;
    SameReason(own::Release(facts), "host presenting");
    facts.hostPresenting = false;
    SameReason(own::Release(facts), "host took over");
    for (unsigned flags = 1; flags < 128; ++flags) {
        facts.role = Role(flags);
        SameReason(own::Release(facts), ExpectedExclusion(flags));
    }
}

static void TestRegistration()
{
    for (unsigned bits = 0; bits < 16; ++bits) {
        const bool processed = (bits & 1u) != 0;
        const bool processable = (bits & 2u) != 0;
        const bool queued = (bits & 4u) != 0;
        const bool enabled = (bits & 8u) != 0;
        const auto expected = processed && processable ? own::Registration::AlreadyProcessed :
                              queued ? own::Registration::Queued :
                              enabled ? own::Registration::Enabled : own::Registration::Unavailable;
        assert(own::Classify(processed, processable, queued, enabled) == expected);
    }
    struct RegistrationCase { own::Registration value; char const* name; };
    const RegistrationCase cases[] = {
        {own::Registration::AlreadyProcessed, "already-processed"},
        {own::Registration::Queued, "queued-dirty-processing"},
        {own::Registration::Enabled, "enabled-processing"},
        {own::Registration::Unavailable, "unavailable"}
    };
    for (auto const& entry : cases) {
        SameReason(own::Name(entry.value), entry.name);
        assert(own::KeepOwnership(true, entry.value));
        assert(!own::KeepOwnership(false, entry.value));
    }
}

static own::Guid Guid(unsigned id)
{
    own::Guid guid{};
    guid[0] = static_cast<uint8_t>(id & 0xffu);
    guid[1] = static_cast<uint8_t>((id >> 8) & 0xffu);
    return guid;
}

static void TestRefusals()
{
    own::RefusalLedger ledger;
    char const* top = nullptr;
    unsigned topCount = 99;
    ledger.Top(top, topCount);
    SameReason(top, "-");
    assert(topCount == 0 && ledger.Unique() == 0 && ledger.Attempts() == 0);
    assert(ledger.Note(Guid(1), "alpha"));
    assert(!ledger.Note(Guid(1), "alpha"));
    assert(ledger.Note(Guid(2), "alpha"));
    assert(ledger.Note(Guid(3), "beta"));
    assert(ledger.Unique() == 3 && ledger.Attempts() == 4);
    assert(ledger.Count("alpha") == 2 && ledger.Count("beta") == 1 && ledger.Count("missing") == 0);
    ledger.Top(top, topCount);
    SameReason(top, "alpha");
    assert(topCount == 2);
    assert(ledger.Note(Guid(1), "beta"));
    assert(!ledger.Note(Guid(1), "beta"));
    assert(ledger.Unique() == 3 && ledger.Attempts() == 6);
    assert(ledger.Count("alpha") == 1 && ledger.Count("beta") == 2);
    SameReason(ledger.Reason(Guid(1)), "beta");
    assert(ledger.Reason(Guid(4)) == nullptr);
    ledger.Top(top, topCount);
    SameReason(top, "beta");
    assert(topCount == 2);
    ledger.Forget(Guid(1));
    ledger.Forget(Guid(4));
    assert(ledger.Reason(Guid(1)) == nullptr);
    assert(ledger.Unique() == 2 && ledger.Count("beta") == 1 && ledger.Attempts() == 6);
    ledger.Forget(Guid(3));
    assert(ledger.Count("beta") == 0);
    ledger.Top(top, topCount);
    SameReason(top, "alpha");
    assert(topCount == 1);
    assert(ledger.Note(Guid(1), "beta"));
    ledger.Clear();
    assert(ledger.Unique() == 0 && ledger.Attempts() == 0 && ledger.Count("alpha") == 0);
    assert(ledger.Reason(Guid(1)) == nullptr);
    ledger.Top(top, topCount);
    SameReason(top, "-");
    assert(topCount == 0);

    unsigned logLines = 0;
    for (unsigned second = 0; second < 3; ++second) {
        for (unsigned call = 0; call < 1500; ++call) {
            if (ledger.Note(Guid(call % 100u), "unchanged")) ++logLines;
        }
    }
    assert(logLines == 100 && ledger.Unique() == 100 && ledger.Attempts() == 4500);
    assert(ledger.Count("unchanged") == 100);
    ledger.Top(top, topCount);
    SameReason(top, "unchanged");
    assert(topCount == 100);
}

static void TestTransitions()
{
    struct EventCase { own::Event value; char const* name; };
    const EventCase events[] = {
        {own::Event::Loaded, "loaded"}, {own::Event::Owned, "owned"},
        {own::Event::Confirmed, "confirmed"}, {own::Event::Waiting, "waiting"},
        {own::Event::Refused, "refused"}, {own::Event::Hit, "hit"},
        {own::Event::Reaction, "reaction"}, {own::Event::Death, "death"},
        {own::Event::Frozen, "frozen"}, {own::Event::Yielded, "yielded"},
        {own::Event::HandBack, "handback"}, {own::Event::Reclaimed, "reclaimed"},
        {own::Event::HostSilent, "host-silent"}, {own::Event::Registration, "registration"}
    };
    own::TransitionLog log;
    for (auto const& event : events) {
        SameReason(own::Name(event.value), event.name);
        assert(log.Changed(Guid(1), event.value));
        assert(!log.Changed(Guid(1), event.value));
    }
    assert(log.Changed(Guid(2), own::Event::Registration));
    assert(!log.Changed(Guid(2), own::Event::Registration));
    log.Forget(Guid(1));
    assert(log.Changed(Guid(1), own::Event::Registration));
    assert(!log.Changed(Guid(2), own::Event::Registration));
    log.Clear();
    assert(log.Changed(Guid(1), own::Event::Registration));
    assert(log.Changed(Guid(2), own::Event::Registration));
}

static void TestFormatting()
{
    own::OwnStats stats;
    stats.hostStreamed = 1; stats.nativeOwned = 2; stats.confirmed = 3;
    stats.visualOnly = 4; stats.waiting = 5; stats.refused = 6; stats.frozen = 7;
    stats.hitsApplied = 8; stats.deaths = 9; stats.handBacks = 10;
    stats.yields = 11; stats.takeovers = 12; stats.reclaims = 13;
    stats.registrationAlready = 14; stats.registrationQueued = 15;
    stats.registrationEnabled = 16; stats.registrationFailed = 17;
    stats.visualTop = "visual"; stats.visualTopCount = 18;
    stats.waitingTop = "waiting"; stats.waitingTopCount = 19;
    stats.refusedTop = "refused"; stats.refusedTopCount = 20;
    stats.frozenTop = "frozen"; stats.frozenTopCount = 21;
    const char* summary =
        "npc-own: summary host_streamed=1 native_owned=2 confirmed=3 visual_only=4(top=visual x18) "
        "waiting=5(top=waiting x19) refused=6(top=refused x20) frozen=7(top=frozen x21) "
        "hits_applied=8 deaths=9 hand_backs=10 yields=11 takeovers=12 reclaims=13 "
        "registration=already:14,queued:15,enabled:16,failed:17";
    char buffer[512];
    int length = own::FormatSummary(buffer, sizeof(buffer), stats);
    assert(length == static_cast<int>(std::strlen(summary)));
    assert(std::strcmp(buffer, summary) == 0);
    char tiny[12];
    length = own::FormatSummary(tiny, sizeof(tiny), stats);
    assert(length == static_cast<int>(std::strlen(summary)) && length >= static_cast<int>(sizeof(tiny)));
    assert(tiny[sizeof(tiny) - 1] == '\0');
    assert(std::strncmp(tiny, summary, sizeof(tiny) - 1) == 0);
    char one = 'x';
    assert(own::FormatSummary(&one, 1, stats) == static_cast<int>(std::strlen(summary)) && one == '\0');
    one = 'x';
    assert(own::FormatSummary(&one, 0, stats) == static_cast<int>(std::strlen(summary)) && one == 'x');

    const char* event = "npc-own: event=handback guid=abcdef name=Wolf hp=42";
    length = own::FormatEvent(buffer, sizeof(buffer), "abcdef", "Wolf", own::Event::HandBack, "hp=42");
    assert(length == static_cast<int>(std::strlen(event)));
    assert(std::strcmp(buffer, event) == 0);
    length = own::FormatEvent(tiny, sizeof(tiny), "abcdef", "Wolf", own::Event::HandBack, "hp=42");
    assert(length == static_cast<int>(std::strlen(event)) && length >= static_cast<int>(sizeof(tiny)));
    assert(tiny[sizeof(tiny) - 1] == '\0');
    assert(std::strncmp(tiny, event, sizeof(tiny) - 1) == 0);
    one = 'x';
    assert(own::FormatEvent(&one, 1, "abcdef", "Wolf", own::Event::HandBack, "hp=42") ==
           static_cast<int>(std::strlen(event)) && one == '\0');
}

static lease::Checkpoint Baseline()
{
    lease::Checkpoint state;
    state.templateId[0] = 1;
    const char name[] = "Client-first NPC";
    std::memcpy(state.name, name, sizeof(name));
    state.maxHp = state.hp = 100;
    return state;
}

static void TestStaleAuthority()
{
    for (uint32_t rtt : {30u, 60u}) {
        for (uint32_t gap : {0u, 2000u, 5000u}) {
            own::Facts candidate;
            candidate.hostPresenting = lease::HostClaim(true, true, false, 2);
            candidate.hostStreamUncertain = !own::AcquisitionTimely(gap, rtt);
            assert(own::Decide(candidate).verdict == own::Verdict::HostStreamed);
            if (gap) assert(!lease::PresentationFresh(true, false, gap, 34));
        }
        assert(own::AcquisitionTimely(1000, rtt)); // unchanged idle heartbeat
    }
    assert(own::AcquisitionTimely(own::AuthorityObservationMs, own::AuthorityObservationMs));
    assert(!own::AcquisitionTimely(own::AuthorityObservationMs + 1, 30));
    assert(!own::AcquisitionTimely(0, 2000));
    assert(!lease::HostClaim(false, true, false, 2));
    assert(!lease::HostClaim(true, false, false, 2));
    assert(!lease::HostClaim(true, true, true, 2));
    assert(!lease::HostClaim(true, true, false, 0));

    own::Facts candidate;
    candidate.hostStreamUncertain = true;
    assert(own::Decide(candidate).verdict == own::Verdict::Wait);
    candidate.alreadyOwned = true;
    assert(own::Decide(candidate).verdict == own::Verdict::ClientNative);
    candidate.alreadyOwned = false;
    candidate.hostStreamUncertain = false;
    candidate.hostPresenting = lease::HostClaim(true, true, true, 2);
    assert(own::Decide(candidate).verdict == own::Verdict::ClientNative); // explicit release, healthy link
    candidate.hostReleasePending = lease::ReleasePending(true, 32, true, 1000, 3000, 0);
    assert(own::Decide(candidate).verdict == own::Verdict::Wait);
    assert(lease::ReleasePending(true, 32, true, 3000, 3000, 1)); // do not skip a final event
    assert(lease::ReleasePending(true, 1, false, 0, 0, 0));
    assert(lease::ReleasePending(true, 32, true, UINT32_MAX - 1, 2, 0));
    assert(!lease::ReleasePending(false, 32, true, 1000, 3000, 1));
    assert(!lease::ReleasePending(true, 0, false, 0, 3000, 0));
    candidate.hostReleasePending = lease::ReleasePending(true, 32, true, 3000, 3000, 0);
    assert(own::Decide(candidate).verdict == own::Verdict::ClientNative);

    auto local = Baseline();
    local.dead = 1; local.hp = 0; local.effects = lease::NativeDeath;
    own::OwnerFacts owner;
    owner.hostSilent = owner.leaseExpired = true;
    assert(own::Release(owner) == nullptr); // genuinely client-only death is kept
    owner.hostPresenting = lease::HostClaim(true, true, false, 2);
    SameReason(own::Release(owner), "host presenting"); // local terminal outcome has no veto
    g3t::replication::TerminalLife hostLife;
    assert(hostLife.Observe(1, false));
    assert(lease::ReconcileLife(local.dead != 0, false, hostLife.dead));
    assert(hostLife.Observe(1, false)); // a local death did not latch canonical death
    assert(hostLife.Observe(1, true));
    assert(!lease::ReconcileLife(local.dead != 0, false, hostLife.dead));
    assert(!hostLife.Observe(1, false)); // canonical terminal life still cannot resurrect
    assert(hostLife.Observe(2, false));
    assert(lease::ReconcileLife(true, true, hostLife.dead));
    assert(!lease::ReconcileLife(false, false, false));
}

static void TestLatestHandBack()
{
    lease::Lease host;
    lease::Checkpoint latest = Baseline();
    assert(host.Offer(2, 7, 1, 100, latest));
    assert(host.Prepared(1, 101));
    assert(host.phase == lease::Phase::Client);
    for (uint32_t revision = 1; revision <= 5; ++revision) {
        latest.hp -= 10;
        latest.position[0] += 12.5f;
        latest.position[1] -= 2.0f;
        assert(host.AcceptState(2, 7, 1, 1, revision, latest));
        assert(std::memcmp(&host.state, &latest, sizeof(latest)) == 0);
    }
    lease::Checkpoint stale = latest;
    stale.hp = 90;
    stale.position[0] = 12.5f;
    assert(!host.AcceptState(2, 7, 1, 1, 4, stale));
    assert(!host.Cut(2, 7, 1, 1, 4, stale));
    assert(host.phase == lease::Phase::Client);
    assert(std::memcmp(&host.state, &latest, sizeof(latest)) == 0);
    assert(host.Cut(2, 7, 1, 1, 5, latest));
    assert(host.phase == lease::Phase::Frozen);
    assert(std::memcmp(&host.state, &latest, sizeof(latest)) == 0);
    assert(host.HandBack(2, 200));
    assert(host.phase == lease::Phase::Host);
    assert(std::memcmp(&host.state, &latest, sizeof(latest)) == 0);
    assert(lease::Newer(1, UINT32_MAX) && !lease::Newer(UINT32_MAX, 1));
    assert(!lease::Newer(1, 1));
    assert(lease::Reached(1, UINT32_MAX) && lease::Reached(1, 1));
    assert(!lease::Reached(UINT32_MAX, 1));
}

static void TestExactlyOnceDeath()
{
    lease::Lease host;
    lease::Checkpoint state = Baseline();
    assert(host.Offer(2, 7, 1, 100, state));
    assert(host.Prepared(1, 101));
    assert(lease::CommitEffect(state, lease::NativeDeath));
    assert(!lease::CommitEffect(state, lease::NativeDeath));
    assert(state.dead == 1 && state.effects == lease::NativeDeath);
    assert(lease::CommitEffect(state, lease::QuestDeath));
    assert(!lease::CommitEffect(state, lease::QuestDeath));
    assert(state.effects == (lease::NativeDeath | lease::QuestDeath));
    state.hp = 0;
    assert(host.AcceptState(2, 7, 1, 1, 1, state));
    lease::Checkpoint invalid = state;
    invalid.dead = 0;
    assert(!host.AcceptState(2, 7, 1, 1, 2, invalid));
    invalid = state;
    invalid.effects = lease::NativeDeath;
    assert(!host.AcceptState(2, 7, 1, 1, 2, invalid));
    invalid.effects = lease::QuestDeath;
    assert(!host.AcceptState(2, 7, 1, 1, 2, invalid));
    assert(std::memcmp(&host.state, &state, sizeof(state)) == 0);
    assert(host.Cut(2, 7, 1, 1, 1, state));
    assert(host.phase == lease::Phase::Frozen && host.state.dead == 1);
    assert(std::memcmp(&host.state, &state, sizeof(state)) == 0);
}

static void TestDuplicateOffers()
{
    // One Lease represents one GUID; another offer cannot replace its client owner.
    lease::Lease host;
    const lease::Checkpoint state = Baseline();
    assert(host.Offer(2, 7, 1, 100, state));
    assert(host.Prepared(1, 101));
    assert(!host.Offer(2, 7, 2, 102, state));
    assert(!host.Offer(3, 8, 3, 103, state));
    assert(host.phase == lease::Phase::Client && host.owner == 2 && host.epoch == 1);
    assert(host.Cut(2, 7, 1, 1, 0, state));
    assert(!host.HandBack(1, 104));
    assert(host.HandBack(2, 104));
    assert(!host.Offer(2, 7, 1, 105, state));
    assert(!host.Offer(2, 7, 2, 105, state));
    assert(host.Offer(2, 7, 3, 105, state));
    assert(host.phase == lease::Phase::Offering && host.epoch == 3);
}

static void TestClientWindow()
{
    lease::ClientWindow client;
    assert(!client.Commit(1, 100));
    assert(client.Prepare(1, 100, lease::LeaseMs, 101));
    const uint32_t deadline = client.deadline;
    assert(client.Prepare(1, 200, lease::LeaseMs, 201));
    assert(client.deadline == deadline);
    assert(client.Commit(1, 202));
    assert(client.Active(1, deadline - 1));
    assert(!client.Active(1, deadline));
    assert(!client.Active(2, 202));
    assert(client.Renew(1, 300, lease::LeaseMs, 301));
    assert(!client.Renew(1, 300, lease::LeaseMs, 302));
    client.Freeze();
    assert(!client.Active(1, 303));
    assert(!client.Renew(1, 400, lease::LeaseMs, 401));
    client.Reset(2);
    assert(!client.Prepare(2, 500, lease::LeaseMs, 501));
    assert(client.Prepare(3, 500, lease::LeaseMs, 501));
    assert(client.Commit(3, 502));

    lease::ClientReadiness readiness;
    assert(readiness.LossReason() == nullptr);
    readiness.enabled = false;
    SameReason(readiness.LossReason(), "authority fallback disabled");
    assert(lease::LeaseProcessingRange(true, lease::ClientRadiusCm));
    assert(!lease::LeaseProcessingRange(true, lease::ClientRadiusCm + 1.0f));
}

static void TestLeaseAdmission()
{
    // NativeClass is a diagnostic label; it does not supply an ownership exclusion.
    const lease::NativeClass labels[] = {
        lease::NativeClass::Unsupported, lease::NativeClass::Creature, lease::NativeClass::Ordinary
    };
    for (auto label : labels) {
        assert(label == lease::NativeClass::Unsupported || label == lease::NativeClass::Creature ||
               label == lease::NativeClass::Ordinary);
        own::Facts facts;
        assert(own::Decide(facts).verdict == own::Verdict::ClientNative);
        facts.hostPresenting = true;
        assert(own::Decide(facts).verdict == own::Verdict::HostStreamed);
    }
}

int main()
{
    TestRoles();
    TestDecisions();
    TestTiming();
    TestRelease();
    TestRegistration();
    TestRefusals();
    TestTransitions();
    TestFormatting();
    TestLatestHandBack();
    TestExactlyOnceDeath();
    TestDuplicateOffers();
    TestClientWindow();
    TestLeaseAdmission();
    TestStaleAuthority();
    std::cout << "npcown_policy_test: OK (14 groups)\n";
}
