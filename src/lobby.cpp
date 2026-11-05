// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby.h"

#include "config.h"
#include "i18n.h"
#include "net.h"
#include "steam.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace g3t::lobby {
State Snapshot()
{
    State state;
    state.host = GetConfig().host;
    strcpy_s(state.sessionId, GetConfig().sessionId);
    state.id = steam::LobbyId();
    state.linkedPeers = static_cast<unsigned>(steam::LinkedCount());
    state.ping = steam::Ping();
    strncpy_s(state.name, state.host ? steam::PlayerName() : steam::HostName(), _TRUNCATE);
    if (*net::StartupError()) {
        state.phase = Phase::Failed;
        strncpy_s(state.reason, net::StartupError(), _TRUNCATE);
    } else if (!state.host && *net::RefusalReason()) {
        state.phase = Phase::Refused;
        strncpy_s(state.reason, net::RefusalReason(), _TRUNCATE);
    } else if (*steam::Error()) {
        state.phase = Phase::Failed;
        strncpy_s(state.reason, steam::Error(), _TRUNCATE);
    } else if (!steam::Ready()) {
        state.phase = Phase::Starting;
    } else if (state.host) {
        state.phase = state.id ? Phase::Hosting : Phase::Creating;
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            if (net::PeerJoined(id)) {
                ++state.clients;
                state.readyClients += net::InWorld(id) ? 1 : 0;
            }
        }
    } else if (!state.id) {
        state.phase = Phase::Searching;
    } else if (!net::Joined()) {
        state.phase = Phase::Connecting;
    } else {
        state.phase = net::Connected() ? Phase::Joined : Phase::WaitingCampaign;
    }
    return state;
}

void Format(State const& state, wchar_t* out, size_t size)
{
    wchar_t name[64] = {}, reason[256] = {}, ping[32] = {};
    MultiByteToWideChar(CP_UTF8, 0, state.name, -1, name, 64);
    MultiByteToWideChar(CP_UTF8, 0, state.reason, -1, reason, 256);
    if (state.ping < 0) wcscpy_s(ping, i18n::Text(i18n::LobbyPingPending));
    else _snwprintf_s(ping, _TRUNCATE, L"%d ms", state.ping);
    switch (state.phase) {
    case Phase::Starting:
        _snwprintf_s(out, size, _TRUNCATE, L"%s", i18n::Text(i18n::LobbyStarting));
        break;
    case Phase::Creating:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyCreating), name);
        break;
    case Phase::Hosting:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyHosting),
                     name, state.id, state.clients, state.readyClients, state.linkedPeers, ping);
        break;
    case Phase::Searching:
        _snwprintf_s(out, size, _TRUNCATE, L"%s", i18n::Text(i18n::LobbySearching));
        break;
    case Phase::Connecting:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyConnecting), state.id, name, ping);
        break;
    case Phase::WaitingCampaign:
    case Phase::Joined:
        _snwprintf_s(out, size, _TRUNCATE,
                     i18n::Text(state.phase == Phase::Joined ? i18n::LobbyJoined : i18n::LobbyWaiting),
                     name, state.id, ping);
        break;
    case Phase::Failed:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyError), reason);
        break;
    case Phase::Refused:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::RefusedOther), reason);
        break;
    }
    wchar_t session[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, state.sessionId, -1, session, 64);
    const size_t used = std::wcslen(out);
    if (used < size) _snwprintf_s(out + used, size - used, _TRUNCATE, i18n::Text(i18n::LobbySession), session);
}
}
