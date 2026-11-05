// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free checks of the production snapshot sampler and scoped mirror-perception policy.
#include "snapshot.h"
#include "qa_dispatch.h"
#include "ai.h"
#include "replication.h"
#include "processing.h"
#include "contact.h"
#include "guid_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

class eCEntity {};

namespace {
int failures = 0;
void Check(bool condition, char const* description)
{
    if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}

int noArgumentCalls = 0;
int withArgumentCalls = 0;
int firstDuplicateCalls = 0;
int secondDuplicateCalls = 0;
char receivedArgument[32] = {};

void NoArgument(g3t::qa::QaLogFn)
{
    ++noArgumentCalls;
}

void WithArgument(char const* argument, g3t::qa::QaLogFn)
{
    ++withArgumentCalls;
    std::snprintf(receivedArgument, sizeof(receivedArgument), "%s", argument);
}

void FirstDuplicate(g3t::qa::QaLogFn)
{
    ++firstDuplicateCalls;
}

void SecondDuplicate(g3t::qa::QaLogFn)
{
    ++secondDuplicateCalls;
}

void IgnoreLog(char const*) {}

void QaDispatchRouting()
{
    using g3t::qa::QaTypedRoute;
    const QaTypedRoute routes[] = {
        {"noarg", NoArgument, nullptr},
        {"witharg", nullptr, WithArgument},
        {"duplicate", FirstDuplicate, nullptr},
        {"duplicate", SecondDuplicate, nullptr},
    };

    Check(g3t::qa::DispatchTypedRoute("noarg", "", IgnoreLog, routes),
          "typed dispatch matches a no-argument route");
    Check(noArgumentCalls == 1, "typed no-argument dispatch invokes the first matching handler");
    Check(g3t::qa::DispatchTypedRoute("witharg", "matched", IgnoreLog, routes),
          "typed dispatch matches an argument route");
    Check(withArgumentCalls == 1 && std::strcmp(receivedArgument, "matched") == 0,
          "typed argument dispatch forwards the original argument");
    Check(g3t::qa::DispatchTypedRoute("duplicate", "", IgnoreLog, routes),
          "typed dispatch accepts a duplicate route");
    Check(firstDuplicateCalls == 1 && secondDuplicateCalls == 0,
          "typed dispatch preserves first-match precedence");
    Check(!g3t::qa::DispatchTypedRoute("unknown", "", IgnoreLog, routes),
          "typed dispatch rejects unknown actions");
}

void Feed(g3t::body::Track& track, uint32_t sent, uint32_t arrived, float x, int mode = 4)
{
    const float position[3] = {x, 0.0f, 0.0f}, rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g3t::body::Feed(track, position, rotation, sent, arrived, mode);
}

void LossAndJitter()
{
    using namespace g3t::body;
    struct Event { uint32_t sent, arrived; float x; };
    std::vector<Event> events;
    for (unsigned i = 0; i < 70; ++i) {
        if ((i && i % 13 == 0) || (i >= 25 && i <= 27)) continue;
        const uint32_t sent = 1000 + i * 50;
        events.push_back({sent, sent + 15 + (i % 7) * 4, i * 30.0f});
    }
    std::sort(events.begin(), events.end(), [](Event const& a, Event const& b) { return a.arrived < b.arrived; });
    Track track;
    size_t next = 0;
    float previous = 0.0f, maximumStep = 0.0f;
    bool sampled = false;
    for (uint32_t now = 1015; now < 4400; now += 8) {
        while (next < events.size() && events[next].arrived <= now) {
            auto const& event = events[next++];
            Feed(track, event.sent, event.arrived, event.x);
        }
        float position[3], rotation[4];
        Presentation presentation;
        if (!Sample(track, now, position, rotation, -BufferDelay, true, &presentation)) continue;
        if (sampled) {
            const float step = position[0] - previous;
            Check(step >= -0.001f, "loss/jitter must not rewind a steady path");
            maximumStep = std::max(maximumStep, step);
            Check(step <= 10.0f, "loss/jitter must not teleport a steady replica");
        }
        previous = position[0];
        sampled = true;
    }
    Check(sampled && track.serial == 1, "ordinary loss must not reset the path");
    std::printf("Steady 20 Hz path with jitter/loss: maximum 8 ms step %.2f cm\n", maximumStep);
}

void StopsAndTeleports()
{
    using namespace g3t::body;
    Track track;
    Feed(track, 1000, 1020, 0.0f);
    Feed(track, 1050, 1070, 30.0f, 1);
    float position[3], rotation[4];
    Presentation presentation;
    Sample(track, 1500, position, rotation, -BufferDelay, true, &presentation);
    Check(position[0] == 30.0f && presentation.movementMode == 1 && presentation.speed == 0.0f,
          "a standing snapshot must not extrapolate the last run");
    Feed(track, 1100, 1120, 100000.0f);
    Sample(track, 1120, position, rotation);
    Check(track.count == 1 && track.serial > 1 && position[0] == 100000.0f, "true teleport must reset, not interpolate across the map");
}

void ClockAndWrap()
{
    using namespace g3t::body;
    Track track;
    Feed(track, 1000, 1080, 0.0f);
    Feed(track, 1050, 1130, 30.0f);
    Feed(track, 1100, 1180, 60.0f);
    float position[3], rotation[4];
    Sample(track, 1200, position, rotation);
    const uint32_t previous = track.renderSent;
    Feed(track, 1150, 1160, 90.0f);
    Sample(track, 1208, position, rotation);
    Check(track.renderSent - previous <= 9, "a faster path must slew the clock instead of jumping it");
    const uint32_t afterFast = track.renderSent;
    track.offset += 70;
    Sample(track, 1216, position, rotation);
    Check(static_cast<int32_t>(track.renderSent - afterFast) >= 0, "clock adjustment must not rewind the render cursor");
    Track wrapped;
    Feed(wrapped, 0xffffffe0u, 10, 0.0f);
    Feed(wrapped, 20, 62, 31.2f);
    Sample(wrapped, 72, position, rotation, -20);
    Check(position[0] > 0.0f && position[0] < 31.2f, "interpolation must survive 32-bit timer wrap");
    const int count = wrapped.count;
    Feed(wrapped, 10, 70, 9000.0f);
    Check(wrapped.count == count, "out-of-order snapshots must be discarded");
}

void AdaptivePlayback()
{
    using namespace g3t::body;
    Track stable, fast, jittered, fastest;
    for (uint32_t i = 0; i < 100; ++i) {
        Feed(stable, 1000 + i * 50, 1025 + i * 50, i * 30.0f);
        Feed(fast, 1000 + i * 33, 1025 + i * 33, i * 20.0f);
        Feed(jittered, 1000 + i * 50, 1025 + i * 50 + (i % 2) * 40, i * 30.0f);
    }
    for (uint32_t i = 0; i < 300; ++i) Feed(fastest, 1000 + i * 16, 1025 + i * 16, i * 10.0f);
    Check(PlaybackDelay(stable) == 100, "stable 20 Hz retains two intervals of buffering");
    Check(PlaybackDelay(fast) >= 66 && PlaybackDelay(fast) <= 75, "stable 30 Hz can safely reduce presentation delay");
    Check(PlaybackDelay(fastest) == 50, "stable 60 Hz halves the presentation delay, never below its floor");
    Check(PlaybackDelay(jittered) > PlaybackDelay(stable) && PlaybackDelay(jittered) <= 250,
          "arrival jitter increases bounded buffering rather than changing the clock estimate");
    Track cursor;
    Feed(cursor, 1000, 1020, 0);
    Feed(cursor, 1050, 1070, 30);
    float position[3], rotation[4];
    Sample(cursor, 1120, position, rotation, -100);
    uint32_t previous = cursor.renderSent;
    Sample(cursor, 1128, position, rotation, -200);
    Check(static_cast<int32_t>(cursor.renderSent - previous) >= 0 && cursor.renderSent - previous <= 9,
          "increasing the display buffer must not rewind the presentation cursor");
    previous = cursor.renderSent;
    Sample(cursor, 1136, position, rotation, -66);
    Check(cursor.renderSent - previous <= 9, "reducing the display buffer must slew rather than jump forward");
    for (uint32_t now = 1200; now < 2200; now += 8)
        Sample(cursor, now, position, rotation, -PlaybackDelay(cursor));
    Check(PlaybackDelay(cursor) > 100 && PlaybackDelay(cursor) <= 250,
          "extended underruns increase protection within a fixed upper bound");
    Check(IsLocomotionMotion("Hero_Stand_None_1H_P0_Move_Walk_N_Fwd") &&
          IsLocomotionMotion("Scavenger_Stand_None_Fist_P0_Move_Run_N_Fwd"), "walking and running are actual locomotion clips");
    Check(!IsLocomotionMotion("Scavenger_Stand_None_Fist_P0_Move_Stand_N_Fwd") &&
          !IsLocomotionMotion("Hero_Stand_None_1H_P0_Attack_Hit_N_Fwd"),
          "a Move_Stand idle and an attack must never be treated as a walking cycle");
}

void DamageContext()
{
    using namespace g3t::ai;
    eCEntity first, second;
    Check(PerceptionBlocked(&first, true), "mirrors remain pacified outside damage judgement");
    {
        DamagePerception outer(&first);
        Check(!PerceptionBlocked(&first, true) && PerceptionBlocked(&second, true),
              "only the legitimate hit receiver may receive damage perception");
        {
            DamagePerception inner(&second);
            Check(PerceptionBlocked(&first, true) && !PerceptionBlocked(&second, true), "nested damage context isolates receivers");
        }

        Check(!PerceptionBlocked(&first, true), "nested context restores the first receiver");
        Check(!PerceptionBlocked(&second, false), "ordinary NPC perception remains unchanged");
    }
    Check(PerceptionBlocked(&first, true) && PerceptionBlocked(&second, true), "damage allowance must not leak into NPC AI");
}

void PresentationTimeline()
{
    using namespace g3t::body;
    Track track;
    const float rotation[4] = {0, 0, 0, 1}, first[3] = {0, 0, 0}, second[3] = {30, 0, 0};
    MotionState attack;
    attack.motion = 11;
    attack.actionSeq = 3;
    attack.motionFrac = 12000;
    attack.heldRight = 5;
    MotionState block;
    block.motion = 12;
    block.actionSeq = 4;
    block.flags = 64;
    block.heldRight = 6;
    Feed(track, first, rotation, 1000, 1020, 4, attack);
    Feed(track, second, rotation, 1050, 1070, 4, block);
    float position[3], turn[4];
    Presentation shown;
    Sample(track, 1145, position, turn, -BufferDelay, true, &shown);
    Check(position[0] == 15 && shown.sent == 1025 && shown.stateSent == 1000,
          "combat timestamps must identify the pose actually rendered, not the newest packet");
    Check(shown.motion.motion == 11 && shown.motion.actionSeq == 3 && shown.motion.heldRight == 5 && !shown.motion.flags,
          "an interpolated attack must not show a future block or future weapon");
    Sample(track, 1180, position, turn, -BufferDelay, true, &shown);
    Check(shown.motion.motion == 12 && shown.motion.flags == 64, "block metadata changes only when its timeline is reached");
    Sample(track, 1400, position, turn, -BufferDelay, true, &shown);
    Check(shown.speed == 0 && position[0] == 90, "an exhausted extrapolation must hold without continued steering");
    block.flags = 0;
    Feed(track, second, rotation, 1050, 1420, 1, block);
    Check(track.count == 2 && track.snaps[1].motion.flags == 0, "a same-timestamp final state updates metadata without extending history");
}

void PublicationOrdering()
{
    using namespace g3t::replication;
    Sequence origin;
    Check(origin.Accept(0xfffffffeu) && origin.Accept(1) && !origin.Accept(0xffffffffu),
          "sender sequence ordering must survive uint32 wrap and reject late packets");
    ReplicaOrder state;
    Check(state.Snapshot(2, 10) && state.Release(2, 11), "the current owner can release its published state");
    Check(!state.Snapshot(2, 10) && state.released, "a late snapshot must not undo release");
    Check(state.Snapshot(3, 12) && !state.Release(2, 13) && state.owner == 3 && !state.released,
          "a previous owner's release cannot release its successor");
    Check(!state.Snapshot(2, 11) && state.owner == 3, "an old owner publication cannot overwrite a handoff");
    Check(state.Snapshot(3, 14), "the current owner continues after an irrelevant old-owner release");
}

void NpcOnlyProcessing()
{
    using namespace g3t::processing;
    const Sphere ranges[] = {{{10000, 0, 0}, 1000}, {{20000, 0, 0}, 1000}};
    const float box[6] = {19900, -10, -10, 20100, 10, 10};
    Check(!IncludesNpc(false, false, box, ranges, 2), "extra player ranges must not admit static or other non-NPC geometry");
    Check(IncludesNpc(true, false, box, nullptr, 0), "native non-NPC processing decisions must remain unchanged");
    Check(IncludesNpc(false, true, box, ranges, 2), "an NPC near another player must still enter extended processing");
    Check(!IncludesNpc(false, true, box, ranges, 1), "an NPC outside every extra range must retain native exclusion");
    Check(!IncludesNpc(false, true, box, nullptr, 0), "removing all extra ranges must release an otherwise out-of-range NPC");
}

void HostNpcTimeline()
{
    using namespace g3t::body;
    Track track;
    track.predictive = false;
    const float at[3] = {0, 0, 0}, next[3] = {30, 0, 0}, rotation[4] = {0, 0, 0, 1};
    MotionState first, second;
    first.motions[0].active = second.motions[0].active = 3;
    first.motions[0].name = second.motions[0].name = 7;
    first.motions[0].instance = second.motions[0].instance = 23;
    first.motions[0].phase = 0.2f; second.motions[0].phase = 0.6f;
    first.hitPoints = 100; second.hitPoints = 70;
    first.hitPointsMax = 200; second.hitPointsMax = 150;
    first.action = 1; second.action = 2;
    first.attackReason = 17; second.attackReason = 20;
    first.damageType = 1; second.damageType = 4;
    first.damageAmount = 30; second.damageAmount = 80;
    first.damageMultiplier = 1.0f; second.damageMultiplier = 2.5f;
    first.contact = 1;
    Feed(track, at, rotation, 1000, 1020, 4, first);
    Feed(track, next, rotation, 1050, 1070, 4, second);
    float position[3], turn[4];
    Presentation shown;
    Sample(track, 1145, position, turn, -100, true, &shown);
    Check(position[0] == 15 && std::fabs(shown.motion.motions[0].phase - 0.4f) < 0.00001f,
          "NPC pose and animation phase interpolate on the same host time");
    Check(shown.motion.motions[0].active == 3, "native null-descriptor playback survives NPC interpolation");
    Check(shown.motion.hitPoints == 100 && shown.motion.hitPointsMax == 200 && shown.motion.contact == 1,
          "NPC discrete health and maximum health must not leak from a future sample");
    Check(shown.motion.action == 1 && shown.motion.attackReason == 17 && shown.motion.damageType == 1 &&
          shown.motion.damageAmount == 30 && shown.motion.damageMultiplier == 1.0f,
          "NPC contact judgement uses the displayed host action, attack reason and weapon damage");
    Sample(track, 1700, position, turn, -100, true, &shown);
    Check(position[0] == 30 && shown.sent == 1050 && shown.motion.motions[0].phase == 0.6f,
          "an NPC never extrapolates position or animation through a missing packet");
    Check(shown.motion.action == 2 && shown.motion.attackReason == 20 && shown.motion.damageAmount == 80 &&
          shown.motion.damageMultiplier == 2.5f && shown.motion.hitPointsMax == 150,
          "held NPC contact and health-bar data stay aligned with the final host sample");
    Feed(track, next, rotation, 4000, 4020, 4, second);
    Check(!track.predictive, "an NPC timeline reset must not re-enable player prediction");
}

void CanonicalBonePoses()
{
    using namespace g3t::pose;
    Transform first;
    first.name = 17;
    for (int i = 0; i < 12; ++i) first.values[i] = i < 9 ? 0.123456f * (i - 4) : 73.1234f * (i - 10);
    Bone a;
    Check(Encode(first, a), "a complete affine bone pose can be encoded");
    const auto decoded = Decode(a);
    for (int i = 0; i < 12; ++i)
        Check(std::fabs(decoded.values[i] - first.values[i]) <= (i < 9 ? 0.000123f : 0.126f),
              "bone quantization preserves matrix shear and sub-centimetre translation");
    auto invalid = first;
    invalid.values[9] = 9000.0f;
    Check(!Encode(invalid, a), "an unrepresentable bone translation is refused rather than silently clamped");
    Check(Encode(first, a), "a refused bone leaves subsequent valid captures usable");
    Transform second = first;
    second.values[9] += 20;
    Bone b;
    Check(Encode(second, b), "the second canonical pose can be encoded");
    History history;
    Check(history.Push(1, 1000, &a, 1) && history.Push(1, 1020, &b, 1), "ordered canonical poses are retained");
    std::vector<Transform> sampled;
    Check(history.Sample(1, 1010, sampled) && sampled.size() == 1 &&
          std::fabs(sampled[0].values[9] - decoded.values[9] - 10) < 0.001f,
          "bones interpolate at the same host timestamp as the NPC root and clips");
    Check(!history.Sample(1, 1030, sampled) && !history.Sample(1, 990, sampled),
          "bone replay never extrapolates or substitutes a future pose");
    Check(!history.Push(1, 1010, &a, 1), "out-of-order bone poses cannot overwrite newer host state");
    Check(history.Push(2, 1040, &b, 1) && !history.Sample(1, 1020, sampled) &&
          !history.Push(1, 1060, &a, 1), "NPC incarnation fences also protect final skeletal poses");
    Bone changed = b;
    changed.name = 18;
    Check(history.Push(2, 1060, &changed, 1) && history.Sample(2, 1050, sampled) &&
          sampled.size() == 1 && sampled[0].name == b.name &&
          history.Sample(2, 1060, sampled) && sampled[0].name == changed.name,
          "a skeleton change retains the buffered old pose and switches atomically at its host timestamp");
    uint8_t flags = 0;
    Check(history.Push(2, 1080, &changed, 1, Facial) && history.Sample(2, 1070, sampled, &flags) && flags == 0 &&
          history.Sample(2, 1080, sampled, &flags) && flags == Facial,
          "facial-rig changes do not discard the presentation buffer or use a future rig");
    Bone expanded[] = {changed, changed};
    expanded[1].name = 19;
    Check(history.Push(2, 1100, expanded, 2, Facial) && history.Sample(2, 1090, sampled) && sampled.size() == 1 &&
          history.Sample(2, 1100, sampled) && sampled.size() == 2,
          "bone-count transitions hold a complete old skeleton instead of mixing incompatible arrays");
    Check(history.Sample(2, 1040, sampled) && sampled[0].name == b.name && !history.Sample(2, 1101, sampled),
          "schema transitions retain valid history but never permit extrapolated future poses");
    History wrapped;
    Check(wrapped.Push(1, 0xfffffff0u, &a, 1) && wrapped.Push(1, 0x10u, &b, 1) &&
          wrapped.Sample(1, 0, sampled) && std::fabs(sampled[0].values[9] - decoded.values[9] - 10) < 0.001f,
          "canonical pose timelines survive the host clock wrapping");
}

void TerminalCorpseLife()
{
    g3t::replication::TerminalLife life;
    Check(life.Observe(1, false) && life.Observe(1, true), "an NPC can enter death once in its current incarnation");
    Check(!life.Observe(1, false) && life.dead && life.Observe(1, true),
          "a rewound alive sample cannot reset or restart the same corpse");
    Check(life.Observe(2, false) && !life.dead && !life.Observe(1, true),
          "only a newer incarnation may revive a terminal NPC");
    Check(life.Observe(2, true) && !life.Observe(2, false), "second-life deaths also remain terminal");
}

void HostCadence()
{
    g3t::replication::Cadence60 clock;
    unsigned samples = 0;
    for (uint32_t now = 0; now < 1000; ++now) samples += clock.Due(now);
    Check(samples == 60 && clock.missed == 0, "the NPC publisher schedules exactly 60 samples per second");
    g3t::replication::Cadence60 slow;
    samples = 0;
    for (uint32_t now = 0; now < 1000; now += 50) samples += slow.Due(now);
    Check(samples == 20 && slow.missed > 0, "low engine FPS reports missed captures instead of inventing stale samples");
    g3t::replication::Cadence60 wrapped;
    samples = 0;
    for (uint32_t elapsed = 0; elapsed < 1000; ++elapsed) samples += wrapped.Due(0xfffffff0u + elapsed);
    Check(samples == 60, "the fixed NPC cadence survives the engine clock wrapping");
}

void VisibleContactGeometry()
{
    using g3t::contact::SegmentDistanceSquared;
    const float low[3] = {0, 20, 0}, high[3] = {0, 180, 0};
    const float left[3] = {-100, 100, 0}, right[3] = {100, 100, 0};
    const float away[3] = {1000, 100, 0};
    Check(SegmentDistanceSquared(left, right, low, high) < 0.001f, "a displayed blade sweep crosses the hero's body");
    Check(SegmentDistanceSquared(away, away, low, high) > 900000, "a distant visible swing cannot create a contact");
    Check(SegmentDistanceSquared(low, low, low, low) == 0, "degenerate contact segments remain finite");
}

void GuidCodec()
{
    char const* canonical = "12345678-9ABC-DEF0-1234-56789ABCDEF0";
    char text[48] = {};
    uint8_t bytes[16] = {};
    uint8_t expected[16] = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                            0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    Check(g3t::guid::ParseBytes(canonical, bytes), "permissive canonical GUID is accepted");
    Check(std::memcmp(bytes, expected, 16) == 0, "canonical GUID text produces exact 16-byte fields");
    g3t::guid::FormatBytes(bytes, text, sizeof(text));
    Check(std::strcmp(text, canonical) == 0, "16-byte GUID fields format exactly in canonical text");

    Check(g3t::guid::ParseBytes("12345678-9abc-def0-1234-56789abcdef0-suffix", bytes),
          "permissive GUID parsing retains suffix handling");
    Check(g3t::guid::ParseBytes("12345678-9ABC-DEF0-1234-56789ABCDEF", bytes),
          "permissive GUID parsing retains 35-character one-hex-tail acceptance");
    Check(!g3t::guid::ParseBytes("12345678/9ABC-DEF0-1234-56789ABCDEF0", bytes),
          "malformed GUID separators are rejected");
    Check(!g3t::guid::ParseBytes(nullptr, bytes), "null GUID text is rejected");

    Check(g3t::guid::ParseBytesExact(canonical, bytes), "exact GUID text accepts a canonical 36 characters");
    Check(!g3t::guid::ParseBytesExact("12345678-9ABC-DEF0-1234-56789ABCDEF0-extra", bytes),
          "exact GUID text rejects suffix text");
    Check(!g3t::guid::ParseBytesExact("12345678-9ABC-DEF0-1234-56789ABCDEF", bytes),
          "exact GUID text rejects a 35-character input");
    Check(!g3t::guid::ParseBytesExact(nullptr, bytes), "exact GUID text rejects null input");

    Check(g3t::guid::ParseBytes("00000000-0000-0000-0000-000000000000", bytes), "zero GUID remains accepted");
    g3t::guid::FormatBytes(bytes, text, sizeof(text));
    Check(std::strcmp(text, "00000000-0000-0000-0000-000000000000") == 0, "zero GUID formats exactly");
}
}

int main()
{
    QaDispatchRouting();
    LossAndJitter();
    StopsAndTeleports();
    ClockAndWrap();
    AdaptivePlayback();
    DamageContext();
    PresentationTimeline();
    PublicationOrdering();
    NpcOnlyProcessing();
    HostNpcTimeline();
    CanonicalBonePoses();
    TerminalCorpseLife();
    HostCadence();
    VisibleContactGeometry();
    GuidCodec();
    std::printf("RESULT: %s (%d failures; no game or Steam loaded)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
