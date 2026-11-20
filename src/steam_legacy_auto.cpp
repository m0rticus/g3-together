// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
// Legacy opt-in `connection = auto` lobby creation, skip list and friend-list scan; default co-op is invite-driven.
#include "steam_runtime.h"

#include "config.h"
#include "log.h"
#include "player_identity.h"

#include <cstring>

namespace g3t::steam {
void WorkerRuntime::Skip(u64 id, uint32_t forMs)
{
    Skipped* slot = &skipped[0];
    for (auto& s : skipped) {
        if (s.lobby == id) { slot = &s; break; }
        if (!s.lobby || (s.until && static_cast<int32_t>(s.until - slot->until) < 0)) slot = &s;
    }
    slot->lobby = id;
    slot->until = forMs ? (Now() + forMs) | 1 : 0;
}

bool WorkerRuntime::IsSkipped(u64 id) const
{
    for (auto const& s : skipped)
        if (s.lobby == id) return !s.until || static_cast<int32_t>(s.until - Now()) > 0;
    return false;
}

void WorkerRuntime::CreateLobbyAuto()
{
    if (!CreateLobby()) {
        strcpy_s(error, "Steam rejected the lobby creation request");
        Logf("steam: %s", error);
        return;
    }
    Log("steam: creating a friends-only lobby");
}

void WorkerRuntime::Search()
{
    if (candidateLobby) return;
    if (lobby) {
        if (!owner && Now() - joinedAt > JoinWaitMs) {
            Logf("steam: lobby %llu did not answer the join; trying others", lobby);
            RecordClientEnd("Steam lobby did not answer JoinLobby", clientFlow.state());
            Skip(lobby, 30000);
            api.LeaveLobby(matchmaking, lobby);
            lobby = 0;
        }
        return;
    }
    if (pendingLobby) {
        const u64 id = pendingLobby;
        pendingLobby = 0;
        InspectLobby(id, "accepted through Steam");
        return;
    }
    if (Now() - lastScan < ScanMs) return;
    lastScan = Now();
    if (wantedLobby) {
        if (!IsSkipped(wantedLobby)) InspectLobby(wantedLobby, "steam_lobby");
        else if (!wantedWaitLogged) {
            wantedWaitLogged = true;
            Logf("steam: steam_lobby %llu refused or did not answer; waiting for it (no other host)", wantedLobby);
        }
        return;
    }
    char const* wantedHost = GetLocalConfig().steamHost;
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        const u64 id = api.FriendByIndex(friends, i, FriendFlagImmediate);
        FriendGameInfoParam info{};
        if (!api.FriendGamePlayed(friends, id, &info) || (info.game & 0xffffff) != GothicAppId || !info.lobby) continue;
        if (IsSkipped(info.lobby)) continue;
        char const* name = Text(api.FriendPersonaName(friends, id));
        uint64_t wantedId = 0;
        if (*wantedHost && (identity::SteamId(wantedHost, wantedId) ? id != wantedId : _stricmp(name, wantedHost) != 0)) continue;
        error[0] = '\0';
        InspectLobby(info.lobby, *name ? name : "a friend");
        return;
    }
    if (!searchLogged) {
        Logf("steam: no friend hosting a G3 Together game yet%s%s; looking every %u s",
             *wantedHost ? " named " : "", wantedHost, ScanMs / 1000);
        searchLogged = true;
    }
}
}
