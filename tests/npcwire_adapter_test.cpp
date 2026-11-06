// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_adapter.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

int main()
{
    using namespace g3t;
    net::NpcRecord record;
    for (unsigned i = 0; i < 16; ++i) record.guid[i] = static_cast<uint8_t>(i + 1);
    record.position[0] = -9999999.0f; record.position[1] = 2345.5f; record.position[2] = 9999999.0f;
    record.rotation[0] = 0.1f; record.rotation[1] = 0.2f; record.rotation[2] = 0.3f; record.rotation[3] = 0.9f;
    record.movementMode = 7; record.flags = 0xd5; record.hitPoints = -123; record.hitPointsMax = 456;
    record.motion = 42; record.aniState = 4; record.heldRight = 1234; record.heldLeft = 5678;
    record.sent = 0xfedcba98; record.partyPlayer = 2; record.partyType = 3; record.waiting = 1;
    record.guidePlayer = 4; record.aiMode = 5; record.action = 6; record.attackReason = 7;
    record.damageType = 3; record.damageAmount = -99; record.damageMultiplier = 1.25f;
    record.incarnation = 0xabcdef; record.hitPlayer = 3; record.hitSequence = 0xf123;
    record.contact = 1; record.attitude = 4; record.combatTarget = 0xdeadbeef;
    for (unsigned i = 0; i < net::NpcMotionSlots; ++i) {
        auto& m = record.motions[i];
        m.name = 100 + i; m.instance = 200 + i; m.phase = 0.25f; m.speed = -1.5f;
        m.weight = 0.5f; m.intro = 12.5f; m.extro = 1.0f; m.blend = -123;
        for (unsigned j = 0; j < 7; ++j) m.options[j] = 256 * i + j;
        m.active = i % 2 ? 3 : 0;
    }
    npcwire::Control value;
    assert(npcwire::FromNpcRecord(record, 12, 34, true, value));
    assert(value.anchor == 12 && value.baseline == 34 && value.slots == 0xaa);
    net::NpcRecord restored;
    assert(npcwire::ToNpcRecord(value, restored));
    assert(std::memcmp(&record, &restored, sizeof(record)) == 0);
    assert(npcwire::FromNpcRecord(record, 12, 34, false, value));
    assert(std::holds_alternative<npcwire::Quaternion>(value.rotation));

    npcwire::Context sender, receiver;
    constexpr npcwire::Stream stream{5, 1};
    assert(sender.Reset(stream.epoch, stream.generation));
    assert(receiver.Reset(stream.epoch, stream.generation));
    assert(npcwire::FromNpcRecord(record, 12, 34, true, value));
    assert(sender.AddId(stream, 7, value.guid));
    assert(sender.AdvanceLife(stream, 7, record.incarnation));
    assert(sender.AddBaseline(stream, 7, 34, 12, value));
    std::vector<uint8_t> message;
    assert(npcwire::EncodeId(sender, 1, 7, message));
    uint16_t decodedId = 0;
    assert(npcwire::DecodeId(receiver, message.data(), message.size(), decodedId) && decodedId == 7);
    assert(npcwire::EncodeBaseline(sender, 2, 7, 34, message));
    npcwire::BaselineRef baseline;
    assert(npcwire::DecodeBaseline(receiver, message.data(), message.size(), baseline));
    net::NpcRecord update = record;
    update.position[1] += 12.25f;
    update.rotation[0] = update.rotation[2] = 0.0f;
    update.rotation[1] = 0.5f;
    update.rotation[3] = static_cast<float>(std::sqrt(0.75));
    update.sent++;
    update.flags ^= 4;
    update.contact = 0;
    for (auto& motion : update.motions) {
        if (motion.active & 1u) { motion.phase = 0.7f; motion.weight = 0.8f; }
    }
    assert(npcwire::FromNpcRecord(update, 12, 34, false, value));
    assert(npcwire::EncodeBatch(sender, 3, {value}, message));
    std::vector<npcwire::Control> decoded;
    assert(npcwire::DecodeBatch(receiver, message.data(), message.size(), decoded) && decoded.size() == 1);
    assert(npcwire::ToNpcRecord(decoded[0], restored));
    assert(std::fabs(restored.position[1] - update.position[1]) <= 0.5f);
    for (unsigned i = 0; i < net::NpcMotionSlots; ++i) {
        if (update.motions[i].active & 1u) {
            assert(std::fabs(restored.motions[i].phase - update.motions[i].phase) <= 0.5f / 4095 + 0.000001f);
            assert(std::fabs(restored.motions[i].weight - update.motions[i].weight) <= 0.5f / 255 + 0.000001f);
            update.motions[i].phase = restored.motions[i].phase;
            update.motions[i].weight = restored.motions[i].weight;
        }
    }
    std::copy(restored.position, restored.position + 3, update.position);
    std::copy(restored.rotation, restored.rotation + 4, update.rotation);
    assert(std::memcmp(&update, &restored, sizeof(update)) == 0);

    // At the accepted world's largest coordinates, converting an anchored
    // half-step back to float can add another half-centimetre of rounding.
    npcwire::Context edge;
    constexpr npcwire::Stream edgeStream{6, 1};
    assert(edge.Reset(edgeStream.epoch, edgeStream.generation));
    record.position[0] = 8388607.5f;
    assert(npcwire::FromNpcRecord(record, 1, 1, true, value));
    assert(edge.AddId(edgeStream, 1, value.guid));
    assert(edge.AdvanceLife(edgeStream, 1, record.incarnation));
    assert(edge.AddBaseline(edgeStream, 1, 1, 1, value));
    record.position[0] = 8388609.0f;
    assert(npcwire::FromNpcRecord(record, 1, 1, false, value));
    assert(npcwire::EncodeRecord(edge, value, message));
    npcwire::Control edgeDecoded;
    assert(npcwire::DecodeRecord(edge, message.data(), message.size(), edgeDecoded));
    assert(npcwire::ToNpcRecord(edgeDecoded, restored));
    assert(std::fabs(restored.position[0] - record.position[0]) == 1.0f);

    std::mt19937 positionRandom(42);
    std::uniform_real_distribution<double> anchors(-9960000.0, 9960000.0);
    std::uniform_real_distribution<double> offsets(-32760.0, 32760.0);
    for (unsigned i = 0; i < 200; ++i) {
        npcwire::Context positionContext;
        assert(positionContext.Reset(edgeStream.epoch, edgeStream.generation));
        record.position[0] = static_cast<float>(anchors(positionRandom));
        const float anchor = record.position[0];
        assert(npcwire::FromNpcRecord(record, 1, 1, true, value));
        assert(positionContext.AddId(edgeStream, 1, value.guid));
        assert(positionContext.AdvanceLife(edgeStream, 1, record.incarnation));
        assert(positionContext.AddBaseline(edgeStream, 1, 1, 1, value));
        for (unsigned j = 0; j < 100; ++j) {
            record.position[0] = static_cast<float>(anchor + offsets(positionRandom));
            assert(npcwire::FromNpcRecord(record, 1, 1, false, value));
            assert(npcwire::EncodeRecord(positionContext, value, message));
            assert(npcwire::DecodeRecord(positionContext, message.data(), message.size(), edgeDecoded));
            assert(npcwire::ToNpcRecord(edgeDecoded, restored));
            assert(std::fabs(restored.position[0] - record.position[0]) <= 1.0f);
        }
    }

    constexpr double turn = 6.2831853071795864769;
    std::mt19937 random(17);
    std::uniform_real_distribution<double> angles(-turn, turn);
    for (unsigned i = 0; i < 20000; ++i) {
        const double angle = angles(random);
        record.rotation[0] = record.rotation[2] = 0.0f;
        record.rotation[1] = static_cast<float>(std::sin(angle / 2));
        record.rotation[3] = static_cast<float>(std::cos(angle / 2));
        assert(npcwire::FromNpcRecord(record, 12, 34, false, value));
        assert(std::holds_alternative<npcwire::Yaw>(value.rotation));
        uint16_t quantized = 0;
        assert(npcwire::EncodeYaw(std::get<npcwire::Yaw>(value.rotation).radians, quantized));
        value.rotation = npcwire::Yaw{npcwire::DecodeYaw(quantized)};
        assert(npcwire::ToNpcRecord(value, restored));
        const double actual = 2.0 * std::atan2(restored.rotation[1], restored.rotation[3]);
        assert(std::fabs(std::remainder(actual - angle, turn)) <= turn / 131072.0 + 0.000001);
    }
    record.rotation[0] = 0.00001f;
    assert(npcwire::FromNpcRecord(record, 12, 34, false, value));
    assert(std::holds_alternative<npcwire::Quaternion>(value.rotation));
    record.position[1] = std::numeric_limits<float>::quiet_NaN();
    value.sent = 19;
    assert(!npcwire::FromNpcRecord(record, 12, 34, false, value) && value.sent == 19);
    value.position[0] = std::numeric_limits<double>::infinity();
    restored.sent = 37;
    assert(!npcwire::ToNpcRecord(value, restored) && restored.sent == 37);
}
