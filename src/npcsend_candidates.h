// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

inline bool Eligible(Candidate const& candidate)
{
    if (candidate.null || candidate.key.size() != 16) return false;
    bool allZero = true;
    for (char byte : candidate.key) if (static_cast<unsigned char>(byte) != 0) { allZero = false; break; }
    return !allZero && candidate.npc &&
           !candidate.player && !candidate.controlled && !candidate.puppet;
}

inline std::vector<std::string> Union(std::vector<Candidate> const& candidates)
{
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (Candidate const& candidate : candidates) {
        if (!Eligible(candidate) || !seen.insert(candidate.key).second) continue;
        result.push_back(candidate.key);
    }
    return result;
}
}
