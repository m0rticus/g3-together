// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include "combat_timing.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace g3t::npcsend {
// Distances are centimetres; the two bands prevent threshold chatter.
constexpr float NearEnterCm = 2000.0f, NearExitCm = 2500.0f;
constexpr float InterestEnterCm = 8000.0f, InterestExitCm = 10000.0f;
constexpr float ContactSafetyCm = 300.0f; // cm; conservative fallback when remote focus is missing/stale
// Host movement detection, per capture; hold motion classification after the last movement.
constexpr float MovementPositionCm = 0.02f; // cm displacement
constexpr float MovementQuaternionComponent = 0.00025f; // quaternion component difference
constexpr uint32_t MovementHoldMs = 500; // ms
// Integer-millisecond intervals: 16/33 ms are at least 60/30 Hz at a sufficiently fast scan.
// User-selected 30 Hz ordinary locomotion; full capture rate is reserved for
// interaction/contact. Distant/idle animation is 10 Hz, interpolated on mirrors.
constexpr uint32_t FastIntervalMs = 16, OrdinaryIntervalMs = 33, MovingFarIntervalMs = 100, IdleIntervalMs = 100;
// Exit holds let the monotonic display clock slew up to the slow tier's
// 200 ms buffer before captures become sparse, including movement-to-idle.
constexpr uint32_t UrgentHoldMs = 2000, OrdinaryHoldMs = 2000, InterestWarmupMs = 200;
constexpr uint32_t KeyframeMs = 1000;
// A due update (not a quiet heartbeat) that leaves later than this counts as overdue in the diagnostics; one frame hitch
// of ~60 ms fits every tier. Multiplied by the congestion stretch like every interval.
constexpr uint32_t FastMaxStaleMs = 100, OrdinaryMaxStaleMs = 150, SlowMaxStaleMs = 300;
// Join burst: new recipients receive their first (key) update per NPC at this paced rate, spread over about a second
// for a 75-NPC crowd, instead of one reliable+key burst in a single tick. Established NPCs are never affected.
constexpr uint32_t InitialPerSecond = 60, InitialBurst = 6, InitialBurstQuietMs = 250;
constexpr uint32_t QuietCadenceMs = 250; // ms; intentional quiet heartbeat is not playback jitter
constexpr uint32_t MirrorStaleMs = 10000; // ms without a snapshot before a mirror is stale
constexpr uint32_t PoseRetentionMs = 10000; // ms to retain received pose history
constexpr float RootChangeCm = 0.5f;
constexpr double RotationChangeDegrees = 0.499; // degrees; margin for float quaternion quantization at 0.5
constexpr double RotationLengthChange = 0.0001;
constexpr int BoneBasisChange = 8;        // 8 / 4096 matrix units, stricter than a 0.5-degree rotation
constexpr int BoneTranslationChange = 1;  // 1 / 4 cm (the pose wire's smallest translation step)
constexpr float PhaseChange = 0.0009f;    // normalized duration (margin below a meaningful 0.001)
constexpr float WeightChange = 0.0009f;   // rendered blend weight (same float-rounding margin)
constexpr float PhaseDiscontinuity = 0.25f; // normalized motion duration
constexpr int BoneBasisDiscontinuity = 512; // 512 / 4096 matrix units
constexpr int BoneTranslationDiscontinuity = 40; // 10 cm
constexpr float RootDiscontinuityCm = 100.0f;
constexpr double RotationDiscontinuityDegrees = 45.0;

enum class Interest { Distance, Include, Exclude };
enum class Tier { Fast, Ordinary, MovingFar, Idle };
inline uint32_t Interval(Tier tier)
{
    return tier == Tier::Fast ? FastIntervalMs : tier == Tier::Ordinary ? OrdinaryIntervalMs :
           tier == Tier::MovingFar ? MovingFarIntervalMs : IdleIntervalMs;
}
// The congestion stretch (net::PeerBudget::NpcStretchPermille) is 1000 under normal conditions and is applied
// identically to every NPC and tier.
constexpr uint32_t StretchNormal = 1000, StretchMax = 4000;
inline uint32_t Stretched(uint32_t ms, uint32_t stretchPermille)
{
    const uint32_t stretch = stretchPermille < StretchNormal ? StretchNormal :
                             stretchPermille > StretchMax ? StretchMax : stretchPermille;
    return static_cast<uint32_t>((uint64_t(ms) * stretch + 999) / 1000);
}
inline uint32_t MaxStale(Tier tier, uint32_t stretchPermille = StretchNormal)
{
    return Stretched(tier == Tier::Fast ? FastMaxStaleMs : tier == Tier::Ordinary ? OrdinaryMaxStaleMs : SlowMaxStaleMs,
                     stretchPermille);
}
enum class Decision { Ineligible, Invalid, Outside, Wait, Initial, Semantic, Changed, Keyframe, FinalAndRelease };

struct Input {
    net::NpcRecord const* record = nullptr;
    pose::Bone const* bones = nullptr;
    int boneCount = 0; // 0 means no pose; positive counts require a valid pointer
    uint8_t poseFlags = 0;
    uint32_t now = 0;  // monotonic milliseconds modulo 2^32
    float distanceCm = 0.0f;
    Interest interest = Interest::Distance;
    bool eligible = true; // connection/world/ownership gate, not an interest-range decision
    bool nearby = false, focused = false, combat = false, contact = false, moving = false;
    bool force = false; // one-shot caller-known discrete event or pose discontinuity
    uint32_t stretchPermille = StretchNormal; // emergency congestion factor for every interval, 1000 = none
};

// Keep exactly one State per (recipient generation, NPC GUID). Reset externally on
// rejoin/world/generation changes. The snapshot is the last successfully sent one.
struct State {
    bool live = false, nearTier = false;
    uint8_t urgent = 0; // independently acknowledged focus/combat/contact edges
    uint32_t sentAt = 0;
    // Ideal send time of the last update. Advancing it by whole intervals (not resetting to the frame time) carries the
    // phase, so a 33 ms tier on 22 ms frames averages 30 Hz instead of 22.7 Hz.
    uint32_t idealAt = 0;
    uint32_t enteredAt = 0, urgentUntil = 0, ordinaryUntil = 0;
    bool haveUrgentHold = false, haveOrdinaryHold = false;
    net::NpcRecord record{};
    uint8_t poseFlags = 0;
    std::vector<pose::Bone> bones;
};

struct Result {
    Decision decision = Decision::Invalid;
    Tier tier = Tier::Idle;
    bool essential = false; // queue the state reliably; computed against this recipient's last successful send
};

// Keep reliable transitions independent of global/last-captured bookkeeping.
// The caller must compare against its own last successfully queued state.
inline bool EssentialChanged(net::NpcRecord const& sent, net::NpcRecord const& current)
{
    const bool incomingMotion = combat_timing::RemoteCombat(current.flags, current.combatTarget) &&
        current.motions[0].active &&
        (sent.motions[0].instance != current.motions[0].instance ||
         sent.motions[0].name != current.motions[0].name || !sent.motions[0].active);
    return incomingMotion ||
        ((sent.flags ^ current.flags) & 0xBFu) != 0 || // life, combat and player opponent (parry is a motion edge)
        sent.aiMode != current.aiMode ||
        sent.incarnation != current.incarnation ||
        sent.hitPoints != current.hitPoints || sent.hitPointsMax != current.hitPointsMax ||
        sent.partyPlayer != current.partyPlayer || sent.partyType != current.partyType ||
        sent.waiting != current.waiting || sent.guidePlayer != current.guidePlayer ||
        sent.hitPlayer != current.hitPlayer || sent.hitSequence != current.hitSequence ||
        sent.contact != current.contact || sent.attitude != current.attitude ||
        sent.attackReason != current.attackReason ||
        sent.heldRight != current.heldRight || sent.heldLeft != current.heldLeft ||
        sent.combatTarget != current.combatTarget;
}

inline bool ImmediateInitial(Input const& in)
{
    return in.record && (in.distanceCm <= NearExitCm || in.focused || in.combat || in.contact ||
        in.record->contact || in.record->partyPlayer || in.record->guidePlayer);
}

namespace detail {
inline bool Valid(Input const& in)
{
    if (!in.record || !std::isfinite(in.distanceCm) || in.distanceCm < 0 ||
        (in.interest != Interest::Distance && in.interest != Interest::Include && in.interest != Interest::Exclude) ||
        in.boneCount < 0 || in.boneCount > pose::MaxBones ||
        (in.boneCount && !in.bones) || (in.poseFlags & ~pose::Facial) ||
        (!in.boneCount && in.poseFlags)) return false;
    auto const& r = *in.record;
    if (!r.incarnation || r.hitPlayer >= net::MaxPlayers || r.guidePlayer >= net::MaxPlayers ||
        r.partyPlayer >= net::MaxPlayers || r.partyType > 8 || r.waiting > 1 || r.contact > 1 ||
        r.attitude > 8 || r.aiMode > 11 || r.attackReason > 21 || r.damageType > 7 ||
        r.damageAmount < -1000000 || r.damageAmount > 1000000 ||
        r.hitPoints < -1000000 || r.hitPoints > 1000000 || r.hitPointsMax < 1 || r.hitPointsMax > 1000000 ||
        !std::isfinite(r.damageMultiplier) || std::fabs(r.damageMultiplier) > 1000.0f) return false;
    for (float v : r.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    double length = 0;
    for (float v : r.rotation) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
        length += double(v) * v;
    }
    if (length <= 0.01) return false;
    for (auto const& m : r.motions) {
        if ((m.active & ~3u) || (m.active && !(m.active & 1u)) ||
            (m.active && (!m.name || !m.instance)) ||
            !std::isfinite(m.phase) || m.phase < 0 || m.phase > 1 ||
            !std::isfinite(m.speed) || std::fabs(m.speed) > 100 ||
            !std::isfinite(m.weight) || m.weight < 0 || m.weight > 10 ||
            !std::isfinite(m.intro) || m.intro < 0 || m.intro > 100000 ||
            !std::isfinite(m.extro) || m.extro < 0 || m.extro > 100000) return false;
    }
    constexpr unsigned NameSlots = pose::MaxBones * 2;
    static_assert((NameSlots & (NameSlots - 1)) == 0, "bone name table must have power-of-two capacity");
    uint32_t names[NameSlots] = {};
    for (int i = 0; i < in.boneCount; ++i) {
        const uint32_t name = in.bones[i].name;
        if (!name) return false;
        unsigned slot = (name * 2654435761u) & (NameSlots - 1);
        while (names[slot] && names[slot] != name) slot = (slot + 1) & (NameSlots - 1);
        if (names[slot]) return false;
        names[slot] = name;
    }
    return true;
}

inline bool ExactFloat(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

inline uint8_t Urgency(Input const& in)
{
    return static_cast<uint8_t>((in.focused ? 1 : 0) | (in.combat ? 2 : 0) |
                                ((in.contact || in.record->contact) ? 4 : 0));
}

inline bool SemanticRecord(net::NpcRecord const& a, net::NpcRecord const& b)
{
    if (std::memcmp(a.guid, b.guid, sizeof(a.guid)) || a.movementMode != b.movementMode ||
        a.flags != b.flags || a.hitPoints != b.hitPoints || a.hitPointsMax != b.hitPointsMax ||
        a.motion != b.motion || a.aniState != b.aniState || a.heldRight != b.heldRight ||
        a.heldLeft != b.heldLeft || a.partyPlayer != b.partyPlayer || a.partyType != b.partyType ||
        a.waiting != b.waiting || a.guidePlayer != b.guidePlayer || a.aiMode != b.aiMode ||
        a.action != b.action || a.attackReason != b.attackReason || a.damageType != b.damageType ||
        a.damageAmount != b.damageAmount || !ExactFloat(a.damageMultiplier, b.damageMultiplier) ||
        a.incarnation != b.incarnation || a.hitPlayer != b.hitPlayer || a.hitSequence != b.hitSequence ||
        a.contact != b.contact || a.attitude != b.attitude || a.combatTarget != b.combatTarget) return true;
    for (int i = 0; i < net::NpcMotionSlots; ++i) {
        auto const& x = a.motions[i];
        auto const& y = b.motions[i];
        if (x.name != y.name || x.instance != y.instance || x.active != y.active || x.blend != y.blend ||
            !ExactFloat(x.speed, y.speed) || !ExactFloat(x.intro, y.intro) || !ExactFloat(x.extro, y.extro) ||
            std::memcmp(x.options, y.options, sizeof(x.options)) ||
            (x.weight == 0) != (y.weight == 0) || std::fabs(x.phase - y.phase) >= PhaseDiscontinuity) return true;
    }
    return false;
}

inline double DistanceSquared(float const* a, float const* b)
{
    double sum = 0;
    for (int i = 0; i < 3; ++i) { const double d = double(a[i]) - b[i]; sum += d * d; }
    return sum;
}

inline bool RotationChanged(float const* a, float const* b, double degrees)
{
    double dot = 0, aa = 0, bb = 0;
    for (int i = 0; i < 4; ++i) { dot += double(a[i]) * b[i]; aa += double(a[i]) * a[i]; bb += double(b[i]) * b[i]; }
    // abs(dot) makes q and -q equivalent. Normalize in double precision.
    const double cosine = std::fabs(dot) / std::sqrt(aa * bb);
    constexpr double Pi = 3.14159265358979323846;
    return cosine <= std::cos(degrees * Pi / 360.0);
}

inline bool RotationLengthChanged(float const* a, float const* b)
{
    double aa = 0, bb = 0;
    for (int i = 0; i < 4; ++i) { aa += double(a[i]) * a[i]; bb += double(b[i]) * b[i]; }
    return std::fabs(std::sqrt(aa) - std::sqrt(bb)) >= RotationLengthChange;
}

inline bool PoseSemantic(State const& state, Input const& in)
{
    if (state.poseFlags != in.poseFlags || state.bones.size() != static_cast<size_t>(in.boneCount)) return true;
    for (int i = 0; i < in.boneCount; ++i) {
        auto const& a = state.bones[i];
        auto const& b = in.bones[i];
        if (a.name != b.name) return true;
    }

    return false;
}

inline bool MotionEdge(net::NpcRecord const& a, net::NpcRecord const& b)
{
    for (int i = 0; i < net::NpcMotionSlots; ++i) {
        auto const& x = a.motions[i];
        auto const& y = b.motions[i];
        if (x.name != y.name || x.instance != y.instance || x.active != y.active ||
            (x.weight == 0) != (y.weight == 0)) return true;
    }
    return false;
}

inline bool DynamicChanged(State const& state, Input const& in)
{
    auto const& a = state.record;
    auto const& b = *in.record;
    if (DistanceSquared(a.position, b.position) >= double(RootChangeCm) * RootChangeCm ||
        RotationChanged(a.rotation, b.rotation, RotationChangeDegrees) ||
        RotationLengthChanged(a.rotation, b.rotation)) return true;
    for (int i = 0; i < net::NpcMotionSlots; ++i)
        if (std::fabs(a.motions[i].phase - b.motions[i].phase) >= PhaseChange ||
            std::fabs(a.motions[i].weight - b.motions[i].weight) >= WeightChange) return true;
    for (int i = 0; i < in.boneCount; ++i)
        for (int k = 0; k < 12; ++k)
            if (std::abs(int(state.bones[i].values[k]) - int(in.bones[i].values[k])) >=
                (k < 9 ? BoneBasisChange : BoneTranslationChange)) return true;
    return false;
}
}

// Evaluate never changes the sent baseline or clears a live entry. It does track
// the observed near band, even on Wait. Ineligible leaves all state untouched.
inline Result Evaluate(State& state, Input const& in)
{
    if (!in.eligible) return {Decision::Ineligible, Tier::Idle};
    if (!detail::Valid(in)) return {Decision::Invalid, Tier::Idle};
    if (state.live && (std::memcmp(state.record.guid, in.record->guid, sizeof(state.record.guid)) ||
                       uint32_t(in.now - state.sentAt) > 0x7fffffffu))
        return {Decision::Invalid, Tier::Idle}; // wrong key, or time went backwards / more than 2^31 ms
    const bool interested = in.interest == Interest::Include ||
        (in.interest == Interest::Distance && in.distanceCm <= (state.live ? InterestExitCm : InterestEnterCm));
    if (!interested) return {state.live ? Decision::FinalAndRelease : Decision::Outside, Tier::Idle, state.live};
    const bool nearTier = in.distanceCm <= (state.live && state.nearTier ? NearExitCm : NearEnterCm);
    if (state.live) state.nearTier = nearTier;
    const uint8_t urgent = detail::Urgency(in);
    if (state.live) state.urgent &= urgent;
    if (state.live && urgent) { state.urgentUntil = in.now + UrgentHoldMs; state.haveUrgentHold = true; }
    const bool ordinaryWanted = (nearTier || in.nearby) && in.moving;
    if (state.live && ordinaryWanted) { state.ordinaryUntil = in.now + OrdinaryHoldMs; state.haveOrdinaryHold = true; }
    const bool fast = urgent || (state.live && in.now - state.enteredAt < InterestWarmupMs) ||
        (state.haveUrgentHold && static_cast<int32_t>(state.urgentUntil - in.now) > 0);
    const bool ordinary = ordinaryWanted ||
        (state.haveOrdinaryHold && static_cast<int32_t>(state.ordinaryUntil - in.now) > 0);
    const Tier tier = fast ? Tier::Fast : ordinary
        ? Tier::Ordinary : in.moving ? Tier::MovingFar : Tier::Idle;
    if (!state.live) return {Decision::Initial, Tier::Fast, true};
    const bool essential = EssentialChanged(state.record, *in.record);
    if ((urgent & ~state.urgent) || in.force || essential || detail::MotionEdge(state.record, *in.record) || detail::PoseSemantic(state, in) ||
        detail::DistanceSquared(state.record.position, in.record->position) >=
            double(RootDiscontinuityCm) * RootDiscontinuityCm ||
        detail::RotationChanged(state.record.rotation, in.record->rotation, RotationDiscontinuityDegrees))
        return {Decision::Semantic, tier, essential};
    const uint32_t elapsed = in.now - state.sentAt;
    const uint32_t stretch = in.stretchPermille;
    if (elapsed >= Stretched(KeyframeMs, stretch)) return {Decision::Keyframe, tier};
    const uint32_t interval = Stretched(Interval(tier), stretch);
    const bool due = in.now - state.idealAt >= interval;
    if (due && detail::SemanticRecord(state.record, *in.record)) return {Decision::Semantic, tier};
    if (due && detail::DynamicChanged(state, in)) return {Decision::Changed, tier};
    return {Decision::Wait, tier};
}

// Call only after the indicated full snapshot (including any pose) was queued
// to this recipient. FinalAndRelease requires BOTH final snapshot and ordered
// release queued first; failed/partial sends leave the baseline intact for retry.
// The same Input and returned decision must be used; false means no commit.
inline bool Commit(State& state, Input const& in, Decision decision)
{
    if (decision != Decision::Initial && decision != Decision::Semantic && decision != Decision::Changed &&
        decision != Decision::Keyframe && decision != Decision::FinalAndRelease) return false;
    const Result evaluated = Evaluate(state, in);
    if (evaluated.decision != decision) return false;
    if (decision == Decision::FinalAndRelease) { state = State{}; return true; }
    const bool wasLive = state.live;
    const uint32_t interval = Stretched(Interval(evaluated.tier), in.stretchPermille);
    const uint32_t behind = in.now - state.idealAt;
    state.idealAt = wasLive && behind >= interval && behind < 2 * interval ? state.idealAt + interval : in.now;
    state.record = *in.record;
    state.sentAt = in.now;
    state.poseFlags = in.poseFlags;
    if (in.boneCount) state.bones.assign(in.bones, in.bones + in.boneCount);
    else state.bones.clear();
    if (!wasLive) { state.nearTier = in.distanceCm <= NearEnterCm; state.enteredAt = in.now; }
    state.urgent = detail::Urgency(in);
    state.live = true;
    return true;
}

inline uint32_t PresentationInterval(State const& state, Input const& in)
{
    if (detail::Urgency(in)) return FastIntervalMs;
    const bool nearBand = in.distanceCm <= (state.live && state.nearTier ? NearExitCm : NearEnterCm);
    return (nearBand || in.nearby) && in.moving ? OrdinaryIntervalMs :
        in.moving ? MovingFarIntervalMs : IdleIntervalMs;
}

// Achieved-rate diagnostics (local only, never on the wire). One entry per scheduling tier: 0 fast, 1 ordinary, 2 slow.
inline unsigned StatsTier(Tier tier) { return tier == Tier::Fast ? 0u : tier == Tier::Ordinary ? 1u : 2u; }
struct TierStats {
    uint32_t count = 0, overdue = 0, minMs = UINT32_MAX, maxMs = 0;
    uint64_t sumMs = 0;
    double AverageMs() const { return count ? double(sumMs) / count : 0.0; }
};
struct Stats {
    TierStats tiers[3];
    uint32_t edges = 0, heartbeats = 0, initials = 0, paced = 0;

    // elapsed: time since this NPC's previous update to the same recipient. onSchedule: the update left because its phase
    // deadline (State::idealAt + interval) was reached, as opposed to an immediate edge (essential/motion/discontinuity).
    // On-schedule updates feed the per-tier achieved interval (with phase carrying, gaps alternate around the interval,
    // e.g. 22/44 ms for a 33 ms tier on 22 ms frames); edges and quiet heartbeats are counted apart.
    void Record(Decision decision, Tier tier, uint32_t elapsedMs, uint32_t stretchPermille, bool onSchedule)
    {
        if (decision == Decision::Initial) { ++initials; return; }
        if (decision == Decision::Keyframe) { ++heartbeats; return; }
        if (decision != Decision::Changed && decision != Decision::Semantic) return;
        if (!onSchedule) { ++edges; return; }
        TierStats& s = tiers[StatsTier(tier)];
        ++s.count;
        s.sumMs += elapsedMs;
        if (elapsedMs < s.minMs) s.minMs = elapsedMs;
        if (elapsedMs > s.maxMs) s.maxMs = elapsedMs;
        if (elapsedMs > MaxStale(tier, stretchPermille)) ++s.overdue;
    }
};

// Token bucket that spreads the first (key) update of every NPC for one recipient over time after a join or world
// entry. It only ever delays NPCs that have never been sent to this recipient.
struct InitialPacer {
    bool started = false, active = false;
    uint32_t lastAt = 0, burstStartedAt = 0, lastDeferredAt = 0;
    int64_t milliTokens = 0;
    uint32_t admittedInBurst = 0, lastBurstMs = 0, lastBurstNpcs = 0, bursts = 0;

    void Reset() { *this = InitialPacer{}; }

    bool Admit(uint32_t now, uint32_t stretchPermille)
    {
        const int64_t cap = int64_t(InitialBurst) * 1000;
        if (!started) { started = true; lastAt = now; milliTokens = cap; }
        const uint32_t elapsed = now - lastAt;
        if (elapsed && elapsed < 0x7fffffffu) {
            // InitialPerSecond tokens/s = InitialPerSecond milli-tokens per ms, slowed by the congestion stretch.
            milliTokens += int64_t(elapsed) * InitialPerSecond * StretchNormal /
                           (stretchPermille < StretchNormal ? StretchNormal : stretchPermille);
            if (milliTokens > cap) milliTokens = cap;
            lastAt = now;
        }
        if (milliTokens < 1000) {
            if (!active) { active = true; burstStartedAt = now; admittedInBurst = 0; }
            lastDeferredAt = now;
            return false;
        }
        milliTokens -= 1000;
        if (active) ++admittedInBurst;
        return true;
    }

    // Call once per scheduling pass; closes a burst after the deferrals stop.
    void Poll(uint32_t now)
    {
        if (!active || now - lastDeferredAt < InitialBurstQuietMs) return;
        active = false;
        lastBurstMs = lastDeferredAt - burstStartedAt;
        lastBurstNpcs = admittedInBurst;
        ++bursts;
    }
};
}
