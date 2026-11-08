// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_control.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace g3t::npcwire;

namespace {
constexpr Stream S{31, 1};
constexpr std::uint32_t AllFields = (std::uint32_t{1} << 21) - 1;
constexpr std::size_t Widths[] = {
    1, 4, 4, 4, 1, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 1, 2, 1, 4
};
static_assert(MaxMetadataBytes == 49, "wire maximum");

std::uint32_t bits(float v) {
    std::uint32_t result;
    std::memcpy(&result, &v, sizeof(result));
    return result;
}
float fromBits(std::uint32_t v) {
    float result;
    std::memcpy(&result, &v, sizeof(result));
    return result;
}
Control snapshot(unsigned id, std::uint8_t slots = 0xff) {
    Control v;
    v.guid[0] = static_cast<std::uint8_t>(id);
    v.guid[1] = static_cast<std::uint8_t>(id >> 8);
    v.anchor = 10;
    v.baseline = 20;
    v.incarnation = 3;
    v.slots = slots;
    v.rotation = Quaternion{{{0.25f, -0.5f, 0.125f, 1.0f}}};
    v.position = {{1.0, -2.0, 3.0}};
    v.hitPointsMax = 0;
    for (unsigned i = 0; i < MotionSlots; ++i) {
        v.motions[i].active = slots & (1u << i) ? 3 : 2;
        v.motions[i].phase = slots & (1u << i) ? 0.5f : 2.0f;
        v.motions[i].weight = slots & (1u << i) ? 1.0f : 3.0f;
        v.motions[i].descriptor.name = 100 + i;
    }
    return v;
}
void install(Context& c, const Control& v, std::uint16_t id) {
    assert(c.AddId(S, id, v.guid));
    assert(c.AdvanceLife(S, id, v.incarnation));
    assert(c.AddBaseline(S, id, v.baseline, v.anchor, v));
}
void setField(Control& v, unsigned bit, unsigned variant) {
    const std::uint8_t u8 = variant % 2 ? 0xff : 1;
    const std::uint32_t u32 = variant % 2 ? 0xffffffffu : 1;
    const auto i32 = variant % 2 ? std::numeric_limits<std::int32_t>::min()
                                 : std::numeric_limits<std::int32_t>::max();
    switch (bit) {
    case 0: v.movementMode = u8; break;
    case 1: v.hitPoints = i32; break;
    case 2: v.hitPointsMax = i32; break;
    case 3: v.motion = u32; break;
    case 4: v.aniState = u8; break;
    case 5: v.heldRight = u32; break;
    case 6: v.heldLeft = u32; break;
    case 7: v.partyPlayer = u8; break;
    case 8: v.partyType = u8; break;
    case 9: v.waiting = u8; break;
    case 10: v.guidePlayer = u8; break;
    case 11: v.aiMode = u8; break;
    case 12: v.action = u8; break;
    case 13: v.attackReason = u8; break;
    case 14: v.damageType = u8; break;
    case 15: v.damageAmount = i32; break;
    case 16: {
        constexpr std::uint32_t choices[] = {
            0x80000000u, 0x00000001u, 0x80000001u, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u
        };
        v.damageMultiplier = fromBits(choices[variant % 6]);
        break;
    }
    case 17: v.hitPlayer = u8; break;
    case 18: v.hitSequence = variant % 2 ? 0xffffu : 1u; break;
    case 19: v.attitude = u8; break;
    case 20: v.combatTarget = u32; break;
    default: assert(false);
    }
}
void setRandomField(Control& v, unsigned bit, std::uint32_t raw) {
    if (!raw) raw = 1;
    const auto u8 = static_cast<std::uint8_t>(raw) ? static_cast<std::uint8_t>(raw) : std::uint8_t{1};
    const auto u16 = static_cast<std::uint16_t>(raw) ? static_cast<std::uint16_t>(raw) : std::uint16_t{1};
    const auto i32 = raw <= 0x7fffffffu ? static_cast<std::int32_t>(raw)
                                        : -1 - static_cast<std::int32_t>(0xffffffffu - raw);
    switch (bit) {
    case 0: v.movementMode = u8; break;
    case 1: v.hitPoints = i32; break;
    case 2: v.hitPointsMax = i32; break;
    case 3: v.motion = raw; break;
    case 4: v.aniState = u8; break;
    case 5: v.heldRight = raw; break;
    case 6: v.heldLeft = raw; break;
    case 7: v.partyPlayer = u8; break;
    case 8: v.partyType = u8; break;
    case 9: v.waiting = u8; break;
    case 10: v.guidePlayer = u8; break;
    case 11: v.aiMode = u8; break;
    case 12: v.action = u8; break;
    case 13: v.attackReason = u8; break;
    case 14: v.damageType = u8; break;
    case 15: v.damageAmount = i32; break;
    case 16:
        if ((raw & 0x7f800000u) == 0x7f800000u) raw &= ~0x00800000u;
        v.damageMultiplier = fromBits(raw);
        break;
    case 17: v.hitPlayer = u8; break;
    case 18: v.hitSequence = u16; break;
    case 19: v.attitude = u8; break;
    case 20: v.combatTarget = raw; break;
    default: assert(false);
    }
}
std::uint32_t fieldValue(const Control& v, unsigned bit) {
    switch (bit) {
    case 0: return v.movementMode;
    case 1: return static_cast<std::uint32_t>(v.hitPoints);
    case 2: return static_cast<std::uint32_t>(v.hitPointsMax);
    case 3: return v.motion;
    case 4: return v.aniState;
    case 5: return v.heldRight;
    case 6: return v.heldLeft;
    case 7: return v.partyPlayer;
    case 8: return v.partyType;
    case 9: return v.waiting;
    case 10: return v.guidePlayer;
    case 11: return v.aiMode;
    case 12: return v.action;
    case 13: return v.attackReason;
    case 14: return v.damageType;
    case 15: return static_cast<std::uint32_t>(v.damageAmount);
    case 16: return bits(v.damageMultiplier);
    case 17: return v.hitPlayer;
    case 18: return v.hitSequence;
    case 19: return v.attitude;
    case 20: return v.combatTarget;
    default: assert(false); return 0;
    }
}
void checkFields(const Control& expected, const Control& actual) {
    for (unsigned bit = 0; bit < 21; ++bit)
        assert(fieldValue(expected, bit) == fieldValue(actual, bit));
}
std::size_t extensionOffset(const Control& v) {
    unsigned slots = 0;
    for (unsigned i = 0; i < MotionSlots; ++i) slots += (v.slots >> i) & 1u;
    return sizeof(WireControl) + (std::holds_alternative<Quaternion>(v.rotation) ? 16 : 0) + slots * 3;
}
std::vector<std::uint8_t> expectedExtension(const Control& v, std::uint32_t mask) {
    std::vector<std::uint8_t> result;
    for (unsigned i = 0; i < 3; ++i) result.push_back(static_cast<std::uint8_t>(mask >> (i * 8)));
    for (unsigned bit = 0; bit < 21; ++bit) if (mask & (std::uint32_t{1} << bit)) {
        const auto value = fieldValue(v, bit);
        for (std::size_t i = 0; i < Widths[bit]; ++i)
            result.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    return result;
}
void checkWire(const Context& sender, const Context& receiver, const Control& value, std::uint32_t mask) {
    std::vector<std::uint8_t> wire;
    assert(EncodeRecord(sender, value, wire));
    const auto offset = extensionOffset(value);
    assert((wire[offsetof(WireControl, orientation)] & 0x80u) == (mask ? 0x80u : 0u));
    const auto expected = mask ? expectedExtension(value, mask) : std::vector<std::uint8_t>{};
    assert(wire.size() == offset + expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) assert(wire[offset + i] == expected[i]);
    std::size_t length = 999;
    assert(RecordBytes(wire.data(), wire.size(), length) && length == wire.size());
    Control decoded;
    assert(DecodeRecord(receiver, wire.data(), wire.size(), decoded));
    checkFields(value, decoded);
    assert(decoded.flags == value.flags && decoded.contact == value.contact);
}
void checkMotion(const Control& expected, const Control& actual) {
    assert(expected.slots == actual.slots);
    for (unsigned i = 0; i < MotionSlots; ++i) {
        const auto& a = expected.motions[i];
        const auto& b = actual.motions[i];
        assert(a.descriptor.name == b.descriptor.name &&
               a.descriptor.instance == b.descriptor.instance &&
               bits(a.descriptor.speed) == bits(b.descriptor.speed) &&
               bits(a.descriptor.intro) == bits(b.descriptor.intro) &&
               bits(a.descriptor.extro) == bits(b.descriptor.extro) &&
               a.descriptor.blend == b.descriptor.blend &&
               a.descriptor.options == b.descriptor.options &&
               a.active == b.active);
        if (a.active & 1u)
            assert(std::fabs(a.phase - b.phase) <= 0.5f / 4095 + 1e-7 &&
                   std::fabs(a.weight - b.weight) <= 0.5f / 255 + 1e-7);
        else
            assert(bits(a.phase) == bits(b.phase) && bits(a.weight) == bits(b.weight));
    }
}
void checkTruncations(const Context& receiver, const std::vector<std::uint8_t>& wire) {
    for (std::size_t n = 0; n < wire.size(); ++n) {
        std::size_t length = 123456;
        assert(!RecordBytes(wire.data(), n, length) && length == 123456);
        Control untouched; untouched.sent = 0x12345678;
        assert(!DecodeRecord(receiver, wire.data(), n, untouched) && untouched.sent == 0x12345678);
    }
}
}

int main() {
    Context sender, receiver;
    assert(sender.Reset(S.epoch, S.generation) && receiver.Reset(S.epoch, S.generation));
    const auto base = snapshot(1);
    install(sender, base, 1);
    install(receiver, base, 1);

    // Verify every bit, width, endianness and integer extrema independently.
    for (unsigned orientation = 0; orientation < 2; ++orientation) {
        for (unsigned bit = 0; bit < 21; ++bit) for (unsigned variant = 0; variant < 2; ++variant) {
            auto v = base;
            if (!orientation) v.rotation = Yaw{0.3};
            setField(v, bit, variant);
            checkWire(sender, receiver, v, std::uint32_t{1} << bit);
            if (!variant) {
                std::vector<std::uint8_t> one;
                assert(EncodeRecord(sender, v, one));
                checkTruncations(receiver, one);
            }
        }
    }
    for (unsigned variant = 0; variant < 6; ++variant) {
        auto v = base;
        setField(v, 16, variant);
        checkWire(sender, receiver, v, std::uint32_t{1} << 16);
    }
    auto all = base;
    for (unsigned bit = 0; bit < 21; ++bit) setField(all, bit, bit);
    all.flags = 0xff; all.contact = 0xff;
    checkWire(sender, receiver, all, AllFields);
    std::vector<std::uint8_t> maxWire;
    assert(EncodeRecord(sender, all, maxWire));
    assert(maxWire.size() == 24 + 16 + 8 * 3 + MaxMetadataBytes);
    auto exact = all;
    exact.position = {{-0.0, static_cast<double>(0.123456f), static_cast<double>(12345.678f)}};
    for (auto& motion : exact.motions) { motion.phase = 0.123456f; motion.weight = 0.654321f; }
    std::vector<std::uint8_t> exactWire;
    assert(EncodeRecord(sender, exact, exactWire, true));
    assert(exactWire.size() == 24 + 16 + 12 + 8 * 8 + MaxMetadataBytes);
    checkTruncations(receiver, exactWire);
    Control exactDecoded;
    assert(DecodeRecord(receiver, exactWire.data(), exactWire.size(), exactDecoded));
    assert(exactDecoded.position == exact.position && std::signbit(exactDecoded.position[0]));
    checkFields(exact, exactDecoded);
    for (unsigned i = 0; i < MotionSlots; ++i) {
        assert(bits(exactDecoded.motions[i].phase) == bits(exact.motions[i].phase));
        assert(bits(exactDecoded.motions[i].weight) == bits(exact.motions[i].weight));
    }
    auto corruptExact = exactWire;
    corruptExact[offsetof(WireControl, delta)] = 1;
    assert(!DecodeRecord(receiver, corruptExact.data(), corruptExact.size(), exactDecoded));
    corruptExact = exactWire;
    corruptExact[24 + 16 + 12 + 2] = 0xc0;
    corruptExact[24 + 16 + 12 + 3] = 0x7f; // NaN phase, structurally correct length
    assert(!DecodeRecord(receiver, corruptExact.data(), corruptExact.size(), exactDecoded));

    std::mt19937 rng(0x5ca1ab1e);
    for (unsigned i = 0; i < 10000; ++i) {
        auto v = base;
        if (rng() & 1u) v.rotation = Yaw{static_cast<double>(rng() % 1000) / 100};
        const auto mask = rng() & AllFields;
        for (unsigned bit = 0; bit < 21; ++bit)
            if (mask & (std::uint32_t{1} << bit)) setRandomField(v, bit, rng());
        v.flags = static_cast<std::uint8_t>(rng());
        v.contact = static_cast<std::uint8_t>(rng());
        checkWire(sender, receiver, v, mask);
    }

    // Three named motion overrides change descriptors and active/inactive state
    // without replacing or re-keying the immutable baseline.
    auto dynamic = base;
    dynamic.rotation = Yaw{0};
    dynamic.motions[0].descriptor.name++;
    dynamic.motions[0].descriptor.instance++;
    dynamic.motions[0].descriptor.speed = 2.25f;
    dynamic.motions[3].active = 2;
    dynamic.motions[3].phase = 2.5f;
    dynamic.motions[3].weight = 3.0f;
    dynamic.motions[7].descriptor.options[6]++;
    dynamic.motions[7].weight = 0.9f;
    dynamic.slots = ActiveSlots(dynamic);
    std::vector<std::uint8_t> dynamicWire;
    assert(EncodeRecord(sender, dynamic, dynamicWire));
    assert((dynamicWire[offsetof(WireControl, orientation)] & 0x20u) != 0);
    assert(dynamicWire.size() == 24 + 7 * 3 + 1 + 14 + 11 + 6);
    const std::size_t dynamicOverride = 24 + 7 * 3;
    assert(dynamicWire[dynamicOverride] == 0x89);
    std::size_t dynamicLength = 0;
    assert(RecordBytes(dynamicWire.data(), dynamicWire.size(), dynamicLength) &&
           dynamicLength == dynamicWire.size());
    Control dynamicDecoded;
    assert(DecodeRecord(receiver, dynamicWire.data(), dynamicWire.size(), dynamicDecoded));
    checkMotion(dynamic, dynamicDecoded);
    checkTruncations(receiver, dynamicWire);

    // Active slots in the control record come from the current value. A baseline
    // with no slots can gain two slots and each gained slot is full-encoded.
    auto noSlots = snapshot(1, 0);
    noSlots.baseline = 23;
    assert(sender.AddBaseline(S, 1, 23, noSlots.anchor, noSlots));
    assert(receiver.AddBaseline(S, 1, 23, noSlots.anchor, noSlots));
    auto appeared = noSlots;
    appeared.rotation = Yaw{0};
    appeared.motions[0].active = 3;
    appeared.motions[7].active = 1;
    appeared.motions[0].phase = 0.25f;
    appeared.motions[0].weight = 0.75f;
    appeared.motions[7].phase = 0.125f;
    appeared.motions[7].weight = 0.75f;
    appeared.slots = ActiveSlots(appeared);
    std::vector<std::uint8_t> appearedWire;
    assert(EncodeRecord(sender, appeared, appearedWire));
    assert(appearedWire[offsetof(WireControl, slots)] == 0x81);
    assert((appearedWire[offsetof(WireControl, orientation)] & 0x20u) != 0);
    assert(appearedWire.size() == 24 + 2 * 3 + 1 + 2 * 3);
    std::size_t appearedLength = 0;
    assert(RecordBytes(appearedWire.data(), appearedWire.size(), appearedLength) &&
           appearedLength == appearedWire.size());
    Control appearedDecoded;
    assert(DecodeRecord(receiver, appearedWire.data(), appearedWire.size(), appearedDecoded));
    checkMotion(appeared, appearedDecoded);

    // All eight slots are losslessly overridden together; exact mode also
    // preserves the full fields and exact position/motion float bits.
    auto allDynamic = base;
    allDynamic.rotation = Yaw{0};
    for (unsigned i = 0; i < MotionSlots; ++i) {
        allDynamic.motions[i].descriptor.name += 1000;
        allDynamic.motions[i].descriptor.instance += 1000;
        allDynamic.motions[i].descriptor.speed = static_cast<float>(i) + 0.25f;
        allDynamic.motions[i].descriptor.intro = -static_cast<float>(i);
        allDynamic.motions[i].descriptor.extro = static_cast<float>(i) + 0.5f;
        allDynamic.motions[i].descriptor.blend = static_cast<std::int32_t>(i) - 4;
        allDynamic.motions[i].descriptor.options[0] = 0x10000u + i;
    }
    std::vector<std::uint8_t> allDynamicWire;
    assert(EncodeRecord(sender, allDynamic, allDynamicWire));
    assert((allDynamicWire[offsetof(WireControl, orientation)] & 0x20u) != 0);
    assert(allDynamicWire[24 + 8 * 3] == 0xff);
    Control allDynamicDecoded;
    assert(DecodeRecord(receiver, allDynamicWire.data(), allDynamicWire.size(), allDynamicDecoded));
    checkMotion(allDynamic, allDynamicDecoded);

    auto exactDynamic = allDynamic;
    exactDynamic.position = {{-0.0, static_cast<double>(0.123456f), static_cast<double>(12345.678f)}};
    exactDynamic.rotation = Quaternion{{{0.125f, -0.25f, 0.5f, 0.875f}}};
    for (unsigned i = 0; i < MotionSlots; ++i) {
        exactDynamic.motions[i].phase = 0.123456f + 0.01f * i;
        exactDynamic.motions[i].weight = 0.654321f + i;
    }
    std::vector<std::uint8_t> exactDynamicWire;
    assert(EncodeRecord(sender, exactDynamic, exactDynamicWire, true));
    assert((exactDynamicWire[offsetof(WireControl, orientation)] & 0x20u) != 0);
    std::size_t exactDynamicLength = 0;
    assert(RecordBytes(exactDynamicWire.data(), exactDynamicWire.size(), exactDynamicLength) &&
           exactDynamicLength == exactDynamicWire.size());
    Control exactDynamicDecoded;
    assert(DecodeRecord(receiver, exactDynamicWire.data(), exactDynamicWire.size(), exactDynamicDecoded));
    checkMotion(exactDynamic, exactDynamicDecoded);
    assert(exactDynamicDecoded.position == exactDynamic.position &&
           std::signbit(exactDynamicDecoded.position[0]));
    for (unsigned i = 0; i < MotionSlots; ++i) {
        assert(bits(exactDynamicDecoded.motions[i].phase) == bits(exactDynamic.motions[i].phase));
        assert(bits(exactDynamicDecoded.motions[i].weight) == bits(exactDynamic.motions[i].weight));
    }
    exactDynamic.motions[0].weight = 2.0f;
    exactDynamic.motions[7].weight = 10.0f;
    assert(EncodeRecord(sender, exactDynamic, exactDynamicWire, true));
    assert(DecodeRecord(receiver, exactDynamicWire.data(), exactDynamicWire.size(), exactDynamicDecoded));
    assert(exactDynamicDecoded.motions[0].weight == 2.0f &&
           exactDynamicDecoded.motions[7].weight == 10.0f);
    exactDynamic.motions[0].weight = 10.0001f;
    std::vector<std::uint8_t> rejected{9};
    assert(!EncodeRecord(sender, exactDynamic, rejected, true) &&
           rejected == std::vector<std::uint8_t>{9});

    // Independent updates restore every omitted value, even after reordering.
    auto a = base, b = base;
    a.rotation = b.rotation = Yaw{0};
    setField(a, 1, 1); setField(a, 7, 1); setField(a, 16, 0);
    setField(b, 2, 0); setField(b, 20, 1);
    std::vector<std::uint8_t> wireA, wireB, wireBase;
    assert(EncodeRecord(sender, a, wireA) && EncodeRecord(sender, b, wireB));
    auto unchanged = base; unchanged.rotation = Yaw{0};
    assert(EncodeRecord(sender, unchanged, wireBase));
    assert(wireBase.size() == 24 + 8 * 3);
    Control decoded;
    assert(DecodeRecord(receiver, wireA.data(), wireA.size(), decoded)); checkFields(a, decoded);
    assert(DecodeRecord(receiver, wireB.data(), wireB.size(), decoded)); checkFields(b, decoded);
    assert(DecodeRecord(receiver, wireBase.data(), wireBase.size(), decoded)); checkFields(base, decoded);
    assert(DecodeRecord(receiver, wireA.data(), wireA.size(), decoded)); checkFields(a, decoded);

    // Distinct named baselines at the same anchor supply their own omitted fields.
    auto second = base; second.baseline = 21; second.hitPoints = 17; second.action = 11;
    assert(sender.AddBaseline(S, 1, second.baseline, second.anchor, second));
    assert(receiver.AddBaseline(S, 1, second.baseline, second.anchor, second));
    auto withSecond = second; withSecond.rotation = Yaw{0}; withSecond.combatTarget = 42;
    checkWire(sender, receiver, withSecond, std::uint32_t{1} << 20);
    std::vector<std::uint8_t> secondWire;
    assert(EncodeRecord(sender, withSecond, secondWire));
    assert(DecodeRecord(receiver, secondWire.data(), secondWire.size(), decoded));
    checkFields(withSecond, decoded);

    // Truncations and malformed representations fail without touching outputs.
    checkTruncations(receiver, maxWire);
    auto yawAll = all; yawAll.rotation = Yaw{1};
    std::vector<std::uint8_t> yawMax;
    assert(EncodeRecord(sender, yawAll, yawMax));
    checkTruncations(receiver, yawMax);
    std::size_t length = 123456;
    assert(!RecordBytes(nullptr, maxWire.size(), length) && length == 123456);
    auto trailing = maxWire; trailing.push_back(0);
    assert(RecordBytes(trailing.data(), trailing.size(), length) && length == maxWire.size());
    decoded.sent = 0x12345678;
    assert(!DecodeRecord(receiver, trailing.data(), trailing.size(), decoded) && decoded.sent == 0x12345678);
    const auto ext = extensionOffset(all);
    auto bad = maxWire;
    for (unsigned bit = 1; bit <= 4; ++bit) {
        bad = maxWire; bad[offsetof(WireControl, orientation)] |= static_cast<std::uint8_t>(1u << bit);
        length = 123456;
        assert(!RecordBytes(bad.data(), bad.size(), length) && length == 123456);
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    }
    bad = maxWire; bad[offsetof(WireControl, orientation)] &= 0x7f;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    bad = maxWire; bad[offsetof(WireControl, yaw)] = 1;
    assert(!RecordBytes(bad.data(), bad.size(), length));
    bad = maxWire; bad[sizeof(WireControl) + sizeof(WireQuaternion) + 2] |= 0x10;
    assert(!RecordBytes(bad.data(), bad.size(), length));
    bad = maxWire;
    bad[sizeof(WireControl)] = 0; bad[sizeof(WireControl) + 1] = 0;
    bad[sizeof(WireControl) + 2] = 0xc0; bad[sizeof(WireControl) + 3] = 0x7f;
    assert(RecordBytes(bad.data(), bad.size(), length));
    decoded.sent = 0x12345678;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded) && decoded.sent == 0x12345678);
    bad = maxWire; bad[ext] = bad[ext + 1] = bad[ext + 2] = 0;
    assert(!RecordBytes(bad.data(), bad.size(), length));
    for (unsigned bit = 5; bit < 8; ++bit) {
        bad = maxWire; bad[ext + 2] |= static_cast<std::uint8_t>(1u << bit);
        assert(!RecordBytes(bad.data(), bad.size(), length));
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    }
    // Removing any present bit leaves trailing bytes and is never canonical.
    for (unsigned bit = 0; bit < 21; ++bit) {
        bad = maxWire;
        bad[ext + bit / 8] &= static_cast<std::uint8_t>(~(1u << (bit % 8)));
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    }
    // A selected value equal to its baseline is an overlong/noncanonical encoding.
    for (unsigned bit = 0; bit < 21; ++bit) {
        auto one = base; one.rotation = Yaw{0}; setField(one, bit, 1);
        assert(EncodeRecord(sender, one, bad));
        const auto offset = extensionOffset(one);
        for (std::size_t i = 0; i < Widths[bit]; ++i) bad[offset + 3 + i] = 0;
        assert(RecordBytes(bad.data(), bad.size(), length) && length == bad.size());
        decoded.sent = 0x12345678;
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded) && decoded.sent == 0x12345678);
        // Adding a field with no bytes cannot be parsed.
        const auto extraBit = (bit + 1) % 21;
        bad[offset + extraBit / 8] |= static_cast<std::uint8_t>(1u << (extraBit % 8));
        assert(!RecordBytes(bad.data(), bad.size(), length));
    }

    // Motion override masks and payloads are canonical and fail atomically.
    bad = dynamicWire;
    bad[dynamicOverride] = 0;
    assert(!RecordBytes(bad.data(), bad.size(), length));
    decoded.sent = 0x12345678;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded) &&
           decoded.sent == 0x12345678);

    bad = dynamicWire;
    bad[dynamicOverride] = 0x80;
    assert(RecordBytes(bad.data(), bad.size(), length));
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));

    bad = dynamicWire;
    const std::size_t slot0Payload = dynamicOverride + 1;
    bad[slot0Payload] = 0x07;
    bad[slot0Payload + 1] = 0;
    const std::uint32_t baselineName = 100;
    for (unsigned i = 0; i < 4; ++i)
        bad[slot0Payload + 2 + i] = static_cast<std::uint8_t>(baselineName >> (8 * i));
    const std::uint32_t baselineInstance = 0;
    for (unsigned i = 0; i < 4; ++i)
        bad[slot0Payload + 6 + i] = static_cast<std::uint8_t>(baselineInstance >> (8 * i));
    const std::uint32_t baselineSpeed = 0x3f800000u;
    for (unsigned i = 0; i < 4; ++i)
        bad[slot0Payload + 10 + i] = static_cast<std::uint8_t>(baselineSpeed >> (8 * i));
    assert(RecordBytes(bad.data(), bad.size(), length) && length == bad.size());
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));

    bad = dynamicWire;
    bad[slot0Payload] = 0;
    bad[slot0Payload + 1] = 0;
    assert(!RecordBytes(bad.data(), bad.size(), length));
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));

    bad = dynamicWire;
    const std::size_t slot3Payload = slot0Payload + 14;
    bad[slot3Payload] = 0xe0;
    bad[slot3Payload + 1] = 0xe0;
    bad[slot3Payload + 2] = 1;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));

    // Changed NaN/infinity values are invalid, while both signs of zero differ.
    for (const auto raw : {0x7f800000u, 0xff800000u, 0x7fc01234u}) {
        auto value = base; value.damageMultiplier = fromBits(raw);
        std::vector<std::uint8_t> untouched{7};
        assert(!EncodeRecord(sender, value, untouched) && untouched == std::vector<std::uint8_t>{7});
        value.damageMultiplier = -0.0f;
        assert(EncodeRecord(sender, value, bad));
        const auto offset = extensionOffset(value) + 3;
        for (unsigned i = 0; i < 4; ++i) bad[offset + i] = static_cast<std::uint8_t>(raw >> (8 * i));
        assert(RecordBytes(bad.data(), bad.size(), length));
        decoded.sent = 0x12345678;
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded) && decoded.sent == 0x12345678);
    }
    auto negativeZero = base; negativeZero.baseline = 22; negativeZero.damageMultiplier = -0.0f;
    assert(sender.AddBaseline(S, 1, 22, negativeZero.anchor, negativeZero));
    assert(receiver.AddBaseline(S, 1, 22, negativeZero.anchor, negativeZero));
    negativeZero.damageMultiplier = 0.0f;
    checkWire(sender, receiver, negativeZero, std::uint32_t{1} << 16);

    // Unknown ID, anchor, baseline and old life never apply an override.
    Context empty, oldLife;
    assert(empty.Reset(S.epoch, S.generation) && oldLife.Reset(S.epoch, S.generation));
    install(oldLife, base, 1);
    assert(!DecodeRecord(empty, wireA.data(), wireA.size(), decoded));
    for (const auto offset : {offsetof(WireControl, id), offsetof(WireControl, anchor), offsetof(WireControl, baseline)}) {
        bad = wireA; bad[offset] ^= 0x40;
        decoded.sent = 0x12345678;
        assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded) && decoded.sent == 0x12345678);
    }
    assert(oldLife.AdvanceLife(S, 1, base.incarnation + 1));
    assert(!DecodeRecord(oldLife, wireA.data(), wireA.size(), decoded));

    // An unencodable active weight still takes the unchanged full fallback.
    auto overweight = all; overweight.motions[0].weight = 2.0f;
    std::vector<std::uint8_t> fallback;
    assert(!EncodeRecord(sender, overweight, fallback));
    assert(EncodeFullFallback(sender, 1, overweight, fallback));
    Context fallbackReceiver;
    assert(fallbackReceiver.Reset(S.epoch, S.generation)); install(fallbackReceiver, base, 1);
    assert(DecodeFullFallback(fallbackReceiver, fallback.data(), fallback.size(), decoded));
    checkFields(overweight, decoded);
    assert(decoded.motions[0].weight == 2.0f);

    // Mixed lengths/modes in a batch, and failed batches do not consume sequence.
    Context batchSender, batchReceiver;
    assert(batchSender.Reset(S.epoch, S.generation) && batchReceiver.Reset(S.epoch, S.generation));
    auto first = snapshot(2, 0), middle = snapshot(3, 0x81), last = snapshot(4, 0xff);
    install(batchSender, first, 2); install(batchReceiver, first, 2);
    install(batchSender, middle, 3); install(batchReceiver, middle, 3);
    install(batchSender, last, 4); install(batchReceiver, last, 4);
    first.rotation = Yaw{0}; middle.rotation = Yaw{1}; middle.hitPoints = -99;
    last.combatTarget = 123; last.damageMultiplier = -0.0f;
    std::vector<std::uint8_t> batch;
    assert(EncodeBatch(batchSender, 10, {first, middle, last}, batch));
    std::size_t cursor = sizeof(WireBatchHeader), middleOffset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 1) middleOffset = cursor;
        assert(RecordBytes(batch.data() + cursor, batch.size() - cursor, length));
        cursor += length;
    }
    assert(cursor == batch.size());
    bad = batch;
    const auto middleMask = middleOffset + extensionOffset(middle);
    bad[middleMask] = bad[middleMask + 1] = bad[middleMask + 2] = 0;
    std::vector<Control> out{base};
    assert(!DecodeBatch(batchReceiver, bad.data(), bad.size(), out));
    assert(out.size() == 1 && out[0].guid == base.guid);
    bad = batch;
    for (unsigned i = 0; i < 4; ++i) bad[middleMask + 3 + i] = 0;
    assert(!DecodeBatch(batchReceiver, bad.data(), bad.size(), out));
    assert(out.size() == 1 && out[0].guid == base.guid);
    for (std::size_t n = 0; n < batch.size(); ++n)
        assert(!DecodeBatch(batchReceiver, batch.data(), n, out) && out.size() == 1);
    assert(DecodeBatch(batchReceiver, batch.data(), batch.size(), out) && out.size() == 3);
    checkFields(first, out[0]); checkFields(middle, out[1]); checkFields(last, out[2]);
    assert(!DecodeBatch(batchReceiver, batch.data(), batch.size(), out));
    middle.hitPoints = 0; last.combatTarget = 0; last.damageMultiplier = 0.0f;
    assert(EncodeBatch(batchSender, 12, {first, middle, last}, batch));
    assert(DecodeBatch(batchReceiver, batch.data(), batch.size(), out));
    checkFields(middle, out[1]); checkFields(last, out[2]);
}
