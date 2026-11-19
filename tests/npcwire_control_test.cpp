// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcwire_control.h"
#include "../src/net.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace g3t::npcwire;
static_assert(sizeof(WireFullRecord) == sizeof(g3t::net::NpcRecord), "full legacy size");
static_assert(offsetof(WireFullRecord, combatTarget) == offsetof(g3t::net::NpcRecord, combatTarget), "target offset");
static_assert(offsetof(WireFullRecord, motions) == offsetof(g3t::net::NpcRecord, motions), "motion offset");
static_assert(offsetof(WireFullMotion, options) == offsetof(g3t::net::NpcMotion, options), "options offset");

namespace {
constexpr Stream S{20, 1};
constexpr double Turn = 6.283185307179586476925286766559;
Guid guid(unsigned n) {
    Guid g{};
    g[0] = static_cast<std::uint8_t>(n);
    g[1] = static_cast<std::uint8_t>(n >> 8);
    return g;
}
Control snapshot(unsigned id, std::uint8_t mask) {
    Control v;
    v.guid = guid(id); v.anchor = 100; v.baseline = 200; v.incarnation = 7; v.slots = mask;
    v.position = {{1244.125, -2007.5, 42.75}};
    v.rotation = Quaternion{{{0.1f, 0.2f, 0.3f, 0.9f}}};
    v.sent = 0xfedcba98u; v.movementMode = 7; v.flags = 0xdf;
    v.hitPoints = std::numeric_limits<std::int32_t>::min(); v.hitPointsMax = 2147483647;
    v.motion = 0xaabbccddu; v.aniState = 9; v.heldRight = 123; v.heldLeft = 456;
    v.partyPlayer = 1; v.partyType = 2; v.waiting = 3; v.guidePlayer = 4;
    v.aiMode = 5; v.action = 6; v.attackReason = 7; v.damageType = 8;
    v.damageAmount = -12345678; v.damageMultiplier = 17.25f;
    v.hitPlayer = 12; v.hitSequence = 0xfabc; v.contact = 0x80; v.attitude = 99;
    v.combatTarget = 0xffffffffu;
    for (unsigned i = 0; i < MotionSlots; ++i) {
        auto& m = v.motions[i];
        m.descriptor.name = 1000 + i; m.descriptor.instance = 0xf0001000u + i;
        m.descriptor.speed = -1.5f; m.descriptor.intro = 12.5f; m.descriptor.extro = -2.0f;
        m.descriptor.blend = -1234;
        for (unsigned j = 0; j < 7; ++j) m.descriptor.options[j] = 0xfe000000u + 256 * i + j;
        m.active = mask & (1u << i) ? 3 : 2;
        m.phase = (mask & (1u << i)) ? 0.25f : 2.5f;
        m.weight = (mask & (1u << i)) ? 0.5f : 10.0f;
    }
    return v;
}
void install(Context& c, const Control& v, Stream stream = S, std::uint16_t id = 1) {
    assert(c.AddId(stream, id, v.guid));
    assert(c.AdvanceLife(stream, id, v.incarnation));
    assert(c.AddBaseline(stream, id, v.baseline, v.anchor, v));
}
void checkStatic(const Control& a, const Control& b) {
    assert(a.guid == b.guid && a.movementMode == b.movementMode);
    assert(a.hitPoints == b.hitPoints && a.hitPointsMax == b.hitPointsMax && a.motion == b.motion);
    assert(a.aniState == b.aniState && a.heldRight == b.heldRight && a.heldLeft == b.heldLeft);
    assert(a.partyPlayer == b.partyPlayer && a.partyType == b.partyType && a.waiting == b.waiting &&
           a.guidePlayer == b.guidePlayer && a.aiMode == b.aiMode && a.action == b.action);
    assert(a.attackReason == b.attackReason && a.damageType == b.damageType &&
           a.damageAmount == b.damageAmount && a.damageMultiplier == b.damageMultiplier);
    assert(a.incarnation == b.incarnation && a.hitPlayer == b.hitPlayer && a.hitSequence == b.hitSequence &&
           a.attitude == b.attitude && a.combatTarget == b.combatTarget);
    for (unsigned i = 0; i < MotionSlots; ++i) {
        const auto& x = a.motions[i]; const auto& y = b.motions[i];
        assert(x.descriptor.name == y.descriptor.name && x.descriptor.instance == y.descriptor.instance);
        assert(x.descriptor.speed == y.descriptor.speed && x.descriptor.intro == y.descriptor.intro &&
               x.descriptor.extro == y.descriptor.extro && x.descriptor.blend == y.descriptor.blend &&
               x.descriptor.options == y.descriptor.options && x.active == y.active);
        if (!(x.active & 1u)) assert(x.phase == y.phase && x.weight == y.weight);
    }
}
g3t::net::NpcRecord legacy(const Control& v) {
    g3t::net::NpcRecord n;
    for (unsigned i = 0; i < 16; ++i) n.guid[i] = v.guid[i];
    for (unsigned i = 0; i < 3; ++i) n.position[i] = static_cast<float>(v.position[i]);
    for (unsigned i = 0; i < 4; ++i) n.rotation[i] = std::get<Quaternion>(v.rotation).components[i];
    n.movementMode = v.movementMode; n.flags = v.flags; n.hitPoints = v.hitPoints; n.hitPointsMax = v.hitPointsMax;
    n.motion = v.motion; n.aniState = v.aniState; n.heldRight = v.heldRight; n.heldLeft = v.heldLeft; n.sent = v.sent;
    n.partyPlayer = v.partyPlayer; n.partyType = v.partyType; n.waiting = v.waiting; n.guidePlayer = v.guidePlayer;
    n.aiMode = v.aiMode; n.action = v.action; n.attackReason = v.attackReason; n.damageType = v.damageType;
    n.damageAmount = v.damageAmount; n.damageMultiplier = v.damageMultiplier; n.incarnation = v.incarnation;
    n.hitPlayer = v.hitPlayer; n.hitSequence = v.hitSequence; n.contact = v.contact; n.attitude = v.attitude;
    n.combatTarget = v.combatTarget;
    for (unsigned i = 0; i < MotionSlots; ++i) {
        const auto& m = v.motions[i]; auto& d = n.motions[i];
        d.name = m.descriptor.name; d.instance = m.descriptor.instance; d.phase = m.phase; d.speed = m.descriptor.speed;
        d.weight = m.weight; d.intro = m.descriptor.intro; d.extro = m.descriptor.extro; d.blend = m.descriptor.blend;
        for (unsigned j = 0; j < 7; ++j) d.options[j] = m.descriptor.options[j];
        d.active = m.active;
    }
    return n;
}
}

int main() {
    assert(Newer(0, 0xffffffffu) && !Newer(0xffffffffu, 0) && !Newer(3, 3));
    assert(!Newer(0x80000000u, 0) && !Newer(0, 0x80000000u));
    Context wrap;
    assert(wrap.Reset(0xffffffffu, 0xffffffffu));
    assert(wrap.AddId({0xffffffffu, 0xffffffffu}, 1, guid(1)));
    assert(wrap.AdvanceLife({0xffffffffu, 0xffffffffu}, 1, 0xffffffffu));
    assert(wrap.AdvanceLife({0xffffffffu, 0xffffffffu}, 1, 1));
    assert(!wrap.AdvanceLife({0xffffffffu, 0xffffffffu}, 1, 0xffffffffu));
    assert(wrap.Reset(1, 1));
    assert(!wrap.Matches({0xffffffffu, 0xffffffffu}));

    // No retained state can become valid again after the incarnation wraps.
    Context lifeSender, lifeReceiver, explicitLife;
    assert(lifeSender.Reset(S.epoch, S.generation));
    assert(lifeReceiver.Reset(S.epoch, S.generation));
    assert(explicitLife.Reset(S.epoch, S.generation));
    auto wrappedLife = snapshot(1, 1);
    wrappedLife.incarnation = 1; wrappedLife.baseline = 1; wrappedLife.anchor = 1;
    install(lifeSender, wrappedLife);
    install(explicitLife, wrappedLife);
    std::vector<std::uint8_t> wrappedId, wrappedBase, wrappedRecord;
    assert(EncodeId(lifeSender, 1, 1, wrappedId));
    std::uint16_t wrappedIdOut = 0;
    assert(DecodeId(lifeReceiver, wrappedId.data(), wrappedId.size(), wrappedIdOut));
    assert(EncodeBaseline(lifeSender, 1, 1, 1, wrappedBase));
    BaselineRef wrappedRef;
    assert(DecodeBaseline(lifeReceiver, wrappedBase.data(), wrappedBase.size(), wrappedRef));
    assert(EncodeRecord(lifeSender, wrappedLife, wrappedRecord));
    const std::uint32_t incarnations[] = {0x80000000u, 0xffffffffu, 1u};
    for (unsigned i = 0; i < 3; ++i) {
        assert(explicitLife.AdvanceLife(S, 1, incarnations[i]));
        wrappedLife.incarnation = incarnations[i];
        wrappedLife.baseline = static_cast<std::uint16_t>(i + 2);
        wrappedLife.anchor = i + 2;
        assert(lifeSender.AdvanceLife(S, 1, wrappedLife.incarnation));
        assert(lifeSender.AddBaseline(S, 1, wrappedLife.baseline, wrappedLife.anchor, wrappedLife));
        std::vector<std::uint8_t> next;
        assert(EncodeBaseline(lifeSender, i + 2, 1, wrappedLife.baseline, next));
        assert(DecodeBaseline(lifeReceiver, next.data(), next.size(), wrappedRef));
    }
    Control wrappedOutput;
    assert(!DecodeRecord(explicitLife, wrappedRecord.data(), wrappedRecord.size(), wrappedOutput));
    assert(!DecodeRecord(lifeReceiver, wrappedRecord.data(), wrappedRecord.size(), wrappedOutput));
    assert(!DecodeBaseline(lifeReceiver, wrappedBase.data(), wrappedBase.size(), wrappedRef));
    wrappedLife.baseline = 1; wrappedLife.anchor = 1;
    assert(!explicitLife.AddBaseline(S, 1, 1, 1, wrappedLife));
    assert(!lifeReceiver.AddBaseline(S, 1, 1, 1, wrappedLife));

    std::uint16_t yaw = 55;
    assert(!EncodeYaw(std::numeric_limits<double>::quiet_NaN(), yaw) && yaw == 55);
    std::mt19937 rng(91417);
    std::uniform_real_distribution<double> angles(-1000 * Turn, 1000 * Turn);
    for (int i = 0; i < 50000; ++i) {
        const double a = angles(rng);
        assert(EncodeYaw(a, yaw));
        assert(std::abs(std::remainder(DecodeYaw(yaw) - a, Turn)) <= Turn / 131072.0 + 1e-11);
    }
    assert(EncodeYaw(-Turn / 65536, yaw) && yaw == 65535);
    assert(EncodeYaw(Turn, yaw) && yaw == 0);

    Context sender, receiver;
    assert(sender.Reset(S.epoch, S.generation) && receiver.Reset(S.epoch, S.generation));
    Control v = snapshot(1, 0x81);
    install(sender, v);
    assert(sender.AddId(S, 1, v.guid));
    assert(!sender.AddId(S, 1, guid(2)) && !sender.AddId(S, 2, v.guid) && !sender.AddId(S, 0, guid(2)));
    assert(sender.AddBaseline(S, 1, 200, 100, v));
    auto conflict = v; conflict.motions[1].descriptor.options[6]++;
    assert(!sender.AddBaseline(S, 1, 200, 100, conflict));
    assert(sender.AddBaseline(S, 1, 201, 100, conflict));
    auto otherAnchor = v;
    otherAnchor.position[0] += 1;
    assert(!sender.AddBaseline(S, 1, 202, 100, otherAnchor));
    Anchor saved;
    assert(sender.GetAnchor(1, 100, saved));
    assert(saved.position[0] == 1244.125f && saved.rotation == std::get<Quaternion>(v.rotation).components);

    std::vector<std::uint8_t> idWire, baseWire, laterBase, bytes, full;
    assert(EncodeId(sender, 1, 1, idWire) && idWire.size() == 36);
    std::uint16_t outId = 999;
    for (std::size_t n = 0; n < idWire.size(); ++n)
        assert(!DecodeId(receiver, idWire.data(), n, outId) && outId == 999);
    assert(DecodeId(receiver, idWire.data(), idWire.size(), outId) && outId == 1);
    assert(DecodeId(receiver, idWire.data(), idWire.size(), outId) && outId == 1);
    auto bad = idWire; bad.back() ^= 1;
    assert(!DecodeId(receiver, bad.data(), bad.size(), outId));
    bad = idWire; bad[sizeof(WireBatchHeader)] = 2; // same GUID, different ID
    assert(!DecodeId(receiver, bad.data(), bad.size(), outId));
    bad = idWire; bad[4] ^= 1;
    assert(!DecodeId(receiver, bad.data(), bad.size(), outId));
    assert(EncodeBaseline(sender, 2, 1, 200, baseWire) && baseWire.size() == 614);
    assert(EncodeBaseline(sender, 3, 1, 201, laterBase) && laterBase.size() == 614);
    BaselineRef ref;
    for (std::size_t n = 0; n < baseWire.size(); ++n)
        assert(!DecodeBaseline(receiver, baseWire.data(), n, ref) && ref.id == 0);
    // Installing an older reliable baseline after a newer one is safe.
    assert(DecodeBaseline(receiver, laterBase.data(), laterBase.size(), ref));
    assert(DecodeBaseline(receiver, baseWire.data(), baseWire.size(), ref));
    assert(ref.id == 1 && ref.baseline == 200 && ref.anchor == 100 && ref.incarnation == 7);
    assert(DecodeBaseline(receiver, baseWire.data(), baseWire.size(), ref));
    bad = baseWire; bad.back() ^= 1;
    assert(!DecodeBaseline(receiver, bad.data(), bad.size(), ref));
    bad = baseWire; bad[sizeof(WireBatchHeader) + offsetof(WireBaselinePrefix, anchor)]++;
    assert(!DecodeBaseline(receiver, bad.data(), bad.size(), ref));
    const auto oldRecord = legacy(v);
    const std::uint16_t endianProbe = 1;
    if (*reinterpret_cast<const std::uint8_t*>(&endianProbe) == 1)
        assert(std::memcmp(baseWire.data() + sizeof(WireBatchHeader) + sizeof(WireBaselinePrefix),
                           &oldRecord, sizeof(oldRecord)) == 0);

    Signal ack;
    ack.reference = {1, 200, 7, 100};
    assert(EncodeSignal(receiver, 9, ack, bytes) && bytes.size() == 31);
    Signal parsed;
    assert(DecodeSignal(sender, bytes.data(), bytes.size(), parsed));
    assert(parsed.kind == SignalKind::Ack && parsed.sequence == 9 && parsed.reference.baseline == 200);
    assert(DecodeSignal(sender, bytes.data(), bytes.size(), parsed)); // idempotent ACK
    bad = bytes; bad[sizeof(WireBatchHeader) + offsetof(WireSignal, anchor)]++;
    assert(!DecodeSignal(sender, bad.data(), bad.size(), parsed));
    for (std::size_t n = 0; n < bytes.size(); ++n) assert(!DecodeSignal(sender, bytes.data(), n, parsed));
    Signal request;
    request.kind = SignalKind::Resync; request.reference.id = 65535;
    request.reason = ResyncReason::UnknownId;
    assert(EncodeSignal(receiver, 10, request, bytes));
    assert(DecodeSignal(sender, bytes.data(), bytes.size(), parsed));
    assert(parsed.kind == SignalKind::Resync && parsed.reason == ResyncReason::UnknownId);
    request.reason = ResyncReason::None;
    assert(!EncodeSignal(receiver, 10, request, bytes));

    // A compact update carries only dynamic fields, independently of prior updates.
    auto update = v;
    update.rotation = Yaw{Turn - 0.00001};
    update.position = {{1254.425, -2014.5, 42.75}};
    update.sent = 0x12345678; update.flags = 0x41; update.contact = 0xe7;
    update.motions[0].phase = 0.1337f; update.motions[0].weight = 0.9123f;
    update.motions[7].phase = 1.0f; update.motions[7].weight = 0.0f;
    assert(EncodeRecord(sender, update, bytes) && bytes.size() == 24 + 2 * 3);
    Control decoded;
    assert(DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    checkStatic(v, decoded);
    assert(decoded.sent == update.sent && decoded.flags == update.flags && decoded.contact == update.contact);
    assert(std::abs(decoded.position[0] - update.position[0]) <= 0.5);
    assert(std::abs(decoded.motions[0].phase - update.motions[0].phase) <= 0.5 / 4095 + 1e-7);
    assert(std::abs(decoded.motions[0].weight - update.motions[0].weight) <= 0.5 / 255 + 1e-7);
    assert(decoded.motions[1].phase == 2.5f && decoded.motions[1].weight == 10.0f);
    assert(decoded.motions[7].phase == 1 && decoded.motions[7].weight == 0);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        Control untouched; untouched.sent = 0x123;
        assert(!DecodeRecord(receiver, bytes.data(), n, untouched) && untouched.sent == 0x123);
    }
    bad = bytes; bad.back() |= 0xf0;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    bad = bytes; bad[offsetof(WireControl, slots)] ^= 2;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    bad = bytes; bad[offsetof(WireControl, baseline)] = 99;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    bad = bytes; bad[offsetof(WireControl, anchor)] = 99;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));
    auto fullOrientation = update;
    fullOrientation.rotation = Quaternion{{{0.1f, 0.2f, 0.3f, 0.9f}}};
    assert(EncodeRecord(sender, fullOrientation, bytes) && bytes.size() == 24 + 16 + 6);
    assert(DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    assert(std::get<Quaternion>(decoded.rotation).components == std::get<Quaternion>(fullOrientation.rotation).components);
    bad = bytes; bad[offsetof(WireControl, yaw)] = 1;
    assert(!DecodeRecord(receiver, bad.data(), bad.size(), decoded));

    // Scalar metadata changes are reconstructed from the named baseline.
    auto reject = [&](const Control& x) {
        std::vector<std::uint8_t> unchanged{9};
        assert(!EncodeRecord(sender, x, unchanged) && unchanged == std::vector<std::uint8_t>{9});
    };
    auto accept = [&](const Control& x) {
        std::vector<std::uint8_t> wire;
        Control result;
        assert(EncodeRecord(sender, x, wire));
        assert(DecodeRecord(receiver, wire.data(), wire.size(), result));
        checkStatic(x, result);
    };
    auto changed = update; changed.hitPoints++; accept(changed);
    changed = update; changed.hitPointsMax--; accept(changed);
    changed = update; changed.movementMode++; accept(changed);
    changed = update; changed.motion++; accept(changed);
    changed = update; changed.aniState++; accept(changed);
    changed = update; changed.heldRight++; accept(changed);
    changed = update; changed.heldLeft++; accept(changed);
    changed = update; changed.partyPlayer++; accept(changed);
    changed = update; changed.partyType++; accept(changed);
    changed = update; changed.waiting++; accept(changed);
    changed = update; changed.guidePlayer++; accept(changed);
    changed = update; changed.aiMode++; accept(changed);
    changed = update; changed.action++; accept(changed);
    changed = update; changed.attackReason++; accept(changed);
    changed = update; changed.damageType++; accept(changed);
    changed = update; changed.damageAmount++; accept(changed);
    changed = update; changed.damageMultiplier += 1; accept(changed);
    changed = update; changed.hitPlayer++; accept(changed);
    changed = update; changed.hitSequence++; accept(changed);
    changed = update; changed.attitude++; accept(changed);
    changed = update; changed.combatTarget--; accept(changed);
    // Life and GUID are immutable; descriptors and motion state use full overrides.
    changed = update; changed.incarnation++; reject(changed);
    changed = update; changed.motions[0].descriptor.instance++; accept(changed);
    changed = update; changed.motions[1].descriptor.options[6]++; accept(changed);
    changed = update; changed.motions[0].active = 2; accept(changed);
    changed = update; changed.motions[1].phase++; accept(changed);
    changed = update; changed.motions[1].weight = 9.0f; accept(changed);
    changed = update; changed.motions[0].weight = 10.0f; reject(changed);
    changed = update; changed.motions[0].phase = 1.00001f; reject(changed);
    changed = update; changed.motions[0].weight = std::numeric_limits<float>::quiet_NaN(); reject(changed);
    changed = update; changed.position[0] = 1244.125 + 32767.001; reject(changed);
    changed = update; changed.position[0] = 1244.125 - 32768.001; reject(changed);
    auto edges = update;
    edges.position[0] = 1244.125 - 32768;
    edges.position[1] = -2007.5 + 32767;
    assert(EncodeRecord(sender, edges, bytes) && DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    assert(decoded.position[0] == edges.position[0] && decoded.position[1] == edges.position[1]);

    // Exact fallback supports extended weights and inactive slot data.
    auto extended = v; extended.motions[0].weight = 10;
    assert(!EncodeRecord(sender, extended, bytes));
    assert(EncodeFullFallback(sender, 1, extended, full) && full.size() == 606);
    assert(DecodeFullFallback(receiver, full.data(), full.size(), decoded));
    checkStatic(extended, decoded);
    assert(decoded.motions[0].weight == 10 && decoded.motions[1].weight == 10 && decoded.slots == 0x81);
    assert(!DecodeFullFallback(receiver, full.data(), full.size(), decoded));
    auto exactExtended = v;
    exactExtended.motions[0].weight = 2.0f;
    assert(EncodeRecord(sender, exactExtended, bytes, true));
    assert(DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    assert(decoded.motions[0].weight == 2.0f);
    exactExtended.motions[0].weight = 10.0f;
    assert(EncodeRecord(sender, exactExtended, bytes, true));
    assert(DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    assert(decoded.motions[0].weight == 10.0f);
    exactExtended.motions[0].weight = 10.0001f;
    assert(!EncodeRecord(sender, exactExtended, bytes, true));
    auto overflow = v; overflow.position[0] = static_cast<double>(1244.125f + 40000.0f);
    assert(!EncodeRecord(sender, overflow, bytes));
    assert(EncodeFullFallback(sender, 2, overflow, bytes));
    assert(DecodeFullFallback(receiver, bytes.data(), bytes.size(), decoded));
    assert(decoded.position == overflow.position);

    // Wrap, loss, reordering and atomicity are shared by full and compact paths.
    Context ordered;
    assert(ordered.Reset(S.epoch, S.generation)); install(ordered, v);
    assert(EncodeBatch(sender, 0xfffffffeu, {update}, bytes));
    std::vector<Control> out;
    assert(DecodeBatch(ordered, bytes.data(), bytes.size(), out));
    assert(!DecodeBatch(ordered, bytes.data(), bytes.size(), out));
    assert(EncodeBatch(sender, 0, {update}, bytes));
    assert(DecodeBatch(ordered, bytes.data(), bytes.size(), out));
    const std::array<double, 3> expectedPosition{{1254.125, -2014.5, 42.75}};
    assert(out[0].position == expectedPosition);
    assert(DecodeBaseline(ordered, baseWire.data(), baseWire.size(), ref));
    assert(!DecodeBatch(ordered, bytes.data(), bytes.size(), out)); // Baseline receipt did not reset the fence.
    assert(EncodeBatch(sender, 0xffffffffu, {update}, bytes));
    assert(!DecodeBatch(ordered, bytes.data(), bytes.size(), out));
    assert(!EncodeBatch(sender, 1, {update, update}, bytes));

    Context lossless, lossy;
    assert(lossless.Reset(S.epoch, S.generation) && lossy.Reset(S.epoch, S.generation));
    install(lossless, v); install(lossy, v);
    for (std::uint32_t sequence = 1; sequence <= 500; ++sequence) {
        auto x = update;
        x.position[0] = 1244.125 + static_cast<int>(rng() % 60001) - 30000;
        x.position[1] = -2007.5 + static_cast<int>(rng() % 60001) - 30000;
        x.motions[7].weight = (rng() % 10001) / 10000.0f;
        x.sent = rng(); x.flags = static_cast<std::uint8_t>(rng());
        assert(EncodeBatch(sender, sequence, {x}, bytes));
        std::vector<Control> a, b;
        assert(DecodeBatch(lossless, bytes.data(), bytes.size(), a));
        if (sequence % 7 != 0) continue;
        assert(DecodeBatch(lossy, bytes.data(), bytes.size(), b));
        assert(a[0].position == b[0].position && a[0].sent == b[0].sent && a[0].flags == b[0].flags);
        assert(a[0].motions[7].weight == b[0].motions[7].weight);
    }

    // All masks and quantization bounds, including arbitrary inactive slot state.
    for (unsigned mask = 0; mask <= 0xff; ++mask) {
        Context c;
        assert(c.Reset(S.epoch, S.generation));
        auto x = snapshot(1, static_cast<std::uint8_t>(mask));
        install(c, x);
        x.rotation = Yaw{0.25};
        for (auto& m : x.motions) if (m.active & 1u) {
            m.phase = (rng() % 10001) / 10000.0f;
            m.weight = (rng() % 10001) / 10000.0f;
        }
        assert(EncodeRecord(c, x, bytes));
        assert(DecodeRecord(c, bytes.data(), bytes.size(), decoded));
        checkStatic(x, decoded);
        for (unsigned i = 0; i < MotionSlots; ++i) if (mask & (1u << i)) {
            assert(std::abs(decoded.motions[i].phase - x.motions[i].phase) <= 0.5 / 4095 + 1e-7);
            assert(std::abs(decoded.motions[i].weight - x.motions[i].weight) <= 0.5 / 255 + 1e-7);
        }
    }

    Context many, manyReceiver;
    assert(many.Reset(100, 1) && manyReceiver.Reset(100, 1));
    std::vector<Control> entries;
    for (unsigned i = 1; i <= 49; ++i) {
        auto x = snapshot(i, 0); install(many, x, {100, 1}, static_cast<std::uint16_t>(i));
        install(manyReceiver, x, {100, 1}, static_cast<std::uint16_t>(i));
        x.rotation = Yaw{0};
        entries.push_back(x);
    }
    auto fit = entries; fit.pop_back();
    assert(EncodeBatch(many, 1, fit, bytes) && bytes.size() == 1170 && bytes.size() <= MaxBatchBytes);
    std::vector<std::uint8_t> untouched{7};
    assert(!EncodeBatch(many, 1, entries, untouched) && untouched == std::vector<std::uint8_t>{7});
    bad = bytes;
    bad[sizeof(WireBatchHeader) + sizeof(WireControl) + offsetof(WireControl, baseline)] = 99;
    out = {v};
    assert(!DecodeBatch(manyReceiver, bad.data(), bad.size(), out) && out.size() == 1 && out[0].guid == v.guid);
    assert(DecodeBatch(manyReceiver, bytes.data(), bytes.size(), out) && out.size() == 48);

    // A newer life prevents late baseline/compact/full resurrection.
    assert(EncodeRecord(sender, update, bytes));
    assert(receiver.AdvanceLife(S, 1, 8));
    assert(!DecodeBaseline(receiver, baseWire.data(), baseWire.size(), ref));
    assert(!DecodeFullFallback(receiver, full.data(), full.size(), decoded));
    assert(!DecodeRecord(receiver, bytes.data(), bytes.size(), decoded));
    auto nextLife = v; nextLife.incarnation = 8; nextLife.baseline = 202; nextLife.anchor = 101;
    assert(sender.AdvanceLife(S, 1, 8) && sender.AddBaseline(S, 1, 202, 101, nextLife));
    assert(EncodeBaseline(sender, 100, 1, 202, bytes));
    assert(DecodeBaseline(receiver, bytes.data(), bytes.size(), ref) && ref.incarnation == 8);
    assert(!DecodeBaseline(receiver, baseWire.data(), baseWire.size(), ref));
    Context autoLife;
    assert(autoLife.Reset(S.epoch, S.generation));
    assert(DecodeId(autoLife, idWire.data(), idWire.size(), outId));
    assert(DecodeBaseline(autoLife, baseWire.data(), baseWire.size(), ref) && ref.incarnation == 7);
    assert(DecodeBaseline(autoLife, bytes.data(), bytes.size(), ref) && ref.incarnation == 8);
    assert(!DecodeBaseline(autoLife, baseWire.data(), baseWire.size(), ref));
    assert(!receiver.Reset(20, 1) && !receiver.Reset(19, 2));
    assert(receiver.Reset(20, 2));
    assert(!DecodeId(receiver, idWire.data(), idWire.size(), outId));
    assert(!DecodeBaseline(receiver, baseWire.data(), baseWire.size(), ref));
    assert(!receiver.AddId(S, 1, v.guid));

    // Failed new-life install must not advance life or poison its tokens.
    Context atomicLife;
    assert(atomicLife.Reset(S.epoch, S.generation));
    assert(DecodeId(atomicLife, idWire.data(), idWire.size(), outId));
    assert(DecodeBaseline(atomicLife, baseWire.data(), baseWire.size(), ref));
    auto collision = baseWire;
    const auto fullOffset = sizeof(WireBatchHeader) + sizeof(WireBaselinePrefix);
    collision[fullOffset + offsetof(WireFullRecord, incarnation)] = 8;
    assert(!DecodeBaseline(atomicLife, collision.data(), collision.size(), ref));
    assert(atomicLife.GetAnchor(1, 100, saved));
    assert(EncodeRecord(ordered, update, bytes) && DecodeRecord(atomicLife, bytes.data(), bytes.size(), decoded));
    assert(EncodeBaseline(sender, 100, 1, 202, bytes));
    assert(DecodeBaseline(atomicLife, bytes.data(), bytes.size(), ref) && ref.incarnation == 8);
    auto reusedLifeAnchor = nextLife; reusedLifeAnchor.anchor = 100; reusedLifeAnchor.baseline = 203;
    assert(!atomicLife.AddBaseline(S, 1, 203, 100, reusedLifeAnchor));

    // Anchor IDs may arrive slightly out of order, but cannot wrap or be
    // resurrected after the bounded seen window.
    Context reordered;
    assert(reordered.Reset(S.epoch, S.generation));
    auto first = snapshot(1, 0); first.anchor = 1000; first.baseline = 1;
    install(reordered, first);
    auto backwards = first; backwards.anchor = 998; backwards.baseline = 2; backwards.position[0] += 1;
    assert(reordered.AddBaseline(S, 1, 2, 998, backwards));
    auto tooOld = first; tooOld.anchor = 936; tooOld.baseline = 3;
    assert(!reordered.AddBaseline(S, 1, 3, 936, tooOld));
    auto sameAnchor = first; sameAnchor.baseline = 3; sameAnchor.damageAmount++;
    assert(reordered.AddBaseline(S, 1, 3, 1000, sameAnchor));
    assert(reordered.RetainedBaselines(1) == 3 && reordered.RetainedAnchors(1) == 2);

    // Shared anchors are retained until their last baseline is evicted.
    Context shared;
    assert(shared.Reset(S.epoch, S.generation));
    auto sharedBase = snapshot(1, 0); sharedBase.anchor = 1; sharedBase.baseline = 1;
    install(shared, sharedBase);
    for (unsigned token = 2; token <= 17; ++token) {
        sharedBase.baseline = static_cast<std::uint16_t>(token);
        sharedBase.damageAmount = static_cast<int>(token);
        assert(shared.AddBaseline(S, 1, sharedBase.baseline, 1, sharedBase));
    }
    assert(shared.RetainedBaselines(1) == MaxBaselinesPerNpc && shared.RetainedAnchors(1) == 1);
    assert(shared.GetAnchor(1, 1, saved));

    // Full-record baseline and fallback codecs reject non-finite or out-of-range motion/damage floats.
    Context fullGuard;
    assert(fullGuard.Reset(S.epoch, S.generation));
    assert(fullGuard.AddId(S, 1, guid(1)));
    assert(fullGuard.AdvanceLife(S, 1, 7));
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto badDamage = snapshot(1, 1);
    badDamage.damageMultiplier = qnan;
    assert(!fullGuard.AddBaseline(S, 1, 1, 1, badDamage));
    assert(!EncodeFullFallback(fullGuard, 1, badDamage, bytes));
    auto badSpeed = snapshot(1, 1);
    badSpeed.motions[0].descriptor.speed = inf;
    assert(!fullGuard.AddBaseline(S, 1, 1, 1, badSpeed));
    assert(!EncodeFullFallback(fullGuard, 1, badSpeed, bytes));
    auto badActivePhase = snapshot(1, 1);
    badActivePhase.motions[0].phase = 1.25f;
    assert(!fullGuard.AddBaseline(S, 1, 1, 1, badActivePhase));
    assert(!EncodeFullFallback(fullGuard, 1, badActivePhase, bytes));
    auto badWeight = snapshot(1, 1);
    badWeight.motions[0].weight = 10.5f;
    assert(!fullGuard.AddBaseline(S, 1, 1, 1, badWeight));
    assert(!EncodeFullFallback(fullGuard, 1, badWeight, bytes));

    Control validFull = snapshot(1, 1);
    assert(EncodeFullFallback(fullGuard, 1, validFull, full));
    const auto fallbackOffset = full.size() - sizeof(WireFullRecord);
    const std::uint32_t nanBits = 0x7fc00000u;
    auto corruptFallback = full;
    std::memcpy(corruptFallback.data() + fallbackOffset + offsetof(WireFullRecord, damageMultiplier),
                &nanBits, sizeof(nanBits));
    assert(!DecodeFullFallback(fullGuard, corruptFallback.data(), corruptFallback.size(), decoded));
    auto corruptBaseline = baseWire;
    std::memcpy(corruptBaseline.data() + fullOffset + offsetof(WireFullRecord, damageMultiplier),
                &nanBits, sizeof(nanBits));
    assert(!DecodeBaseline(fullGuard, corruptBaseline.data(), corruptBaseline.size(), ref));
    for (unsigned token = 18; token <= 33; ++token) {
        sharedBase.baseline = static_cast<std::uint16_t>(token);
        sharedBase.anchor = token;
        sharedBase.position[0] = 10000.0 + token;
        assert(shared.AddBaseline(S, 1, sharedBase.baseline, sharedBase.anchor, sharedBase));
    }
    assert(shared.RetainedBaselines(1) == MaxBaselinesPerNpc && shared.RetainedAnchors(1) == MaxBaselinesPerNpc);
    assert(!shared.GetAnchor(1, 1, saved));
    auto resurrect = snapshot(1, 0); resurrect.baseline = 34; resurrect.anchor = 1;
    assert(!shared.AddBaseline(S, 1, 34, 1, resurrect));

    // 25,600 keyframes across 100 NPCs; both ends remain bounded per NPC.
    Context stressSender, stressReceiver;
    constexpr Stream stressStream{300, 1};
    assert(stressSender.Reset(300, 1) && stressReceiver.Reset(300, 1));
    for (unsigned id = 1; id <= 100; ++id) {
        assert(stressSender.AddId(stressStream, static_cast<std::uint16_t>(id), guid(id)));
        assert(stressSender.AdvanceLife(stressStream, static_cast<std::uint16_t>(id), 7));
        assert(stressReceiver.AddId(stressStream, static_cast<std::uint16_t>(id), guid(id)));
    }
    std::vector<std::uint8_t> evictedWire, evictedControl;
    Control last;
    for (unsigned step = 1; step <= 256; ++step) {
        for (unsigned id = 1; id <= 100; ++id) {
            auto x = snapshot(id, 1);
            x.baseline = static_cast<std::uint16_t>(step); x.anchor = step;
            x.position[0] = 10000.0 + step;
            x.motions[0].descriptor.instance = 0xff000000u + step;
            x.sent = step;
            assert(stressSender.AddBaseline(stressStream, static_cast<std::uint16_t>(id), x.baseline, x.anchor, x));
            assert(EncodeBaseline(stressSender, step, static_cast<std::uint16_t>(id), x.baseline, bytes));
            assert(DecodeBaseline(stressReceiver, bytes.data(), bytes.size(), ref));
            if (id == 1 && step == 1) {
                evictedWire = bytes;
                x.rotation = Yaw{0};
                assert(EncodeRecord(stressSender, x, evictedControl));
            }
            if (id == 100 && step == 256) last = x;
            assert(stressSender.RetainedBaselines(static_cast<std::uint16_t>(id)) <= MaxBaselinesPerNpc);
            assert(stressReceiver.RetainedBaselines(static_cast<std::uint16_t>(id)) <= MaxBaselinesPerNpc);
            assert(stressSender.RetainedAnchors(static_cast<std::uint16_t>(id)) <= MaxBaselinesPerNpc);
            assert(stressReceiver.RetainedAnchors(static_cast<std::uint16_t>(id)) <= MaxBaselinesPerNpc);
        }
    }
    assert(stressReceiver.RetainedBaselines(1) == MaxBaselinesPerNpc);
    assert(stressReceiver.RetainedAnchors(1) == MaxBaselinesPerNpc);
    assert(!DecodeBaseline(stressReceiver, evictedWire.data(), evictedWire.size(), ref));
    collision = evictedWire; collision.back() ^= 1;
    assert(!DecodeBaseline(stressReceiver, collision.data(), collision.size(), ref));
    assert(!DecodeRecord(stressReceiver, evictedControl.data(), evictedControl.size(), decoded));
    assert(!stressReceiver.GetAnchor(1, 1, saved));
    collision = evictedWire;
    collision[fullOffset + offsetof(WireFullRecord, incarnation)] = 8;
    assert(!DecodeBaseline(stressReceiver, collision.data(), collision.size(), ref));
    Signal recovery;
    recovery.kind = SignalKind::Resync; recovery.reason = ResyncReason::UnknownBaseline;
    recovery.reference = {1, 1, 7, 1};
    assert(EncodeSignal(stressReceiver, 257, recovery, bytes));
    assert(DecodeSignal(stressSender, bytes.data(), bytes.size(), parsed));
    auto recoveryBase = snapshot(1, 1);
    recoveryBase.baseline = 257; recoveryBase.anchor = 257;
    recoveryBase.position[0] = 10257.0;
    assert(stressSender.AddBaseline(stressStream, 1, 257, 257, recoveryBase));
    assert(EncodeBaseline(stressSender, 257, 1, 257, bytes));
    assert(DecodeBaseline(stressReceiver, bytes.data(), bytes.size(), ref));
    recoveryBase.rotation = Yaw{0};
    assert(EncodeRecord(stressSender, recoveryBase, bytes));
    assert(DecodeRecord(stressReceiver, bytes.data(), bytes.size(), decoded) && decoded.guid == guid(1));
    last.rotation = Yaw{0};
    assert(EncodeRecord(stressSender, last, bytes));
    assert(DecodeRecord(stressReceiver, bytes.data(), bytes.size(), decoded));
    assert(decoded.guid == guid(100));
    assert(EncodeBaseline(stressSender, 999, 100, 256, bytes));
    assert(DecodeBaseline(stressReceiver, bytes.data(), bytes.size(), ref));
    assert(DecodeBaseline(stressReceiver, bytes.data(), bytes.size(), ref));
    collision = bytes; collision.back() ^= 1;
    assert(!DecodeBaseline(stressReceiver, collision.data(), collision.size(), ref));
    const auto beforeCount = stressReceiver.RetainedBaselines(100);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        assert(!DecodeBaseline(stressReceiver, bytes.data(), n, ref));
    assert(stressReceiver.RetainedBaselines(100) == beforeCount);
    assert(stressReceiver.Reset(300, 2));
    assert(stressReceiver.RetainedBaselines(100) == 0 && stressReceiver.RetainedAnchors(100) == 0);
    assert(!DecodeBaseline(stressReceiver, evictedWire.data(), evictedWire.size(), ref));
    auto resetBase = snapshot(1, 0); resetBase.baseline = 1; resetBase.anchor = 1;
    assert(stressReceiver.AddId({300, 2}, 1, resetBase.guid));
    assert(stressReceiver.AdvanceLife({300, 2}, 1, 7));
    assert(stressReceiver.AddBaseline({300, 2}, 1, 1, 1, resetBase));

    // Sparse baseline tokens across distant 64-token pages remain fenced after
    // window eviction and clear cleanly when life advances.
    Context sparseFence;
    constexpr Stream sparseStream{400, 1};
    assert(sparseFence.Reset(sparseStream.epoch, sparseStream.generation));
    assert(sparseFence.AddId(sparseStream, 1, guid(1)));
    assert(sparseFence.AdvanceLife(sparseStream, 1, 7));
    const std::uint16_t sparseTokens[] = {0x0001u, 0x1040u, 0x7fffu, 0xfffeu};
    std::uint16_t anchorSeq = 1;
    for (std::uint16_t tok : sparseTokens) {
        auto s = snapshot(1, 0); s.baseline = tok; s.anchor = anchorSeq++;
        assert(sparseFence.AddBaseline(sparseStream, 1, tok, s.anchor, s));
    }
    for (std::uint16_t extra = 0x2000u; extra < 0x2000u + MaxBaselinesPerNpc; ++extra) {
        auto s = snapshot(1, 0); s.baseline = extra; s.anchor = anchorSeq++;
        assert(sparseFence.AddBaseline(sparseStream, 1, extra, s.anchor, s));
    }
    assert(sparseFence.RetainedBaselines(1) == MaxBaselinesPerNpc);
    for (std::uint16_t tok : sparseTokens) {
        auto s = snapshot(1, 0); s.baseline = tok; s.anchor = anchorSeq++;
        assert(!sparseFence.AddBaseline(sparseStream, 1, tok, s.anchor, s));
    }
    assert(sparseFence.AdvanceLife(sparseStream, 1, 8));
    for (std::uint16_t tok : sparseTokens) {
        auto s = snapshot(1, 0); s.incarnation = 8; s.baseline = tok; s.anchor = anchorSeq++;
        assert(!sparseFence.AddBaseline(sparseStream, 1, tok, s.anchor, s));
    }
    constexpr Stream nextSparseStream{400, 2};
    assert(sparseFence.Reset(nextSparseStream.epoch, nextSparseStream.generation));
    assert(sparseFence.AddId(nextSparseStream, 1, guid(1)));
    assert(sparseFence.AdvanceLife(nextSparseStream, 1, 8));
    anchorSeq = 1;
    for (std::uint16_t tok : sparseTokens) {
        auto s = snapshot(1, 0); s.incarnation = 8; s.baseline = tok; s.anchor = anchorSeq++;
        assert(sparseFence.AddBaseline(nextSparseStream, 1, tok, s.anchor, s));
    }
}
