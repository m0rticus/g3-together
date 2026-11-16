// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy decisions for host NPC approach lifecycle and client address requests.
#pragma once

#include <cstdint>

namespace g3t::address_policy {

enum class HostApproachAction : uint8_t {
    Continue,
    StartedRemoteTalk,
    CancelKeepState,
    GiveUpAndRestore,
    EraseOnly,
};

struct HostNpcState {
    bool entityExists = false;
    bool isDead = false;
    bool isDown = false;
    bool isMirrored = false;
    bool hasNavigation = false;
    bool heldForRemoteTalk = false;
    bool inHostDialog = false;
    int aiMode = 0;
    bool puppetExists = false;
    bool tooFar = false;
};

constexpr HostApproachAction DecideHostApproach(HostNpcState const& s, bool expired) noexcept
{
    if (!s.entityExists || s.isDead) return HostApproachAction::EraseOnly;
    if (s.heldForRemoteTalk) return HostApproachAction::StartedRemoteTalk;
    if (s.inHostDialog || s.isDown || s.isMirrored || !s.hasNavigation || s.aiMode != 0)
        return HostApproachAction::CancelKeepState;
    if (!s.puppetExists || expired || s.tooFar)
        return HostApproachAction::GiveUpAndRestore;
    return HostApproachAction::Continue;
}

constexpr char const* RejectHostAddressRequest(HostNpcState const& s, bool alreadyApproaching) noexcept
{
    if (!s.entityExists || s.isDead || s.isDown) return "gone";
    if (!s.puppetExists) return "no puppet";
    if (s.isMirrored) return "not simulated here";
    if (s.heldForRemoteTalk || s.inHostDialog) return "in a dialog";
    if (s.aiMode != 0) return "not in its routine";
    if (!s.hasNavigation) return "no navigation";
    if (s.tooFar) return "too far";
    if (alreadyApproaching) return "already on its way";
    return nullptr;
}

struct ClientWaitState {
    bool entityExists = false;
    bool isDead = false;
    bool isDown = false;
    bool expired = false;
};

constexpr bool ShouldCancelClientWait(ClientWaitState const& s) noexcept
{
    return !s.entityExists || s.isDead || s.isDown || s.expired;
}

} // namespace g3t::address_policy
