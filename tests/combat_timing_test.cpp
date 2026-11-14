#include "../src/combat_timing.h"
#include "../src/contact.h"
#include "../src/npcsend_policy.h"
#include "../src/snapshot.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace g3t;

constexpr uint32_t HostAttackSent = 10000;
constexpr uint32_t HitDurationMs = 400;
constexpr uint32_t PollFrameMs = 16;

body::MotionState IncomingAttack(uint32_t instance, uint32_t name, float phase,
                                 uint8_t flags = 20, uint32_t combatTarget = 0)
{
    body::MotionState motion;
    motion.flags = flags;
    motion.combatTarget = combatTarget;
    motion.motions[0].active = 1;
    motion.motions[0].instance = instance;
    motion.motions[0].name = name;
    motion.motions[0].phase = phase;
    motion.motions[0].speed = 1.0f;
    motion.motions[0].weight = 1.0f;
    return motion;
}

void SetIncomingTrack(body::Track& track)
{
    track.predictive = false;
    track.combatPlayer = 2;
}

float PresentedPhase(body::Presentation const& presentation)
{
    return presentation.motion.motions[0].active ? presentation.motion.motions[0].phase : -1.0f;
}

uint32_t PresentedInstance(body::Presentation const& presentation)
{
    return presentation.motion.motions[0].active ? presentation.motion.motions[0].instance : 0;
}

struct VisibleJudgement
{
    uint32_t instance = 0;
    bool previous = false;
    bool consumed = false;
    int32_t firstVisibleFrame = -1;
    int32_t judgedFrame = -1;
    int32_t seedFrame = -1;
    unsigned acceptedHits = 0;
};

enum class DefenseMode
{
    None,
    BlockAtWindup100,
    BlockJustBeforeHit,
    BlockAfterHit,
    DodgeAtHit,
    RetreatAtWindup100
};

bool TickVisibleJudgement(VisibleJudgement& judgement, uint32_t instance, float phase,
                         int32_t nowFrame, DefenseMode defense, int32_t visibleStart,
                         bool& block, bool& dodge, bool& overlap)
{
    if (judgement.instance != instance) {
        judgement.instance = instance;
        judgement.previous = false;
        judgement.consumed = false;
        judgement.seedFrame = -1;
    }
    if (judgement.firstVisibleFrame < 0)
        judgement.firstVisibleFrame = nowFrame;

    if (defense == DefenseMode::BlockAtWindup100 && visibleStart >= 0 &&
        nowFrame - visibleStart >= 100)
        block = true;
    if (defense == DefenseMode::BlockJustBeforeHit && phase >=
        combat_timing::NativeHitPhase - 0.050f && phase < combat_timing::NativeHitPhase)
        block = true;
    if (defense == DefenseMode::BlockAfterHit && phase >= combat_timing::NativeHitPhase + 0.020f)
        block = false; // deliberately too late: this hit has already been eligible
    if (defense == DefenseMode::DodgeAtHit && combat_timing::HitPhaseReached(phase))
        dodge = true;
    if (defense == DefenseMode::RetreatAtWindup100 && visibleStart >= 0 &&
        nowFrame - visibleStart >= 100)
        overlap = false;

    if (!combat_timing::HitPhaseReached(phase)) {
        // Actual TryLocalBlow clears its sweep history on any pre-hit frame.
        judgement.previous = false;
        judgement.seedFrame = -1;
        return false;
    }
    if (!judgement.previous) {
        // First eligible frame seeds the sweep, exactly like contactAt/old-position setup.
        judgement.previous = true;
        judgement.seedFrame = nowFrame;
        return false;
    }
    if (!overlap || judgement.consumed)
        return false;
    judgement.consumed = true;
    judgement.judgedFrame = nowFrame;
    if (!block && !dodge)
        ++judgement.acceptedHits;
    return !block && !dodge;
}

enum class JitterMode
{
    None,
    Repeating
};

struct TimelineOptions
{
    uint32_t latencyMs = 30;
    int32_t displayDelayMs = 34;
    uint32_t cadenceMs = 16;
    JitterMode jitter = JitterMode::None;
    bool missing128 = false;
    bool shrinkD = false;
    DefenseMode defense = DefenseMode::None;
};

struct TimelineResult
{
    int32_t visibleStart = -1;
    int32_t judged = -1;
    int32_t reactionWindow = -1;
    int32_t oldReactionWindow = -1;
    float finalPhase = -1.0f;
    bool protectedClock = false;
    unsigned acceptedHits = 0;
};

TimelineResult RunTimeline(TimelineOptions const& options)
{
    const float position[3] = {100.0f, 200.0f, 300.0f};
    const float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    body::Track track;
    SetIncomingTrack(track);
    body::SetPoseInterval(track, options.cadenceMs);

    // A real idle prehistory establishes offset, cadence and a newest/oldest buffer
    // before the host starts the new motion instance.
    for (uint32_t sent = HostAttackSent - 512; sent < HostAttackSent; sent += options.cadenceMs) {
        const uint32_t arrived = sent + options.latencyMs;
        body::Feed(track, position, rotation, sent, arrived, 2, body::MotionState{});
    }

    TimelineResult result;
    VisibleJudgement judgement;
    bool block = false;
    bool dodge = false;
    bool overlap = true;
    int32_t firstClientFrame = static_cast<int32_t>(HostAttackSent + options.latencyMs - 32);
    int32_t currentDelay = options.displayDelayMs;
    uint32_t deliveredPackets = 0;
    uint32_t missedPackets = 0;
    int32_t shrinkAtFrame = -1;
    std::vector<bool> deliveredPacketsByElapsed;
    for (uint32_t elapsed = 0; elapsed <= 800; elapsed += options.cadenceMs)
        deliveredPacketsByElapsed.push_back(false);

    // Poll the client at 16ms frames. For each frame, deliver all packets due at
    // arrival time, then call the production body::Sample with the production clock.
    for (int32_t frame = 0; frame <= 54; ++frame) {
        const int32_t nowFrame = firstClientFrame + static_cast<int32_t>(frame * PollFrameMs);
        for (uint32_t elapsed = 0; elapsed <= 800; elapsed += options.cadenceMs) {
            const uint32_t packet = elapsed / options.cadenceMs;
            if (packet >= deliveredPacketsByElapsed.size())
                break;
            if (deliveredPacketsByElapsed[packet])
                continue;
            int32_t jitter = 0;
            if (options.jitter == JitterMode::Repeating) {
                const int32_t schedule[] = {12, 0, -12, 0};
                jitter = schedule[packet & 3u];
            }
            const uint32_t sent = HostAttackSent + elapsed;
            const int32_t deliveryOffset = static_cast<int32_t>(options.latencyMs) + jitter;
            const uint32_t delivery = deliveryOffset >= 0
                ? sent + static_cast<uint32_t>(deliveryOffset)
                : sent - static_cast<uint32_t>(-deliveryOffset);

            if (static_cast<int32_t>(static_cast<uint32_t>(nowFrame) - delivery) < 0)
                continue;

            if (options.missing128 && elapsed >= 160 && elapsed < 288) {
                ++missedPackets;
                deliveredPacketsByElapsed[packet] = true;
                continue;
            }
            const float phase = elapsed <= HitDurationMs
                ? static_cast<float>(elapsed) / static_cast<float>(HitDurationMs)
                : 1.0f;
            const uint32_t instance = elapsed <= 400 ? 1 : 2;
            const uint32_t name = elapsed <= 400 ? 101 : 102;
            body::Feed(track, position, rotation, sent, delivery, 2,
                       IncomingAttack(instance, name, phase));
            ++deliveredPackets;
            deliveredPacketsByElapsed[packet] = true;
        }

        float shown[3], turn[4];
        body::Presentation presentation;
        assert(body::Sample(track, static_cast<uint32_t>(nowFrame), shown, turn,
                            -currentDelay, true, &presentation));

        const uint32_t instance = PresentedInstance(presentation);
        const float phase = PresentedPhase(presentation);
        result.finalPhase = phase;
        result.protectedClock = result.protectedClock || track.combatClockProtected;

        if (instance == 1 && result.visibleStart < 0)
            result.visibleStart = nowFrame - static_cast<int32_t>(HostAttackSent);

        if (result.visibleStart >= 0 && options.shrinkD && shrinkAtFrame < 0) {
            currentDelay = 34;
            shrinkAtFrame = nowFrame;
        }

        if (instance == 1) {
            TickVisibleJudgement(judgement, instance, phase, nowFrame, options.defense,
                                 result.visibleStart, block, dodge, overlap);
        }
        if (judgement.judgedFrame >= 0 && result.judged < 0)
            result.judged = judgement.judgedFrame - static_cast<int32_t>(HostAttackSent);
    }

    if (judgement.firstVisibleFrame >= 0)
        result.visibleStart = judgement.firstVisibleFrame - static_cast<int32_t>(HostAttackSent);

    assert(deliveredPackets > 0);
    if (options.missing128)
        assert(missedPackets > 0);

    if (result.visibleStart >= 0) {
        if (judgement.judgedFrame >= 0)
            result.reactionWindow = judgement.judgedFrame - judgement.firstVisibleFrame;
        // Old overlap/judge whole clip: first visible frame plus one captured 16ms sweep frame.
        result.oldReactionWindow = PollFrameMs;
    }
    result.acceptedHits = judgement.acceptedHits;
    return result;
}

void TestHitPhaseAndQuantization()
{
    assert(!combat_timing::HitPhaseReached(-1.0f));
    assert(!combat_timing::HitPhaseReached(0.0f));
    assert(!combat_timing::HitPhaseReached(combat_timing::NativeHitPhase -
                                           2.0f * combat_timing::PhaseTolerance));
    assert(combat_timing::HitPhaseReached(combat_timing::NativeHitPhase -
                                          combat_timing::PhaseTolerance));
    assert(combat_timing::HitPhaseReached(combat_timing::NativeHitPhase));
    assert(combat_timing::HitPhaseReached(1.0f));
    assert(!combat_timing::HitPhaseReached(1.0001f));
    assert(!combat_timing::HitPhaseReached(std::numeric_limits<float>::infinity()));
    assert(!combat_timing::HitPhaseReached(std::numeric_limits<float>::quiet_NaN()));
}

void TestReactionWindowMatrix()
{
    const uint32_t latencies[] = {30, 80, 150};
    const int32_t delays[] = {34, 66, 200};
    const uint32_t cadences[] = {16, 33};

    std::printf("REACTION_WINDOWS_LD nominal240; base capture+frame tolerance 16=>32ms, 33=>49ms; jitter=>44/61ms\n");
    for (uint32_t latency : latencies) {
        for (int32_t delay : delays) {
            int32_t minimum = 100000, maximum = -100000;
            for (uint32_t cadence : cadences) {
                for (JitterMode jitter : {JitterMode::None, JitterMode::Repeating}) {
                    TimelineResult result = RunTimeline({
                        latency, delay, cadence, jitter, false, false, DefenseMode::None
                    });
                    assert(result.visibleStart >= 0);
                    assert(result.judged >= 0);
                    assert(result.reactionWindow > 0);
                    minimum = std::min(minimum, result.reactionWindow);
                    maximum = std::max(maximum, result.reactionWindow);

                    const int explicitTolerance =
                        static_cast<int>(cadence) + static_cast<int>(PollFrameMs) +
                        (jitter == JitterMode::Repeating ? 12 : 0);
                    assert(result.reactionWindow >=
                           static_cast<int>(combat_timing::NativeHitPhase * HitDurationMs) -
                           explicitTolerance);
                    assert(result.reactionWindow <=
                           static_cast<int>(combat_timing::NativeHitPhase * HitDurationMs) +
                           explicitTolerance);
                    assert(result.oldReactionWindow != result.reactionWindow);
                    assert(result.oldReactionWindow < 240 - explicitTolerance);
                    assert(result.oldReactionWindow == 16);
                    assert(result.oldReactionWindow < 100);
                }
            }
            std::printf("L=%u D=%d min=%d max=%d\n", latency, delay, minimum, maximum);
        }
    }
}

void TestMissingAndShrink()
{
    unsigned missingShrinkCases = 0;
    for (uint32_t latency : {30u, 80u, 150u}) {
        for (int32_t delay : {34, 66, 200}) {
            for (uint32_t cadence : {16u, 33u}) {
                for (JitterMode jitter : {JitterMode::None, JitterMode::Repeating}) {
                    ++missingShrinkCases;
                    const TimelineResult missing = RunTimeline({
                        latency, delay, cadence, jitter, true, false, DefenseMode::None
                    });
                    assert(missing.visibleStart >= 0);
                    assert(missing.judged >= 0);
                    assert(missing.reactionWindow > 0);

                    const TimelineResult shrink = RunTimeline({
                        latency, 66, cadence, jitter, false, true, DefenseMode::None
                    });
                    assert(shrink.visibleStart >= 0);
                    assert(shrink.judged >= 0);
                    assert(shrink.reactionWindow > 0);

                    const TimelineResult fixedBefore = RunTimeline({
                        latency, 66, cadence, jitter, false, false, DefenseMode::None
                    });
                    const TimelineResult fixedAfter = RunTimeline({
                        latency, 34, cadence, jitter, false, false, DefenseMode::None
                    });
                    const int tolerance = static_cast<int>(cadence) + static_cast<int>(PollFrameMs) +
                                          (jitter == JitterMode::Repeating ? 12 : 0);
                    assert(shrink.reactionWindow >= 240 - tolerance);
                    assert(shrink.reactionWindow <= 240 + tolerance);
                    assert(shrink.reactionWindow >=
                           std::min(fixedBefore.reactionWindow, fixedAfter.reactionWindow) - tolerance);

                    if (missing.reactionWindow < 240 - tolerance ||
                        missing.reactionWindow > 240 + 128 + tolerance) {
                        std::fprintf(stderr,
                                     "missing-window-failure L=%u D=%d cadence=%u jitter=%d window=%d tolerance=%d\n",
                                     latency, delay, cadence,
                                     jitter == JitterMode::Repeating ? 1 : 0,
                                     missing.reactionWindow, tolerance);
                    }
                    assert(missing.reactionWindow >= 240 - tolerance);
                    assert(missing.reactionWindow <= 240 + 128 + tolerance);
                }
            }
        }
    }
    std::printf("MISSING128_AND_DSHRINK_CASES=%u; missing lower bound=240-tolerance, upper=240+128+tolerance; shrink=66-to-34\n",
                missingShrinkCases);
}

void TestDefenseAndOncePerInstance()
{
    unsigned scenarios = 0;
    for (uint32_t latency : {30u, 80u, 150u}) {
        for (int32_t delay : {34, 66, 200}) {
            for (uint32_t cadence : {16u, 33u}) {
                for (DefenseMode defense :
                     {DefenseMode::BlockAtWindup100, DefenseMode::BlockJustBeforeHit,
                      DefenseMode::BlockAfterHit, DefenseMode::DodgeAtHit,
                      DefenseMode::RetreatAtWindup100}) {
                    TimelineResult result = RunTimeline({
                        latency, delay, cadence, JitterMode::Repeating, false, false, defense
                    });
                    assert(result.visibleStart >= 0);
                    if (result.acceptedHits != (defense == DefenseMode::BlockAfterHit ? 1u : 0u)) {
                        std::fprintf(stderr,
                                     "defense-failure mode=%d L=%u D=%d cadence=%u accepted=%u judged=%d visible=%d window=%d\n",
                                     static_cast<int>(defense), latency, delay, cadence,
                                     result.acceptedHits, result.judged,
                                     result.visibleStart, result.reactionWindow);
                    }
                    if (defense == DefenseMode::BlockAfterHit)
                        assert(result.acceptedHits == 1);
                    else
                        assert(result.acceptedHits == 0);
                    ++scenarios;
                }
            }
        }
    }

    // Once-per-instance repeats, sequence restart and independent two-attacker sequences.
    VisibleJudgement first;
    bool block = false, dodge = false, overlap = true;
    assert(!TickVisibleJudgement(first, 77, 0.02f, 10, DefenseMode::None, 10,
                                 block, dodge, overlap));
    assert(!TickVisibleJudgement(first, 77, 0.60f, 30, DefenseMode::None, 10,
                                 block, dodge, overlap));
    assert(TickVisibleJudgement(first, 77, 0.61f, 46, DefenseMode::None, 10,
                                block, dodge, overlap));
    assert(!TickVisibleJudgement(first, 77, 0.62f, 62, DefenseMode::None, 10,
                                 block, dodge, overlap));
    assert(first.acceptedHits == 1);

    VisibleJudgement restart;
    block = dodge = false; overlap = true;
    assert(TickVisibleJudgement(restart, 100, 0.60f, 100, DefenseMode::None, 100,
                                block, dodge, overlap) == false);
    assert(TickVisibleJudgement(restart, 100, 0.61f, 116, DefenseMode::None, 100,
                                block, dodge, overlap));
    assert(TickVisibleJudgement(restart, 101, 0.60f, 132, DefenseMode::None, 100,
                                block, dodge, overlap) == false);
    assert(TickVisibleJudgement(restart, 101, 0.61f, 148, DefenseMode::None, 100,
                                block, dodge, overlap));
    assert(restart.acceptedHits == 2);

    VisibleJudgement twoA, twoB;
    block = dodge = false; overlap = true;
    TickVisibleJudgement(twoA, 200, 0.60f, 200, DefenseMode::None, 200, block, dodge, overlap);
    assert(TickVisibleJudgement(twoA, 200, 0.61f, 216, DefenseMode::None, 200,
                                block, dodge, overlap));
    TickVisibleJudgement(twoB, 201, 0.60f, 200, DefenseMode::None, 200, block, dodge, overlap);
    assert(TickVisibleJudgement(twoB, 201, 0.61f, 216, DefenseMode::None, 200,
                                block, dodge, overlap));
    assert(scenarios == 90);
    std::printf("DEFENSE_SCENARIOS=%u; ONCE_INSTANCE repeated=1 restart=2 two_attackers=2\n",
                scenarios);
}

void TestDisplayClockProtectionAndWrap()
{
    const float position[3] = {};
    const float rotation[4] = {0, 0, 0, 1};
    float shown[3], turn[4];
    body::Presentation presentation;

    body::Track wrapped;
    SetIncomingTrack(wrapped);
    body::Feed(wrapped, position, rotation, UINT32_MAX - 100, UINT32_MAX - 70, 2,
               IncomingAttack(7, 701, 0.20f));
    assert(body::Sample(wrapped, UINT32_MAX - 70, shown, turn, -34, true, &presentation));
    body::Feed(wrapped, position, rotation, 4, 34, 2,
               IncomingAttack(7, 701, 0.50f));
    assert(body::Sample(wrapped, 34, shown, turn, -34, true, &presentation));
    assert(wrapped.count == 2);

    body::Track initial;
    SetIncomingTrack(initial);
    body::Feed(initial, position, rotation, UINT32_MAX - 20, UINT32_MAX - 10, 2,
               IncomingAttack(8, 801, 0.10f));
    assert(body::Sample(initial, UINT32_MAX - 10, shown, turn, -34, true, &presentation));
    assert(initial.haveRenderTime);
    assert(initial.combatClockProtected);

    body::Track unprotected;
    unprotected.predictive = false;
    unprotected.combatPlayer = 0;
    body::Feed(unprotected, position, rotation, 1000, 1005, 2,
               IncomingAttack(9, 901, 0.10f, 0));
    body::Feed(unprotected, position, rotation, 1016, 1021, 2,
               IncomingAttack(9, 901, 0.20f, 0));
    assert(body::Sample(unprotected, 1500, shown, turn, 100, true, &presentation));
    const uint32_t aheadRender = unprotected.renderSent;
    assert(static_cast<int32_t>(aheadRender - 1016) > 0);

    body::MotionState combat = IncomingAttack(9, 901, 0.20f);
    body::Feed(unprotected, position, rotation, 1016, 1021, 2, combat);
    unprotected.combatPlayer = 2;
    assert(body::Sample(unprotected, 1516, shown, turn, 100, true, &presentation));
    assert(unprotected.renderSent == aheadRender);

    body::Track host;
    body::Feed(host, position, rotation, 3000, 3010, 2,
               IncomingAttack(10, 1001, 0.10f));
    assert(body::Sample(host, 4000, shown, turn, 100, true, &presentation));
    assert(!host.combatClockProtected);

    body::Track npcVersusNpc;
    SetIncomingTrack(npcVersusNpc);
    body::Feed(npcVersusNpc, position, rotation, 4000, 4030, 2,
               IncomingAttack(11, 1101, 0.10f, 20, 12345));
    assert(body::Sample(npcVersusNpc, 5000, shown, turn, 100, true, &presentation));
    assert(!npcVersusNpc.combatClockProtected);

    body::Track predictive;
    predictive.predictive = true;
    predictive.combatPlayer = 2;
    body::Feed(predictive, position, rotation, 5000, 5030, 2,
               IncomingAttack(12, 1201, 0.10f));
    assert(body::Sample(predictive, 6000, shown, turn, 100, true, &presentation));
    assert(!predictive.combatClockProtected);
}

void TestReliableMotionDescriptors()
{
    net::NpcRecord sent;
    sent.flags = 20;
    sent.combatTarget = 0;
    sent.incarnation = 1;

    net::NpcRecord current = sent;
    current.motions[0].active = 1;
    current.motions[0].instance = 1;
    current.motions[0].name = 100;
    assert(npcsend::EssentialChanged(sent, current));

    sent = current;
    current.motions[0].instance = 2;
    current.motions[0].name = 101;
    assert(npcsend::EssentialChanged(sent, current));
    sent = current;
    current.motions[0].instance = 3;
    current.motions[0].name = 102;
    assert(npcsend::EssentialChanged(sent, current));
    sent = current;
    current.motions[0].instance = 4;
    current.motions[0].name = 103;
    assert(npcsend::EssentialChanged(sent, current));

    sent = current;
    current.motions[0].phase = 0.45f;
    current.motions[0].weight = 0.75f;
    assert(!npcsend::EssentialChanged(sent, current));
    sent = current;
    current.motions[0].options[0]++;
    assert(!npcsend::EssentialChanged(sent, current));

    net::NpcRecord npcCombat = sent;
    npcCombat.combatTarget = 12345;
    net::NpcRecord npcNext = npcCombat;
    npcNext.motions[0].instance++;
    npcNext.motions[0].name++;
    assert(!npcsend::EssentialChanged(npcCombat, npcNext));

    net::NpcRecord notIncoming = sent;
    notIncoming.flags = 16;
    net::NpcRecord notIncomingNext = notIncoming;
    notIncomingNext.motions[0].instance++;
    notIncomingNext.motions[0].name++;
    assert(!npcsend::EssentialChanged(notIncoming, notIncomingNext));
}

void TestBetweenSwingClockCatchUpAndEvictedHitPhase()
{
    assert(combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Attack_Raise_N_Fwd_00.xmot", 0));
    assert(combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Attack_Hit_N_Fwd_00.xmot", 1));
    assert(combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_RunAttack_Hit_N_Fwd_00.xmot", 0));
    assert(combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_WhirlAttack_Begin_00.xmot", 0));
    assert(combat_timing::IncomingAttackMotion(nullptr, 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Attack_Recover_N_Fwd_00.xmot", 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Move_Run_Loop_N_Fwd_00.xmot", 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Ambient_Loop_00.xmot", 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_StumbleR_Hit_N_Back_00.xmot", 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_StumbleR_Recover_N_Fwd_00.xmot", 0));
    assert(!combat_timing::IncomingAttackMotion("Orc_Stand_None_2H_P0_Parade_Loop_00.xmot", 0));

    // Simulate real 16 ms client frames + 30 ms host combat packets with +/-12 ms jitter AND a
    // 1,200 ms client hitch mid-fight: between-swing recovery/locomotion must keep steady-state
    // backlog near zero (eliminating the Build 75 5,287 ms ratchet) and drain post-hitch backlog
    // back under 50 ms while still protecting every active Attack_Raise / Attack_Hit window.
    const float position[3] = {};
    const float rotation[4] = {0, 0, 0, 1};
    float shown[3], turn[4];
    body::Presentation presentation;
    body::Track track;
    SetIncomingTrack(track);

    uint32_t nextHostSend = 10000;
    uint32_t clientNow = 10020;
    uint32_t currentInstance = 10;
    bool prevWasAttack = false;
    uint32_t packetIndex = 0;
    bool sawProtectedAttack = false;
    bool sawUnprotectedRecovery = false;

    auto deliverDuePackets = [&](uint32_t upToNow) {
        while (true) {
            const int32_t jitterSchedule[4] = {12, 0, -12, 0};
            const int32_t jitter = jitterSchedule[packetIndex & 3u];
            const uint32_t arrival = static_cast<uint32_t>(static_cast<int32_t>(nextHostSend) + 20 + jitter);
            if (static_cast<int32_t>(upToNow - arrival) < 0)
                break;
            const uint32_t cyclePos = (nextHostSend - 10000u) % 1050u;
            const bool isAttack = cyclePos < 450u;
            if (packetIndex == 0 || isAttack != prevWasAttack) {
                ++currentInstance;
                prevWasAttack = isAttack;
            }
            const float phase = isAttack
                ? static_cast<float>(cyclePos) / 450.0f
                : static_cast<float>(cyclePos - 450u) / 600.0f;
            body::MotionState m = IncomingAttack(currentInstance, isAttack ? 501u : 502u, phase);
            m.contact = isAttack ? 1 : 0;
            m.nonAttackCombat = !isAttack;
            body::Feed(track, position, rotation, nextHostSend, arrival, 2, m);
            nextHostSend += 30;
            ++packetIndex;
        }
    };

    // 1) 4 seconds of jittery combat: backlog must not ratchet upward.
    for (int frame = 0; frame < 250; ++frame, clientNow += 16) {
        deliverDuePackets(clientNow);
        if (!track.count) continue;
        assert(body::Sample(track, clientNow, shown, turn, -body::PlaybackDelay(track), true, &presentation));
        if (!presentation.motion.nonAttackCombat && track.combatClockProtected)
            sawProtectedAttack = true;
        if (presentation.motion.nonAttackCombat && !track.combatClockProtected)
            sawUnprotectedRecovery = true;
    }
    assert(sawProtectedAttack && sawUnprotectedRecovery);
    assert(track.backlogMs <= 50);

    // 2) 1,200 ms client stall mid-combat followed by 8 seconds of normal frames: drains back <= 50 ms.
    clientNow += 1200;
    deliverDuePackets(clientNow);
    for (int frame = 0; frame < 500; ++frame, clientNow += 16) {
        deliverDuePackets(clientNow);
        assert(body::Sample(track, clientNow, shown, turn, -body::PlaybackDelay(track), true, &presentation));
    }
    if (track.backlogMs > 50 || track.catchUpFrames == 0) {
        std::fprintf(stderr, "post-stall debug: backlogMs=%d catchUpFrames=%u delayMs=%d pending=%zu prot=%d\n",
                     track.backlogMs, track.catchUpFrames, track.delayMs, body::PendingEvents(track),
                     track.combatClockProtected ? 1 : 0);
    }
    assert(track.backlogMs <= 50 && track.catchUpFrames > 0);

    // Even if intermediate frames of an Attack_Hit clip were evicted/dropped before Attack_Recover,
    // Interpolate advances contact=1 Hit phase across 0.60 instead of freezing at 0.047.
    body::Track evicted;
    SetIncomingTrack(evicted);
    body::MotionState startHit = IncomingAttack(90, 901, 0.047f);
    startHit.contact = 1;
    body::MotionState startRecover = IncomingAttack(91, 902, 0.0f);
    startRecover.contact = 0;
    startRecover.nonAttackCombat = true;
    body::Feed(evicted, position, rotation, 2000, 2020, 2, startHit);
    body::Feed(evicted, position, rotation, 2450, 2470, 2, startRecover);
    // Prime initial event presentation at 2000, then sample across [2000, 2450).
    assert(body::Sample(evicted, 2054, shown, turn, -34, true, &presentation));
    bool reachedHitPhase = false;
    for (uint32_t now = 2070; now <= 2460; now += 16) {
        assert(body::Sample(evicted, now, shown, turn, -34, true, &presentation));
        if (presentation.motion.contact &&
            PresentedInstance(presentation) == 90 &&
            combat_timing::HitPhaseReached(PresentedPhase(presentation))) {
            reachedHitPhase = true;
        }
    }
    assert(reachedHitPhase);
}

void TestSweptWeaponMidBladeCleave()
{
    // A 140 cm 2H Orc axe sweeps horizontally from left (-X) to right (+X) across a hero standing
    // 120 cm in front of the attacker: grip passes at z=50, tip passes at z=190, end-of-tick blade is at x>=20..80.
    const float oldA[3] = {-20.0f, 100.0f, 50.0f};
    const float oldB[3] = {-80.0f, 100.0f, 190.0f};
    const float a[3] = {20.0f, 100.0f, 50.0f};
    const float b[3] = {80.0f, 100.0f, 190.0f};
    const float heroFirst[3] = {0.0f, 35.0f, 120.0f};
    const float heroLast[3] = {0.0f, 155.0f, 120.0f};
    const float reach = 42.0f; // 35 cm capsule radius + 7 cm blade half-thickness
    const float limit = reach * reach;

    // Endpoint-only segments all miss (> 42 cm), proving why endpoint-only SweepsBody dropped mid-blade hits.
    assert(contact::SegmentDistanceSquared(oldA, a, heroFirst, heroLast) > limit);
    assert(contact::SegmentDistanceSquared(oldB, b, heroFirst, heroLast) > limit);
    assert(contact::SegmentDistanceSquared(a, b, heroFirst, heroLast) > limit);

    // Full swept weapon surface catches the mid-blade passing right through (0, 100, 120).
    assert(contact::SweptSegmentDistanceSquared(oldA, oldB, a, b, heroFirst, heroLast, true) <= limit);

    // Retreated hero at z=240 cm stays safely outside the swept surface.
    const float retreatedFirst[3] = {0.0f, 35.0f, 240.0f};
    const float retreatedLast[3] = {0.0f, 155.0f, 240.0f};
    assert(contact::SweptSegmentDistanceSquared(oldA, oldB, a, b, retreatedFirst, retreatedLast, true) > limit);
}
}

int main()
{
    TestHitPhaseAndQuantization();
    TestReactionWindowMatrix();
    TestMissingAndShrink();
    TestDefenseAndOncePerInstance();
    TestDisplayClockProtectionAndWrap();
    TestReliableMotionDescriptors();
    TestBetweenSwingClockCatchUpAndEvictedHitPhase();
    TestSweptWeaponMidBladeCleave();
    std::puts("combat timing engine-free tests passed");
    return 0;
}
