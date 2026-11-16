// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::occupancy_policy {

struct InteractUserState {
    bool validObject = false;
    bool hasUser = false;
    bool userIsCharacter = false;
    bool userIsLocalHero = false;
    uint8_t puppetOwner = 0;
    uint8_t markedPlayer = 0;
    bool userSuppressed = false;
    bool userPassive = false;
    bool userDeadOrDown = false;
    bool userInactiveRoutine = false;
};

// A world interactable's Interaction.User is stale when it still points to a silenced/passive, dead/down,
// suppressed, or non-interacting NPC, or to a puppet that is not actively marked as occupying it.
inline bool ShouldClearInteractUser(InteractUserState const& s) noexcept
{
    if (!s.validObject || !s.hasUser || !s.userIsCharacter) return false;
    if (s.userIsLocalHero) return false;
    if (s.puppetOwner != 0) return s.markedPlayer != s.puppetOwner;
    return s.userSuppressed || s.userPassive || s.userDeadOrDown || s.userInactiveRoutine;
}

// Native NPC interaction scripts increment gCAnchor_PS::UserCount, whereas occupancy::OnState sets
// Interaction.User to a remote puppet without touching UserCount. Only decrement for non-puppet NPCs.
inline bool ShouldDecrementAnchorUserCount(InteractUserState const& s) noexcept
{
    return ShouldClearInteractUser(s) && s.puppetOwner == 0;
}

}  // namespace g3t::occupancy_policy
