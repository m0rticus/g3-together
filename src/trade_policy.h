// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_item_quality_policy.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace g3t::trade_policy {

inline constexpr char const* GoldTemplateName = "It_Gold";
inline constexpr int32_t MaxStackAmount = 1000000;
inline constexpr int32_t MaxGoldAmount = 10000000;
inline constexpr uint16_t QualityMask = static_cast<uint16_t>(npc_item_quality::QualityMask);
inline constexpr uint16_t MaxQuality = QualityMask + 1u;

inline std::string StackKeyBytes(void const* guid16, uint16_t quality)
{
    std::string key(static_cast<char const*>(guid16), 16);
    key.append(reinterpret_cast<char const*>(&quality), sizeof(quality));
    return key;
}

inline uint16_t StackQuality(std::string const& key)
{
    uint16_t value = 0;
    if (key.size() >= 18) std::memcpy(&value, key.data() + 16, sizeof(value));
    return value;
}

// Parses an optional "-q<digits>" suffix from `itemName`.
// If no "-q" suffix is present, leaves `itemName` unchanged, sets `qualityOut = -1`, and returns true.
// If "-q" is present, requires a non-empty decimal integer in [0, MaxQuality - 1].
inline bool ParseQualitySuffix(std::string& itemName, int& qualityOut)
{
    qualityOut = -1;
    const size_t q = itemName.rfind("-q");
    if (q == std::string::npos) return true;
    char const* digits = itemName.c_str() + q + 2;
    if (!*digits || !std::isdigit(static_cast<unsigned char>(*digits))) return false;
    char* end = nullptr;
    const long parsed = std::strtol(digits, &end, 10);
    if (end == digits || *end != '\0' || parsed < 0 || parsed >= MaxQuality) return false;
    qualityOut = static_cast<int>(parsed);
    itemName.resize(q);
    return true;
}

// Parses "puppet<id>" with a positive integer id in [1, maxPlayers - 1].
inline bool ParsePuppetId(char const* text, int maxPlayers, int& idOut)
{
    idOut = 0;
    if (!text || std::strncmp(text, "puppet", 6) != 0) return false;
    char const* digits = text + 6;
    if (!*digits || !std::isdigit(static_cast<unsigned char>(*digits))) return false;
    char* end = nullptr;
    const long parsed = std::strtol(digits, &end, 10);
    if (end == digits || *end != '\0' || parsed <= 0 || parsed >= maxPlayers) return false;
    idOut = static_cast<int>(parsed);
    return true;
}

} // namespace g3t::trade_policy
