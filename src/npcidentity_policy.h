// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace g3t::npcidentity {
using Guid = std::array<uint8_t, 16>;

inline bool Valid(Guid const& guid)
{
    return std::any_of(guid.begin(), guid.end(), [](uint8_t byte) { return byte != 0; });
}

struct Candidate {
    uintptr_t instance = 0;
    Guid guid{}, templateId{};
    std::string name;
    bool protectedRole = true;
};

inline bool AddPointer(std::unordered_set<uintptr_t>& seen, uintptr_t instance)
{
    return instance && seen.insert(instance).second;
}

inline bool SameIdentity(Candidate const& a, Candidate const& b)
{
    return Valid(a.guid) && a.guid == b.guid && Valid(a.templateId) &&
        a.templateId == b.templateId && !a.name.empty() && a.name == b.name;
}

inline bool Quarantine(Candidate const& extra, Candidate const& keeper, uintptr_t bound)
{
    return extra.instance && keeper.instance && extra.instance != keeper.instance &&
        (!bound || bound == keeper.instance) && !extra.protectedRole && !keeper.protectedRole &&
        SameIdentity(extra, keeper);
}
}
