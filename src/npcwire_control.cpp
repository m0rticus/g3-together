// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcwire_control.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace g3t::npcwire {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE-754 binary32 required");
constexpr double Turn = 6.283185307179586476925286766559;

struct Writer {
    std::vector<std::uint8_t> data;
    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
    void u32(std::uint32_t v) { u16(static_cast<std::uint16_t>(v)); u16(static_cast<std::uint16_t>(v >> 16)); }
    void f32(float v) { std::uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
};
struct Reader {
    const std::uint8_t* data;
    std::size_t size, offset = 0;
    bool ok = true;
    std::uint8_t u8() {
        if (offset >= size) { ok = false; return 0; }
        return data[offset++];
    }
    std::uint16_t u16() {
        const auto a = u8(); const auto b = u8();
        return static_cast<std::uint16_t>(a | (static_cast<std::uint16_t>(b) << 8));
    }
    std::uint32_t u32() {
        const auto a = u16(); const auto b = u16();
        return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 16);
    }
    float f32() { const auto u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
};
void writeBatchHeader(Writer& w, char kind, std::uint32_t epoch, std::uint32_t generation,
                      std::uint32_t sequence, std::uint16_t count) {
    w.u8('N'); w.u8(static_cast<std::uint8_t>(kind)); w.u8(1); w.u8(0);
    w.u32(epoch); w.u32(generation); w.u32(sequence); w.u16(count);
}
bool readBatchHeader(Reader& r, char expectedKind, Stream& stream, std::uint32_t& sequence, std::uint16_t& count) {
    const auto magic1 = r.u8(), magic2 = r.u8(), version = r.u8(), reserved = r.u8();
    stream = Stream{r.u32(), r.u32()};
    sequence = r.u32();
    count = r.u16();
    return r.ok && magic1 == 'N' && (expectedKind == 0 || magic2 == static_cast<std::uint8_t>(expectedKind)) &&
           version == 1 && !reserved;
}
bool appendUniqueId(std::vector<std::uint16_t>& seen, std::uint16_t id) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) return false;
    seen.push_back(id);
    return true;
}
std::int32_t signed32(std::uint32_t u) {
    return u <= 0x7fffffffu ? static_cast<std::int32_t>(u)
                            : -1 - static_cast<std::int32_t>(0xffffffffu - u);
}
std::int32_t signed16(std::uint16_t u) {
    return u <= 0x7fffu ? static_cast<std::int32_t>(u) : static_cast<std::int32_t>(u) - 65536;
}
bool sameFloat(float a, float b) {
    std::uint32_t x, y;
    std::memcpy(&x, &a, 4); std::memcpy(&y, &b, 4);
    return x == y;
}
template<class T> bool sameMetadataValue(T a, T b) { return a == b; }
bool sameMetadataValue(float a, float b) { return sameFloat(a, b); }

#define NPCWIRE_METADATA_FIELDS(X) \
    X(0, movementMode, 1) X(1, hitPoints, 4) X(2, hitPointsMax, 4) \
    X(3, motion, 4) X(4, aniState, 1) X(5, heldRight, 4) X(6, heldLeft, 4) \
    X(7, partyPlayer, 1) X(8, partyType, 1) X(9, waiting, 1) X(10, guidePlayer, 1) \
    X(11, aiMode, 1) X(12, action, 1) X(13, attackReason, 1) X(14, damageType, 1) \
    X(15, damageAmount, 4) X(16, damageMultiplier, 4) X(17, hitPlayer, 1) \
    X(18, hitSequence, 2) X(19, attitude, 1) X(20, combatTarget, 4)

#define METADATA_WIDTH(bit, field, width) width,
constexpr std::uint8_t MetadataWidths[] = { NPCWIRE_METADATA_FIELDS(METADATA_WIDTH) };
#undef METADATA_WIDTH
#define METADATA_ADD_WIDTH(bit, field, width) + width
static_assert(MaxMetadataBytes == 3 NPCWIRE_METADATA_FIELDS(METADATA_ADD_WIDTH), "metadata maximum");
#undef METADATA_ADD_WIDTH

std::uint32_t metadataMask(const Control& v, const Control& base) {
    std::uint32_t mask = 0;
#define METADATA_COMPARE(bit, field, width) \
    if (!sameMetadataValue(v.field, base.field)) mask |= std::uint32_t{1} << bit;
    NPCWIRE_METADATA_FIELDS(METADATA_COMPARE)
#undef METADATA_COMPARE
    return mask;
}
void writeMetadataValue(Writer& w, std::uint8_t v) { w.u8(v); }
void writeMetadataValue(Writer& w, std::uint16_t v) { w.u16(v); }
void writeMetadataValue(Writer& w, std::uint32_t v) { w.u32(v); }
void writeMetadataValue(Writer& w, std::int32_t v) { w.u32(static_cast<std::uint32_t>(v)); }
void writeMetadataValue(Writer& w, float v) { w.f32(v); }
bool readMetadataValue(Reader& r, std::uint8_t& v) { v = r.u8(); return r.ok; }
bool readMetadataValue(Reader& r, std::uint16_t& v) { v = r.u16(); return r.ok; }
bool readMetadataValue(Reader& r, std::uint32_t& v) { v = r.u32(); return r.ok; }
bool readMetadataValue(Reader& r, std::int32_t& v) { v = signed32(r.u32()); return r.ok; }
bool readMetadataValue(Reader& r, float& v) { v = r.f32(); return r.ok && std::isfinite(v); }
void writeMetadata(Writer& w, const Control& v, std::uint32_t mask) {
    w.u8(static_cast<std::uint8_t>(mask));
    w.u8(static_cast<std::uint8_t>(mask >> 8));
    w.u8(static_cast<std::uint8_t>(mask >> 16));
#define METADATA_WRITE(bit, field, width) \
    if (mask & (std::uint32_t{1} << bit)) writeMetadataValue(w, v.field);
    NPCWIRE_METADATA_FIELDS(METADATA_WRITE)
#undef METADATA_WRITE
}
bool readMetadata(Reader& r, Control& v, const Control& base) {
    const auto a = r.u8(), b = r.u8(), c = r.u8();
    const std::uint32_t mask = a | (static_cast<std::uint32_t>(b) << 8) |
                               (static_cast<std::uint32_t>(c) << 16);
#define METADATA_READ(bit, field, width) \
    if (mask & (std::uint32_t{1} << bit)) { \
        if (!readMetadataValue(r, v.field) || sameMetadataValue(v.field, base.field)) return false; \
    }
    NPCWIRE_METADATA_FIELDS(METADATA_READ)
#undef METADATA_READ
    return r.ok;
}
#undef NPCWIRE_METADATA_FIELDS

bool sameDescriptor(const MotionDescriptor& a, const MotionDescriptor& b) {
    return a.name == b.name && a.instance == b.instance && sameFloat(a.speed, b.speed) &&
           sameFloat(a.intro, b.intro) && sameFloat(a.extro, b.extro) && a.blend == b.blend &&
           a.options == b.options;
}

#define NPCWIRE_MOTION_FIELDS(X) \
    X(0, descriptor.name, 4) X(1, descriptor.instance, 4) \
    X(2, descriptor.speed, 4) X(3, descriptor.intro, 4) X(4, descriptor.extro, 4) \
    X(5, descriptor.blend, 4) \
    X(6, descriptor.options[0], 4) X(7, descriptor.options[1], 4) \
    X(8, descriptor.options[2], 4) X(9, descriptor.options[3], 4) \
    X(10, descriptor.options[4], 4) X(11, descriptor.options[5], 4) \
    X(12, descriptor.options[6], 4) X(13, active, 1)

#define MOTION_WIDTH(bit, field, width) width,
constexpr std::uint8_t MotionFieldWidths[] = { NPCWIRE_MOTION_FIELDS(MOTION_WIDTH) 4, 4 };
#undef MOTION_WIDTH

std::uint16_t motionFieldMask(const Motion& current, const Motion& baseline) {
    std::uint16_t mask = 0;
#define MOTION_COMPARE(bit, field, width) \
    if (!sameMetadataValue(current.field, baseline.field)) mask |= std::uint16_t{1} << bit;
    NPCWIRE_MOTION_FIELDS(MOTION_COMPARE)
#undef MOTION_COMPARE
    if (!(current.active & 1u)) {
        if (!sameFloat(current.phase, baseline.phase)) mask |= std::uint16_t{1} << 14;
        if (!sameFloat(current.weight, baseline.weight)) mask |= std::uint16_t{1} << 15;
    }
    return mask;
}
std::uint8_t motionOverrideMask(const Control& current, const Control& baseline) {
    std::uint8_t result = 0;
    for (unsigned i = 0; i < MotionSlots; ++i)
        if (motionFieldMask(current.motions[i], baseline.motions[i]))
            result |= static_cast<std::uint8_t>(1u << i);
    return result;
}
bool validFullMotion(const Motion& m, std::uint8_t slots, unsigned slot) {
    if (!std::isfinite(m.phase) || !std::isfinite(m.weight) ||
        !std::isfinite(m.descriptor.speed) || !std::isfinite(m.descriptor.intro) ||
        !std::isfinite(m.descriptor.extro) ||
        m.weight < 0.0f || m.weight > 10.0f)
        return false;
    if ((m.active & 1u) != ((slots >> slot) & 1u)) return false;
    if (m.active & 1u) {
        if (m.phase < 0.0f || m.phase > 1.0f) return false;
    }
    return true;
}
void writeMotionFields(Writer& w, const Motion& m, std::uint16_t mask) {
#define MOTION_WRITE(bit, field, width) \
    if (mask & (std::uint16_t{1} << bit)) writeMetadataValue(w, m.field);
    NPCWIRE_MOTION_FIELDS(MOTION_WRITE)
#undef MOTION_WRITE
    if (mask & (std::uint16_t{1} << 14)) w.f32(m.phase);
    if (mask & (std::uint16_t{1} << 15)) w.f32(m.weight);
}
bool readMotionFields(Reader& r, Motion& m, std::uint16_t mask) {
#define MOTION_READ(bit, field, width) \
    if ((mask & (std::uint16_t{1} << bit)) && !readMetadataValue(r, m.field)) return false;
    NPCWIRE_MOTION_FIELDS(MOTION_READ)
#undef MOTION_READ
    if ((mask & (std::uint16_t{1} << 14)) && !readMetadataValue(r, m.phase)) return false;
    if ((mask & (std::uint16_t{1} << 15)) &&
        (!readMetadataValue(r, m.weight) || m.weight < 0.0f || m.weight > 10.0f)) return false;
    return r.ok;
}
#undef NPCWIRE_MOTION_FIELDS
std::uint8_t activeMask(const Control& v) {
    std::uint8_t result = 0;
    for (unsigned i = 0; i < MotionSlots; ++i)
        if (v.motions[i].active & 1u) result |= static_cast<std::uint8_t>(1u << i);
    return result;
}
std::size_t slotCount(std::uint8_t mask) {
    std::size_t count = 0;
    for (unsigned i = 0; i < MotionSlots; ++i) count += (mask >> i) & 1u;
    return count;
}
bool validAnchor(const Anchor& anchor) {
    for (float v : anchor.position) if (!std::isfinite(v)) return false;
    for (float v : anchor.rotation) if (!std::isfinite(v)) return false;
    return true;
}
bool writeFull(Writer& w, const Control& v) {
    if (!std::holds_alternative<Quaternion>(v.rotation) || !std::isfinite(v.damageMultiplier)) return false;
    std::array<float, 3> position{};
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(v.position[i]) ||
            std::abs(v.position[i]) > static_cast<double>(std::numeric_limits<float>::max())) return false;
        position[i] = static_cast<float>(v.position[i]);
        if (static_cast<double>(position[i]) != v.position[i]) return false;
    }
    const auto& quaternion = std::get<Quaternion>(v.rotation).components;
    for (float x : quaternion) if (!std::isfinite(x)) return false;
    const auto slots = activeMask(v);
    for (unsigned i = 0; i < MotionSlots; ++i)
        if (!validFullMotion(v.motions[i], slots, i)) return false;
    for (auto x : v.guid) w.u8(x);
    for (auto x : position) w.f32(x);
    for (auto x : quaternion) w.f32(x);
    w.u8(v.movementMode); w.u8(v.flags);
    w.u32(static_cast<std::uint32_t>(v.hitPoints)); w.u32(static_cast<std::uint32_t>(v.hitPointsMax));
    w.u32(v.motion); w.u8(v.aniState); w.u32(v.heldRight); w.u32(v.heldLeft); w.u32(v.sent);
    w.u8(v.partyPlayer); w.u8(v.partyType); w.u8(v.waiting); w.u8(v.guidePlayer);
    w.u8(v.aiMode); w.u8(v.action); w.u8(v.attackReason); w.u8(v.damageType);
    w.u32(static_cast<std::uint32_t>(v.damageAmount)); w.f32(v.damageMultiplier); w.u32(v.incarnation);
    w.u8(v.hitPlayer); w.u16(v.hitSequence); w.u8(v.contact); w.u8(v.attitude); w.u32(v.combatTarget);
    for (const auto& m : v.motions) {
        w.u32(m.descriptor.name); w.u32(m.descriptor.instance); w.f32(m.phase); w.f32(m.descriptor.speed);
        w.f32(m.weight); w.f32(m.descriptor.intro); w.f32(m.descriptor.extro);
        w.u32(static_cast<std::uint32_t>(m.descriptor.blend));
        for (auto x : m.descriptor.options) w.u32(x);
        w.u8(m.active);
    }
    return true;
}
bool readFull(Reader& r, Control& output) {
    Control v;
    for (auto& x : v.guid) x = r.u8();
    for (auto& x : v.position) { x = r.f32(); if (!std::isfinite(x)) return false; }
    Quaternion q;
    for (auto& x : q.components) { x = r.f32(); if (!std::isfinite(x)) return false; }
    v.rotation = q;
    v.movementMode = r.u8(); v.flags = r.u8();
    v.hitPoints = signed32(r.u32()); v.hitPointsMax = signed32(r.u32());
    v.motion = r.u32(); v.aniState = r.u8(); v.heldRight = r.u32(); v.heldLeft = r.u32(); v.sent = r.u32();
    v.partyPlayer = r.u8(); v.partyType = r.u8(); v.waiting = r.u8(); v.guidePlayer = r.u8();
    v.aiMode = r.u8(); v.action = r.u8(); v.attackReason = r.u8(); v.damageType = r.u8();
    v.damageAmount = signed32(r.u32()); v.damageMultiplier = r.f32(); v.incarnation = r.u32();
    v.hitPlayer = r.u8(); v.hitSequence = r.u16(); v.contact = r.u8(); v.attitude = r.u8();
    v.combatTarget = r.u32();
    for (auto& m : v.motions) {
        m.descriptor.name = r.u32(); m.descriptor.instance = r.u32(); m.phase = r.f32();
        m.descriptor.speed = r.f32(); m.weight = r.f32(); m.descriptor.intro = r.f32();
        m.descriptor.extro = r.f32(); m.descriptor.blend = signed32(r.u32());
        for (auto& x : m.descriptor.options) x = r.u32();
        m.active = r.u8();
    }
    if (!r.ok || !std::isfinite(v.damageMultiplier)) return false;
    v.slots = activeMask(v);
    for (unsigned i = 0; i < MotionSlots; ++i)
        if (!validFullMotion(v.motions[i], v.slots, i)) return false;
    output = v;
    return true;
}
} // namespace

bool Newer(std::uint32_t value, std::uint32_t previous) {
    const std::uint32_t difference = value - previous;
    return difference != 0 && difference < 0x80000000u;
}
std::uint8_t ActiveSlots(const Control& value) { return activeMask(value); }
bool EncodeYaw(double radians, std::uint16_t& result) {
    if (!std::isfinite(radians)) return false;
    double wrapped = std::fmod(radians, Turn);
    if (wrapped < 0) wrapped += Turn;
    const auto rounded = static_cast<std::uint32_t>(std::floor(wrapped * (65536.0 / Turn) + 0.5));
    result = static_cast<std::uint16_t>(rounded & 0xffffu);
    return true;
}
double DecodeYaw(std::uint16_t value) { return value * (Turn / 65536.0); }

bool RecordBytes(const std::uint8_t* bytes, std::size_t available, std::size_t& length) {
    if (!bytes || available < sizeof(WireControl)) return false;
    const auto orientation = bytes[offsetof(WireControl, orientation)];
    if (orientation & 0x1eu) return false;
    const bool full = (orientation & 1u) != 0;
    const bool exact = (orientation & 0x40u) != 0;
    const bool motionOverrides = (orientation & 0x20u) != 0;
    if (exact && !full) return false;
    if (exact)
        for (std::size_t i = 0; i < 6; ++i)
            if (bytes[offsetof(WireControl, delta) + i]) return false;
    if (full && (bytes[offsetof(WireControl, yaw)] || bytes[offsetof(WireControl, yaw) + 1])) return false;
    const auto slots = bytes[offsetof(WireControl, slots)];
    const auto ticks = sizeof(WireControl) + (full ? sizeof(WireQuaternion) : 0) + (exact ? 12 : 0);
    std::size_t result = ticks + slotCount(slots) * (exact ? 8 : sizeof(WireMotionTick));
    if (available < result) return false;
    if (!exact)
        for (std::size_t offset = ticks; offset < result; offset += sizeof(WireMotionTick))
            if (bytes[offset + 2] & 0xf0u) return false;
    if (orientation & 0x80u) {
        if (available - result < 3) return false;
        const auto mask = static_cast<std::uint32_t>(bytes[result]) |
                          (static_cast<std::uint32_t>(bytes[result + 1]) << 8) |
                          (static_cast<std::uint32_t>(bytes[result + 2]) << 16);
        if (!mask || (mask & 0xe00000u)) return false;
        result += 3;
        for (std::size_t bit = 0; bit < sizeof(MetadataWidths); ++bit)
            if (mask & (std::uint32_t{1} << bit)) result += MetadataWidths[bit];
        if (available < result) return false;
    }
    if (motionOverrides) {
        if (available <= result) return false;
        const std::uint8_t overrideMask = bytes[result++];
        if (!overrideMask) return false;
        for (unsigned slot = 0; slot < MotionSlots; ++slot) {
            if (!(overrideMask & (1u << slot))) continue;
            if (available - result < 2) return false;
            const std::uint16_t fieldMask =
                static_cast<std::uint16_t>(bytes[result]) |
                (static_cast<std::uint16_t>(bytes[result + 1]) << 8);
            if (!fieldMask) return false;
            result += 2;
            for (unsigned bit = 0; bit < 16; ++bit)
                if (fieldMask & (std::uint16_t{1} << bit))
                    result += MotionFieldWidths[bit];
            if (available < result) return false;
        }
    }
    length = result;
    return true;
}

bool Context::Reset(std::uint32_t epoch, std::uint32_t generation) {
    if (!epoch || !generation) return false;
    if (ready_ && !(epoch == epoch_ ? Newer(generation, generation_) : Newer(epoch, epoch_))) return false;
    epoch_ = epoch; generation_ = generation; ready_ = true;
    ids_.clear(); guids_.clear(); lives_.clear(); anchors_.clear();
    baselines_.clear(); history_.clear(); sequences_.clear();
    return true;
}
bool Context::Matches(Stream stream) const {
    return ready_ && stream.epoch == epoch_ && stream.generation == generation_;
}
bool Context::AddId(Stream stream, std::uint16_t id, const Guid& guid) {
    if (!Matches(stream) || !id || guid == Guid{}) return false;
    const auto existing = ids_.find(id);
    if (existing != ids_.end()) return existing->second == guid;
    if (guids_.count(guid)) return false;
    ids_.emplace(id, guid); guids_.emplace(guid, id);
    return true;
}
bool Context::FindId(const Guid& guid, std::uint16_t& id) const {
    const auto it = guids_.find(guid);
    if (it == guids_.end()) return false;
    id = it->second; return true;
}
bool Context::FindGuid(std::uint16_t id, Guid& guid) const {
    const auto it = ids_.find(id);
    if (it == ids_.end()) return false;
    guid = it->second; return true;
}
bool Context::AdvanceLife(Stream stream, std::uint16_t id, std::uint32_t incarnation) {
    if (!Matches(stream) || !ids_.count(id) || !incarnation) return false;
    auto it = lives_.find(id);
    if (it != lives_.end() && incarnation != it->second && !Newer(incarnation, it->second)) return false;
    const bool changed = it != lives_.end() && incarnation != it->second;
    lives_[id] = incarnation;
    if (changed) RetireOtherLives(id, incarnation);
    return true;
}
void Context::RetireOtherLives(std::uint16_t id, std::uint32_t incarnation) {
    auto baseline = baselines_.lower_bound({id, std::uint16_t{0}});
    while (baseline != baselines_.end() && baseline->first.first == id) {
        if (baseline->second.life != incarnation) baseline = baselines_.erase(baseline);
        else ++baseline;
    }
    auto anchor = anchors_.lower_bound({id, std::uint32_t{0}});
    while (anchor != anchors_.end() && anchor->first.first == id) {
        if (anchor->second.life != incarnation) anchor = anchors_.erase(anchor);
        else ++anchor;
    }
    const auto history = history_.find(id);
    if (history != history_.end()) {
        auto& retained = history->second.retained;
        for (auto it = retained.begin(); it != retained.end();) {
            if (!baselines_.count({id, *it})) it = retained.erase(it);
            else ++it;
        }
    }
}
bool Context::GetAnchor(std::uint16_t id, std::uint32_t token, Anchor& anchor) const {
    const auto life = lives_.find(id);
    const auto it = anchors_.find({id, token});
    if (life == lives_.end() || it == anchors_.end() || it->second.life != life->second) return false;
    anchor = it->second.value; return true;
}
std::size_t Context::RetainedBaselines(std::uint16_t id) const {
    const auto it = history_.find(id);
    return it == history_.end() ? 0 : it->second.retained.size();
}
std::size_t Context::RetainedAnchors(std::uint16_t id) const {
    std::size_t count = 0;
    for (auto it = anchors_.lower_bound({id, 0}); it != anchors_.end() && it->first.first == id; ++it) ++count;
    return count;
}
bool Context::AddBaseline(Stream stream, std::uint16_t id, std::uint16_t token,
                          std::uint32_t anchorToken, const Control& snapshot) {
    return InstallBaseline(stream, id, token, anchorToken, snapshot, false, nullptr);
}
bool Context::InstallBaseline(Stream stream, std::uint16_t id, std::uint16_t token,
                              std::uint32_t anchorToken, const Control& snapshot, bool advanceLife,
                              const std::uint8_t* originalBytes) {
    const auto life = lives_.find(id);
    const bool lifeChanged = life != lives_.end() && life->second != snapshot.incarnation;
    std::uint16_t mapped = 0;
    if (!Matches(stream) || !snapshot.incarnation || !token || !anchorToken ||
        !FindId(snapshot.guid, mapped) || mapped != id) return false;
    if (life == lives_.end()) {
        if (!advanceLife) return false;
    } else if (life->second != snapshot.incarnation &&
               (!advanceLife || !Newer(snapshot.incarnation, life->second))) return false;
    std::vector<std::uint8_t> full;
    if (originalBytes) {
        full.assign(originalBytes, originalBytes + sizeof(WireFullRecord));
    } else {
        Writer w;
        if (!writeFull(w, snapshot) || w.data.size() != sizeof(WireFullRecord)) return false;
        full = std::move(w.data);
    }
    const auto old = baselines_.find({id, token});
    if (old != baselines_.end())
        return old->second.life == snapshot.incarnation && old->second.anchor == anchorToken && old->second.bytes == full;
    const auto history = history_.find(id);
    History next = history == history_.end() ? History{} : history->second;
    if (next.usedBaselines.test(token)) return false;
    Anchor anchor;
    for (unsigned i = 0; i < 3; ++i) anchor.position[i] = static_cast<float>(snapshot.position[i]);
    anchor.rotation = std::get<Quaternion>(snapshot.rotation).components;
    if (!validAnchor(anchor)) return false;
    const auto previous = anchors_.find({id, anchorToken});
    if (previous != anchors_.end()) {
        if (previous->second.life != snapshot.incarnation) return false;
        for (unsigned i = 0; i < 3; ++i) if (!sameFloat(previous->second.value.position[i], anchor.position[i])) return false;
        for (unsigned i = 0; i < 4; ++i) if (!sameFloat(previous->second.value.rotation[i], anchor.rotation[i])) return false;
    } else if (!next.highestAnchor) {
        next.highestAnchor = anchorToken;
        next.usedAnchors = 1;
    } else if (anchorToken > next.highestAnchor) {
        const auto distance = anchorToken - next.highestAnchor;
        next.usedAnchors = distance >= AnchorReorderWindow ? 1 : (next.usedAnchors << distance) | 1;
        next.highestAnchor = anchorToken;
    } else {
        const auto distance = next.highestAnchor - anchorToken;
        if (distance >= AnchorReorderWindow || (next.usedAnchors & (std::uint64_t{1} << distance))) return false;
        next.usedAnchors |= std::uint64_t{1} << distance;
    }
    Control stored = snapshot;
    stored.anchor = anchorToken; stored.baseline = token; stored.slots = activeMask(stored);
    next.usedBaselines.set(token);
    next.retained.push_back(token);
    std::uint16_t evict = 0;
    if (next.retained.size() > MaxBaselinesPerNpc) {
        evict = next.retained.front();
        next.retained.pop_front();
    }
    bool insertedAnchor = false, insertedBaseline = false, insertedHistory = false, insertedLife = false;
    try {
        if (previous == anchors_.end()) {
            anchors_.emplace(std::make_pair(id, anchorToken), LifeAnchor{snapshot.incarnation, anchor});
            insertedAnchor = true;
        }
        baselines_.emplace(std::make_pair(id, token), LifeBaseline{snapshot.incarnation, anchorToken, stored, std::move(full)});
        insertedBaseline = true;
        if (history == history_.end()) {
            history_.emplace(id, History{});
            insertedHistory = true;
        }
        if (life == lives_.end()) {
            lives_.emplace(id, snapshot.incarnation);
            insertedLife = true;
        }
    } catch (...) {
        if (insertedLife) lives_.erase(id);
        if (insertedHistory) history_.erase(id);
        if (insertedBaseline) baselines_.erase({id, token});
        if (insertedAnchor) anchors_.erase({id, anchorToken});
        throw;
    }
    auto& committed = history_.find(id)->second;
    committed.usedBaselines.swap(next.usedBaselines);
    committed.retained.swap(next.retained);
    committed.highestAnchor = next.highestAnchor;
    committed.usedAnchors = next.usedAnchors;
    lives_.find(id)->second = snapshot.incarnation;
    if (evict) {
        const auto it = baselines_.find({id, evict});
        const auto evictedAnchor = it->second.anchor;
        baselines_.erase(it);
        bool referenced = false;
        for (auto retained : committed.retained)
            if (baselines_.find({id, retained})->second.anchor == evictedAnchor) { referenced = true; break; }
        if (!referenced) anchors_.erase({id, evictedAnchor});
    }
    if (lifeChanged) RetireOtherLives(id, snapshot.incarnation);
    return true;
}

bool EncodeRecord(const Context& c, const Control& v, std::vector<std::uint8_t>& output, bool exact) {
    std::uint16_t id;
    if (!c.FindId(v.guid, id)) return false;
    const auto life = c.lives_.find(id);
    const auto anchor = c.anchors_.find({id, v.anchor});
    const auto baseline = c.baselines_.find({id, v.baseline});
    if (life == c.lives_.end() || life->second != v.incarnation || anchor == c.anchors_.end() ||
        anchor->second.life != v.incarnation || baseline == c.baselines_.end() ||
        baseline->second.life != v.incarnation || baseline->second.anchor != v.anchor ||
        baseline->second.value.guid != v.guid ||
        baseline->second.value.incarnation != v.incarnation) return false;
    const auto metadata = metadataMask(v, baseline->second.value);
    if ((metadata & (std::uint32_t{1} << 16)) && !std::isfinite(v.damageMultiplier)) return false;
    const std::uint8_t overrides = motionOverrideMask(v, baseline->second.value);
    const std::uint8_t slots = activeMask(v);
    std::uint16_t deltas[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        if (exact) {
            if (!std::isfinite(v.position[i]) ||
                std::abs(v.position[i]) > static_cast<double>(std::numeric_limits<float>::max()) ||
                static_cast<double>(static_cast<float>(v.position[i])) != v.position[i]) return false;
            continue;
        }
        const double delta = v.position[i] - static_cast<double>(anchor->second.value.position[i]);
        if (!std::isfinite(v.position[i]) || !std::isfinite(delta) || delta < -32768.0 || delta > 32767.0)
            return false;
        const auto rounded = static_cast<std::int32_t>(std::round(delta));
        deltas[i] = static_cast<std::uint16_t>(rounded);
    }
    std::uint32_t ticks[MotionSlots]{};
    for (std::size_t i = 0; i < MotionSlots; ++i) {
        if (!(slots & (1u << i))) continue;
        const auto phase = v.motions[i].phase, weight = v.motions[i].weight;
        if (!std::isfinite(phase) || !std::isfinite(weight) || phase < 0 || phase > 1 ||
            weight < 0 || weight > (exact ? 10.0f : 1.0f))
            return false;
        const auto p = static_cast<std::uint32_t>(std::floor(static_cast<double>(phase) * 4095 + 0.5));
        const auto w = static_cast<std::uint32_t>(std::floor(static_cast<double>(weight) * 255 + 0.5));
        ticks[i] = p | (w << 12);
    }
    for (std::size_t i = 0; i < MotionSlots; ++i) {
        if (overrides & (1u << i)) {
            if (!validFullMotion(v.motions[i], slots, i)) return false;
        }
    }
    std::uint16_t yaw = 0;
    const bool full = std::holds_alternative<Quaternion>(v.rotation);
    if (exact && !full) return false;
    if (full) {
        for (auto x : std::get<Quaternion>(v.rotation).components) if (!std::isfinite(x)) return false;
    } else if (!EncodeYaw(std::get<Yaw>(v.rotation).radians, yaw)) return false;

    Writer w;
    w.u16(id); w.u32(v.anchor); w.u16(v.baseline);
    for (auto d : deltas) w.u16(d);
    w.u16(yaw); w.u8(static_cast<std::uint8_t>((full ? 1 : 0) | (metadata ? 0x80 : 0) |
                                               (exact ? 0x40 : 0) | (overrides ? 0x20 : 0)));
    w.u32(v.sent); w.u8(v.flags); w.u8(v.contact); w.u8(slots);
    if (full) for (auto x : std::get<Quaternion>(v.rotation).components) w.f32(x);
    if (exact) for (auto x : v.position) w.f32(static_cast<float>(x));
    for (std::size_t i = 0; i < MotionSlots; ++i) if (slots & (1u << i)) {
        if (exact) {
            w.f32(v.motions[i].phase); w.f32(v.motions[i].weight);
        } else {
            w.u8(static_cast<std::uint8_t>(ticks[i]));
            w.u8(static_cast<std::uint8_t>(ticks[i] >> 8));
            w.u8(static_cast<std::uint8_t>(ticks[i] >> 16));
        }
    }
    if (metadata) writeMetadata(w, v, metadata);
    if (overrides) {
        w.u8(overrides);
        for (std::size_t i = 0; i < MotionSlots; ++i) {
            if (!(overrides & (1u << i))) continue;
            const std::uint16_t fieldMask =
                motionFieldMask(v.motions[i], baseline->second.value.motions[i]);
            w.u16(fieldMask);
            writeMotionFields(w, v.motions[i], fieldMask);
        }
    }
    if (w.data.size() > MaxBatchBytes) return false;
    output = std::move(w.data); return true;
}

bool DecodeRecord(const Context& c, const std::uint8_t* bytes, std::size_t size, Control& output) {
    std::size_t length = 0;
    if (size > MaxBatchBytes || !RecordBytes(bytes, size, length) || length != size) return false;
    Reader r{bytes, size};
    const auto id = r.u16(); const auto anchorToken = r.u32(); const auto baselineToken = r.u16();
    std::int32_t delta[3]; for (auto& d : delta) d = signed16(r.u16());
    const auto yaw = r.u16(); const auto orientation = r.u8();
    const auto sent = r.u32(); const auto flags = r.u8(), contact = r.u8(), slots = r.u8();
    if (!r.ok) return false;
    const auto life = c.lives_.find(id);
    const auto anchor = c.anchors_.find({id, anchorToken});
    const auto baseline = c.baselines_.find({id, baselineToken});
    const auto idEntry = c.ids_.find(id);
    if (life == c.lives_.end() || idEntry == c.ids_.end() ||
        anchor == c.anchors_.end() || baseline == c.baselines_.end() ||
        anchor->second.life != life->second || baseline->second.life != life->second ||
        baseline->second.anchor != anchorToken ||
        baseline->second.value.guid != idEntry->second ||
        baseline->second.value.incarnation != life->second) return false;
    Control v = baseline->second.value;
    v.sent = sent; v.flags = flags; v.contact = contact; v.slots = slots;
    for (unsigned i = 0; i < 3; ++i) v.position[i] = static_cast<double>(anchor->second.value.position[i]) + delta[i];
    if (orientation & 1u) {
        Quaternion q;
        for (auto& x : q.components) { x = r.f32(); if (!std::isfinite(x)) return false; }
        v.rotation = q;
    } else v.rotation = Yaw{DecodeYaw(yaw)};
    const bool exact = (orientation & 0x40u) != 0;
    if (exact) for (auto& x : v.position) {
        x = r.f32();
        if (!std::isfinite(x)) return false;
    }
    for (std::size_t i = 0; i < MotionSlots; ++i) {
        if (!(slots & (1u << i))) continue;
        if (exact) {
            auto& motion = v.motions[i];
            motion.phase = r.f32(); motion.weight = r.f32();
            if (!std::isfinite(motion.phase) || !std::isfinite(motion.weight) ||
                motion.phase < 0 || motion.phase > 1 || motion.weight < 0 || motion.weight > 10)
                return false;
            continue;
        }
        const auto a = r.u8(), b = r.u8(), d = r.u8();
        if (d & 0xf0u) return false;
        const std::uint32_t packed = a | (static_cast<std::uint32_t>(b) << 8) | (static_cast<std::uint32_t>(d) << 16);
        v.motions[i].phase = static_cast<float>(packed & 0xfffu) / 4095.0f;
        v.motions[i].weight = static_cast<float>((packed >> 12) & 0xffu) / 255.0f;
    }
    if ((orientation & 0x80u) && !readMetadata(r, v, baseline->second.value)) return false;
    if (orientation & 0x20u) {
        const std::uint8_t overrideMask = r.u8();
        if (!overrideMask) return false;
        for (std::size_t i = 0; i < MotionSlots; ++i) {
            if (!(overrideMask & (1u << i))) continue;
            const std::uint16_t fieldMask = r.u16();
            if (!fieldMask) return false;
            Motion& motion = v.motions[i];
            if (!readMotionFields(r, motion, fieldMask) ||
                !validFullMotion(motion, slots, i) ||
                motionFieldMask(motion, baseline->second.value.motions[i]) != fieldMask)
                return false;
        }
        for (std::size_t i = 0; i < MotionSlots; ++i) {
            if (overrideMask & (1u << i)) continue;
            const Motion& omitted = v.motions[i];
            const Motion& immutable = baseline->second.value.motions[i];
            if (!sameDescriptor(omitted.descriptor, immutable.descriptor) ||
                omitted.active != immutable.active)
                return false;
            if (motionFieldMask(omitted, immutable) != 0) return false;
        }
        if (activeMask(v) != slots) return false;
        std::uint8_t canonicalMask = 0;
        for (std::size_t i = 0; i < MotionSlots; ++i)
            if (motionFieldMask(v.motions[i], baseline->second.value.motions[i]))
                canonicalMask |= static_cast<std::uint8_t>(1u << i);
        if (canonicalMask != overrideMask) return false;
    } else if (activeMask(v) != slots) {
        return false;
    }
    if (!r.ok || r.offset != size) return false;
    output = v; return true;
}

bool EncodeBatch(const Context& c, std::uint32_t sequence, const std::vector<Control>& values,
                 std::vector<std::uint8_t>& output, bool exact) {
    if (!c.ready_ || values.empty() || values.size() > 0xffffu) return false;
    Writer w;
    writeBatchHeader(w, 'C', c.epoch_, c.generation_, sequence, static_cast<std::uint16_t>(values.size()));
    std::vector<std::uint16_t> seen;
    seen.reserve(values.size());
    std::vector<std::uint8_t> record;
    for (const auto& value : values) {
        std::uint16_t id;
        record.clear();
        if (!c.FindId(value.guid, id) || !appendUniqueId(seen, id) || !EncodeRecord(c, value, record, exact) ||
            record.size() > MaxBatchBytes - w.data.size()) return false;
        w.data.insert(w.data.end(), record.begin(), record.end());
    }
    output = std::move(w.data); return true;
}

bool DecodeBatch(Context& c, const std::uint8_t* bytes, std::size_t size, std::vector<Control>& output) {
    if (!c.ready_ || !bytes || size < sizeof(WireBatchHeader) || size > MaxBatchBytes) return false;
    Reader r{bytes, size};
    Stream stream{}; std::uint32_t sequence = 0; std::uint16_t count = 0;
    if (!readBatchHeader(r, 'C', stream, sequence, count) || !c.Matches(stream) || !count ||
        count > (size - sizeof(WireBatchHeader)) / sizeof(WireControl)) return false;
    std::vector<Control> decoded;
    decoded.reserve(count);
    std::vector<std::uint16_t> seen;
    seen.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto* start = bytes + r.offset;
        std::size_t length = 0;
        if (!RecordBytes(start, size - r.offset, length)) return false;
        Control v;
        std::uint16_t id;
        if (!DecodeRecord(c, start, length, v) || !c.FindId(v.guid, id) || !appendUniqueId(seen, id)) return false;
        const auto previous = c.sequences_.find(id);
        if (previous != c.sequences_.end() && !Newer(sequence, previous->second)) return false;
        decoded.push_back(v); r.offset += length;
    }
    if (r.offset != size) return false;
    for (auto id : seen) c.sequences_[id] = sequence;
    output = std::move(decoded); return true;
}

bool EncodeFullFallback(const Context& c, std::uint32_t sequence, const Control& v,
                        std::vector<std::uint8_t>& output) {
    std::uint16_t id;
    if (!c.FindId(v.guid, id)) return false;
    const auto life = c.lives_.find(id);
    if (life == c.lives_.end() || life->second != v.incarnation) return false;
    Writer w;
    writeBatchHeader(w, 'F', c.epoch_, c.generation_, sequence, 1);
    if (!writeFull(w, v) || w.data.size() != sizeof(WireBatchHeader) + sizeof(WireFullRecord) ||
        w.data.size() > MaxBatchBytes) return false;
    output = std::move(w.data); return true;
}

bool DecodeFullFallback(Context& c, const std::uint8_t* bytes, std::size_t size, Control& output) {
    if (!c.ready_ || !bytes || size != sizeof(WireBatchHeader) + sizeof(WireFullRecord) ||
        size > MaxBatchBytes) return false;
    Reader r{bytes, size};
    Stream stream{}; std::uint32_t sequence = 0; std::uint16_t count = 0;
    if (!readBatchHeader(r, 'F', stream, sequence, count) || !c.Matches(stream) || count != 1) return false;
    Control v;
    if (!readFull(r, v)) return false;
    std::uint16_t id;
    if (!r.ok || r.offset != size || !c.FindId(v.guid, id)) return false;
    const auto life = c.lives_.find(id);
    const auto previous = c.sequences_.find(id);
    if (life == c.lives_.end() || life->second != v.incarnation ||
        (previous != c.sequences_.end() && !Newer(sequence, previous->second))) return false;
    c.sequences_[id] = sequence;
    output = v; return true;
}

bool EncodeId(const Context& c, std::uint32_t sequence, std::uint16_t id, std::vector<std::uint8_t>& output) {
    Guid guid;
    if (!c.ready_ || !c.FindGuid(id, guid)) return false;
    Writer w;
    writeBatchHeader(w, 'I', c.epoch_, c.generation_, sequence, 1);
    w.u16(id);
    for (auto x : guid) w.u8(x);
    output = std::move(w.data); return true;
}
bool DecodeId(Context& c, const std::uint8_t* bytes, std::size_t size, std::uint16_t& output) {
    if (!bytes || size != sizeof(WireBatchHeader) + sizeof(WireIdEntry) || size > MaxBatchBytes) return false;
    Reader r{bytes, size};
    Stream stream{}; std::uint32_t sequence = 0; std::uint16_t count = 0;
    if (!readBatchHeader(r, 'I', stream, sequence, count)) return false;
    const auto id = r.u16();
    Guid guid; for (auto& x : guid) x = r.u8();
    if (!r.ok || r.offset != size || count != 1 || !c.AddId(stream, id, guid)) return false;
    output = id; return true;
}
bool EncodeBaseline(const Context& c, std::uint32_t sequence, std::uint16_t id, std::uint16_t token,
                    std::vector<std::uint8_t>& output) {
    const auto life = c.lives_.find(id);
    const auto baseline = c.baselines_.find({id, token});
    if (!c.ready_ || life == c.lives_.end() || baseline == c.baselines_.end() ||
        baseline->second.life != life->second) return false;
    Writer w;
    writeBatchHeader(w, 'B', c.epoch_, c.generation_, sequence, 1);
    w.u16(id); w.u32(baseline->second.anchor); w.u16(token);
    w.data.insert(w.data.end(), baseline->second.bytes.begin(), baseline->second.bytes.end());
    if (w.data.size() != sizeof(WireBatchHeader) + sizeof(WireBaselinePrefix) + sizeof(WireFullRecord) ||
        w.data.size() > MaxBatchBytes) return false;
    output = std::move(w.data); return true;
}
bool DecodeBaseline(Context& c, const std::uint8_t* bytes, std::size_t size, BaselineRef& output) {
    if (!bytes || size != sizeof(WireBatchHeader) + sizeof(WireBaselinePrefix) + sizeof(WireFullRecord) ||
        size > MaxBatchBytes) return false;
    Reader r{bytes, size};
    Stream stream{}; std::uint32_t sequence = 0; std::uint16_t count = 0;
    if (!readBatchHeader(r, 'B', stream, sequence, count) || count != 1 || !c.Matches(stream)) return false;
    const auto id = r.u16();
    const auto anchor = r.u32(); const auto token = r.u16();
    if (!r.ok || !id || !anchor || !token || !c.ids_.count(id)) return false;
    const auto* payload = bytes + r.offset;
    Control snapshot;
    if (!readFull(r, snapshot) || r.offset != size ||
        !c.InstallBaseline(stream, id, token, anchor, snapshot, true, payload)) return false;
    output = BaselineRef{id, token, snapshot.incarnation, anchor};
    return true;
}
bool EncodeSignal(const Context& c, std::uint32_t sequence, const Signal& signal,
                  std::vector<std::uint8_t>& output) {
    const auto& ref = signal.reference;
    const auto reason = static_cast<std::uint8_t>(signal.reason);
    if (!c.ready_ || !ref.id) return false;
    if (signal.kind == SignalKind::Ack) {
        const auto life = c.lives_.find(ref.id);
        const auto base = c.baselines_.find({ref.id, ref.baseline});
        if (reason || life == c.lives_.end() || life->second != ref.incarnation ||
            base == c.baselines_.end() || base->second.life != ref.incarnation ||
            base->second.anchor != ref.anchor) return false;
    } else if (signal.kind != SignalKind::Resync || reason < 1 || reason > 4) return false;
    Writer w;
    writeBatchHeader(w, signal.kind == SignalKind::Ack ? 'A' : 'R', c.epoch_, c.generation_, sequence, 1);
    w.u16(ref.id); w.u32(ref.incarnation); w.u32(ref.anchor); w.u16(ref.baseline); w.u8(reason);
    output = std::move(w.data); return true;
}
bool DecodeSignal(const Context& c, const std::uint8_t* bytes, std::size_t size, Signal& output) {
    if (!bytes || size != sizeof(WireBatchHeader) + sizeof(WireSignal) || size > MaxBatchBytes) return false;
    Reader r{bytes, size};
    const auto magic2 = bytes[1];
    Stream stream{}; std::uint32_t sequence = 0; std::uint16_t count = 0;
    if (!readBatchHeader(r, 0, stream, sequence, count) || count != 1 || !c.Matches(stream)) return false;
    Signal s;
    s.sequence = sequence;
    s.reference.id = r.u16(); s.reference.incarnation = r.u32(); s.reference.anchor = r.u32();
    s.reference.baseline = r.u16(); const auto reason = r.u8();
    if (!r.ok || r.offset != size || !s.reference.id) return false;
    if (magic2 == 'A') {
        s.kind = SignalKind::Ack;
        const auto life = c.lives_.find(s.reference.id);
        const auto base = c.baselines_.find({s.reference.id, s.reference.baseline});
        if (reason || life == c.lives_.end() || life->second != s.reference.incarnation ||
            base == c.baselines_.end() || base->second.life != s.reference.incarnation ||
            base->second.anchor != s.reference.anchor) return false;
    } else if (magic2 == 'R' && reason >= 1 && reason <= 4) {
        s.kind = SignalKind::Resync;
    } else return false;
    s.reason = static_cast<ResyncReason>(reason);
    output = s; return true;
}
} // namespace g3t::npcwire
