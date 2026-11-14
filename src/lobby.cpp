// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby.h"

#include "config.h"
#include "i18n.h"
#include "net.h"
#include "saving_policy.h"
#include "steam.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace g3t::lobby {
namespace connect = steam::connect;

namespace {
void CountJoinedClients(State& state)
{
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        if (net::PeerJoined(id)) {
            ++state.clients;
            state.readyClients += net::InWorld(id) ? 1 : 0;
        }
    }
}

Phase ClientSessionPhase()
{
    if (!net::Joined()) return Phase::Connecting;
    return net::Connected() ? Phase::Joined : Phase::WaitingCampaign;
}
}

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
    } else {
        const steam::Connection flow = steam::Flow();
        if (flow.automatic) {
            if (state.host) {
                state.phase = state.id ? Phase::Hosting : Phase::Creating;
                CountJoinedClients(state);
            } else if (!state.id) {
                state.phase = Phase::Searching;
            } else {
                state.phase = ClientSessionPhase();
            }
        } else if (state.host) {
            switch (flow.hostState) {
            case connect::HostFlow::State::Starting:
                state.phase = Phase::Starting;
                break;
            case connect::HostFlow::State::Waiting:
                state.phase = Phase::InviteWait;
                break;
            case connect::HostFlow::State::Opening:
                state.phase = Phase::PartnerOpening;
                strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::HostFlow::State::Offered:
                state.phase = Phase::PartnerOffered;
                strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::HostFlow::State::Linked:
                state.phase = Phase::Hosting;
                CountJoinedClients(state);
                break;
            }
        } else {
            switch (flow.clientState) {
            case connect::ClientFlow::State::Starting:
                state.phase = Phase::Starting;
                break;
            case connect::ClientFlow::State::Waiting:
                state.phase = flow.lobbyInvitePending ? Phase::LobbyInvited : Phase::WaitingInvite;
                if (flow.lobbyInvitePending) strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::ClientFlow::State::Requesting:
                state.phase = Phase::Requesting;
                strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::ClientFlow::State::Joining:
                state.phase = state.id ? Phase::Connecting : Phase::Requesting;
                if (!state.id || !*state.name) strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::ClientFlow::State::Linked:
                state.phase = ClientSessionPhase();
                break;
            case connect::ClientFlow::State::Declined:
                state.phase = Phase::Declined;
                state.why = flow.why;
                state.arg = flow.arg;
                strncpy_s(state.hostSession, flow.hostSession, _TRUNCATE);
                strncpy_s(state.name, flow.name, _TRUNCATE);
                break;
            case connect::ClientFlow::State::Lost:
                state.phase = Phase::Lost;
                strncpy_s(state.name, flow.name, _TRUNCATE);
                strncpy_s(state.reason, flow.endReason, _TRUNCATE);
                state.endedImmediately = flow.endedImmediately;
                break;
            }
        }
    }
    return state;
}

void Format(State const& state, wchar_t* out, size_t size)
{
    if (!out || !size) return;
    wchar_t name[64] = {}, reason[512] = {}, ping[32] = {}, session[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, state.name, -1, name, 64);
    MultiByteToWideChar(CP_UTF8, 0, state.reason, -1, reason, 512);
    MultiByteToWideChar(CP_UTF8, 0, state.sessionId, -1, session, 64);
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
    case Phase::InviteWait:
        _snwprintf_s(out, size, _TRUNCATE, L"%s", i18n::Text(i18n::LobbyInviteWait));
        break;
    case Phase::PartnerOpening:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyPartnerOpening), name);
        break;
    case Phase::PartnerOffered:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyPartnerOffered),
                     static_cast<unsigned long long>(state.id), name);
        break;
    case Phase::WaitingInvite:
        _snwprintf_s(out, size, _TRUNCATE, L"%s", i18n::Text(i18n::LobbyWaitingInvite));
        break;
    case Phase::LobbyInvited:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyLobbyInvited), name);
        break;
    case Phase::Requesting:
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyRequesting), name);
        break;
    case Phase::Lost:
        _snwprintf_s(out, size, _TRUNCATE,
                     i18n::Text(state.endedImmediately ? i18n::LobbyLostImmediate : i18n::LobbyLost), name, reason);
        break;
    case Phase::Declined: {
        wchar_t hostSession[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, state.hostSession, -1, hostSession, 64);
        switch (state.why) {
        case connect::Why::NotInvite:
            _snwprintf_s(reason, _TRUNCATE, L"%s", i18n::Text(i18n::DeclineNotInvite));
            break;
        case connect::Why::SelfInvite:
            _snwprintf_s(reason, _TRUNCATE, L"%s", i18n::Text(i18n::DeclineSelf));
            break;
        case connect::Why::WrongSession:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineSession), name, hostSession, session);
            break;
        case connect::Why::WrongProtocol:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::RefusedVersion),
                         static_cast<unsigned>(saving::Protocol), static_cast<unsigned>(state.arg));
            break;
        case connect::Why::Busy:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineBusy), name);
            break;
        case connect::Why::NotFriends:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineNotFriends), name);
            break;
        case connect::Why::HostError:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineHostError), name, static_cast<unsigned>(state.arg));
            break;
        case connect::Why::NoAnswer:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineNoAnswer), name);
            break;
        case connect::Why::Unreachable:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineUnreachable), name);
            break;
        case connect::Why::LobbyGone:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineLobbyGone), name);
            break;
        case connect::Why::NotG3T:
            _snwprintf_s(reason, _TRUNCATE, i18n::Text(i18n::DeclineNotG3T), name);
            break;
        case connect::Why::MetadataTimeout:
            _snwprintf_s(reason, _TRUNCATE, L"%s", i18n::Text(i18n::DeclineMetadataTimeout));
            break;
        case connect::Why::None:
            _snwprintf_s(reason, _TRUNCATE, L"?");
            break;
        }
        _snwprintf_s(out, size, _TRUNCATE, i18n::Text(i18n::LobbyDeclined), reason);
        break;
    }
    }
    const size_t used = std::wcslen(out);
    if (used < size) _snwprintf_s(out + used, size - used, _TRUNCATE, i18n::Text(i18n::LobbySession), session);
}
}
