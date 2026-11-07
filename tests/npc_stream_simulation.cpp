// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free simulation of the host NPC stream for one client: 75 NPCs, a loading client followed by a join burst, the real
// scheduler (npcsend), the real pose codec (npcwire_pose.h) and the real new budget valve, against a frozen copy of the
// protocol-75 build-18 budget that gated the stream and starved it. Deterministic; no clock, network or engine.
#include "../src/npcsend_policy.h"
#include "../src/npcwire_pose.h"
#include "../src/netbudget.h"
#include "support/netbudget_v75_legacy.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

using namespace g3t;
using namespace g3t::npcwire::bones;

static void Check(bool condition, char const* expression, int line, char const* format = nullptr, ...)
{
    if (condition) return;
    std::printf("CHECK FAILED line %d: %s", line, expression);
    if (format) {
        std::printf(" [");
        va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
        std::printf("]");
    }
    std::printf("\n");
    std::fflush(stdout);
    std::exit(1);
}
#define CHECK(condition, ...) Check((condition), #condition, __LINE__, ##__VA_ARGS__)

namespace {
constexpr int Bones = 80;
constexpr uint32_t OneWayMs = 12, LoadingMs = 3000, SteadyFromMs = 4000, SteadyForMs = 16000;
constexpr size_t FullPoseBytes = 3 + Bones * sizeof(pose::Bone);

enum class Kind { Fighter, Walker, Idle, Static, Far };
struct Mix { int fighters, walkers, idle, statics, far; };
struct Episode { uint32_t from, to, throttle; uint64_t queue; };

struct Npc {
    Kind kind = Kind::Static;
    net::NpcRecord record;
    npcsend::State policy;
    PoseCache sender, receiver;
    std::vector<pose::Bone> bones = std::vector<pose::Bone>(Bones);
    uint32_t token = 0, baselineAckedAt = 0;
    uint8_t baselineAni = 0;
    double amplitude[Bones] = {}, phase[Bones] = {};
    std::vector<uint32_t> commits, arrivals;
    uint32_t firstUpdateAt = 0;
};

struct Params {
    bool legacy = false;
    Mix mix{5, 35, 15, 15, 5};
    std::vector<Episode> episodes;
    uint32_t steadyEndMs = LoadingMs + SteadyFromMs + SteadyForMs;
};

struct Result {
    double bytesPerSecond = 0, meanBytesPerUpdate = 0;
    uint64_t keys = 0, deltas = 0, refused = 0, paced = 0, firstFrameBytes = 0, maxWindow100 = 0, burstWindow100 = 0;
    std::vector<double> rates;       // per animated NPC, steady window
    std::vector<uint32_t> maxGaps;   // per animated NPC, steady window (host commits)
    std::vector<uint32_t> clientMaxGap;
    std::vector<uint32_t> firstUpdate;  // ms after world entry, per animated NPC
    std::vector<Kind> kinds;
    std::vector<uint32_t> counts;
    npcsend::Stats steady, episode[4];
    npcsend::InitialPacer pacer;
    uint32_t maxStretch = 1000, stretchBackTo1000After[4] = {};
    uint32_t refreshAllAfterBurstMs = 0;
};

uint64_t Pack(std::vector<size_t> const& commands, uint64_t& datagrams)
{
    uint64_t total = 0;
    size_t current = 0;
    auto flush = [&] { if (current) { total += current + 16; ++datagrams; current = 0; } };
    for (size_t payload : commands) {
        for (size_t offset = 0; offset < payload; offset += 1172) {
            const size_t part = std::min<size_t>(1172, payload - offset), header = payload > 1172 ? 24 : 8;
            if (!current) current = 4;
            if (current + header + part > 1200) { flush(); current = 4; }
            current += header + part;
        }
    }
    flush();
    return total;
}

void MakePose(Npc& n, uint32_t tMs)
{
    const double t = tMs / 1000.0;
    const double f = n.kind == Kind::Fighter ? 2.0 : n.kind == Kind::Idle ? 0.25 : n.kind == Kind::Static ? 0.0 : 1.0;
    for (int b = 0; b < Bones; ++b) {
        const double a[3] = {n.amplitude[b] * std::sin(6.283185307 * f * t + n.phase[b]),
                             n.amplitude[b] * std::sin(6.283185307 * f * t + n.phase[b] + 1.3),
                             n.amplitude[b] * std::sin(6.283185307 * f * t + n.phase[b] + 2.6)};
        const double cz = std::cos(a[0]), sz = std::sin(a[0]), cy = std::cos(a[1]), sy = std::sin(a[1]);
        const double cx = std::cos(a[2]), sx = std::sin(a[2]);
        // Rz * Ry * Rx
        const double m[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                             sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                             -sy, cy * sx, cy * cx};
        pose::Transform bone;
        bone.name = static_cast<uint32_t>(b + 1);
        for (int k = 0; k < 9; ++k) bone.values[k] = static_cast<float>(m[k]);
        bone.values[9] = static_cast<float>(b * 0.5);
        bone.values[10] = static_cast<float>(n.amplitude[b] * 3.0 * std::sin(6.283185307 * f * t + n.phase[b]));
        bone.values[11] = static_cast<float>(b * 0.1);
        CHECK(pose::Encode(bone, n.bones[b]));
    }
}

Result Run(Params const& p)
{
    using Legacy = net_legacy_v75::PeerBudget;
    std::mt19937 random(75), jitter(1975);
    Dictionary dictionary;
    CHECK(dictionary.Reset(1, 1) == Status::Ok);
    std::vector<uint32_t> names;
    for (int i = 0; i < Bones; ++i) names.push_back(static_cast<uint32_t>(i + 1));
    CHECK(dictionary.Register(1, names.data(), names.size()) == Status::Ok);

    std::vector<Npc> npcs;
    auto add = [&](Kind kind, int count) {
        for (int i = 0; i < count; ++i) {
            npcs.emplace_back();
            Npc& n = npcs.back();
            n.kind = kind;
            for (int b = 0; b < Bones; ++b) {
                const double u = std::uniform_real_distribution<double>(0.0, 1.0)(random);
                const double limb = b < 4 ? 0.2 : b < 48 ? 0.5 : 0.06;
                const double walk = b < 4 ? 0.12 : b < 48 ? 0.4 : 0.03;
                const double idle = b < 4 ? 0.025 : b < 48 ? 0.01 : 0.004;
                const double base = kind == Kind::Fighter ? limb : kind == Kind::Idle ? idle : kind == Kind::Static ? 0.0 : walk;
                n.amplitude[b] = base * (0.7 + 0.3 * u);
                n.phase[b] = u * 6.28;
            }
            n.record.guid[0] = static_cast<uint8_t>(npcs.size());
            n.record.rotation[3] = 1;
            n.record.hitPointsMax = n.record.hitPoints = 100;
        }
    };
    add(Kind::Fighter, p.mix.fighters); add(Kind::Walker, p.mix.walkers); add(Kind::Idle, p.mix.idle);
    add(Kind::Static, p.mix.statics); add(Kind::Far, p.mix.far);

    Legacy legacy;
    net_legacy_v75::BudgetConfig legacyConfig;
    legacy.Reset(0, legacyConfig);
    net::PeerBudget valve;
    valve.Reset(0, net::BudgetConfig{});
    npcsend::InitialPacer pacer;
    Result result;
    const uint32_t entry = LoadingMs;
    const uint32_t steadyFrom = entry + SteadyFromMs, steadyTo = steadyFrom + SteadyForMs;
    std::deque<std::pair<uint32_t, std::pair<size_t, uint32_t>>> acks;  // due, npc, token
    std::vector<uint64_t> windows(p.steadyEndMs / 100 + 2, 0);
    uint64_t steadyBytes = 0, steadyDatagrams = 0, updates = 0, boneBytes = 0;
    uint32_t dictionaryAckAt = UINT32_MAX;
    bool dictionaryOffered = false, stretchWasRaised[4] = {};
    std::mt19937 loss(7);
    const auto lossy = [&](int percent) { return int(loss() % 100) < percent; };

    for (uint32_t now = 0; now < p.steadyEndMs;) {
        const uint32_t next = now + 22 + static_cast<uint32_t>(jitter() % 7) - 3;
        const bool entered = now >= entry;
        while (!acks.empty() && acks.front().first <= now) {
            npcs[acks.front().second.first].sender.Ack(acks.front().second.second, 1);
            acks.pop_front();
        }

        // Budgets see the loading client's huge RTT with an empty queue; the valve additionally sees congestion episodes.
        const uint32_t rtt = !entered ? 100 + (now * 1300 / LoadingMs) : 25;
        uint32_t throttle = 32;
        uint64_t queue = 0;
        for (Episode const& e : p.episodes) if (now >= e.from && now < e.to) { throttle = e.throttle; queue = e.queue; }
        net::BudgetTelemetry sample;
        sample.rttMs = rtt; sample.validRtt = true; sample.throttle = throttle; sample.queuedPayloadBytes = queue;
        sample.packetsSent = 1000; sample.packetsLost = 0; sample.validLoss = true;
        valve.Advance(now, sample);
        net_legacy_v75::BudgetTelemetry old;
        old.rttMs = rtt; old.referenceRttMs = 25; old.validRtt = true;
        legacy.Advance(now, old);
        const uint32_t stretch = p.legacy ? 1000u : valve.NpcStretchPermille();
        result.maxStretch = std::max(result.maxStretch, stretch);
        for (size_t e = 0; e < p.episodes.size() && e < 4; ++e) {
            if (stretch > 1000 && now >= p.episodes[e].from) stretchWasRaised[e] = true;
            if (stretchWasRaised[e] && !result.stretchBackTo1000After[e] && now >= p.episodes[e].to && stretch == 1000)
                result.stretchBackTo1000After[e] = now - p.episodes[e].to;
        }

        std::vector<size_t> commands;
        if (entered) {
            for (size_t i = 0; i < npcs.size(); ++i) {
                Npc& n = npcs[i];
                const bool fighting = n.kind == Kind::Fighter;
                const bool moving = n.kind == Kind::Fighter || n.kind == Kind::Walker || n.kind == Kind::Far;
                n.record.sent = now;
                if (moving) n.record.position[0] += 0.14f * (next - now);
                if (n.kind != Kind::Static) {
                    n.record.motions[0].name = n.record.motions[0].instance = 1;
                    n.record.motions[0].active = 1;
                    n.record.motions[0].weight = 1;
                    n.record.motions[0].phase = std::fmod(float(now) / (fighting ? 500.0f : 1000.0f), 1.0f);
                }
                if (fighting) {
                    n.record.contact = ((now + i * 37) % 1000) < 100 ? 1 : 0;  // two reliable edges per second
                    n.record.aniState = static_cast<uint8_t>(((now + i * 91) / 500) % 2 ? 4 : 5);
                }
                MakePose(n, now);
                npcsend::Input in;
                in.record = &n.record; in.bones = n.bones.data(); in.boneCount = Bones; in.now = now;
                in.distanceCm = n.kind == Kind::Far ? 4000.0f : 1500.0f;
                in.combat = fighting; in.contact = n.record.contact != 0; in.moving = moving;
                in.stretchPermille = stretch;
                const auto decision = npcsend::Evaluate(n.policy, in);
                if (decision.decision == npcsend::Decision::Wait) continue;
                if (decision.decision == npcsend::Decision::Outside || decision.decision == npcsend::Decision::Invalid) continue;
                if (!p.legacy && decision.decision == npcsend::Decision::Initial && !pacer.Admit(now, stretch)) {
                    ++result.paced;
                    continue;
                }
                if (p.legacy && !decision.essential) {
                    const auto cls = decision.tier == npcsend::Tier::Fast || decision.tier == npcsend::Tier::Ordinary
                        ? net_legacy_v75::BudgetClass::NpcActive : net_legacy_v75::BudgetClass::NpcFarIdle;
                    const uint32_t estimate = static_cast<uint32_t>(sizeof(net::NpcRecord) + 32 + Bones * sizeof(pose::Bone));
                    const auto verdict = legacy.Preview(cls, net_legacy_v75::BudgetCharge(estimate, 1200, 16, 0));
                    legacy.RecordDecision(verdict);
                    if (verdict != net_legacy_v75::BudgetDecision::Allow) { ++result.refused; continue; }
                }
                const bool compact = now >= dictionaryAckAt;
                if (!dictionaryOffered) { dictionaryOffered = true; dictionaryAckAt = now + 2 * OneWayMs + 1; }
                std::vector<size_t> own;
                const bool baselineChanged = n.baselineAckedAt == 0 || n.baselineAni != n.record.aniState;
                if (decision.essential || baselineChanged) {
                    if (decision.essential) own.push_back(611);
                    own.push_back(37); own.push_back(650);
                    n.baselineAckedAt = now + 2 * OneWayMs;
                    n.baselineAni = n.record.aniState;
                }
                const bool controlReady = n.baselineAckedAt != 0 && now >= n.baselineAckedAt;
                std::vector<uint8_t> bytes;
                bool key = true;
                size_t poseBytes = FullPoseBytes;
                if (compact) {
                    CHECK(n.sender.Encode(dictionary, 1, 1, 0, n.bones.data(), Bones, now, bytes, key) == Status::Ok);
                    poseBytes = bytes.size();
                    ++(key ? result.keys : result.deltas);
                    boneBytes += poseBytes;
                } else {
                    ++result.keys;
                    boneBytes += poseBytes;
                }
                const size_t pose = 23 + 2 + (compact && controlReady ? 65 : sizeof(net::NpcRecord)) + poseBytes;
                own.push_back(compact ? pose : 23 + sizeof(net::NpcRecord) + 4 + FullPoseBytes);
                for (size_t bytesOfCommand : own) {
                    const auto charge = net_legacy_v75::BudgetCharge(bytesOfCommand, 1200, 16, 0);
                    legacy.CommitQueued(net_legacy_v75::BudgetClass::NpcActive, charge);
                    valve.CommitQueued(net::BudgetClass::NpcActive, net::BudgetCharge(bytesOfCommand, 1200, 16, 0));
                    commands.push_back(bytesOfCommand);
                }
                const uint32_t previous = n.policy.sentAt;
                const bool onSchedule = now - n.policy.idealAt >= npcsend::Stretched(npcsend::Interval(decision.tier), stretch);
                CHECK(npcsend::Commit(n.policy, in, decision.decision));
                if (!n.firstUpdateAt) n.firstUpdateAt = now;
                n.commits.push_back(now);
                if (!p.legacy) {
                    if (now >= steadyFrom && now < steadyTo) result.steady.Record(decision.decision, decision.tier, now - previous, stretch, onSchedule);
                    for (size_t e = 0; e < p.episodes.size() && e < 4; ++e)
                        if (now >= p.episodes[e].from + 400 && now < p.episodes[e].to)
                            result.episode[e].Record(decision.decision, decision.tier, now - previous, stretch, onSchedule);
                }
                if (compact) {
                    const uint32_t id = ++n.token;
                    CHECK(n.sender.Store(id, 1, 1, 0, n.bones.data(), Bones, now, key));
                    if (!lossy(3)) {
                        std::vector<pose::Bone> decoded;
                        uint16_t layout = 0;
                        CHECK(n.receiver.Decode(dictionary, bytes.data(), bytes.size(), 1, 0, decoded, layout) == Status::Ok);
                        CHECK(decoded.size() == n.bones.size());
                        n.receiver.Store(id, 1, 1, 0, decoded.data(), Bones, now, false);
                        n.arrivals.push_back(now + OneWayMs);
                        if (!lossy(3)) acks.push_back({now + 2 * OneWayMs, {i, id}});
                    }
                } else {
                    n.arrivals.push_back(now + OneWayMs);
                }
                ++updates;
                (void)boneBytes;
            }
            pacer.Poll(now);
        }
        uint64_t datagrams = 0;
        const uint64_t wire = Pack(commands, datagrams);
        if (entered) {
            windows[(now - entry) / 100] += wire;
            if (now - entry < 25) result.firstFrameBytes += wire;
            if (now >= steadyFrom && now < steadyTo) { steadyBytes += wire; steadyDatagrams += datagrams; }
        }
        now = next;
    }
    (void)steadyDatagrams;
    for (uint64_t w : windows) result.maxWindow100 = std::max(result.maxWindow100, w);
    for (size_t w = 0; w < 15 && w < windows.size(); ++w) result.burstWindow100 = std::max(result.burstWindow100, windows[w]);
    result.bytesPerSecond = steadyBytes * 1000.0 / SteadyForMs;
    result.meanBytesPerUpdate = updates ? double(boneBytes) / updates : 0.0;
    result.pacer = pacer;
    uint32_t lastFirst = 0;
    for (Npc const& n : npcs) {
        if (n.kind == Kind::Static) continue;
        uint32_t count = 0, gap = 0, last = 0, clientGap = 0, clientLast = 0;
        for (uint32_t t : n.commits) {
            if (t < steadyFrom || t >= steadyTo) continue;
            ++count;
            if (last) gap = std::max(gap, t - last);
            last = t;
        }
        for (uint32_t t : n.arrivals) {
            if (t < steadyFrom || t >= steadyTo) continue;
            if (clientLast) clientGap = std::max(clientGap, t - clientLast);
            clientLast = t;
        }
        result.rates.push_back(count * 1000.0 / SteadyForMs);
        result.counts.push_back(count);
        result.kinds.push_back(n.kind);
        result.maxGaps.push_back(gap);
        result.clientMaxGap.push_back(clientGap);
        result.firstUpdate.push_back(n.firstUpdateAt ? n.firstUpdateAt - entry : UINT32_MAX);
        lastFirst = std::max(lastFirst, n.firstUpdateAt ? n.firstUpdateAt - entry : UINT32_MAX);
    }
    result.refreshAllAfterBurstMs = lastFirst;
    return result;
}

void Print(char const* name, Result const& r)
{
    double minRate = 1e9, maxRate = 0, sum = 0;
    uint32_t worstGap = 0, worstClient = 0;
    for (size_t i = 0; i < r.rates.size(); ++i) {
        minRate = std::min(minRate, r.rates[i]); maxRate = std::max(maxRate, r.rates[i]); sum += r.rates[i];
        worstGap = std::max(worstGap, r.maxGaps[i]); worstClient = std::max(worstClient, r.clientMaxGap[i]);
    }
    std::printf("STREAM-SIM %s: per-NPC achieved Hz min/avg/max %.2f/%.2f/%.2f, worst host gap %u ms, worst client gap %u ms, "
                "refused %llu, paced %llu, bytes/s %.0f (envelope incl.), mean pose bytes/update %.1f keys=%llu deltas=%llu, "
                "first-frame bytes %llu, max 100 ms window %llu (first 1.5 s: %llu), last first-update %u ms, max stretch %u\n",
                name, minRate, r.rates.empty() ? 0.0 : sum / r.rates.size(), maxRate, worstGap, worstClient,
                static_cast<unsigned long long>(r.refused), static_cast<unsigned long long>(r.paced), r.bytesPerSecond,
                r.meanBytesPerUpdate, static_cast<unsigned long long>(r.keys), static_cast<unsigned long long>(r.deltas),
                static_cast<unsigned long long>(r.firstFrameBytes), static_cast<unsigned long long>(r.maxWindow100),
                static_cast<unsigned long long>(r.burstWindow100), r.refreshAllAfterBurstMs, r.maxStretch);
}

void PrintTiers(char const* name, npcsend::Stats const& s)
{
    static char const* const tiers[3] = {"fast", "ordinary", "slow"};
    for (unsigned t = 0; t < 3; ++t)
        std::printf("STREAM-SIM %s tier %s: n=%u min=%u avg=%.1f max=%u overdue=%u\n", name, tiers[t], s.tiers[t].count,
                    s.tiers[t].count ? s.tiers[t].minMs : 0u, s.tiers[t].AverageMs(), s.tiers[t].maxMs, s.tiers[t].overdue);
    std::printf("STREAM-SIM %s edges=%u heartbeats=%u initials=%u\n", name, s.edges, s.heartbeats, s.initials);
}
}

int main()
{
    // 1. The regression, reproduced with the real build-18 budget and scheduler rule.
    Params legacyParams;
    legacyParams.legacy = true;
    const Result legacy = Run(legacyParams);
    Print("legacy-design-mix", legacy);

    // 2. The fix with the real new scheduler, pacer and valve.
    const Result fixedRun = Run(Params{});
    Print("new-design-mix", fixedRun);
    PrintTiers("new-design-mix", fixedRun.steady);

    double legacyMin = 1e9, newMin = 1e9, ordinaryMin = 1e9, ordinaryMax = 0;
    uint32_t legacyWorstGap = 0, newWorstGap = 0;
    for (size_t i = 0; i < legacy.rates.size(); ++i) {
        legacyMin = std::min(legacyMin, legacy.rates[i]);
        legacyWorstGap = std::max(legacyWorstGap, legacy.maxGaps[i]);
    }
    for (size_t i = 0; i < fixedRun.rates.size(); ++i) {
        newMin = std::min(newMin, fixedRun.rates[i]);
        newWorstGap = std::max(newWorstGap, fixedRun.maxGaps[i]);
        if (fixedRun.kinds[i] == Kind::Walker) {
            ordinaryMin = std::min<double>(ordinaryMin, fixedRun.counts[i]);
            ordinaryMax = std::max<double>(ordinaryMax, fixedRun.counts[i]);
        }
    }
    CHECK(legacyMin < 3.0, "legacy min rate %.2f", legacyMin);
    CHECK(legacyWorstGap > 1500, "legacy worst gap %u", legacyWorstGap);
    CHECK(newMin > 8.0 && newMin > legacyMin * 3.0, "new min rate %.2f legacy %.2f", newMin, legacyMin);
    CHECK(legacy.firstFrameBytes > 150000, "legacy burst %llu", static_cast<unsigned long long>(legacy.firstFrameBytes));

    npcsend::Stats const& s = fixedRun.steady;
    CHECK(s.tiers[0].count && s.tiers[0].AverageMs() <= 25.0, "fast avg %.1f", s.tiers[0].AverageMs());
    CHECK(std::fabs(s.tiers[1].AverageMs() - 33.0) <= 3.0, "ordinary avg %.1f", s.tiers[1].AverageMs());
    CHECK(std::fabs(s.tiers[2].AverageMs() - 100.0) <= 8.0, "slow avg %.1f", s.tiers[2].AverageMs());
    for (unsigned t = 0; t < 3; ++t) CHECK(s.tiers[t].overdue == 0, "overdue tier %u = %u", t, s.tiers[t].overdue);
    CHECK(newWorstGap <= 150, "worst host gap of a changing NPC %u ms (slow tier limit 300)", newWorstGap);
    CHECK(ordinaryMin >= ordinaryMax * 0.9, "ordinary fairness %.0f..%.0f", ordinaryMin, ordinaryMax);
    for (size_t i = 0; i < fixedRun.firstUpdate.size(); ++i)
        CHECK(fixedRun.firstUpdate[i] <= 1400, "first update of NPC %zu at %u ms", i, fixedRun.firstUpdate[i]);
    CHECK(fixedRun.pacer.lastBurstMs <= 1300 && fixedRun.pacer.bursts == 1, "burst %u ms x%u", fixedRun.pacer.lastBurstMs, fixedRun.pacer.bursts);
    CHECK(fixedRun.burstWindow100 <= fixedRun.bytesPerSecond / 10 * 1.5, "paced burst window %llu vs steady %.0f per 100 ms",
          static_cast<unsigned long long>(fixedRun.burstWindow100), fixedRun.bytesPerSecond / 10);
    CHECK(legacy.burstWindow100 > 3 * fixedRun.firstFrameBytes, "legacy burst window %llu", static_cast<unsigned long long>(legacy.burstWindow100));
    CHECK(fixedRun.maxStretch == 1000, "stretch %u in normal conditions incl. a loading client (RTT 1400 ms, empty queue)", fixedRun.maxStretch);
    // The delta coder at the designed rates (synthetic full-rotation rig). Far below the 5.9 MB/s that preceded the bandwidth
    // work, but NOT below 1 MB/s with this rig: see the report; do not weaken the per-update guard without new evidence.
    CHECK(fixedRun.meanBytesPerUpdate < 1000.0, "mean pose bytes per update %.1f", fixedRun.meanBytesPerUpdate);
    CHECK(fixedRun.bytesPerSecond < 2000000.0, "design mix %.0f B/s", fixedRun.bytesPerSecond);

    // 3. Emergency valve: real congestion signals stretch every NPC alike, nobody starves, and it decays back.
    Params valve;
    const uint32_t entry = LoadingMs, base = entry + SteadyFromMs;
    valve.episodes = {{base + 3000, base + 5000, 16, 0}, {base + 9000, base + 10000, 32, 600000}};
    const Result congested = Run(valve);
    Print("new-valve-episodes", congested);
    PrintTiers("new-valve-throttle16", congested.episode[0]);
    PrintTiers("new-valve-queue600k", congested.episode[1]);
    CHECK(congested.maxStretch >= 2000 && congested.maxStretch <= 4000, "stretch peak %u", congested.maxStretch);
    for (unsigned e = 0; e < 2; ++e) {
        for (unsigned t = 0; t < 3; ++t) {
            CHECK(congested.episode[e].tiers[t].overdue == 0, "episode %u tier %u overdue %u", e, t, congested.episode[e].tiers[t].overdue);
            CHECK(congested.episode[e].tiers[t].count > 0 || t == 0, "episode %u tier %u starved", e, t);
        }
        CHECK(congested.stretchBackTo1000After[e] <= 3000, "episode %u decay %u ms", e, congested.stretchBackTo1000After[e]);
    }
    const double ordinaryNormal = fixedRun.steady.tiers[1].AverageMs(), ordinaryThrottled = congested.episode[0].tiers[1].AverageMs();
    CHECK(ordinaryThrottled > ordinaryNormal * 1.7 && ordinaryThrottled < ordinaryNormal * 2.3,
          "ordinary %.1f ms -> %.1f ms under throttle 16", ordinaryNormal, ordinaryThrottled);
    for (size_t i = 0; i < congested.rates.size(); ++i)
        if (congested.kinds[i] != Kind::Static) CHECK(congested.maxGaps[i] <= 4 * 1000 + 100, "episode run NPC %zu gap %u", i, congested.maxGaps[i]);

    // 4. Worst case: every NPC fighting. Numbers only; this scene is the live crowd fight (the game-start orc raid).
    Params brawl;
    brawl.mix = {70, 0, 0, 5, 0};
    const Result fight = Run(brawl);
    Print("new-brawl-70-fighters", fight);
    PrintTiers("new-brawl-70-fighters", fight.steady);
    std::printf("STREAM-SIM summary: legacy min Hz %.2f worst gap %u ms -> new min Hz %.2f worst gap %u ms; design mix %.0f B/s "
                "(legacy %.0f B/s), brawl %.0f B/s\n", legacyMin, legacyWorstGap, newMin, newWorstGap, fixedRun.bytesPerSecond,
                legacy.bytesPerSecond, fight.bytesPerSecond);
    return 0;
}
