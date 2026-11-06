// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
constexpr uint16_t Protocol = 72;

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
inline std::string Slot(std::string const& save) { return Id(save) ? "G3T_" + save : std::string(); }
inline std::string RecordName(Key const& key, bool latest)
{
    return "g3t-character-" + key.world + "-" + std::to_string(key.player) + "-" +
           (latest ? "latest" : key.save) + ".txt";
}
inline uint64_t LegacyNativeOwner(std::string const& slot, uint64_t localAccount)
{
    return Prefix(slot, "G3T_host_") ? UINT64_MAX : localAccount;
}
inline bool WorldMatches(uint32_t hostEpoch, uint32_t admittedEpoch, uint32_t loadedEpoch,
                         bool sameCheckpoint, bool freshNewWorld)
{
    return hostEpoch && (admittedEpoch == hostEpoch || (loadedEpoch == hostEpoch && sameCheckpoint) || freshNewWorld);
}

enum class Restore { Exact, Latest, Legacy, NewHero, Wait };
inline Restore Select(bool exactExists, bool latestExists, bool legacyExists, bool cleanHero)
{
    // Existence, not validity: a damaged exact record must be refused, never bypassed.
    return exactExists ? Restore::Exact : latestExists ? Restore::Latest : legacyExists ? Restore::Legacy
         : cleanHero ? Restore::NewHero : Restore::Wait;
}

struct Barrier {
    enum Phase { Idle, Preparing, Writing, Committing };
    struct Member { uint32_t generation = 0; bool ready = false, stored = false; };
    std::array<Member, 8> members{};
    Phase phase = Idle;
    std::string save;
    uint32_t epoch = 0;

    bool Matches(std::string const& id, uint32_t worldEpoch) const
    {
        return phase != Idle && Id(id) && save == id && epoch == worldEpoch;
    }
    bool Ready(unsigned id, uint32_t generation, std::string const& checkpoint, uint32_t worldEpoch)
    {
        if (phase != Preparing || !Matches(checkpoint, worldEpoch) || id < 2 || id >= members.size() ||
            !generation || members[id].generation != generation) return false;
        members[id].ready = true;
        return true;
    }
    bool Stored(unsigned id, uint32_t generation, std::string const& checkpoint, uint32_t worldEpoch)
    {
        if (phase != Committing || !Matches(checkpoint, worldEpoch) || id < 2 || id >= members.size() ||
            !generation || members[id].generation != generation || !members[id].ready) return false;
        members[id].stored = true;
        return true;
    }
    bool AllReady() const
    {
        for (auto const& m : members) if (m.generation && !m.ready) return false;
        return true;
    }
    bool AllStored() const
    {
        for (auto const& m : members) if (m.generation && !m.stored) return false;
        return true;
    }
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
}
