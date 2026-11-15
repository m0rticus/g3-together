// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/playerwire.h"
#include "../src/goods_schedule.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    using namespace g3t;
    constexpr unsigned Seconds = 60, Rate = 30;
    net::PlayerState state{};
    state.id = 1;
    state.flags = 1;
    state.world = 123;
    state.rotation[3] = 1;
    std::snprintf(state.name, sizeof(state.name), "%s", "Host");
    std::snprintf(state.heldRight, sizeof(state.heldRight), "%s", "It_1H_OrcSlayer_01");
    std::snprintf(state.heldLeft, sizeof(state.heldLeft), "%s", "It_Shield_01");
    std::snprintf(state.armor, sizeof(state.armor), "%s", "Body_Human_Armor_01");
    std::snprintf(state.helmet, sizeof(state.helmet), "%s", "It_Helmet_01");
    std::snprintf(state.look, sizeof(state.look), "%s", "PC_Hero");
    playerwire::Encoder encoder;
    playerwire::Decoder decoder;
    uint64_t application = 0, enet = 0, steam = 0, ack = 0;
    unsigned reliable = 0, full = 0;
    for (unsigned tick = 0; tick < Seconds * Rate; ++tick) {
        state.sequence = tick + 1;
        state.sent = tick * 1000 / Rate;
        state.position[0] = tick * 3.125f;
        state.motionFrac = static_cast<uint16_t>(tick * 37);
        state.actionSeq = static_cast<uint8_t>(tick / 60);
        state.focusNpc = tick / 300 % 2 ? 567 : 123;
        state.targetNpc = tick / 600 % 2 ? 0 : 123;
        std::snprintf(state.animation, sizeof(state.animation), "%s", tick / 60 % 2 ? "Walk" : "Stand");
        playerwire::Packet packet;
        assert(encoder.Plan(state, state.sent, false, packet) == playerwire::Status::Ok);
        net::PlayerState restored;
        assert(decoder.Decode(packet.bytes.data(), packet.bytes.size(), restored) == playerwire::Status::Ok);
        assert(restored.focusNpc == state.focusNpc && restored.targetNpc == state.targetNpc);
        encoder.Commit(state, state.sent, packet);
        application += packet.bytes.size() + 1;
        enet += packet.bytes.size() + 1 + 10;
        steam += packet.bytes.size() + 1 + 10 + 16;
        if (packet.reliable) { ++reliable; ack += 4 + 8 + 16; }
        if (packet.full) ++full;
    }
    const double applicationBps = double(application) / Seconds;
    const double steamBps = double(steam) / Seconds;
    const double originalApplication = 60 * 402;
    const double originalSteam = 60 * (402 + 10 + 72);
    assert(applicationBps < 2500 && steamBps < 3500);
    assert(applicationBps / originalApplication < 0.1);
    assert(full == Seconds / 5);
    trade::GoodsSchedule goods;
    unsigned goodsFrames = 0;
    for (unsigned now = 0; now < Seconds * 1000; now += 250) {
        if (goods.Due(now, false, 0, 0)) {
            ++goodsFrames;
            goods.Published(now, 0, 0);
        }
    }
    assert(goodsFrames == 2);
    std::printf("PLAYER_BPS before_app=%.2f after_app=%.2f before_enet=%.2f after_enet=%.2f "
                "before_steam_api=%.2f after_steam_api=%.2f reverse_ack=%.2f reliable=%u full=%u\n",
                originalApplication, applicationBps, double(60 * (402 + 10)), double(enet) / Seconds,
                originalSteam, steamBps, double(ack) / Seconds, reliable, full);
    std::printf("GOODS_BPS base_and_after=%.2f entries=250 keyframe_ms=30000\n", double(19 + 48 * 250) * goodsFrames / Seconds);
    std::printf("RELAY_SAVING supplied_datagrams_per_s=%.2f envelope_bytes_before=72 after=16 bytes_per_s_saved=%.2f\n",
                30492.0 / 5.015, 56 * 30492.0 / 5.015);
}
