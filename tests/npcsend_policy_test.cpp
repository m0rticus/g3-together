// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcsend_policy.h"
#include "../src/npcsend_candidates.h"
#include "../src/net_metrics.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace g3t;
using namespace g3t::npcsend;

struct Fixture {
    net::NpcRecord record{};
    std::vector<pose::Bone> bones;
    Input in{};
    State state{};

    Fixture(int boneCount = 0)
    {
        record.guid[0] = 1;
        record.rotation[3] = 1;
        for (auto& motion : record.motions) {
            motion.name = motion.instance = 1;
            motion.active = 1;
            motion.phase = 0.2f;
            motion.weight = 0.5f;
        }
        for (int i = 0; i < boneCount; ++i) {
            pose::Bone b{};
            b.name = static_cast<uint32_t>(i + 1);
            b.values[0] = b.values[4] = b.values[8] = 4096;
            bones.push_back(b);
        }
        in.record = &record;
        in.distanceCm = 1000;
        Bind();
    }

    void Bind() { in.bones = bones.empty() ? nullptr : bones.data(); in.boneCount = static_cast<int>(bones.size()); }
    Result Eval() { return Evaluate(state, in); }
    void Send(Decision expected)
    {
        if (Eval().decision != expected)
            std::fprintf(stderr, "policy expected=%d actual=%d now=%u sent=%u\n",
                         int(expected), int(Eval().decision), in.now, state.sentAt);
        assert(Eval().decision == expected);
        assert(Commit(state, in, expected));
        if (expected == Decision::Initial) state.enteredAt = in.now - InterestWarmupMs;
    }
};

static void TestInitialAndHysteresis()
{
    Fixture f(2);
    f.in.distanceCm = InterestEnterCm + 1;
    assert(f.Eval().decision == Decision::Outside);
    assert(!f.state.live);
    f.in.distanceCm = InterestEnterCm;
    f.Send(Decision::Initial);
    f.in.now = 1;
    f.in.distanceCm = InterestExitCm;
    assert(f.Eval().decision == Decision::Wait);
    f.in.distanceCm = InterestExitCm + 1;
    assert(f.Eval().decision == Decision::FinalAndRelease);
    assert(f.Eval().decision == Decision::FinalAndRelease); // failed final/release: retry, keep baseline
    assert(f.state.live && f.state.bones.size() == 2);
    assert(Commit(f.state, f.in, Decision::FinalAndRelease)); // only after BOTH operations succeeded
    assert(!f.state.live && f.state.bones.empty());
    assert(f.Eval().decision == Decision::Outside);
    f.in.distanceCm = InterestEnterCm + 1;
    assert(f.Eval().decision == Decision::Outside);
    f.in.distanceCm = InterestEnterCm;
    f.Send(Decision::Initial); // explicit full reentry baseline

    f.in.moving = true;
    f.in.distanceCm = NearEnterCm;
    assert(f.Eval().tier == Tier::Ordinary);
    f.in.distanceCm = NearExitCm;
    assert(f.Eval().tier == Tier::Ordinary);
    f.in.distanceCm = NearExitCm + 1;
    f.in.now += OrdinaryHoldMs;
    assert(f.Eval().tier == Tier::MovingFar);
    f.in.distanceCm = NearEnterCm + 1;
    assert(f.Eval().tier == Tier::MovingFar);
    f.in.distanceCm = NearEnterCm;
    assert(f.Eval().tier == Tier::Ordinary);
    f.in.nearby = true;
    f.in.distanceCm = NearExitCm + 1;
    assert(f.Eval().tier == Tier::Ordinary);
    f.in.interest = Interest::Exclude;
    assert(f.Eval().decision == Decision::FinalAndRelease);
    f.Send(Decision::FinalAndRelease);
    f.in.interest = Interest::Include;
    f.in.distanceCm = 100000;
    f.Send(Decision::Initial);
    f.state = State{}; // caller discards state on reconnect/world/generation
    f.Send(Decision::Initial);
}

static void TestGatesAndInvalid()
{
    Fixture f(2);
    f.in.eligible = false;
    f.in.record = nullptr;
    f.in.distanceCm = -1;
    assert(f.Eval().decision == Decision::Ineligible);
    assert(!f.state.live);
    f.in.eligible = true;
    assert(f.Eval().decision == Decision::Invalid);
    f.in.record = &f.record;
    f.in.distanceCm = 100;
    f.Send(Decision::Initial);
    f.in.eligible = false;
    f.in.distanceCm = 99999;
    assert(f.Eval().decision == Decision::Ineligible && f.state.live);
    f.in.eligible = true;
    f.in.distanceCm = std::numeric_limits<float>::quiet_NaN();
    assert(f.Eval().decision == Decision::Invalid && f.state.live);
    f.in.distanceCm = 100;
    f.in.bones = nullptr;
    assert(f.Eval().decision == Decision::Invalid);
    f.Bind();
    f.in.boneCount = pose::MaxBones + 1;
    assert(f.Eval().decision == Decision::Invalid);
    f.Bind();
    f.in.poseFlags = 0x80;
    assert(f.Eval().decision == Decision::Invalid);
    f.in.poseFlags = 0;
    f.bones[1].name = f.bones[0].name;
    assert(f.Eval().decision == Decision::Invalid);
    f.bones[1].name = 0;
    assert(f.Eval().decision == Decision::Invalid);
    f.bones[1].name = 2;
    f.record.rotation[3] = 0;
    assert(f.Eval().decision == Decision::Invalid);
    f.record.rotation[3] = 1;
    f.record.motions[7].weight = std::numeric_limits<float>::infinity();
    assert(f.Eval().decision == Decision::Invalid);
    f.record.motions[7].weight = .5f;
    f.record.guid[0] = 2;
    assert(f.Eval().decision == Decision::Invalid); // different NPC needs a different state
    f.record.guid[0] = 1;
    f.in.now = UINT32_MAX;
    assert(f.Eval().decision == Decision::Invalid); // backwards by 1, not a wrap
    f.in.now = 0;
    assert(!Commit(f.state, f.in, Decision::Changed));
    assert(f.state.live);
}

static void TestCadenceAndUrgency()
{
    Fixture f(1);
    f.Send(Decision::Initial);
    f.record.position[0] = 1;
    f.in.moving = true;
    f.in.now = OrdinaryIntervalMs - 1;
    assert(f.Eval().decision == Decision::Wait && f.Eval().tier == Tier::Ordinary);
    f.in.now = OrdinaryIntervalMs;
    f.Send(Decision::Changed);
    f.in.distanceCm = NearExitCm + 1;
    f.in.now += OrdinaryHoldMs;
    f.Send(Decision::Keyframe);
    f.record.position[0] = 2;
    f.in.now += MovingFarIntervalMs - 1;
    assert(f.Eval().decision == Decision::Wait && f.Eval().tier == Tier::MovingFar);
    ++f.in.now;
    f.Send(Decision::Changed);
    f.record.position[0] = 3;
    f.in.moving = false;
    f.in.distanceCm = NearExitCm + 1;
    f.in.now += IdleIntervalMs - 1;
    assert(f.Eval().decision == Decision::Wait && f.Eval().tier == Tier::Idle);
    ++f.in.now;
    f.Send(Decision::Changed);
    ++f.in.now;
    f.in.focused = true;
    f.Send(Decision::Semantic); // unchanged snapshot, new focus is urgent
    ++f.in.now;
    assert(f.Eval().decision == Decision::Wait && f.Eval().tier == Tier::Fast);
    f.in.focused = false;
    assert(f.Eval().decision == Decision::Wait);
    f.in.combat = true;
    f.Send(Decision::Semantic);
    ++f.in.now;
    assert(f.Eval().decision == Decision::Wait);
    f.in.focused = true; // focus rising while already in combat is still an edge
    f.Send(Decision::Semantic);
    ++f.in.now;
    assert(f.Eval().decision == Decision::Wait);
    f.in.focused = false;
    f.in.combat = false;
    assert(f.Eval().decision == Decision::Wait);
    f.in.contact = true;
    assert(f.Eval().decision == Decision::Semantic);
    assert(f.Eval().decision == Decision::Semantic); // retry if send failed
    f.Send(Decision::Semantic);
    ++f.in.now;
    assert(f.Eval().decision == Decision::Wait);
    f.in.contact = false;
    f.in.force = true;
    f.Send(Decision::Semantic);
    f.in.force = false;
    f.in.now += KeepaliveMs - 1;
    assert(f.Eval().decision == Decision::Wait);
    ++f.in.now;
    f.Send(Decision::Keyframe);
    f.in.now += KeepaliveMs;
    f.Send(Decision::Keyframe); // lost keyframe self-heals on next full keyframe
}

static void TestDiscreteFields()
{
    Fixture f(1);
    f.in.moving = true;
    f.Send(Decision::Initial);
    f.in.now = OrdinaryIntervalMs;
    const net::NpcRecord baseline = f.record;
    auto check = [&]() { assert(f.Eval().decision == Decision::Semantic); f.record = baseline; };
    f.record.movementMode++; check();
    f.record.flags ^= 1; check();
    f.record.hitPoints++; check();
    f.record.hitPointsMax++; check();
    f.record.motion++; check();
    f.record.aniState++; check();
    f.record.heldRight++; check();
    f.record.heldLeft++; check();
    f.record.partyPlayer++; check();
    f.record.partyType++; check();
    f.record.waiting++; check();
    f.record.guidePlayer++; check();
    f.record.aiMode++; check();
    f.record.action++; check();
    f.record.attackReason++; check();
    f.record.damageType++; check();
    f.record.damageAmount++; check();
    f.record.damageMultiplier = -0.0f; check(); // exact transmitted float bits
    f.record.incarnation++; check();
    f.record.hitPlayer++; check();
    f.record.hitSequence++; check();
    f.record.contact++; check();
    f.record.attitude++; check();
    f.record.combatTarget++; check();
    for (int i = 0; i < net::NpcMotionSlots; ++i) {
        auto& m = f.record.motions[i];
        m.name++; check();
        m.instance++; check();
        m.speed += 0.00001f; check();
        m.intro += 0.00001f; check();
        m.extro += 0.00001f; check();
        m.blend++; check();
        for (int j = 0; j < 7; ++j) { m.options[j]++; check(); }
        m.active = 3; check();
        m.weight = 0; check(); // contact/blend edge
        m.phase = .9f; check(); // discontinuous seek/loop
    }
    f.record.sent = 123456;
    assert(f.Eval().decision == Decision::Wait); // capture timestamp alone is not a change
    f.in.poseFlags = pose::Facial;
    assert(f.Eval().decision == Decision::Semantic);
    f.in.poseFlags = 0;
    f.bones[0].name++;
    assert(f.Eval().decision == Decision::Semantic);
    f.bones[0].name--;
    f.bones[0].values[9] += BoneTranslationDiscontinuity;
    assert(f.Eval().decision == Decision::Changed);
    f.bones[0].values[9] -= BoneTranslationDiscontinuity;
    f.bones[0].values[1] += BoneBasisDiscontinuity;
    assert(f.Eval().decision == Decision::Changed);
    f.bones[0].values[1] -= BoneBasisDiscontinuity;
    f.bones.clear();
    f.Bind();
    assert(f.Eval().decision == Decision::Semantic); // pose disappearance
    f.Send(Decision::Semantic);
    pose::Bone b{};
    b.name = 1;
    f.bones.push_back(b);
    f.Bind();
    assert(f.Eval().decision == Decision::Semantic); // first pose cannot be suppressed by cadence
}

static void TestReliableRetry()
{
    Fixture a(1), b(1);
    a.Send(Decision::Initial);
    b.Send(Decision::Initial);
    a.record.hitSequence = b.record.hitSequence = 1;
    ++a.in.now;
    ++b.in.now;
    assert(a.Eval().decision == Decision::Semantic && a.Eval().essential);
    assert(b.Eval().decision == Decision::Semantic && b.Eval().essential);
    a.Send(Decision::Semantic); // peer A succeeded, peer B failed to queue
    assert(!a.Eval().essential && a.Eval().decision == Decision::Wait);
    assert(b.Eval().essential && b.Eval().decision == Decision::Semantic);
    b.Send(Decision::Semantic);
    const net::NpcRecord sent = b.record;
    auto reliable = [&]() {
        assert(EssentialChanged(sent, b.record));
        assert(b.Eval().decision == Decision::Semantic && b.Eval().essential);
        b.record = sent;
    };
    b.record.flags ^= 1; reliable();
    b.record.flags ^= 2; reliable();
    b.record.flags ^= 4; reliable();
    b.record.flags ^= 8; reliable();
    b.record.flags ^= 16; reliable();
    b.record.flags ^= 32; reliable();
    b.record.flags ^= 128; reliable();
    b.record.aiMode++; reliable();
    b.record.flags ^= 4; reliable();
    b.record.flags ^= 16; reliable();
    b.record.incarnation++; reliable();
    b.record.hitPoints++; reliable();
    b.record.hitPointsMax++; reliable();
    b.record.partyPlayer++; reliable();
    b.record.partyType++; reliable();
    b.record.waiting++; reliable();
    b.record.guidePlayer++; reliable();
    b.record.hitPlayer++; reliable();
    b.record.hitSequence++; reliable();
    b.record.contact++; reliable();
    b.record.attitude++; reliable();
    b.record.aiMode++; reliable();
    b.record.attackReason++; reliable();
    b.record.heldRight++; reliable();
    b.record.heldLeft++; reliable();
    b.record.combatTarget++; reliable();
    b.record.action++;
    b.in.now += IdleIntervalMs;
    assert(b.Eval().decision == Decision::Semantic && !b.Eval().essential);
    b.record = sent;
    b.in.distanceCm = InterestExitCm + 1;
    assert(b.Eval().decision == Decision::FinalAndRelease && b.Eval().essential);
}

static void TestRetargetBeforeNextCadence()
{
    Fixture f(1);
    f.record.flags = 4 | (2 << 3);
    f.record.aiMode = 3;
    f.record.heldRight = 123;
    f.Send(Decision::Initial);
    f.in.combat = true;
    f.in.now = 1;
    f.record.flags = 4 | (1 << 3); // client -> host, with identical pose, weapon and motion
    assert(f.Eval().decision == Decision::Semantic && f.Eval().essential);
    assert(f.state.record.flags == (4 | (2 << 3))); // a failed queue cannot hide the opponent edge
    f.Send(Decision::Semantic);
    f.in.now = 2;
    f.record.flags = 4;
    f.record.combatTarget = 456; // player -> native NPC
    assert(f.Eval().decision == Decision::Semantic && f.Eval().essential);
    f.Send(Decision::Semantic);
    f.in.now = 3;
    f.record.combatTarget = 0;
    assert(f.Eval().decision == Decision::Semantic && f.Eval().essential);
    f.Send(Decision::Semantic);
    f.in.now = 4;
    f.record.aiMode = 1; // vanilla relax, not a new attack script on the receiver
    f.record.flags = 0;
    assert(f.Eval().decision == Decision::Semantic && f.Eval().essential);
    assert(f.record.heldRight == 123 && f.record.motions[0].instance == 1);
}

static void TestDynamicAndSentBaseline()
{
    Fixture f(3);
    f.Send(Decision::Initial);
    f.in.now = 100;
    f.record.position[0] = .3f;
    f.record.position[1] = .3f;
    assert(f.Eval().decision == Decision::Wait);
    f.record.position[1] = .4f; // Euclidean vector length 0.5 cm
    assert(f.Eval().decision == Decision::Changed);
    f.record.position[0] = f.record.position[1] = 0;
    constexpr double pi = 3.14159265358979323846;
    const double halfAngle = .5 * pi / 360;
    f.record.rotation[2] = static_cast<float>(std::sin(halfAngle));
    f.record.rotation[3] = static_cast<float>(std::cos(halfAngle));
    assert(f.Eval().decision == Decision::Changed);
    f.record.rotation[2] = 0;
    f.record.rotation[3] = -1;
    assert(f.Eval().decision == Decision::Wait); // q and -q
    f.record.rotation[3] = 1;
    for (int i = 0; i < net::NpcMotionSlots; ++i) {
        f.record.motions[i].phase += .001f;
        assert(f.Eval().decision == Decision::Changed);
        f.record.motions[i].phase -= .001f;
        f.record.motions[i].weight += .001f;
        assert(f.Eval().decision == Decision::Changed);
        f.record.motions[i].weight -= .001f;
    }
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 12; ++k) {
            f.bones[i].values[k] += static_cast<int16_t>(k < 9 ? BoneBasisChange : BoneTranslationChange);
            assert(f.Eval().decision == Decision::Changed);
            f.bones[i].values[k] -= static_cast<int16_t>(k < 9 ? BoneBasisChange : BoneTranslationChange);
        }
    }
    f.record.position[0] = .3f;
    assert(f.Eval().decision == Decision::Wait);
    f.record.position[0] = .6f;
    assert(f.Eval().decision == Decision::Changed); // accumulated against last SENT, not last observed
    f.Send(Decision::Changed);
    f.record.position[0] = .9f;
    f.in.now += 100;
    assert(f.Eval().decision == Decision::Wait);
    f.record.position[0] = 1.2f;
    assert(f.Eval().decision == Decision::Changed);
    f.record.position[0] = 101;
    f.in.now++;
    assert(f.Eval().decision == Decision::Semantic); // root teleport bypasses cadence
}

static void TestIndependentStatesAndWrap()
{
    Fixture a, b;
    a.in.now = b.in.now = UINT32_MAX - 10;
    a.in.moving = b.in.moving = true;
    a.Send(Decision::Initial);
    assert(!b.state.live);
    b.Send(Decision::Initial);
    a.record.hitPoints = 2;
    a.in.now++;
    a.Send(Decision::Semantic);
    assert(b.Eval().decision == Decision::Wait);
    b.record.position[0] = 1;
    b.in.now = OrdinaryIntervalMs; // ordinary interval across uint32 wrap
    b.Send(Decision::Changed);
    b.in.now = OrdinaryIntervalMs + KeyframeMs;
    b.Send(Decision::Keyframe);
}

static void TestLostSnapshotSelfHeals()
{
    Fixture f(1);
    f.Send(Decision::Initial);
    net::NpcRecord receiver = f.record;
    std::vector<pose::Bone> receivedBones = f.bones;
    f.record.hitPoints = 50;
    f.bones[0].values[9] = 4;
    f.in.now = 1;
    assert(f.Eval().essential);
    f.Send(Decision::Semantic); // queued, but the simulated receiver loses it
    assert(receiver.hitPoints != f.record.hitPoints);
    f.in.now += KeyframeMs;
    assert(f.Eval().decision == Decision::Keyframe);
    receiver = f.record;
    receivedBones = f.bones;
    f.Send(Decision::Keyframe);
    assert(receiver.hitPoints == 50 && receivedBones[0].values[9] == 4);
}

static void TestRotatedBoneSensitivity()
{
    constexpr double pi = 3.14159265358979323846;
    for (int n = 0; n < 180; ++n) {
        Fixture f(1);
        const double a = n * pi / 90;
        const double base[9] = {std::cos(a), -std::sin(a), 0,
                                std::sin(a), std::cos(a), 0, 0, 0, 1};
        const double x = std::cos(a), y = std::sin(a), z = 1;
        const double len = std::sqrt(x*x + y*y + z*z);
        const double v[3] = {x/len, y/len, z/len};
        const double s = std::sin(.5 * pi / 180), c = std::cos(.5 * pi / 180);
        const double rotate[9] = {
            c+v[0]*v[0]*(1-c), v[0]*v[1]*(1-c)-v[2]*s, v[0]*v[2]*(1-c)+v[1]*s,
            v[1]*v[0]*(1-c)+v[2]*s, c+v[1]*v[1]*(1-c), v[1]*v[2]*(1-c)-v[0]*s,
            v[2]*v[0]*(1-c)-v[1]*s, v[2]*v[1]*(1-c)+v[0]*s, c+v[2]*v[2]*(1-c)};
        for (int k = 0; k < 9; ++k)
            f.bones[0].values[k] = static_cast<int16_t>(std::lround(base[k] * pose::BasisScale));
        f.Send(Decision::Initial);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) {
                double value = 0;
                for (int k = 0; k < 3; ++k) value += base[row*3+k] * rotate[k*3+col];
                f.bones[0].values[row*3+col] = static_cast<int16_t>(std::lround(value * pose::BasisScale));
            }
        f.in.now = IdleIntervalMs;
        assert(f.Eval().decision == Decision::Changed);
    }
}

static void TestSimulation()
{
    constexpr int npcCount = 75, seconds = 20, scanHz = 43, oldHz = 43;
    std::vector<Fixture> npcs;
    npcs.reserve(npcCount);
    for (int i = 0; i < npcCount; ++i) npcs.emplace_back(80);
    // Fixture moves invalidate its internal pointer: rebind after vector construction.
    for (auto& f : npcs) { f.in.record = &f.record; f.Bind(); }
    uint64_t sends = 0, byClass[5] = {};
    for (int tick = 0; tick < seconds * scanHz; ++tick) {
        const uint32_t now = static_cast<uint32_t>(tick * 1000 / scanHz);
        for (int i = 0; i < npcCount; ++i) {
            Fixture& f = npcs[i];
            const int kind = i < 5 ? 0 : i < 12 ? 1 : i < 15 ? 2 : i < 65 ? 3 : 4;
            f.in.now = now;
            f.in.combat = kind == 0;
            f.in.moving = kind == 1 || kind == 2;
            f.in.distanceCm = kind < 2 ? 1000 : 3000;
            // Exercise the explicit nearby override independently of distance hysteresis.
            f.in.nearby = f.in.distanceCm <= InterestExitCm;
            // Animated classes change every scan; stationary class stays byte-identical.
            if (kind != 4) {
                f.record.motions[0].phase = static_cast<float>(tick % 600) / 600.0f;
                f.bones[0].values[9] = static_cast<int16_t>(tick % 20);
            }
            const auto result = f.Eval();
            if (result.decision != Decision::Wait) {
                assert(result.decision == Decision::Initial || result.decision == Decision::Changed ||
                       result.decision == Decision::Semantic || result.decision == Decision::Keyframe);
                assert(Commit(f.state, f.in, result.decision));
                ++sends;
                ++byClass[kind];
            }
        }
    }
    // Per peer, no batching/retransmits/control packets: ENet MTU 1200 reserves
    // a 4-byte header and 24-byte fragment command for fragmentation. An
    // unreliable-only UDP payload actually carries a 2-byte ENet header plus
    // each 24-byte fragment command (no sent-time field). Each pose is 3 fragments.
    constexpr uint64_t payload = 23 + sizeof(net::NpcRecord) + 3 + 80 * sizeof(pose::Bone);
    constexpr uint64_t fragments = (payload + 1172 - 1) / 1172;
    constexpr uint64_t udpBytes = payload + fragments * 26;
    constexpr uint64_t oldSends = uint64_t(npcCount) * oldHz * seconds;
    static_assert(payload == 2854 && fragments == 3 && udpBytes == 2932, "simulation assumptions");
    assert(sends < oldSends && byClass[0] >= 5u * seconds * 42u &&
           byClass[1] >= 7u * seconds * 10u && byClass[2] >= 3u * seconds * 8u &&
           byClass[3] >= 50u * seconds * 8u && byClass[4] >= 10u * seconds);
    std::printf("75 NPC, %ds, one recipient, 80 bones, estimated unreliable-only/no batching/control: "
                "old %llu sends, %llu datagrams, %llu UDP-payload bytes; "
                "policy %llu sends, %llu datagrams, %llu UDP-payload bytes; savings %.2f%%. "
                "Policy class sends combat=%llu near-moving=%llu far-moving=%llu animated-idle=%llu static=%llu\n",
                seconds, static_cast<unsigned long long>(oldSends),
                static_cast<unsigned long long>(oldSends * fragments), static_cast<unsigned long long>(oldSends * udpBytes),
                static_cast<unsigned long long>(sends), static_cast<unsigned long long>(sends * fragments),
                static_cast<unsigned long long>(sends * udpBytes), 100.0 * (1.0 - double(sends) / oldSends),
                static_cast<unsigned long long>(byClass[0]), static_cast<unsigned long long>(byClass[1]),
                static_cast<unsigned long long>(byClass[2]), static_cast<unsigned long long>(byClass[3]),
                static_cast<unsigned long long>(byClass[4]));
}

static void TestWarmupAndUrgentHold()
{
    Fixture f(1);
    f.in.moving = true;
    assert(Commit(f.state, f.in, Decision::Initial));
    assert(PresentationInterval(f.state, f.in) == OrdinaryIntervalMs);
    for (uint32_t now : {16u, 32u, 100u, 199u}) {
        f.in.now = now;
        assert(f.Eval().tier == Tier::Fast);
    }
    f.in.now = InterestWarmupMs;
    assert(f.Eval().tier == Tier::Ordinary);
    f.in.combat = true;
    f.in.now++;
    f.Send(Decision::Semantic);
    f.in.combat = false;
    assert(PresentationInterval(f.state, f.in) == OrdinaryIntervalMs);
    f.in.now += UrgentHoldMs - 1;
    assert(f.Eval().tier == Tier::Fast);
    ++f.in.now;
    assert(f.Eval().tier == Tier::Ordinary);
    // Large ordinary skeletal motion cannot bypass the pacing interval.
    f.in.now += 100;
    f.Send(Decision::Keyframe);
    f.record.motions[0].phase += .01f;
    f.in.now += OrdinaryIntervalMs;
    f.Send(Decision::Changed);
    ++f.in.now;
    f.bones[0].values[1] += BoneBasisDiscontinuity;
    assert(f.Eval().decision == Decision::Wait);
    f.record.hitPoints++;
    assert(f.Eval().decision == Decision::Semantic && f.Eval().essential);
}

static void TestNearbyIdleAndOrdinaryExit()
{
    Fixture f(80);
    f.Send(Decision::Initial);
    f.bones[0].values[9] += 4;
    assert(PresentationInterval(f.state, f.in) == IdleIntervalMs);
    f.in.now = IdleIntervalMs - 1;
    assert(f.Eval().decision == Decision::Wait && f.Eval().tier == Tier::Idle);
    ++f.in.now;
    f.Send(Decision::Changed);
    f.in.moving = true;
    f.record.position[0] += 1;
    f.in.now += OrdinaryIntervalMs;
    f.Send(Decision::Changed);
    f.in.moving = false;
    // Announce 200 ms buffering while ordinary captures continue for two seconds.
    assert(PresentationInterval(f.state, f.in) == IdleIntervalMs);
    f.in.now += OrdinaryHoldMs - 1;
    assert(f.Eval().tier == Tier::Ordinary);
    ++f.in.now;
    assert(f.Eval().tier == Tier::Idle);
    f.Send(Decision::Keyframe);
    f.bones[0].values[9] += 4;
    f.in.now += IdleIntervalMs - 1;
    assert(f.Eval().decision == Decision::Wait);
    ++f.in.now;
    f.Send(Decision::Changed);
}

static void TestSendOwnedCandidateUnion()
{
    using npcsend_candidates::Candidate;
    const std::string actorA(16, 'A');
    const std::string actorB(16, 'B');
    const std::string player(16, 'P');
    const std::string staticEntity(16, 'S');
    const std::string zero(16, '\0');
    std::vector<Candidate> candidates = {
        {actorA, true, false, false, false, false},
        {actorB, true, false, false, false, false},
        {actorA, true, false, false, false, false},
        {player, true, true, false, false, false},
        {staticEntity, false, false, false, false, false},
        {zero, true, false, false, false, true},
    };
    const std::vector<std::string> result = npcsend_candidates::Union(candidates);
    assert(result.size() == 2);
    assert(result[0] == actorA && result[1] == actorB);

    using npcsend_candidates::GuidCandidate;
    GuidKey gA{}, gB{}, gP{}, gS{}, gZ{};
    gA.fill(0xAA); gB.fill(0xBB); gP.fill(0xCC); gS.fill(0xDD);
    std::vector<GuidCandidate> guidCandidates = {
        {gA, true, false, false, false, false},
        {gB, true, false, false, false, false},
        {gA, true, false, false, false, false},
        {gP, true, true, false, false, false},
        {gS, false, false, false, false, false},
        {gZ, true, false, false, false, true},
    };
    const std::vector<GuidKey> guidResult = npcsend_candidates::Union(guidCandidates);
    assert(guidResult.size() == 2 && guidResult[0] == gA && guidResult[1] == gB);
}

static void TestPacedCounter()
{
    net::detail::BandwidthCounters counters;
    counters.Npc(net::NpcSendDecision::SkippedPaced, 1);
    assert(counters.considered == 1 && counters.skippedPaced == 1 && !counters.sent && !counters.sendFailed);
}

static void TestPhaseCarryingDeadlines()
{
    Fixture ordinary;
    ordinary.in.moving = true;
    ordinary.Send(Decision::Initial);
    Stats stats;
    uint32_t previous = 0, count = 0;
    for (uint32_t now = 22; now <= 10000; now += 22) {
        ordinary.in.now = now;
        ordinary.record.position[0] += 1;
        const auto result = ordinary.Eval();
        assert(result.tier == Tier::Ordinary);
        if (result.decision == Decision::Wait) continue;
        assert(result.decision == Decision::Changed);
        assert(now - previous == (count % 2 == 0 ? 44u : 22u));
        stats.Record(result.decision, result.tier, now - previous, ordinary.in.stretchPermille, true);
        assert(Commit(ordinary.state, ordinary.in, result.decision));
        previous = now;
        ++count;
    }
    assert(count > 300);
    assert(std::fabs(double(previous) / count - OrdinaryIntervalMs) <= 1.0);
    assert(stats.tiers[1].count == count && stats.tiers[1].minMs == 22 && stats.tiers[1].maxMs == 44);
    assert(std::fabs(stats.tiers[1].AverageMs() - OrdinaryIntervalMs) <= 1.0 && stats.edges == 0);

    Fixture hitch;
    hitch.in.moving = true;
    hitch.Send(Decision::Initial);
    hitch.record.position[0] = 1;
    hitch.in.now = 44;
    hitch.Send(Decision::Changed);
    assert(hitch.state.idealAt == 33);
    hitch.record.position[0] = 2;
    hitch.in.now = 133;
    hitch.Send(Decision::Changed);
    assert(hitch.state.idealAt == 133);
    hitch.record.position[0] = 3;
    hitch.in.now = 165;
    assert(hitch.Eval().decision == Decision::Wait);
    ++hitch.in.now;
    hitch.Send(Decision::Changed);

    for (uint32_t frame : {16u, 22u}) {
        Fixture fast;
        fast.in.combat = true;
        fast.Send(Decision::Initial);
        for (uint32_t now = frame; now <= 1000; now += frame) {
            fast.in.now = now;
            fast.record.position[0] += 1;
            assert(fast.Eval().tier == Tier::Fast);
            fast.Send(Decision::Changed);
        }
    }
}

static void TestUnchangedPresentationFreshness()
{
    static_assert(npclease::PresentationGraceMs == 350);
    for (uint32_t stretch : {StretchNormal, 2000u, StretchMax}) {
        for (Tier tier : {Tier::Fast, Tier::Ordinary, Tier::MovingFar, Tier::Idle}) {
            for (uint32_t frame : {1u, 16u, 22u, 33u, 66u, 100u}) {
                Fixture f(1);
                f.in.stretchPermille = stretch;
                f.in.moving = tier == Tier::Fast || tier == Tier::Ordinary || tier == Tier::MovingFar;
                f.in.distanceCm = tier == Tier::MovingFar ? 3000 : 1000;
                f.in.focused = tier == Tier::Fast;
                f.Send(Decision::Initial);

                uint32_t lastSend = 0;
                for (uint32_t now = 1; now <= 3000; ++now) {
                    f.in.now = now;
                    const uint32_t interval = PresentationInterval(f.state, f.in);
                    assert(npclease::PresentationFresh(true, false, now - lastSend, interval));
                    if (now % frame) continue;
                    const Result result = f.Eval();
                    if (result.decision == Decision::Keyframe) {
                        const uint32_t gap = now - lastSend;
                        assert(npclease::PresentationFresh(true, false, gap, interval));
                        assert(gap <= npclease::PresentationGraceMs);
                        assert(Commit(f.state, f.in, Decision::Keyframe));
                        lastSend = now;
                    } else {
                        assert(result.decision == Decision::Wait);
                    }
                }
                assert(lastSend >= 3000 - KeepaliveMs - frame);
            }
        }
    }

    Fixture boundary(1);
    boundary.in.stretchPermille = StretchMax;
    boundary.Send(Decision::Initial);
    boundary.in.now = KeepaliveMs - 1;
    assert(boundary.Eval().decision == Decision::Wait);
    ++boundary.in.now;
    assert(boundary.Eval().decision == Decision::Keyframe);
}

static void TestChangingSlowAtMaximumStretch()
{
    for (Tier tier : {Tier::MovingFar, Tier::Idle}) {
        Fixture f(1);
        f.in.distanceCm = 3000;
        f.in.moving = tier == Tier::MovingFar;
        f.in.stretchPermille = StretchMax;
        f.Send(Decision::Initial);
        assert(PresentationInterval(f.state, f.in) == 100);
        assert(npclease::PresentationFresh(true, false, 400, PresentationInterval(f.state, f.in)));
        assert(npclease::PresentationFresh(true, false, 500, PresentationInterval(f.state, f.in)));
        assert(!npclease::PresentationFresh(true, false, 501, PresentationInterval(f.state, f.in)));
        Stats stats;
        uint32_t lastSend = 0;
        for (uint32_t now = 22; now <= 5000; now += 22) {
            f.in.now = now;
            f.bones[0].values[9] += 1;
            const Result result = f.Eval();
            assert(result.tier == tier);
            if (result.decision == Decision::Wait) continue;
            assert(result.decision == Decision::Changed);
            const bool due = now - f.state.idealAt >= Stretched(Interval(tier), StretchMax);
            assert(due);
            stats.Record(result.decision, tier, now - lastSend, StretchMax, due);
            assert(Commit(f.state, f.in, result.decision));
            lastSend = now;
        }
        assert(stats.tiers[2].count >= 12 && stats.heartbeats == 0 && stats.edges == 0);
        assert(std::fabs(stats.tiers[2].AverageMs() - 400.0) <= 2.0);
        assert(stats.tiers[2].overdue == 0);
    }

    Fixture pending(1);
    pending.in.stretchPermille = StretchMax;
    pending.Send(Decision::Initial);
    pending.bones[0].values[9] += 1;
    pending.in.now = KeepaliveMs;
    assert(pending.Eval().decision == Decision::Wait);
    pending.in.now = 400;
    pending.Send(Decision::Changed);

    Fixture semantic;
    semantic.in.stretchPermille = StretchMax;
    semantic.Send(Decision::Initial);
    semantic.record.movementMode++;
    semantic.in.now = KeepaliveMs;
    assert(semantic.Eval().decision == Decision::Wait);
    semantic.in.now = 400;
    semantic.Send(Decision::Semantic);
}

static void TestStretchScaling()
{
    for (Tier tier : {Tier::Ordinary, Tier::MovingFar, Tier::Idle}) {
        Fixture f;
        f.in.moving = tier != Tier::Idle;
        f.in.distanceCm = tier == Tier::MovingFar ? 3000 : 1000;
        f.in.stretchPermille = 2000;
        f.Send(Decision::Initial);
        f.record.position[0] = 1;
        f.in.now = 2 * Interval(tier) - 1;
        assert(f.Eval().tier == tier && f.Eval().decision == Decision::Wait);
        ++f.in.now;
        assert(f.Eval().tier == tier);
        f.Send(Decision::Changed);
    }
    Fixture heartbeat;
    heartbeat.in.stretchPermille = 2000;
    heartbeat.Send(Decision::Initial);
    heartbeat.in.now = KeepaliveMs - 1;
    assert(heartbeat.Eval().decision == Decision::Wait);
    ++heartbeat.in.now;
    heartbeat.Send(Decision::Keyframe);

    Fixture defaultStretch, explicitStretch;
    defaultStretch.in.moving = explicitStretch.in.moving = true;
    explicitStretch.in.stretchPermille = 1000;
    defaultStretch.Send(Decision::Initial);
    explicitStretch.Send(Decision::Initial);
    for (uint32_t now = 11; now <= 10000; now += 11) {
        defaultStretch.in.now = explicitStretch.in.now = now;
        defaultStretch.record.position[0] += 1;
        explicitStretch.record.position[0] += 1;
        const auto a = defaultStretch.Eval(), b = explicitStretch.Eval();
        assert(a.decision == b.decision && a.tier == b.tier);
        if (a.decision != Decision::Wait) {
            assert(Commit(defaultStretch.state, defaultStretch.in, a.decision));
            assert(Commit(explicitStretch.state, explicitStretch.in, b.decision));
            assert(defaultStretch.state.idealAt == explicitStretch.state.idealAt);
        }
    }
}

static void TestMaxStaleAndStats()
{
    assert(MaxStale(Tier::Fast) == 100 && MaxStale(Tier::Ordinary) == 150);
    assert(MaxStale(Tier::MovingFar) == 300 && MaxStale(Tier::Idle) == 300);
    assert(MaxStale(Tier::Fast, 2000) == 200 && MaxStale(Tier::Ordinary, 2000) == 300);
    assert(MaxStale(Tier::Idle, 2000) == 600);
    assert(Stretched(33, 1001) == 34 && Stretched(33, 0) == 33 && Stretched(33, 5000) == 132);

    Stats stats;
    stats.Record(Decision::Initial, Tier::Fast, 0, 1000, false);
    stats.Record(Decision::Keyframe, Tier::Idle, 2000, 2000, false);
    stats.Record(Decision::Semantic, Tier::Fast, 15, 1000, false);
    stats.Record(Decision::Semantic, Tier::Ordinary, 1000, 1000, false);
    stats.Record(Decision::Changed, Tier::Fast, 16, 1000, true);
    stats.Record(Decision::Changed, Tier::Fast, 100, 1000, true);
    stats.Record(Decision::Changed, Tier::Fast, 101, 1000, true);
    stats.Record(Decision::Changed, Tier::Ordinary, 22, 1000, true);
    stats.Record(Decision::Changed, Tier::Ordinary, 44, 1000, true);
    stats.Record(Decision::Changed, Tier::Ordinary, 66, 2000, true);
    stats.Record(Decision::Semantic, Tier::Ordinary, 300, 2000, true);
    stats.Record(Decision::Changed, Tier::Ordinary, 301, 2000, true);
    stats.Record(Decision::Changed, Tier::MovingFar, 100, 1000, true);
    stats.Record(Decision::Changed, Tier::Idle, 300, 1000, true);
    stats.Record(Decision::Changed, Tier::Idle, 301, 1000, true);
    stats.Record(Decision::Wait, Tier::Fast, 500, 1000, true);
    stats.Record(Decision::FinalAndRelease, Tier::Fast, 500, 1000, true);
    assert(stats.initials == 1 && stats.heartbeats == 1 && stats.edges == 2);
    assert(stats.tiers[0].count == 3 && stats.tiers[0].overdue == 1);
    assert(stats.tiers[0].minMs == 16 && stats.tiers[0].maxMs == 101 && stats.tiers[0].sumMs == 217);
    assert(std::fabs(stats.tiers[0].AverageMs() - 217.0 / 3) < 0.001);
    assert(stats.tiers[1].count == 5 && stats.tiers[1].overdue == 1 && stats.tiers[1].sumMs == 733);
    assert(stats.tiers[1].minMs == 22 && stats.tiers[1].maxMs == 301);
    assert(stats.tiers[2].count == 3 && stats.tiers[2].overdue == 1 && stats.tiers[2].sumMs == 701);
}

static void TestInitialPacer()
{
    InitialPacer pacer;
    unsigned admitted = 0;
    for (; admitted < InitialBurst; ++admitted) assert(pacer.Admit(0, 1000));
    assert(!pacer.Admit(0, 1000));
    uint32_t finishedAt = 0;
    for (uint32_t now = 22; admitted < 75 && now <= 1300; now += 22) {
        while (admitted < 75 && pacer.Admit(now, 1000)) ++admitted;
        finishedAt = now;
    }
    assert(admitted == 75 && finishedAt <= 1300);
    assert(pacer.active && pacer.admittedInBurst == 75 - InitialBurst);
    const uint32_t lastDeferred = pacer.lastDeferredAt;
    pacer.Poll(lastDeferred + InitialBurstQuietMs - 1);
    assert(pacer.active && pacer.bursts == 0);
    pacer.Poll(lastDeferred + InitialBurstQuietMs);
    assert(!pacer.active && pacer.bursts == 1);
    assert(pacer.lastBurstMs == lastDeferred && pacer.lastBurstNpcs == 75 - InitialBurst);
    pacer.Reset();
    assert(!pacer.started && !pacer.active && pacer.bursts == 0);
    for (unsigned i = 0; i < InitialBurst; ++i) assert(pacer.Admit(2000, 1000));
    assert(!pacer.Admit(2000, 1000));

    for (uint32_t stretch : {1000u, 2000u}) {
        InitialPacer rate;
        for (unsigned i = 0; i < InitialBurst; ++i) assert(rate.Admit(0, stretch));
        unsigned extra = 0;
        for (uint32_t now = 20; now <= 1000; now += 20)
            while (rate.Admit(now, stretch)) ++extra;
        assert(extra == (stretch == 1000 ? 60u : 30u));
    }
}

static void TestImmediateCombatPublication()
{
    Fixture f;
    f.in.distanceCm = NearExitCm;
    assert(ImmediateInitial(f.in));
    f.in.distanceCm = NearExitCm + 1;
    assert(!ImmediateInitial(f.in));
    f.in.focused = true;
    assert(ImmediateInitial(f.in));
    f.in.focused = false;
    f.in.combat = true;
    assert(ImmediateInitial(f.in));
    f.in.combat = false;
    f.in.contact = true;
    assert(ImmediateInitial(f.in));
    f.in.contact = false;
    f.record.partyPlayer = 2;
    assert(ImmediateInitial(f.in));
    f.record.partyPlayer = 0;
    f.record.guidePlayer = 2;
    assert(ImmediateInitial(f.in));
    f.record.guidePlayer = 0;
    f.record.contact = 1;
    assert(ImmediateInitial(f.in));
    f.record.contact = 0;
    f.in.distanceCm = 100;
    f.record.attitude = 4;
    f.record.aiMode = 3;
    f.record.partyPlayer = 2;
    f.record.guidePlayer = 2;
    f.record.combatTarget = 123;
    assert(f.Eval().decision == Decision::Initial && f.Eval().essential);
    f.Send(Decision::Initial);
    assert(f.state.record.attitude == 4 && f.state.record.aiMode == 3 &&
           f.state.record.partyPlayer == 2 && f.state.record.guidePlayer == 2 &&
           f.state.record.combatTarget == 123);
    f.state = State{}; // reconnect/resync/interest reentry cannot inherit an incomplete initial record
    assert(f.Eval().decision == Decision::Initial && f.Eval().essential && ImmediateInitial(f.in));
    f.in.record = nullptr;
    assert(!ImmediateInitial(f.in));
}

int main()
{
    TestSendOwnedCandidateUnion();
    static_assert(QuietCadenceMs == 250 && MirrorStaleMs == 10000 && PoseRetentionMs == 10000);
    static_assert(MovementPositionCm == 0.02f && MovementQuaternionComponent == 0.00025f && MovementHoldMs == 500);
    TestInitialAndHysteresis();
    TestGatesAndInvalid();
    TestCadenceAndUrgency();
    TestDiscreteFields();
    TestReliableRetry();
    TestRetargetBeforeNextCadence();
    TestDynamicAndSentBaseline();
    TestIndependentStatesAndWrap();
    TestLostSnapshotSelfHeals();
    TestRotatedBoneSensitivity();
    TestWarmupAndUrgentHold();
    TestNearbyIdleAndOrdinaryExit();
    TestPacedCounter();
    TestPhaseCarryingDeadlines();
    TestUnchangedPresentationFreshness();
    TestChangingSlowAtMaximumStretch();
    TestStretchScaling();
    TestMaxStaleAndStats();
    TestInitialPacer();
    TestImmediateCombatPublication();
    TestSimulation();
}
