// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/goods_schedule.h"
#include "../src/net_metrics.h"
#include "../src/trade_policy.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

int main()
{
    using g3t::trade::GoodsSchedule;
    GoodsSchedule schedule;
    assert(schedule.Due(10, false, 0, 0));
    schedule.Published(10, 0, 0);
    assert(!schedule.Due(20010, false, 0, 0));
    assert(!schedule.Due(30009, false, 0, 0));
    assert(schedule.Due(30010, false, 0, 0));
    assert(schedule.Due(11, true, 0, 0));
    assert(schedule.Due(11, false, 1, 0));
    assert(schedule.Due(12, false, 1, 0));  // a failed attempt did not commit the join
    schedule.Published(12, 1, 0);
    assert(!schedule.Due(13, false, 1, 0));
    assert(schedule.Due(13, false, 1, 1));  // re-entering on the same connection
    schedule.Published(UINT32_MAX - 100, 1, 1);
    assert(!schedule.Due(100, false, 1, 1));
    assert(schedule.Due(29900, false, 1, 1));  // unsigned tick wrap
    schedule = GoodsSchedule{};
    assert(schedule.Due(0, false, 0, 0));

    using g3t::net::NpcSendDecision;
    g3t::net::detail::BandwidthCounters counters;
    counters.Tx(5, 23);
    counters.Tx(5, 23);  // two recipients
    counters.Rx(5, 23);
    counters.Rx(255, 7);
    assert(counters.kinds[5].txPackets == 2 && counters.kinds[5].txBytes == 46);
    assert(counters.kinds[5].rxPackets == 1 && counters.kinds[5].rxBytes == 23);
    assert(counters.kinds[255].rxPackets == 1 && counters.kinds[255].rxBytes == 7);
    counters.Npc(NpcSendDecision::Sent, 2);
    counters.Npc(NpcSendDecision::SkippedUnchanged, 3);
    counters.Npc(NpcSendDecision::SkippedFar, 4);
    counters.Npc(NpcSendDecision::SkippedIrrelevant, 5);
    counters.Npc(NpcSendDecision::SendFailed, 6);
    counters.Npc(static_cast<NpcSendDecision>(99), 7);
    assert(counters.considered == 20 && counters.sent == 2 && counters.skippedUnchanged == 3 &&
           counters.skippedFar == 4 && counters.skippedIrrelevant == 5 && counters.sendFailed == 6);
    counters.Pose(16, 500, 420, 76, false);
    counters.PoseComponents(13, 76, 2500, 500, 70, false, true, 45, 23, 60, 2);
    counters.Pose(100, 1900, 1831, 76, true);
    counters.PoseComponents(7, 0, 76 * 144, 76 * 48, 76, true, false, 45, 23);
    assert(counters.poseDeltas == 1 && counters.poseKeys == 1 && counters.poseBones == 152);
    assert(counters.tierUpdates[0] == 1 && counters.tierUpdates[2] == 1 && counters.tierBytes[0] == 500);
    assert(counters.poseHeaderBytes == 20 && counters.poseMaskBits == 76);
    assert(counters.poseRotationBits == 2500 + 76 * 144 && counters.poseTranslationBits == 500 + 76 * 48);
    assert(counters.poseIncludedDeltaBones == 70 && counters.posePredictiveUpdates == 1);
    assert(counters.poseControlBytes == 90 && counters.poseOuterBytes == 46);
    assert(counters.poseRigidBones == 60 && counters.poseMaxBasisError == 2);

    std::vector<std::string> lines;
    g3t::net::detail::ForEachKindLine(counters, [&](char const* line) { lines.emplace_back(line); });
    assert(lines.size() == 1 && lines[0].find(" 5=2/46,1/23") != std::string::npos);
    assert(lines[0].find(" 255=0/0,1/7") != std::string::npos);

    // Even the largest possible values for all 256 kinds fit without truncating any entry.
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    for (auto& kind : counters.kinds) kind = {max, max, max, max};
    lines.clear();
    g3t::net::detail::ForEachKindLine(counters, [&](char const* line) { lines.emplace_back(line); });
    assert(lines.size() > 1);
    for (auto const& line : lines) assert(line.size() < 760 && line.size() < 900);
    for (unsigned kind = 0; kind < 256; ++kind) {
        const std::string token = " " + std::to_string(kind) + "=18446744073709551615/18446744073709551615,"
                                  "18446744073709551615/18446744073709551615";
        unsigned found = 0;
        for (auto const& line : lines) if (line.find(token) != std::string::npos) ++found;
        assert(found == 1);
    }

    // Trade stack-key encoding and strict QA suffix/puppet-ID parsers.
    uint8_t guidA[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::string keyA0 = g3t::trade_policy::StackKeyBytes(guidA, 0);
    const std::string keyA7 = g3t::trade_policy::StackKeyBytes(guidA, 7);
    assert(keyA0.size() == 18 && keyA7.size() == 18 && keyA0 != keyA7);
    assert(g3t::trade_policy::StackQuality(keyA0) == 0);
    assert(g3t::trade_policy::StackQuality(keyA7) == 7);
    assert(g3t::trade_policy::StackQuality(std::string("short")) == 0);

    std::string item = "It_1H_Bastardsword-q12";
    int quality = -1;
    assert(g3t::trade_policy::ParseQualitySuffix(item, quality) && item == "It_1H_Bastardsword" && quality == 12);
    item = "It_Gold";
    assert(g3t::trade_policy::ParseQualitySuffix(item, quality) && item == "It_Gold" && quality == -1);
    item = "It_1H_Bastardsword-qabc";
    assert(!g3t::trade_policy::ParseQualitySuffix(item, quality));
    item = "It_1H_Bastardsword-q";
    assert(!g3t::trade_policy::ParseQualitySuffix(item, quality));
    item = "It_1H_Bastardsword-q1024";
    assert(!g3t::trade_policy::ParseQualitySuffix(item, quality));

    int puppetId = 0;
    assert(g3t::trade_policy::ParsePuppetId("puppet2", 16, puppetId) && puppetId == 2);
    assert(!g3t::trade_policy::ParsePuppetId("puppet0", 16, puppetId));
    assert(!g3t::trade_policy::ParsePuppetId("puppet16", 16, puppetId));
    assert(!g3t::trade_policy::ParsePuppetId("puppet", 16, puppetId));
    assert(!g3t::trade_policy::ParsePuppetId("puppet2x", 16, puppetId));
    assert(!g3t::trade_policy::ParsePuppetId("Diego", 16, puppetId));
    assert(!g3t::trade_policy::ParsePuppetId(nullptr, 16, puppetId));
}
