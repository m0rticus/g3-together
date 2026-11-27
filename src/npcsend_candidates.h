// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "guid_key.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace g3t::npcsend_candidates {
struct Candidate {
    std::string key;
    bool npc = false;
    bool player = false;
    bool controlled = false;
    bool puppet = false;
    bool null = false;
};

struct GuidCandidate {
    GuidKey key{};
    bool npc = false;
    bool player = false;
    bool controlled = false;
    bool puppet = false;
    bool null = false;
};

inline bool Eligible(Candidate const& candidate)
{
    if (candidate.null || candidate.key.size() != 16) return false;
    bool allZero = true;
    for (char byte : candidate.key) if (static_cast<unsigned char>(byte) != 0) { allZero = false; break; }
    return !allZero && candidate.npc &&
           !candidate.player && !candidate.controlled && !candidate.puppet;
}

inline bool Eligible(GuidCandidate const& candidate) noexcept
{
    return !candidate.null && !IsZeroGuid(candidate.key) && candidate.npc &&
           !candidate.player && !candidate.controlled && !candidate.puppet;
}

namespace detail {
template <typename CandidateT, typename KeyT, typename HashT = std::hash<KeyT>>
std::vector<KeyT> UnionImpl(std::vector<CandidateT> const& candidates)
{
    std::vector<KeyT> result;
    result.reserve(candidates.size());
    std::unordered_set<KeyT, HashT> seen;
    seen.reserve(candidates.size());
    for (CandidateT const& candidate : candidates) {
        if (!Eligible(candidate) || !seen.insert(candidate.key).second) continue;
        result.push_back(candidate.key);
    }
    return result;
}
} // namespace detail

inline std::vector<std::string> Union(std::vector<Candidate> const& candidates)
{
    return detail::UnionImpl<Candidate, std::string>(candidates);
}

inline std::vector<GuidKey> Union(std::vector<GuidCandidate> const& candidates)
{
    std::vector<GuidKey> result;
    if (candidates.empty()) return result;
    result.reserve(candidates.size());
    constexpr size_t StackSlots = 1024;
    uint16_t stackSlots[StackSlots];
    std::vector<uint32_t> heapSlots;
    uint16_t* smallSlots = nullptr;
    uint32_t* largeSlots = nullptr;
    size_t mask = 0;
    if (candidates.size() <= StackSlots / 2) {
        std::memset(stackSlots, 0, sizeof(stackSlots));
        smallSlots = stackSlots;
        mask = StackSlots - 1;
    } else {
        size_t cap = 2048;
        while (cap < candidates.size() * 2) cap <<= 1;
        heapSlots.assign(cap, 0u);
        largeSlots = heapSlots.data();
        mask = cap - 1;
    }
    const GuidHash hasher{};
    for (GuidCandidate const& candidate : candidates) {
        if (!Eligible(candidate)) continue;
        size_t slot = hasher(candidate.key) & mask;
        while (true) {
            const uint32_t idx = smallSlots ? static_cast<uint32_t>(smallSlots[slot]) : largeSlots[slot];
            if (!idx) {
                result.push_back(candidate.key);
                const uint32_t oneBased = static_cast<uint32_t>(result.size());
                if (smallSlots) smallSlots[slot] = static_cast<uint16_t>(oneBased);
                else largeSlots[slot] = oneBased;
                break;
            }
            if (result[idx - 1] == candidate.key) break;
            slot = (slot + 1) & mask;
        }
    }
    return result;
}
}
