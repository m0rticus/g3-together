// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace g3t::npclease {

constexpr uint8_t WireVersion = 2;
constexpr uint32_t LeaseMs = 3000, LeaseSafetyMs = 300, FarDwellMs = 3000;
constexpr uint32_t RecallWaitMs = 10000, PrepareMs = 3000, StateMs = 200;
constexpr float ClientRadiusCm = 10000.0f;
constexpr size_t MaxLeases = 512;

enum class Kind : uint8_t {
    Request = 1,
    Offer,
    Prepared,
    Commit,
    Ack,
    State,
    Renew,
    Recall,
    Cut,
    Abort,
    Reject,
    Adopt = 12,
    StateFull = 13
};

enum class Phase : uint8_t { Host, Offering, Client, Recalling, Frozen, Dormant };

enum Effect : uint8_t { NativeDeath = 1, QuestDeath = 2 };

#pragma pack(push, 1)
struct Checkpoint {
    uint8_t templateId[16]{};
    float position[3]{};
    float rotation[4]{0, 0, 0, 1};
    int32_t hp = 0, maxHp = 1, mana = 0, maxMana = 0, stamina = 0, maxStamina = 0;
    uint32_t level = 0, status = 0;
    uint8_t dead = 0, defeated = 0, attitude = 0, aiMode = 0, action = 0, attackReason = 0, effects = 0, busy = 0;
    char name[64]{}, routine[64]{};
    uint8_t targetPlayer = 0;
    uint32_t statusAge[5]{};
};

struct Message {
    uint8_t version = WireVersion;
    Kind kind = Kind::Request;
    uint8_t owner = 0;
    uint32_t world = 0, epoch = 0, incarnation = 1, generation = 0, revision = 0, echo = 0;
    uint16_t duration = LeaseMs;
    uint8_t guid[16]{};
    Checkpoint state{};
};
#pragma pack(pop)

static_assert(sizeof(Checkpoint) <= 512, "Checkpoint wire layout must fit");
static_assert(sizeof(Message) < 1000, "Message wire layout must remain fixed");
static_assert(sizeof(Checkpoint) == 233, "Packed Checkpoint wire size");
static_assert(sizeof(Message) == 278, "Packed Message wire size");

namespace detail {

inline bool CstrValid(char const* value, size_t size)
{
    if (!value || value[0] == '\0') return false;
    for (size_t i = 0; i < size; ++i) {
        if (value[i] == '\0') return true;
    }
    return false;
}

inline bool CstrValidEmptyAllowed(char const* value, size_t size)
{
    if (!value) return false;
    for (size_t i = 0; i < size; ++i) if (value[i] == '\0') return true;
    return false;
}

inline bool GuidNonzero(uint8_t const* guid)
{
    for (size_t i = 0; i < 16; ++i) if (guid[i] != 0) return true;
    return false;
}

inline bool ExactState(Checkpoint const& a, Checkpoint const& b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

} // namespace detail

inline bool ValidStateFields(Checkpoint const& c)
{
    if (!detail::GuidNonzero(c.templateId)) return false;

    for (float v : c.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;

    double quaternionLength = 0;
    for (float v : c.rotation) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
        quaternionLength += double(v) * v;
    }
    if (std::fabs(std::sqrt(quaternionLength) - 1.0) > 0.01) return false;

    if (c.maxHp < 1 || c.maxHp > 1000000 ||
        c.hp < 0 || c.hp > c.maxHp ||
        c.maxMana < 0 || c.maxMana > 1000000 || c.mana < 0 || c.mana > c.maxMana ||
        c.maxStamina < 0 || c.maxStamina > 1000000 || c.stamina < 0 || c.stamina > c.maxStamina ||
        c.level > 1000000) return false;

    if ((c.dead > 1) || (c.defeated > 1) || (c.busy > 1) ||
        c.attitude > 6 || c.aiMode > 11 || c.action >= 145 || c.attackReason > 21 ||
        c.targetPlayer > 7 ||
        (c.effects & static_cast<uint8_t>(~uint8_t(NativeDeath | QuestDeath))) != 0) return false;

    return true;
}

inline bool Valid(Checkpoint const& c)
{
    return ValidStateFields(c) &&
           detail::CstrValid(c.name, sizeof(c.name)) &&
           detail::CstrValidEmptyAllowed(c.routine, sizeof(c.routine));
}

inline bool ValidMetadata(Message const& m)
{
    if (m.version != WireVersion) return false;
    if (m.kind < Kind::Request || m.kind > Kind::StateFull) return false;
    if (!m.world || !m.incarnation || m.owner >= 8 || m.duration > LeaseMs) return false;
    if (!detail::GuidNonzero(m.guid)) return false;
    return true;
}

inline bool Valid(Message const& m)
{
    if (!ValidMetadata(m)) return false;
    if (m.kind == Kind::Request || m.kind == Kind::Offer ||
        m.kind == Kind::State || m.kind == Kind::StateFull || m.kind == Kind::Commit ||
        m.kind == Kind::Cut || m.kind == Kind::Adopt)
        return Valid(m.state);
    if (m.kind == Kind::Renew)
        return (m.state.effects & static_cast<uint8_t>(~uint8_t(NativeDeath | QuestDeath))) == 0;
    return detail::CstrValidEmptyAllowed(m.state.name, sizeof(m.state.name)) &&
           detail::CstrValidEmptyAllowed(m.state.routine, sizeof(m.state.routine));
}

inline bool Reached(uint32_t now, uint32_t deadline)
{
    const uint32_t d = now - deadline;
    if (d & 0x80000000u) {
        const int64_t signedDiff = int64_t(d) - int64_t(uint64_t(1) << 32);
        return signedDiff >= 0;
    }
    return int64_t(d) >= 0;
}

inline bool Newer(uint32_t next, uint32_t previous)
{
    const uint32_t d = next - previous;
    if (d & 0x80000000u) {
        const int64_t signedDiff = int64_t(d) - int64_t(uint64_t(1) << 32);
        return signedDiff > 0;
    }
    return int64_t(d) > 0;
}

struct Interest {
    bool sameWorld = true, ready = true, eligible = true, conflict = false, hostPresent = true;
    float hostDistance = 0, clientDistance = 0, hostRoi = 8000;
};

struct Eligibility {
    uint32_t farSince = 0;
    bool farHeld = false;
    bool dwellActive = false;

    bool Delegate(Interest const& interest, uint32_t now)
    {
        if (!interest.sameWorld || !interest.ready || !interest.eligible || interest.conflict ||
            !std::isfinite(interest.hostDistance) || !std::isfinite(interest.clientDistance) ||
            !std::isfinite(interest.hostRoi) || interest.hostRoi < 0 ||
            interest.hostDistance < 0 || interest.clientDistance < 0) {
            farSince = 0;
            farHeld = false;
            dwellActive = false;
            return false;
        }

        const float farRadius = interest.hostRoi + 2000.0f > 12000.0f ?
                                interest.hostRoi + 2000.0f : 12000.0f;
        const float recallRadius = farRadius - 2000.0f;

        if (farHeld && interest.hostPresent && interest.hostDistance <= recallRadius) {
            farSince = 0;
            farHeld = false;
            dwellActive = false;
            return false;
        }
        if (interest.clientDistance > ClientRadiusCm) {
            farSince = 0;
            dwellActive = false;
            return false;
        }
        if (!interest.hostPresent) {
            farSince = 0;
            farHeld = true;
            dwellActive = false;
            return true;
        }
        if (farHeld) {
            return true;
        }

        if (interest.hostDistance >= farRadius) {
            if (!dwellActive) {
                farSince = now;
                dwellActive = true;
            }
            if (Reached(now, farSince + FarDwellMs)) {
                farHeld = true;
                dwellActive = false;
                return true;
            }
        } else {
            farSince = 0;
            dwellActive = false;
        }
        return false;
    }

    bool Recall(Interest const& interest) const
    {
        if (!std::isfinite(interest.hostDistance) || !std::isfinite(interest.clientDistance) ||
            !std::isfinite(interest.hostRoi) || interest.hostRoi < 0 ||
            interest.hostDistance < 0 || interest.clientDistance < 0) return true;
        if (!interest.sameWorld || !interest.ready || !interest.eligible || interest.conflict) return true;
        if (!interest.hostPresent) return false;
        const float farRadius = interest.hostRoi + 2000.0f > 12000.0f ?
                                interest.hostRoi + 2000.0f : 12000.0f;
        return interest.hostDistance <= farRadius - 2000.0f;
    }
};

struct Lease {
    Phase phase = Phase::Host;
    uint32_t epoch = 0, incarnation = 1, generation = 0, revision = 0, deadline = 0, started = 0;
    uint8_t owner = 1;
    Checkpoint state{};

    bool Offer(uint8_t client, uint32_t peerGeneration, uint32_t nextEpoch, uint32_t now, Checkpoint const& baseline)
    {
        if ((phase != Phase::Host && phase != Phase::Dormant) ||
            client >= 8 ||
            !Newer(nextEpoch, epoch) || !Valid(baseline)) return false;
        if (state.dead && !baseline.dead) return false;
        if ((state.effects & static_cast<uint8_t>(~baseline.effects)) != 0) return false;
        phase = Phase::Offering;
        epoch = nextEpoch;
        generation = peerGeneration;
        revision = 0;
        deadline = now + PrepareMs;
        started = now;
        owner = client;
        state = baseline;
        return true;
    }

    bool Prepared(uint32_t authorityEpoch, uint32_t now)
    {
        if (phase != Phase::Offering || authorityEpoch != epoch ||
            Reached(now, deadline)) return false;
        phase = Phase::Client;
        deadline = now + LeaseMs + LeaseSafetyMs;
        started = now;
        return true;
    }

    bool AcceptState(uint8_t client, uint32_t peerGeneration, uint32_t authorityEpoch,
                     uint32_t life, uint32_t nextRevision, Checkpoint const& value)
    {
        if ((phase != Phase::Client && phase != Phase::Recalling) ||
            client != owner || peerGeneration != generation ||
            authorityEpoch != epoch || life != incarnation || !Valid(value)) return false;
        if (state.dead && !value.dead) return false;
        if ((state.effects & static_cast<uint8_t>(~value.effects)) != 0) return false;
        if (Newer(nextRevision, revision)) {
            revision = nextRevision;
            state = value;
            return true;
        }
        if (nextRevision == revision && detail::ExactState(state, value)) return false;
        return false;
    }

    bool Recall(uint32_t now)
    {
        if (phase != Phase::Client) return false;
        phase = Phase::Recalling;
        started = now;
        return true;
    }

    bool Cut(uint8_t client, uint32_t peerGeneration, uint32_t authorityEpoch,
             uint32_t life, uint32_t nextRevision, Checkpoint const& value)
    {
        if ((phase != Phase::Client && phase != Phase::Recalling) ||
            client != owner || peerGeneration != generation ||
            authorityEpoch != epoch || life != incarnation || !Valid(value)) return false;
        if (state.dead && !value.dead) return false;
        if ((state.effects & static_cast<uint8_t>(~value.effects)) != 0) return false;

        if (Newer(nextRevision, revision)) {
            revision = nextRevision;
            state = value;
            phase = Phase::Frozen;
            return true;
        }
        if (nextRevision == revision && detail::ExactState(state, value)) {
            phase = Phase::Frozen;
            return true;
        }
        return false;
    }

    bool HandBack(uint32_t nextEpoch, uint32_t now)
    {
        if (phase != Phase::Frozen || !Newer(nextEpoch, epoch)) return false;
        epoch = nextEpoch;
        generation = 0;
        deadline = 0;
        started = now;
        owner = 1;
        phase = Phase::Host;
        return true;
    }

    bool Recover(uint32_t now)
    {
        if (phase == Phase::Offering) {
            if (!Reached(now, deadline)) return false;
            phase = Phase::Host;
            generation = 0;
            deadline = 0;
            started = 0;
            owner = 1;
            return true;
        }
        if (phase == Phase::Client || phase == Phase::Recalling) {
            if (!Reached(now, deadline)) return false;
            phase = Phase::Dormant;
            generation = 0;
            deadline = 0;
            started = 0;
            owner = 1;
            return true;
        }
        return false;
    }

    bool ActiveLeases() const
    {
        return phase == Phase::Offering || phase == Phase::Client ||
               phase == Phase::Recalling || phase == Phase::Frozen;
    }
};

inline bool CommitEffect(Checkpoint& state, Effect effect)
{
    if (effect != NativeDeath && effect != QuestDeath) return false;
    const uint8_t bit = static_cast<uint8_t>(effect);
    if ((state.effects & bit) != 0) return false;
    state.effects = static_cast<uint8_t>(state.effects | bit);
    if (effect == NativeDeath) state.dead = 1;
    return true;
}

struct Saved {
    uint8_t guid[16]{};
    uint32_t incarnation = 1, epoch = 0, revision = 0;
    Checkpoint state{};
    uint8_t adopted = 0;
};

using SaveOverlay = std::vector<Saved>;

namespace detail {

constexpr uint32_t OverlayMagic = 0x314c504eu;
constexpr uint32_t OverlayVersion = 2, LegacyOverlayVersion = 1;
constexpr uint32_t LegacySavedWireSize = 16u + 4u + 4u + 4u + sizeof(Checkpoint);
constexpr uint32_t SavedWireSize = LegacySavedWireSize + 1u;

inline void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

inline uint32_t Get32(uint8_t const* bytes)
{
    return uint32_t(bytes[0]) |
           (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

inline void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

inline uint16_t Get16(uint8_t const* bytes)
{
    return static_cast<uint16_t>(uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8));
}

inline void PutI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value)
{
    Put32(bytes, offset, static_cast<uint32_t>(value));
}

inline int32_t GetI32(uint8_t const* bytes)
{
    const uint32_t value = Get32(bytes);
    int32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline void PutF32(std::vector<uint8_t>& bytes, size_t offset, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    Put32(bytes, offset, bits);
}

inline float GetF32(uint8_t const* bytes)
{
    const uint32_t bits = Get32(bytes);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline uint32_t Crc32(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

inline size_t WriteCheckpoint(std::vector<uint8_t>& bytes, size_t offset, Checkpoint const& c)
{
    std::memcpy(bytes.data() + offset, c.templateId, 16);
    offset += 16;
    for (float v : c.position) { PutF32(bytes, offset, v); offset += 4; }
    for (float v : c.rotation) { PutF32(bytes, offset, v); offset += 4; }

    int32_t ints[] = { c.hp, c.maxHp, c.mana, c.maxMana, c.stamina, c.maxStamina };
    for (int32_t v : ints) { PutI32(bytes, offset, v); offset += 4; }
    Put32(bytes, offset, c.level); offset += 4;
    Put32(bytes, offset, c.status); offset += 4;
    for (uint8_t v : { c.dead, c.defeated, c.attitude, c.aiMode, c.action,
                       c.attackReason, c.effects, c.busy })
    { bytes[offset++] = v; }
    std::memcpy(bytes.data() + offset, c.name, 64); offset += 64;
    std::memcpy(bytes.data() + offset, c.routine, 64); offset += 64;
    bytes[offset++] = c.targetPlayer;
    for (uint32_t v : c.statusAge) { Put32(bytes, offset, v); offset += 4; }
    return offset;
}

inline size_t ReadCheckpoint(uint8_t const* data, size_t offset, Checkpoint& c)
{
    std::memset(&c, 0, sizeof(c));
    c.templateId[0] = 0;
    std::memcpy(c.templateId, data + offset, 16); offset += 16;
    for (float& v : c.position) { v = GetF32(data + offset); offset += 4; }
    for (float& v : c.rotation) { v = GetF32(data + offset); offset += 4; }

    c.hp = GetI32(data + offset); offset += 4;
    c.maxHp = GetI32(data + offset); offset += 4;
    c.mana = GetI32(data + offset); offset += 4;
    c.maxMana = GetI32(data + offset); offset += 4;
    c.stamina = GetI32(data + offset); offset += 4;
    c.maxStamina = GetI32(data + offset); offset += 4;
    c.level = Get32(data + offset); offset += 4;
    c.status = Get32(data + offset); offset += 4;
    c.dead = data[offset++];
    c.defeated = data[offset++];
    c.attitude = data[offset++];
    c.aiMode = data[offset++];
    c.action = data[offset++];
    c.attackReason = data[offset++];
    c.effects = data[offset++];
    c.busy = data[offset++];
    std::memcpy(c.name, data + offset, 64); offset += 64;
    std::memcpy(c.routine, data + offset, 64); offset += 64;
    c.targetPlayer = data[offset++];
    for (uint32_t& v : c.statusAge) { v = Get32(data + offset); offset += 4; }
    return offset;
}

} // namespace detail

constexpr size_t MessageHeaderWireSize = 45;
constexpr size_t FullMessageWireSize = MessageHeaderWireSize + sizeof(Checkpoint);
constexpr size_t StateMessageWireSize = MessageHeaderWireSize + 84 + 21;
constexpr size_t RenewMessageWireSize = MessageHeaderWireSize + 1;
static_assert(FullMessageWireSize == sizeof(Message), "Full message wire size");
static_assert(offsetof(Checkpoint, name) == 84 && offsetof(Checkpoint, targetPlayer) == 212,
              "Compact state ranges must match checkpoint layout");

inline bool EncodeMessage(Message const& message, std::vector<uint8_t>& output)
{
    if (!Valid(message)) return false;
    const size_t size = message.kind == Kind::State ? StateMessageWireSize :
                        message.kind == Kind::Renew ? RenewMessageWireSize : FullMessageWireSize;
    std::vector<uint8_t> encoded(size);
    encoded[0] = message.version;
    encoded[1] = static_cast<uint8_t>(message.kind);
    encoded[2] = message.owner;
    detail::Put32(encoded, 3, message.world);
    detail::Put32(encoded, 7, message.epoch);
    detail::Put32(encoded, 11, message.incarnation);
    detail::Put32(encoded, 15, message.generation);
    detail::Put32(encoded, 19, message.revision);
    detail::Put32(encoded, 23, message.echo);
    detail::Put16(encoded, 27, message.duration);
    std::memcpy(encoded.data() + 29, message.guid, sizeof(message.guid));
    if (message.kind == Kind::Renew) {
        encoded[MessageHeaderWireSize] = message.state.effects;
    } else if (message.kind == Kind::State) {
        std::vector<uint8_t> state(sizeof(Checkpoint));
        detail::WriteCheckpoint(state, 0, message.state);
        std::memcpy(encoded.data() + MessageHeaderWireSize, state.data(), 84);
        std::memcpy(encoded.data() + MessageHeaderWireSize + 84, state.data() + 212, 21);
    } else {
        detail::WriteCheckpoint(encoded, MessageHeaderWireSize, message.state);
    }
    output.swap(encoded);
    return true;
}

inline bool DecodeMessage(uint8_t const* data, size_t size, Message& output)
{
    if (!data || size < MessageHeaderWireSize) return false;
    Message decoded;
    decoded.version = data[0];
    decoded.kind = static_cast<Kind>(data[1]);
    decoded.owner = data[2];
    decoded.world = detail::Get32(data + 3);
    decoded.epoch = detail::Get32(data + 7);
    decoded.incarnation = detail::Get32(data + 11);
    decoded.generation = detail::Get32(data + 15);
    decoded.revision = detail::Get32(data + 19);
    decoded.echo = detail::Get32(data + 23);
    decoded.duration = detail::Get16(data + 27);
    std::memcpy(decoded.guid, data + 29, sizeof(decoded.guid));
    if (!ValidMetadata(decoded)) return false;
    const size_t expected = decoded.kind == Kind::State ? StateMessageWireSize :
                            decoded.kind == Kind::Renew ? RenewMessageWireSize : FullMessageWireSize;
    if (size != expected) return false;
    if (decoded.kind == Kind::Renew) {
        decoded.state.effects = data[MessageHeaderWireSize];
        if (!Valid(decoded)) return false;
    } else if (decoded.kind == Kind::State) {
        std::vector<uint8_t> state(sizeof(Checkpoint), 0);
        std::memcpy(state.data(), data + MessageHeaderWireSize, 84);
        std::memcpy(state.data() + 212, data + MessageHeaderWireSize + 84, 21);
        detail::ReadCheckpoint(state.data(), 0, decoded.state);
        if (!ValidStateFields(decoded.state)) return false;
    } else {
        detail::ReadCheckpoint(data, MessageHeaderWireSize, decoded.state);
        if (!Valid(decoded)) return false;
    }
    output = decoded;
    return true;
}

inline bool EncodeOverlay(SaveOverlay const& overlay, std::vector<uint8_t>& output)
{
    if (overlay.size() > MaxLeases) return false;
    if (overlay.size() > (SIZE_MAX - 20) / detail::SavedWireSize) return false;

    for (size_t i = 0; i < overlay.size(); ++i) {
        const Saved& saved = overlay[i];
        if (!detail::GuidNonzero(saved.guid) || !saved.incarnation || !saved.epoch ||
            saved.adopted > 1 || !Valid(saved.state)) return false;
        for (size_t j = 0; j < i; ++j)
            if (std::memcmp(saved.guid, overlay[j].guid, 16) == 0) return false;
    }

    const size_t payloadSize = overlay.size() * detail::SavedWireSize;
    std::vector<uint8_t> encoded(20 + payloadSize);
    detail::Put32(encoded, 0, detail::OverlayMagic);
    detail::Put32(encoded, 4, detail::OverlayVersion);
    detail::Put32(encoded, 8, static_cast<uint32_t>(overlay.size()));
    detail::Put32(encoded, 12, static_cast<uint32_t>(payloadSize));
    detail::Put32(encoded, 16, 0);

    size_t offset = 20;
    for (Saved const& saved : overlay) {
        std::memcpy(encoded.data() + offset, saved.guid, 16); offset += 16;
        detail::Put32(encoded, offset, saved.incarnation); offset += 4;
        detail::Put32(encoded, offset, saved.epoch); offset += 4;
        detail::Put32(encoded, offset, saved.revision); offset += 4;
        offset = detail::WriteCheckpoint(encoded, offset, saved.state);
        encoded[offset++] = saved.adopted;
    }

    uint32_t combined = 0xffffffffu;
    for (size_t i = 0; i < 16; ++i) {
        combined ^= encoded[i];
        for (int bit = 0; bit < 8; ++bit)
            combined = (combined >> 1) ^ (0xedb88320u & (0u - (combined & 1u)));
    }
    for (size_t i = 20; i < encoded.size(); ++i) {
        combined ^= encoded[i];
        for (int bit = 0; bit < 8; ++bit)
            combined = (combined >> 1) ^ (0xedb88320u & (0u - (combined & 1u)));
    }
    combined ^= 0xffffffffu;
    detail::Put32(encoded, 16, combined);
    output.swap(encoded);
    return true;
}

inline bool DecodeOverlay(uint8_t const* data, size_t size, SaveOverlay& output)
{
    if (!data || size < 20) return false;
    if (detail::Get32(data) != detail::OverlayMagic) return false;
    const uint32_t version = detail::Get32(data + 4);
    if (version != detail::OverlayVersion && version != detail::LegacyOverlayVersion) return false;
    const uint32_t rowSize = version == detail::OverlayVersion ?
                             detail::SavedWireSize : detail::LegacySavedWireSize;

    const uint32_t count = detail::Get32(data + 8);
    const uint32_t payloadSize = detail::Get32(data + 12);
    if (count > MaxLeases) return false;
    if (payloadSize != uint64_t(count) * rowSize) return false;
    if (size != size_t(20) + size_t(payloadSize)) return false;

    uint32_t combined = 0xffffffffu;
    for (size_t i = 0; i < 16; ++i) {
        combined ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            combined = (combined >> 1) ^ (0xedb88320u & (0u - (combined & 1u)));
    }
    for (size_t i = 20; i < size; ++i) {
        combined ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            combined = (combined >> 1) ^ (0xedb88320u & (0u - (combined & 1u)));
    }
    combined ^= 0xffffffffu;
    if (combined != detail::Get32(data + 16)) return false;

    SaveOverlay decoded;
    decoded.reserve(count);
    size_t offset = 20;
    for (uint32_t i = 0; i < count; ++i) {
        Saved saved;
        std::memcpy(saved.guid, data + offset, 16); offset += 16;
        saved.incarnation = detail::Get32(data + offset); offset += 4;
        saved.epoch = detail::Get32(data + offset); offset += 4;
        saved.revision = detail::Get32(data + offset); offset += 4;
        offset = detail::ReadCheckpoint(data, offset, saved.state);
        if (version == detail::OverlayVersion) saved.adopted = data[offset++];
        if (!detail::GuidNonzero(saved.guid) || !saved.incarnation || !saved.epoch ||
            saved.adopted > 1 || !Valid(saved.state)) return false;
        for (Saved const& prior : decoded)
            if (std::memcmp(prior.guid, saved.guid, 16) == 0) return false;
        decoded.push_back(saved);
    }

    output.swap(decoded);
    return true;
}

} // namespace g3t::npclease
