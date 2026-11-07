// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "downed_policy.h"

#include <cmath>
#include <cstddef>

namespace g3t::retarget {
inline bool EligibleTarget(bool npc, bool self, bool unavailablePlayer, int mode)
{
    return npc && !self && !unavailablePlayer && mode != 6 && mode != 8 && mode != 9;
}

inline bool EnemyAttitude(int attitude) { return attitude == 4 || attitude == 5; }

inline bool VisibleFallback(float distanceCm)
{
    return std::isfinite(distanceCm) && distanceCm >= 0 && distanceCm < 2000;
}

enum class Source { Unchanged, Current, Native, Alternative, Player, None };

struct PlayerCandidate {
    bool eligible = false;
    float distanceCm = 0;
};

struct Selection {
    Source source = Source::None;
    size_t player = 0;
};

inline Selection Select(uint32_t bindings, bool current, bool native, bool alternative,
                        PlayerCandidate const* players, size_t count)
{
    if (!downed::StopTargetInstruction(bindings)) return {Source::Unchanged};
    if (!(bindings & downed::Current) && current) return {Source::Current};
    if (native) return {Source::Native};
    if (alternative) return {Source::Alternative};
    Selection best;
    float distance = 2000;
    for (size_t i = 0; i < count; ++i) {
        if (players[i].eligible && VisibleFallback(players[i].distanceCm) && players[i].distanceCm < distance) {
            best = {Source::Player, i};
            distance = players[i].distanceCm;
        }
    }
    return best;
}

inline char const* Reason(Source source)
{
    switch (source) {
    case Source::Unchanged: return "history-only";
    case Source::Current: return "keep-current";
    case Source::Native: return "native-enemy";
    case Source::Alternative: return "alternative";
    case Source::Player: return "visible-player";
    case Source::None: return "no-enemy";
    }
    return "invalid-selection";
}
}
