// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g3t::lobby {
enum class Phase { Starting, Creating, Hosting, Searching, Connecting, WaitingCampaign, Joined, Failed, Refused };
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
};

State Snapshot();
void Format(State const& state, wchar_t* out, size_t size);
}
