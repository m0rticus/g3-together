// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Engine-free decision table for client-processed NPCs.
//
// Principle: a client-loaded NPC is driven by the client exactly when the host is not presenting it (no fresh host stream).
// The host handshake (npclease) is bookkeeping that runs asynchronously; it is never a precondition for animating or reacting
// and a failed registration/lease/round trip never freezes a client-owned NPC.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace g3t::npcown {

// ---- role admission ------------------------------------------------------------------------------------------------
// Only these roles stay out of client ownership: the hero, remote player puppets, replicated summons (and party/owned actors
// that follow a player), item/spell actors, duplicate identities, authored removals and templates the engine cannot run.
// Humans, orcs, goblins, slaves, guards, merchants, named/quest/dialogue actors, immortal actors and every creature class live.
struct RoleFacts {
    bool complete = true;
    bool hero = false, remotePuppet = false;
    bool summonOrFollower = false;
    bool itemOrSpell = false;
    bool duplicateIdentity = false;
    bool removed = false;
};

inline char const* Exclusion(RoleFacts const& role)
{
    if (role.removed) return "authored removal";
    if (role.hero || role.remotePuppet) return "hero or remote player";
    if (role.duplicateIdentity) return "duplicate identity quarantined";
    if (!role.complete) return "incomplete native NPC/template";
    if (role.itemOrSpell) return "item or spell actor";
    if (role.summonOrFollower) return "summon/party/owned actor";
    return nullptr;
}

// ---- the decision ---------------------------------------------------------------------------------------------------
enum class Verdict : uint8_t { HostStreamed, ClientNative, Wait, Excluded };

struct Facts {
    RoleFacts role;
    bool installed = true;        // native authority guards (Kill/quest hooks) ready
    bool configEnabled = true;    // npc_authority_fallback
    bool nativeReady = true;      // processed + processable + animation actor
    bool hostPresenting = false;  // fresh, unreleased host stream for this entity
    bool settled = true;          // host stream settle grace elapsed (once per host session)
    bool cooldownHeld = false;    // just yielded to a host stream (anti-flap)
    bool alreadyOwned = false;
};

struct Decision {
    Verdict verdict = Verdict::Wait;
    char const* reason = "";
};

inline Decision Decide(Facts const& facts)
{
    if (char const* why = Exclusion(facts.role)) return {Verdict::Excluded, why};
    if (!facts.configEnabled) return {Verdict::Excluded, "npc_authority_fallback off"};
    if (facts.hostPresenting) return {Verdict::HostStreamed, "host presenting"};
    if (facts.alreadyOwned) return {Verdict::ClientNative, "owned"};
    if (!facts.installed) return {Verdict::Wait, "native authority guards initializing"};
    if (!facts.nativeReady) return {Verdict::Wait, "native processing/actor not ready"};
    if (!facts.settled) return {Verdict::Wait, "host stream settling"};
    if (facts.cooldownHeld) return {Verdict::Wait, "host yield cool-down"};
    return {Verdict::ClientNative, "host not presenting"};
}

// ---- acquisition timing ---------------------------------------------------------------------------------------------
// No per-NPC dwell: the only delay is one grace per host session so the first stream batch can arrive, and a short cool-down
// after the host proved it presents an NPC (so a marginal stream cannot flip ownership every frame).
constexpr uint32_t StreamSettleMs = 2000;
constexpr uint32_t YieldCooldownMs = 1500;
constexpr uint32_t CutReclaimMs = 2500;

inline bool StreamSettled(uint32_t now, uint32_t sessionReadyAt, bool hostStreamSeen)
{
    return hostStreamSeen || (sessionReadyAt && now - sessionReadyAt >= StreamSettleMs);
}
inline bool CooldownHeld(uint32_t now, uint32_t yieldedAt)
{
    return yieldedAt && now - yieldedAt < YieldCooldownMs;
}

// ---- keeping ownership ----------------------------------------------------------------------------------------------
// The only reasons a client-owned NPC stops running natively. Everything else (lease window expiry, a silent host, a paused
// game, a checkpoint, a failed registration, leaving the processing lists or the 100 m ring) only suspends diagnostics.
struct OwnerFacts {
    RoleFacts role;
    bool hostPresenting = false;     // fresh host stream: the host drives, the client discards its local state
    bool hostCommit = false;         // host announced it took the NPC (Commit owner=1 / Abort)
    bool hostRecallDue = false;      // host recalled it and a safe cut point was reached
    bool leaseExpired = false;
    bool hostSilent = false;
    bool checkpointBusy = false;
    bool paused = false;
    bool registrationFailed = false;
    bool processingListsMissing = false;
    bool outsideRing = false;
    bool disconnected = false;
};

inline char const* Release(OwnerFacts const& facts)
{
    if (char const* why = Exclusion(facts.role)) return why;
    if (facts.hostPresenting) return "host presenting";
    if (facts.hostCommit) return "host took over";
    if (facts.hostRecallDue) return "host recall";
    return nullptr;
}

// ---- processing registration ----------------------------------------------------------------------------------------
// Registration is an optimization (it only puts an entity into the engine's range/processable lists). The ladder reports
// which path worked; the outcome never decides ownership.
enum class Registration : uint8_t { AlreadyProcessed, Queued, Enabled, Unavailable };

inline Registration Classify(bool processed, bool processable, bool queued, bool enabled)
{
    if (processed && processable) return Registration::AlreadyProcessed;
    if (queued) return Registration::Queued;
    if (enabled) return Registration::Enabled;
    return Registration::Unavailable;
}
inline bool KeepOwnership(bool owned, Registration) { return owned; }
inline char const* Name(Registration path)
{
    switch (path) {
    case Registration::AlreadyProcessed: return "already-processed";
    case Registration::Queued: return "queued-dirty-processing";
    case Registration::Enabled: return "enabled-processing";
    default: return "unavailable";
    }
}

// ---- refusal ledger -------------------------------------------------------------------------------------------------
using Guid = std::array<uint8_t, 16>;
struct GuidHash {
    size_t operator()(Guid const& key) const
    {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t byte : key) h = (h ^ byte) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

// One entry per GUID. Note() is true only when the GUID is new or its reason changed, i.e. when a log line is due.
class RefusalLedger {
    std::unordered_map<Guid, std::string, GuidHash> reasons;
    std::unordered_map<std::string, unsigned> counts;
    unsigned attempts = 0;
public:
    bool Note(Guid const& key, char const* reason)
    {
        ++attempts;
        const auto found = reasons.find(key);
        if (found != reasons.end()) {
            if (found->second == reason) return false;
            auto& old = counts[found->second];
            if (old) --old;
            found->second = reason;
        } else reasons.emplace(key, reason);
        ++counts[reason];
        return true;
    }
    void Forget(Guid const& key)
    {
        const auto found = reasons.find(key);
        if (found == reasons.end()) return;
        auto& count = counts[found->second];
        if (count) --count;
        reasons.erase(found);
    }
    char const* Reason(Guid const& key) const
    {
        const auto found = reasons.find(key);
        return found == reasons.end() ? nullptr : found->second.c_str();
    }
    unsigned Unique() const { return static_cast<unsigned>(reasons.size()); }
    unsigned Attempts() const { return attempts; }
    unsigned Count(char const* reason) const
    {
        const auto found = counts.find(reason);
        return found == counts.end() ? 0 : found->second;
    }
    void Top(char const*& reason, unsigned& count) const
    {
        reason = "-";
        count = 0;
        for (auto const& entry : counts)
            if (entry.second > count) { reason = entry.first.c_str(); count = entry.second; }
    }
    void Clear() { reasons.clear(); counts.clear(); attempts = 0; }
};

// ---- the periodic `npc-own:` line -----------------------------------------------------------------------------------
struct OwnStats {
    unsigned hostStreamed = 0, nativeOwned = 0, confirmed = 0, visualOnly = 0, waiting = 0;
    unsigned refused = 0, frozen = 0;
    unsigned hitsApplied = 0, deaths = 0, handBacks = 0, yields = 0, takeovers = 0, reclaims = 0;
    unsigned registrationAlready = 0, registrationQueued = 0, registrationEnabled = 0, registrationFailed = 0;
    char const* refusedTop = "-"; unsigned refusedTopCount = 0;
    char const* waitingTop = "-"; unsigned waitingTopCount = 0;
    char const* frozenTop = "-"; unsigned frozenTopCount = 0;
    char const* visualTop = "-"; unsigned visualTopCount = 0;
};

inline int FormatSummary(char* out, size_t capacity, OwnStats const& s)
{
    return std::snprintf(out, capacity,
        "npc-own: summary host_streamed=%u native_owned=%u confirmed=%u visual_only=%u(top=%s x%u) waiting=%u(top=%s x%u) "
        "refused=%u(top=%s x%u) frozen=%u(top=%s x%u) hits_applied=%u deaths=%u hand_backs=%u yields=%u takeovers=%u reclaims=%u "
        "registration=already:%u,queued:%u,enabled:%u,failed:%u",
        s.hostStreamed, s.nativeOwned, s.confirmed, s.visualOnly, s.visualTop, s.visualTopCount,
        s.waiting, s.waitingTop, s.waitingTopCount, s.refused, s.refusedTop, s.refusedTopCount,
        s.frozen, s.frozenTop, s.frozenTopCount, s.hitsApplied, s.deaths, s.handBacks, s.yields, s.takeovers, s.reclaims,
        s.registrationAlready, s.registrationQueued, s.registrationEnabled, s.registrationFailed);
}

// Events written once per state transition per GUID.
enum class Event : uint8_t {
    Loaded, Owned, Confirmed, Waiting, Refused, Hit, Reaction, Death, Frozen, Yielded, HandBack, Reclaimed, HostSilent,
    Registration
};
inline char const* Name(Event event)
{
    switch (event) {
    case Event::Loaded: return "loaded";
    case Event::Owned: return "owned";
    case Event::Confirmed: return "confirmed";
    case Event::Waiting: return "waiting";
    case Event::Refused: return "refused";
    case Event::Hit: return "hit";
    case Event::Reaction: return "reaction";
    case Event::Death: return "death";
    case Event::Frozen: return "frozen";
    case Event::Yielded: return "yielded";
    case Event::HandBack: return "handback";
    case Event::Reclaimed: return "reclaimed";
    case Event::HostSilent: return "host-silent";
    default: return "registration";
    }
}

// Remembers the last event per GUID so a transition is logged once, never per frame.
class TransitionLog {
    std::unordered_map<Guid, uint8_t, GuidHash> last;
public:
    bool Changed(Guid const& key, Event event)
    {
        const uint8_t value = static_cast<uint8_t>(event) + 1;
        auto found = last.find(key);
        if (found != last.end() && found->second == value) return false;
        last[key] = value;
        return true;
    }
    void Forget(Guid const& key) { last.erase(key); }
    void Clear() { last.clear(); }
};

inline int FormatEvent(char* out, size_t capacity, char const* guid, char const* name, Event event, char const* detail)
{
    return std::snprintf(out, capacity, "npc-own: event=%s guid=%s name=%s %s", Name(event), guid, name, detail);
}

}
