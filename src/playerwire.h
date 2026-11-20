// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-independent codec for one player state (state v1, unchanged since protocol 75).
#pragma once

#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "player_state.h"

namespace g3t::playerwire {

constexpr std::uint8_t Version = 1;
constexpr std::uint8_t FullFlag = 1;
constexpr std::uint16_t MetadataMask = 0xffffu;
constexpr std::uint16_t StringName = 1u << 0;
constexpr std::uint16_t StringAnimation = 1u << 1;
constexpr std::uint16_t StringHeldRight = 1u << 2;
constexpr std::uint16_t StringHeldLeft = 1u << 3;
constexpr std::uint16_t StringShape = 1u << 4;
constexpr std::uint16_t StringArmor = 1u << 5;
constexpr std::uint16_t StringHelmet = 1u << 6;
constexpr std::uint16_t StringLook = 1u << 7;
constexpr std::uint16_t FocusNpc = 1u << 8;
constexpr std::uint16_t TargetNpc = 1u << 9;
constexpr std::uint16_t StringOverlay0 = 1u << 10;
constexpr std::uint16_t StringOverlay1 = 1u << 11;
constexpr std::uint16_t StringWeaponAnim = 1u << 12;
constexpr std::uint16_t OverlayMeta = 1u << 13;
constexpr std::uint16_t HitPoints = 1u << 14;
constexpr std::uint16_t HitPointsMax = 1u << 15;
constexpr std::uint16_t LegacyMetadataMask = MetadataMask & ~(HitPoints | HitPointsMax);
constexpr std::int32_t MinHitPoints = -1000000;
constexpr std::int32_t MaxHitPoints = 1000000;
constexpr std::size_t FixedBytes = 65;
constexpr std::uint32_t FullKeyframeMs = 5000;

inline constexpr std::int32_t ClampHitPoints(std::int32_t value, std::int32_t low = MinHitPoints) noexcept {
    return value < low ? low : (value > MaxHitPoints ? MaxHitPoints : value);
}

enum class Status : std::uint8_t {
    Ok,
    Stale,
    NeedKeyframe,
    Invalid
};

struct Packet {
    std::vector<std::uint8_t> bytes;
    bool reliable = false;
    bool full = false;
    std::uint32_t revision = 0;
};

namespace detail {

inline bool finiteCoordsAndRotation(const net::PlayerState& state) {
    for (float value : state.position)
        if (!std::isfinite(value) || std::fabs(value) > 1.0e7f) return false;
    for (float value : state.rotation)
        if (!std::isfinite(value) || std::fabs(value) > 2.0f) return false;
    return true;
}

inline bool finiteAndLimited(const net::PlayerState& state) {
    if (state.id < 1 || state.id > 7) return false;
    if (state.gold < 0) return false;
    if (state.hitPoints < MinHitPoints || state.hitPoints > MaxHitPoints) return false;
    if (state.hitPointsMax < 0 || state.hitPointsMax > MaxHitPoints) return false;
    return finiteCoordsAndRotation(state);
}

struct Writer {
    std::vector<std::uint8_t> bytes;

    void reserve(std::size_t size) { bytes.reserve(size); }
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value));
        u16(static_cast<std::uint16_t>(value >> 16));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void f32(float value) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(value), "binary32 required");
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
};

struct Reader {
    const std::uint8_t* bytes;
    std::size_t size;
    std::size_t offset = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (offset >= size) { ok = false; return 0; }
        return bytes[offset++];
    }
    std::uint16_t u16() {
        const std::uint8_t a = u8();
        const std::uint8_t b = u8();
        return static_cast<std::uint16_t>(a | (static_cast<std::uint16_t>(b) << 8));
    }
    std::uint32_t u32() {
        const std::uint16_t a = u16();
        const std::uint16_t b = u16();
        return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 16);
    }
    std::int32_t i32() {
        const std::uint32_t bits = u32();
        return bits <= 0x7fffffffu ? static_cast<std::int32_t>(bits)
                                   : -1 - static_cast<std::int32_t>(0xffffffffu - bits);
    }
    float f32() {
        const std::uint32_t bits = u32();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
};

inline bool boundedCString(const char* data, std::size_t capacity, std::size_t& length) {
    for (std::size_t i = 0; i < capacity; ++i) {
        if (data[i] == '\0') {
            length = i;
            return true;
        }
    }
    return false;
}

inline bool stringValid(const char* data, std::size_t capacity) {
    std::size_t length = 0;
    return boundedCString(data, capacity, length);
}

inline void writeString(Writer& writer, const char* data, std::size_t capacity) {
    std::size_t length = 0;
    boundedCString(data, capacity, length);
    writer.u8(static_cast<std::uint8_t>(length));
    for (std::size_t i = 0; i < length; ++i) writer.u8(static_cast<std::uint8_t>(data[i]));
}

inline bool readString(Reader& reader, char* output, std::size_t capacity) {
    const std::uint8_t length = reader.u8();
    if (!reader.ok) return false;
    if (length >= capacity) return false;
    std::memset(output, 0, capacity);
    for (std::uint16_t i = 0; i < length; ++i) {
        const std::uint8_t value = reader.u8();
        if (!reader.ok || value == 0) return false;
        output[i] = static_cast<char>(value);
    }
    return true;
}

inline bool sameCString(const char* a, const char* b, std::size_t capacity) {
    std::size_t lengthA = 0, lengthB = 0;
    if (!boundedCString(a, capacity, lengthA) || !boundedCString(b, capacity, lengthB)) return false;
    return lengthA == lengthB && std::memcmp(a, b, lengthA) == 0;
}

struct StringField {
    std::uint16_t bit;
    std::size_t capacity;
    const char* (*read)(const net::PlayerState&);
    char* (*write)(net::PlayerState&);
};

constexpr StringField StringFields[] = {
    {StringName, sizeof(net::PlayerState::name),
     [](const net::PlayerState& s) -> const char* { return s.name; },
     [](net::PlayerState& s) -> char* { return s.name; }},
    {StringAnimation, sizeof(net::PlayerState::animation),
     [](const net::PlayerState& s) -> const char* { return s.animation; },
     [](net::PlayerState& s) -> char* { return s.animation; }},
    {StringHeldRight, sizeof(net::PlayerState::heldRight),
     [](const net::PlayerState& s) -> const char* { return s.heldRight; },
     [](net::PlayerState& s) -> char* { return s.heldRight; }},
    {StringHeldLeft, sizeof(net::PlayerState::heldLeft),
     [](const net::PlayerState& s) -> const char* { return s.heldLeft; },
     [](net::PlayerState& s) -> char* { return s.heldLeft; }},
    {StringShape, sizeof(net::PlayerState::shape),
     [](const net::PlayerState& s) -> const char* { return s.shape; },
     [](net::PlayerState& s) -> char* { return s.shape; }},
    {StringArmor, sizeof(net::PlayerState::armor),
     [](const net::PlayerState& s) -> const char* { return s.armor; },
     [](net::PlayerState& s) -> char* { return s.armor; }},
    {StringHelmet, sizeof(net::PlayerState::helmet),
     [](const net::PlayerState& s) -> const char* { return s.helmet; },
     [](net::PlayerState& s) -> char* { return s.helmet; }},
    {StringLook, sizeof(net::PlayerState::look),
     [](const net::PlayerState& s) -> const char* { return s.look; },
     [](net::PlayerState& s) -> char* { return s.look; }},
    {StringOverlay0, sizeof(net::PlayerState::overlayAnim0),
     [](const net::PlayerState& s) -> const char* { return s.overlayAnim0; },
     [](net::PlayerState& s) -> char* { return s.overlayAnim0; }},
    {StringOverlay1, sizeof(net::PlayerState::overlayAnim1),
     [](const net::PlayerState& s) -> const char* { return s.overlayAnim1; },
     [](net::PlayerState& s) -> char* { return s.overlayAnim1; }},
    {StringWeaponAnim, sizeof(net::PlayerState::weaponAnim),
     [](const net::PlayerState& s) -> const char* { return s.weaponAnim; },
     [](net::PlayerState& s) -> char* { return s.weaponAnim; }},
};

inline std::uint16_t metadataMask(const net::PlayerState& baseline, const net::PlayerState& state) {
    std::uint16_t mask = 0;
    for (const StringField& field : StringFields)
        if (!sameCString(field.read(baseline), field.read(state), field.capacity)) mask |= field.bit;
    if (baseline.focusNpc != state.focusNpc) mask |= FocusNpc;
    if (baseline.targetNpc != state.targetNpc) mask |= TargetNpc;
    if (baseline.overlayMeta != state.overlayMeta) mask |= OverlayMeta;
    if (baseline.hitPoints != state.hitPoints) mask |= HitPoints;
    if (baseline.hitPointsMax != state.hitPointsMax) mask |= HitPointsMax;
    return mask;
}

inline void writeFixed(Writer& writer, const net::PlayerState& state,
                       std::uint8_t full, std::uint32_t revision, std::uint16_t mask) {
    writer.u8(Version);
    writer.u8(full);
    writer.u8(state.id);
    writer.u32(state.sequence);
    writer.u32(state.sent);
    writer.u32(state.world);
    writer.u32(revision);
    for (float value : state.position) writer.f32(value);
    for (float value : state.rotation) writer.f32(value);
    writer.u8(state.flags);
    writer.u8(state.movementMode);
    writer.u8(state.aniState);
    writer.u32(static_cast<std::uint32_t>(state.gold));
    writer.u8(state.actionSeq);
    writer.u16(state.actionMs);
    writer.u16(state.motionFrac);
    writer.u32(state.downedSequence);
    writer.u16(mask);
}

inline void writeMetadata(Writer& writer, const net::PlayerState& state, std::uint16_t mask) {
    for (const StringField& field : StringFields)
        if (mask & field.bit) writeString(writer, field.read(state), field.capacity);
    if (mask & FocusNpc) writer.u32(state.focusNpc);
    if (mask & TargetNpc) writer.u32(state.targetNpc);
    if (mask & OverlayMeta) writer.u32(state.overlayMeta);
    if (mask & HitPoints) writer.i32(state.hitPoints);
    if (mask & HitPointsMax) writer.i32(state.hitPointsMax);
}

inline bool validStrings(const net::PlayerState& state) {
    for (const StringField& field : StringFields)
        if (!stringValid(field.read(state), field.capacity)) return false;
    return true;
}

inline bool readStrings(Reader& reader, std::uint16_t mask, net::PlayerState& metadata) {
    for (const StringField& field : StringFields)
        if ((mask & field.bit) && !readString(reader, field.write(metadata), field.capacity)) return false;
    return true;
}

inline void copyStrings(std::uint16_t mask, const net::PlayerState& src, net::PlayerState& dst) {
    for (const StringField& field : StringFields)
        if (mask & field.bit) std::memcpy(field.write(dst), field.read(src), field.capacity);
}

inline bool newer(std::uint32_t value, std::uint32_t previous) {
    return static_cast<std::int32_t>(value - previous) > 0;
}

} // namespace detail

inline bool PeekPlayerId(const std::uint8_t* data, std::size_t size, std::uint8_t& id) {
    if (!data || size < 3) return false;
    id = data[2];
    return id >= 1 && id <= 7;
}

class Encoder {
public:
    Status Plan(const net::PlayerState& state, std::uint32_t now, bool forceFull, Packet& packet) {
        packet = Packet{};
        if (!detail::finiteAndLimited(state) || !detail::validStrings(state)) return Status::Invalid;

        const bool first = !haveBaseline_;
        std::uint16_t mask = 0;
        bool full = first || forceFull;
        if (!first) {
            mask = detail::metadataMask(baseline_, state);
            if (!full && static_cast<std::uint32_t>(now - lastFull_) >= FullKeyframeMs) full = true;
        }

        const bool metadataChange = mask != 0;
        if (full) mask = MetadataMask;

        const std::uint32_t packetRevision =
            full || metadataChange ? revision_ + 1u : revision_;

        detail::Writer writer;
        writer.reserve(FixedBytes + 352);
        detail::writeFixed(writer, state, full ? 1u : 0u, packetRevision, mask);
        detail::writeMetadata(writer, state, mask);

        packet.bytes = std::move(writer.bytes);
        packet.full = full;
        packet.revision = packetRevision;
        packet.reliable = first || full || metadataChange;
        if (!first) {
            packet.reliable = packet.reliable ||
                              state.flags != baseline_.flags ||
                              state.downedSequence != baseline_.downedSequence ||
                              state.world != baseline_.world ||
                              state.actionSeq != baseline_.actionSeq;
        }
        return Status::Ok;
    }

    void Commit(const net::PlayerState& state, std::uint32_t now, const Packet& packet) {
        if (packet.full) lastFull_ = now;
        revision_ = packet.revision;
        baseline_ = state;
        haveBaseline_ = true;
    }

    void Reset() {
        haveBaseline_ = false;
        revision_ = 0;
        lastFull_ = 0;
        baseline_ = net::PlayerState{};
    }

    bool HaveBaseline() const { return haveBaseline_; }
    std::uint32_t Revision() const { return revision_; }
    std::uint32_t LastFull() const { return lastFull_; }

private:
    net::PlayerState baseline_{};
    bool haveBaseline_ = false;
    std::uint32_t revision_ = 0;
    std::uint32_t lastFull_ = 0;
};

class Decoder {
public:
    Status Decode(const std::uint8_t* data, std::size_t size, net::PlayerState& output) {
        return Decode(data, size, output, 0);
    }

    Status Decode(const std::uint8_t* data, std::size_t size,
                  net::PlayerState& output, std::uint8_t authoritativeId) {
        if (!data || size < FixedBytes) return Status::Invalid;

        detail::Reader reader{data, size};
        const std::uint8_t version = reader.u8();
        const std::uint8_t full = reader.u8();
        const std::uint8_t id = reader.u8();
        const std::uint32_t sequence = reader.u32();
        const std::uint32_t sent = reader.u32();
        const std::uint32_t world = reader.u32();
        const std::uint32_t revision = reader.u32();
        net::PlayerState value{};
        for (float& component : value.position) component = reader.f32();
        for (float& component : value.rotation) component = reader.f32();
        value.flags = reader.u8();
        value.movementMode = reader.u8();
        value.aniState = reader.u8();
        const std::uint32_t goldBits = reader.u32();
        value.actionSeq = reader.u8();
        value.actionMs = reader.u16();
        value.motionFrac = reader.u16();
        value.downedSequence = reader.u32();
        const std::uint16_t mask = reader.u16();
        if (!reader.ok) return Status::Invalid;

        if (version != Version || (full != 0 && full != FullFlag) ||
            id < 1 || id > 7 || (authoritativeId != 0 &&
            (authoritativeId < 1 || authoritativeId > 7 || authoritativeId != id)))
            return Status::Invalid;
        if ((full == FullFlag) ? (mask != MetadataMask && mask != LegacyMetadataMask) : ((mask & ~MetadataMask) != 0))
            return Status::Invalid;
        if (!detail::finiteCoordsAndRotation(value)) return Status::Invalid;
        if (goldBits > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
            return Status::Invalid;

        value.id = id;
        value.sequence = sequence;
        value.sent = sent;
        value.world = world;
        value.gold = static_cast<std::int32_t>(goldBits);

        net::PlayerState metadata{};
        if (!detail::readStrings(reader, mask, metadata)) return Status::Invalid;
        if (mask & FocusNpc) metadata.focusNpc = reader.u32();
        if (mask & TargetNpc) metadata.targetNpc = reader.u32();
        if (mask & OverlayMeta) metadata.overlayMeta = reader.u32();
        if (mask & HitPoints) metadata.hitPoints = reader.i32();
        if (mask & HitPointsMax) metadata.hitPointsMax = reader.i32();
        if (!reader.ok || reader.offset != size) return Status::Invalid;
        if ((mask & HitPoints) &&
            (metadata.hitPoints < MinHitPoints || metadata.hitPoints > MaxHitPoints))
            return Status::Invalid;
        if ((mask & HitPointsMax) &&
            (metadata.hitPointsMax < 0 || metadata.hitPointsMax > MaxHitPoints))
            return Status::Invalid;

        if (full == FullFlag) {
            if (hasKeyframe_ && static_cast<std::int32_t>(sequence - sequence_) <= 0)
                return Status::Stale;
            if (id != 0 && hasKeyframe_ && id != currentState_.id)
                return Status::Invalid;

            value.focusNpc = metadata.focusNpc;
            value.targetNpc = metadata.targetNpc;
            value.overlayMeta = metadata.overlayMeta;
            value.hitPoints = metadata.hitPoints;
            value.hitPointsMax = metadata.hitPointsMax;
            detail::copyStrings(MetadataMask, metadata, value);

            currentState_ = value;
            hasKeyframe_ = true;
            sequence_ = sequence;
            revision_ = revision;
            output = value;
            return Status::Ok;
        }

        if (!hasKeyframe_) return Status::NeedKeyframe;
        if (static_cast<std::int32_t>(sequence - sequence_) <= 0)
            return Status::Stale;
        if (id != currentState_.id) return Status::Invalid;

        if (mask == 0) {
            if (revision != revision_) return Status::NeedKeyframe;
        } else {
            if (revision != revision_ + 1u) return Status::NeedKeyframe;
        }

        net::PlayerState next = value;
        next.focusNpc = (mask & FocusNpc) ? metadata.focusNpc : currentState_.focusNpc;
        next.targetNpc = (mask & TargetNpc) ? metadata.targetNpc : currentState_.targetNpc;
        next.overlayMeta = (mask & OverlayMeta) ? metadata.overlayMeta : currentState_.overlayMeta;
        next.hitPoints = (mask & HitPoints) ? metadata.hitPoints : currentState_.hitPoints;
        next.hitPointsMax = (mask & HitPointsMax) ? metadata.hitPointsMax : currentState_.hitPointsMax;
        detail::copyStrings(MetadataMask & ~mask, currentState_, next);
        detail::copyStrings(mask, metadata, next);

        if (mask != 0) revision_ = revision;
        sequence_ = sequence;
        currentState_ = next;
        output = next;
        return Status::Ok;
    }

    void Reset() {
        hasKeyframe_ = false;
        sequence_ = 0;
        revision_ = 0;
        currentState_ = net::PlayerState{};
    }

    bool HasKeyframe() const { return hasKeyframe_; }
    std::uint32_t Revision() const { return revision_; }
    std::uint32_t Sequence() const { return sequence_; }

private:
    net::PlayerState currentState_{};
    bool hasKeyframe_ = false;
    std::uint32_t sequence_ = 0;
    std::uint32_t revision_ = 0;
};

class Cadence {
public:
    bool Due(std::uint32_t now, std::uint32_t hz) {
        if (!hz || hz > 1000) return false;
        if (!started_) {
            started_ = true;
            hz_ = hz;
            deadline_ = now;
            remainder_ = 0;
            AdvanceDeadline(hz);
            return false;
        }
        if (hz_ != hz) {
            hz_ = hz;
            deadline_ = now;
            remainder_ = 0;
            AdvanceDeadline(hz);
            return false;
        }
        if (static_cast<std::int32_t>(now - deadline_) < 0) return false;

        const std::uint32_t elapsed = now - deadline_;
        const std::uint32_t upper = 1000u / hz + (1000u % hz ? 1u : 0u);
        if (elapsed > upper * 2u) {
            deadline_ = now;
            remainder_ = 0;
            AdvanceDeadline(hz);
        } else {
            AdvanceDeadline(hz);
        }
        return true;
    }

    void Reset() {
        started_ = false;
        hz_ = 0;
        deadline_ = 0;
        remainder_ = 0;
    }

private:
    std::uint32_t deadline_ = 0;
    std::uint32_t remainder_ = 0;
    std::uint32_t hz_ = 0;
    bool started_ = false;

    void AdvanceDeadline(std::uint32_t hz) {
        const std::uint64_t ticks = static_cast<std::uint64_t>(remainder_) + 1000u;
        const std::uint32_t add = static_cast<std::uint32_t>(ticks / hz);
        remainder_ = static_cast<std::uint32_t>(ticks % hz);
        deadline_ += add;
    }
};

} // namespace g3t::playerwire
