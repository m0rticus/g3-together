// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t {

using GuidKey = std::array<std::uint8_t, 16>;

struct GuidHash {
    std::size_t operator()(GuidKey const& key) const noexcept
    {
        std::uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, key.data(), 8);
        std::memcpy(&hi, key.data() + 8, 8);
        std::uint64_t hash = 14695981039346656037ull;
        hash = (hash ^ lo) * 1099511628211ull;
        hash = (hash ^ hi) * 1099511628211ull;
        if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            return static_cast<std::size_t>(hash ^ (hash >> 32));
        } else {
            return static_cast<std::size_t>(hash);
        }
    }
};

inline GuidKey ToGuidKey(std::uint8_t const guid[16]) noexcept
{
    GuidKey key{};
    if (guid) std::memcpy(key.data(), guid, key.size());
    return key;
}

inline bool IsZeroGuid(GuidKey const& key) noexcept
{
    std::uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, key.data(), 8);
    std::memcpy(&hi, key.data() + 8, 8);
    return (lo | hi) == 0;
}

inline bool IsZeroGuid(std::uint8_t const guid[16]) noexcept
{
    if (!guid) return true;
    std::uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, guid, 8);
    std::memcpy(&hi, guid + 8, 8);
    return (lo | hi) == 0;
}

} // namespace g3t
