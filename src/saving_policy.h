// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_binary.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace g3t::saving {
constexpr uint16_t Protocol = 79;

inline constexpr uint8_t HeroMagic[8] = {'G', '3', 'T', 'H', 'E', 'R', 'O', '\0'};
inline constexpr uint32_t HeroSchemaVersion = 5;
inline constexpr uint32_t HeroMinReaderVersion = 5;
inline constexpr size_t HeroHeaderSize = 48;
inline constexpr size_t HeroSectionHeaderSize = 16;
inline constexpr uint32_t HeroSectionRequired = 0;
inline constexpr uint32_t HeroSectionOptional = 1;
inline constexpr uint32_t HeroSecIdentity = 1;
inline constexpr uint32_t HeroSecPose = 2;
inline constexpr uint32_t HeroSecState = 3;
inline constexpr uint32_t HeroSecAttributes = 4;
inline constexpr uint32_t HeroSecInventory = 5;
inline constexpr uint32_t HeroSecSpells = 6;
inline constexpr uint32_t HeroSecQuickSlots = 7;
inline constexpr uint32_t HeroSecExtension = 256;
inline constexpr size_t HeroMaxRecordBytes = 4 * 1024 * 1024;
inline constexpr size_t HeroMaxExtraSections = 64;
inline constexpr size_t HeroRecordCrcOffset = 24;
inline constexpr size_t HeroHeaderCrcOffset = 44;
inline constexpr size_t HeroSectionCrcOffset = 12;
inline constexpr size_t HeroIdentityPayloadBytes = 72;
inline constexpr size_t HeroIdentitySaveOffset = 32;
inline constexpr size_t HeroIdentityPlayerOffset = 64;
inline constexpr size_t HeroPosePayloadBytes = 28;
inline constexpr uint32_t HeroAllCoreSectionsMask =
    (1u << HeroSecIdentity) | (1u << HeroSecPose) | (1u << HeroSecState) | (1u << HeroSecAttributes) |
    (1u << HeroSecInventory) | (1u << HeroSecSpells) | (1u << HeroSecQuickSlots);

inline bool Id(std::string const& value)
{
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

inline bool Prefix(std::string const& value, char const* prefix)
{
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= value.size()) return false;
        const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
        if (upper(value[i]) != upper(prefix[i])) return false;
    }
    return true;
}

inline bool SafeName(char const* name)
{
    if (!name) return false;
    const size_t n = std::strlen(name);
    if (!n || n > 48 || name[n - 1] == ' ') return false;
    std::string upper;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == ' ')) return false;
        upper += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return false;
    return !(upper.size() == 4 && (upper.substr(0, 3) == "COM" || upper.substr(0, 3) == "LPT") &&
             upper[3] >= '0' && upper[3] <= '9');
}

struct Key {
    std::string world, save;
    uint64_t player = 0;
};

inline bool Valid(Key const& key) { return Id(key.world) && Id(key.save) && key.player != 0; }
inline bool SameCharacter(Key const& a, Key const& b)
{
    return Valid(a) && Valid(b) && a.world == b.world && a.player == b.player;
}
inline bool Exact(Key const& a, Key const& b) { return SameCharacter(a, b) && a.save == b.save; }
inline char const* NativeSaveRefusal(bool dialog, bool controlled, bool dead, bool defeated)
{
    return dialog ? "dialog" : controlled ? "control" : dead ? "dead" : defeated ? "defeated" : nullptr;
}

struct CaptureRound {
    struct Member {
        uint32_t generation = 0, nextTry = 0;
        uint64_t account = 0;
        bool ready = false;
    };
    bool active = false;
    uint32_t epoch = 0, started = 0;
    std::array<Member, 8> members{};
    bool Accept(unsigned peer, uint32_t generation, uint64_t account, uint32_t worldEpoch)
    {
        if (!active || peer < 2 || peer >= members.size() || epoch != worldEpoch ||
            !generation || !account || members[peer].generation != generation || members[peer].account != account) return false;
        members[peer].ready = true;
        return true;
    }
    bool AllReady() const
    {
        if (!active) return false;
        for (auto const& m : members) if (m.generation && !m.ready) return false;
        return true;
    }
    bool Expired(uint32_t now) const { return active && now - started > 10000; }
};

struct RecordPose {
    float position[3] = {};
    float rotation[4] = {};
};

inline bool ValidRecordPose(RecordPose const& pose)
{
    for (float v : pose.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    float length = 0.0f;
    for (float v : pose.rotation) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
        length += v * v;
    }
    return length >= 0.99f && length <= 1.01f;
}

// Engine MoveTo snaps character capsules to ground/geometry and strips non-yaw tilt (pitch/roll).
// Allow bounded capsule/floor settlement and verify yaw alignment (with 3D fallback near vertical pitch).
inline bool PoseRestored(RecordPose const& target, RecordPose const& actual)
{
    for (float v : target.position) if (!std::isfinite(v)) return false;
    for (float v : target.rotation) if (!std::isfinite(v)) return false;
    for (float v : actual.position) if (!std::isfinite(v)) return false;
    for (float v : actual.rotation) if (!std::isfinite(v)) return false;
    const float qLen2 = actual.rotation[0] * actual.rotation[0] + actual.rotation[1] * actual.rotation[1] +
                        actual.rotation[2] * actual.rotation[2] + actual.rotation[3] * actual.rotation[3];
    if (qLen2 < 1e-6f) return false;

    const float dx = actual.position[0] - target.position[0];
    const float dy = actual.position[1] - target.position[1];
    const float dz = actual.position[2] - target.position[2];
    if (dx * dx + dz * dz > 200.0f * 200.0f || std::fabs(dy) > 300.0f) return false;

    auto horizForward = [](float const q[4], float& fx, float& fz) {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        fx = 2.0f * (x * z + w * y);
        fz = 1.0f - 2.0f * (x * x + y * y);
        return fx * fx + fz * fz;
    };
    float tx = 0.0f, tz = 0.0f, ax = 0.0f, az = 0.0f;
    const float tLen2 = horizForward(target.rotation, tx, tz);
    const float aLen2 = horizForward(actual.rotation, ax, az);
    if (tLen2 >= 1e-4f && aLen2 >= 1e-4f) {
        const float yawCos = (tx * ax + tz * az) / std::sqrt(tLen2 * aLen2);
        return std::isfinite(yawCos) && yawCos >= 0.95f;
    }
    const float dot = actual.rotation[0] * target.rotation[0] + actual.rotation[1] * target.rotation[1] +
                      actual.rotation[2] * target.rotation[2] + actual.rotation[3] * target.rotation[3];
    return std::isfinite(dot) && std::fabs(dot) >= 0.90f;
}

namespace detail {
inline std::vector<std::string> RecordWords(std::string const& line)
{
    std::istringstream input(line);
    std::vector<std::string> words;
    std::string word;
    while (input >> word) words.push_back(word);
    return words;
}
}

inline bool ParseRecordDowned(std::string const& line, int version, bool alreadyPresent, int& downed)
{
    if (version < 3 || version > 4 || alreadyPresent || line.size() > 511 ||
        line.find('\0') != std::string::npos) return false;
    const auto words = detail::RecordWords(line);
    if (words.size() != 2 || words[0] != "downed") return false;
    int value = 0;
    const auto parsed = std::from_chars(words[1].data(), words[1].data() + words[1].size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != words[1].data() + words[1].size() ||
        (value != 0 && value != 1)) return false;
    downed = value;
    return true;
}

// Each argument is one complete line, without its line terminator. On failure neither output is changed.
inline bool ParseRecordMetadata(std::string const& header, std::string const& identity, std::string const& poseLine,
                                Key const* expected, Key& key, RecordPose& pose)
{
    if (header != "G3THERO 4" || identity.size() > 511 || poseLine.size() > 511 ||
        identity.find('\0') != std::string::npos || poseLine.find('\0') != std::string::npos) return false;
    auto const identityWords = detail::RecordWords(identity);
    Key candidate;
    if (identityWords.size() != 4 || identityWords[0] != "identity") return false;
    candidate.world = identityWords[1];
    candidate.save = identityWords[2];
    std::string const& player = identityWords[3];
    const auto parsed = std::from_chars(player.data(), player.data() + player.size(), candidate.player);
    if (parsed.ec != std::errc() || parsed.ptr != player.data() + player.size() ||
        !Valid(candidate) || (expected && !Exact(candidate, *expected))) return false;
    auto const poseWords = detail::RecordWords(poseLine);
    if (poseWords.size() != 8 || poseWords[0] != "pose") return false;
    RecordPose candidatePose;
    for (int i = 0; i < 7; ++i) {
        std::string const& text = poseWords[i + 1];
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        if (end == text.c_str() || end != text.c_str() + text.size() || !std::isfinite(value)) return false;
        if (i < 3) candidatePose.position[i] = value;
        else candidatePose.rotation[i - 3] = value;
    }
    if (!ValidRecordPose(candidatePose)) return false;
    key = candidate;
    pose = candidatePose;
    return true;
}

struct RecordAttribute {
    std::string tag;
    int32_t base = 0, modifier = 0, baseMaximum = 0, maximumModifier = 0;
};

inline bool StrictLegacyReady(bool restoring, bool checking, bool spells, bool quick, bool skills)
{
    return restoring && checking && spells && quick && skills;
}

inline bool StatTag(std::string const& tag) { return tag == "HP" || tag == "MP" || tag == "SP"; }

inline bool ValidRecordAttribute(RecordAttribute const& attr)
{
    const bool stat = StatTag(attr.tag);
    if (!stat && attr.tag != "STR" && attr.tag != "DEX" && attr.tag != "INT" &&
        attr.tag != "ALC" && attr.tag != "SMT" && attr.tag != "THF") return false;
    auto inRange = [](int64_t value) { return value >= -1000000 && value <= 1000000; };
    if (!inRange(attr.base) || !inRange(attr.modifier) || !inRange(int64_t(attr.base) + attr.modifier)) return false;
    if (!stat) return attr.baseMaximum == 0 && attr.maximumModifier == 0;
    return inRange(attr.baseMaximum) && inRange(attr.maximumModifier) &&
           inRange(int64_t(attr.baseMaximum) + attr.maximumModifier);
}

inline bool ParseRecordAttribute(std::string const& line, RecordAttribute& attr)
{
    if (line.size() > 511 || line.find('\0') != std::string::npos) return false;
    auto const words = detail::RecordWords(line);
    if ((words.size() != 4 && words.size() != 6) || words[0] != "attr") return false;
    RecordAttribute candidate;
    candidate.tag = words[1];
    if ((words.size() == 6) != StatTag(candidate.tag)) return false;
    auto integer = [](std::string const& text, int32_t& value) {
        auto const parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    };
    if (!integer(words[2], candidate.base) || !integer(words[3], candidate.modifier) ||
        (words.size() == 6 && (!integer(words[4], candidate.baseMaximum) || !integer(words[5], candidate.maximumModifier))) ||
        !ValidRecordAttribute(candidate)) return false;
    attr = candidate;
    return true;
}

// Old records contain effective maximum but not its component modifiers.
inline bool LegacyRecordAttribute(std::string const& tag, int32_t base, int32_t maximum, RecordAttribute& attr)
{
    RecordAttribute candidate;
    candidate.tag = tag;
    candidate.base = base;
    if (StatTag(tag)) candidate.baseMaximum = maximum;
    else {
        const int64_t modifier = int64_t(maximum) - base;
        if (modifier < -1000000 || modifier > 1000000) return false;
        candidate.modifier = static_cast<int32_t>(modifier);
    }
    if (!ValidRecordAttribute(candidate)) return false;
    attr = candidate;
    return true;
}

inline bool IsHeroBinaryMagic(uint8_t const* data, size_t size)
{
    return data && size >= sizeof(HeroMagic) && std::memcmp(data, HeroMagic, sizeof(HeroMagic)) == 0;
}

struct HeroBinarySectionSpan {
    uint32_t type = 0;
    uint32_t flags = 0;
    size_t headerOffset = 0;
    size_t payloadOffset = 0;
    size_t size = 0;
};

struct HeroBinaryEnvelope {
    uint32_t schemaVersion = 0;
    uint32_t minReaderVersion = 0;
    Key key;
    RecordPose pose;
    size_t identityHeaderOffset = 0;
    size_t identityPayloadOffset = 0;
    std::vector<HeroBinarySectionSpan> sections;
};

inline char const* ParseHeroBinaryEnvelope(uint8_t const* data, size_t size, HeroBinaryEnvelope& out)
{
    if (!IsHeroBinaryMagic(data, size) || size < HeroHeaderSize || size > HeroMaxRecordBytes)
        return "damaged or truncated binary hero record";
    size_t at = sizeof(HeroMagic);
    uint64_t schema = 0, minReader = 0, headerSize = 0, payloadLength = 0, recCrc = 0, flags = 0, sectionCount = 0;
    uint64_t reserved = 0, headerCrc = 0;
    if (!GetLe(data, size, at, schema, 4) || !GetLe(data, size, at, minReader, 4) ||
        !GetLe(data, size, at, headerSize, 4) || !GetLe(data, size, at, payloadLength, 4) ||
        !GetLe(data, size, at, recCrc, 4) || !GetLe(data, size, at, flags, 4) ||
        !GetLe(data, size, at, sectionCount, 4) || !GetLe(data, size, at, reserved, 8) ||
        !GetLe(data, size, at, headerCrc, 4))
        return "damaged or truncated binary hero record";
    if (schema < HeroSchemaVersion || minReader == 0 || minReader > HeroSchemaVersion || minReader > schema)
        return "hero record requires a newer reader version";
    if (headerSize < HeroHeaderSize || headerSize > size || payloadLength != size - headerSize ||
        sectionCount > HeroMaxExtraSections + 8)
        return "damaged or truncated binary hero record";
    if (headerCrc != Crc32Zeroed(data, HeroHeaderCrcOffset, HeroRecordCrcOffset, 4))
        return "hero record header checksum mismatch";
    if (recCrc != Crc32Zeroed(data, size, HeroRecordCrcOffset, 4))
        return "hero record checksum mismatch";

    HeroBinaryEnvelope parsed;
    parsed.schemaVersion = static_cast<uint32_t>(schema);
    parsed.minReaderVersion = static_cast<uint32_t>(minReader);
    parsed.sections.reserve(static_cast<size_t>(sectionCount));

    at = static_cast<size_t>(headerSize);
    uint32_t seenCoreMask = 0;
    std::vector<uint32_t> seenExtraTypes;
    for (uint64_t i = 0; i < sectionCount; ++i) {
        const size_t secHeader = at;
        uint64_t type = 0, secFlags = 0, secSize = 0, secCrc = 0;
        if (!GetLe(data, size, at, type, 4) || !GetLe(data, size, at, secFlags, 4) ||
            !GetLe(data, size, at, secSize, 4) || !GetLe(data, size, at, secCrc, 4) ||
            secSize > size - at)
            return "damaged or truncated binary hero section";
        const size_t payloadBytes = static_cast<size_t>(secSize);
        if (secCrc != Crc32(data + at, payloadBytes))
            return "hero section checksum mismatch";
        if (type >= HeroSecIdentity && type <= HeroSecQuickSlots) {
            const uint32_t bit = 1u << static_cast<unsigned>(type);
            if (seenCoreMask & bit) return "duplicate hero record section";
            seenCoreMask |= bit;
        } else if ((secFlags & HeroSectionOptional) != 0) {
            for (uint32_t seen : seenExtraTypes) {
                if (seen == static_cast<uint32_t>(type)) return "too many or duplicate optional hero sections";
            }
            if (seenExtraTypes.size() >= HeroMaxExtraSections) return "too many or duplicate optional hero sections";
            seenExtraTypes.push_back(static_cast<uint32_t>(type));
        } else {
            return "unsupported required hero section";
        }
        if (type == HeroSecIdentity) {
            if (payloadBytes != HeroIdentityPayloadBytes) return "invalid or mismatched identity or pose";
            parsed.key.world.assign(reinterpret_cast<char const*>(data + at), HeroIdentitySaveOffset);
            parsed.key.save.assign(reinterpret_cast<char const*>(data + at + HeroIdentitySaveOffset),
                                   HeroIdentitySaveOffset);
            size_t playerAt = at + HeroIdentityPlayerOffset;
            if (!GetLe(data, size, playerAt, parsed.key.player, 8) || !Valid(parsed.key))
                return "invalid or mismatched identity or pose";
            parsed.identityPayloadOffset = at;
            parsed.identityHeaderOffset = secHeader;
        } else if (type == HeroSecPose) {
            if (payloadBytes != HeroPosePayloadBytes) return "invalid or mismatched identity or pose";
            size_t poseAt = at;
            for (int j = 0; j < 3; ++j)
                if (!GetF32(data, size, poseAt, parsed.pose.position[j])) return "invalid or mismatched identity or pose";
            for (int j = 0; j < 4; ++j)
                if (!GetF32(data, size, poseAt, parsed.pose.rotation[j])) return "invalid or mismatched identity or pose";
            if (!ValidRecordPose(parsed.pose)) return "invalid or mismatched identity or pose";
        }
        parsed.sections.push_back({static_cast<uint32_t>(type), static_cast<uint32_t>(secFlags), secHeader, at,
                                   payloadBytes});
        at += payloadBytes;
    }
    if (at != size) return "trailing data in binary hero record";
    if ((seenCoreMask & HeroAllCoreSectionsMask) != HeroAllCoreSectionsMask) return "missing required hero section";
    out = std::move(parsed);
    return nullptr;
}

inline bool InspectHeroBinary(uint8_t const* data, size_t size, Key& key, uint32_t* outSchemaVersion = nullptr,
                              size_t* outIdentityPayloadOffset = nullptr, size_t* outIdentityHeaderOffset = nullptr,
                              RecordPose* outPose = nullptr)
{
    HeroBinaryEnvelope env;
    if (ParseHeroBinaryEnvelope(data, size, env) != nullptr) return false;
    key = std::move(env.key);
    if (outSchemaVersion) *outSchemaVersion = env.schemaVersion;
    if (outIdentityPayloadOffset) *outIdentityPayloadOffset = env.identityPayloadOffset;
    if (outIdentityHeaderOffset) *outIdentityHeaderOffset = env.identityHeaderOffset;
    if (outPose) *outPose = env.pose;
    return true;
}

inline bool RebindHeroBinary(std::vector<uint8_t>& bytes, Key const& key)
{
    Key previous;
    size_t idPayload = 0, idHeader = 0;
    if (!Valid(key) || !InspectHeroBinary(bytes.data(), bytes.size(), previous, nullptr, &idPayload, &idHeader) ||
        !SameCharacter(previous, key)) return false;
    std::memcpy(bytes.data() + idPayload + HeroIdentitySaveOffset, key.save.data(), HeroIdentitySaveOffset);
    const uint32_t secCrc = Crc32(bytes.data() + idPayload, HeroIdentityPayloadBytes);
    PutLeAt(bytes.data() + idHeader + HeroSectionCrcOffset, secCrc, 4);
    const uint32_t recCrc = Crc32Zeroed(bytes.data(), bytes.size(), HeroRecordCrcOffset, 4);
    PutLeAt(bytes.data() + HeroRecordCrcOffset, recCrc, 4);
    return true;
}
}
