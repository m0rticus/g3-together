// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "guid_key.h"

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
    return detail::UnionImpl<GuidCandidate, GuidKey, GuidHash>(candidates);
}
}
