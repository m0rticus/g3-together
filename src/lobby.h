// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "steam_connect.h"

namespace g3t::lobby {
enum class Phase {
    Starting, Creating, Hosting, Searching, Connecting, WaitingCampaign, Joined, Failed, Refused,
    InviteWait, PartnerOpening, PartnerOffered, WaitingInvite, LobbyInvited, Requesting, Declined, Lost
};
struct State
{
    Phase phase = Phase::Starting;
    bool host = false;
    uint64_t id = 0;
    unsigned clients = 0, readyClients = 0, linkedPeers = 0;
    int ping = -1;
    char name[64] = {};
    char sessionId[64] = "default";
    char reason[256] = {};
    steam::connect::Why why = steam::connect::Why::None;
    uint64_t arg = 0;
    char hostSession[64] = {};
    bool endedImmediately = false;
};

State Snapshot();
void Format(State const& state, wchar_t* out, size_t size);
}
