// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
// Steam manual-dispatch callback handlers; see steam_runtime.h and steam_flow.cpp.
#include "steam_runtime.h"

#include "config.h"
#include "log.h"

#include <cstddef>
#include <cstring>

namespace g3t::steam {
void WorkerRuntime::OnLobbyCreated(char const* p, int size)
{
    if (!p || size < static_cast<int>(sizeof(LobbyCreatedParam))) return;
    LobbyCreatedParam created{};
    std::memcpy(&created, p, sizeof(created));
    const int result = created.result;
    const u64 id = created.lobby;
    if (!isHost || (lobby && lobby == id)) return;
    if (lobby && lobby != id) {
        if (result == ResultOk) {
            api.LeaveLobby(matchmaking, id);
            Logf("connect: extra lobby %llu closed: already hosting lobby %llu", id, lobby);
        }
        return;
    }
    Logf("connect: lobby create result=%d lobby=%llu", result, id);
    const bool invite = connectionMode == connect::Mode::Invite;
    if (invite && hostFlow.state() != connect::HostFlow::State::Opening) {
        if (result == ResultOk) {
            api.LeaveLobby(matchmaking, id);
            Logf("connect: lobby %llu closed again: no join request is waiting for it", id);
        }
        return;
    }
    if (result != ResultOk) {
        if (invite) {
            const connect::HostFlow::State before = hostFlow.state();
            AfterHost("lobby create failed", before, hostFlow.LobbyCreated(false, result, Now()));
            return;
        }
        Format(error, "Steam lobby not created (result %d)", result);
        Logf("steam: %s; trying again in 15 s", error);
        return;
    }
    error[0] = '\0';
    lobby = id;
    owner = me;
    if (!api.SetLobbyData(matchmaking, id, "session_id", GetLocalConfig().sessionId) ||
        !api.SetLobbyData(matchmaking, id, LobbyTag, LobbyVersion)) {
        Log("connect: Steam did not publish the co-op session ID and lobby tag");
        LeaveLobby();
        if (invite) {
            const connect::HostFlow::State before = hostFlow.state();
            AfterHost("lobby data not published", before, hostFlow.LobbyCreated(false, 2, Now()));
        } else {
            strcpy_s(error, "Steam did not publish the co-op session ID and lobby tag");
            Logf("steam: %s", error);
        }
        return;
    }
    if (!api.SetLobbyData(matchmaking, id, "name", persona)) Log("steam: could not publish the lobby's player name");
    Logf("connect: lobby %llu session metadata accepted by Steam; remote propagation may still be pending", id);
    Logf("connect: lobby %llu open (friends-only); session_id=%s (case-sensitive; only matching sessions can connect)",
         id, GetLocalConfig().sessionId);
    if (invite) {
        const connect::HostFlow::State before = hostFlow.state();
        AfterHost("lobby created", before, hostFlow.LobbyCreated(true, result, Now()));
        return;
    }
    char text[64];
    Format(text, "+connect_lobby %llu", id);
    if (!SetPresence("status", "G3 Together") || !SetPresence("connect", text))
        Log("steam: could not publish Join game rich presence");
    Logf("steam: lobby %llu open; friends join over Steam (friends list: Join game, or steam_lobby = %llu)", id, id);
}

void WorkerRuntime::OnLobbyEnter(char const* p, int size)
{
    if (!p || size < static_cast<int>(offsetof(LobbyEnterParam, response) + sizeof(uint32_t))) return;
    LobbyEnterParam enter{};
    std::memcpy(&enter, p, offsetof(LobbyEnterParam, response) + sizeof(uint32_t));
    const u64 id = enter.lobby;
    const unsigned response = enter.response;
    if (isHost || id != lobby || owner) return;
    if (connectionMode == connect::Mode::Auto && candidateJoined && candidateLobby == id) return;
    const bool joined = response == LobbyEnterResponseSuccess;
    const auto metadata = joined ? ReadLobbyMetadata(id) : connect::LobbyMetadata::Pending;
    Logf("connect: LobbyEnter lobby=%llu response=%u metadata=%s", id, response, !joined ? "-" :
         metadata == connect::LobbyMetadata::Pending ? "pending" :
         metadata == connect::LobbyMetadata::Match ? "ok" : "different session_id or transport");
    if (connectionMode == connect::Mode::Invite) {
        const u64 lobbyOwner = joined ? api.GetLobbyOwner(matchmaking, id) : 0;
        const connect::ClientFlow::State before = clientFlow.state();
        const unsigned fx = clientFlow.LobbyEntered(id, joined, metadata, lobbyOwner, Now());
        AfterClient("lobby entered", before, fx);
        return;
    }
    if (!joined || metadata == connect::LobbyMetadata::Different) {
        Logf("steam: lobby %llu refused (response %u, incompatible session ID or transport)", id, response);
        Format(error, response != LobbyEnterResponseSuccess
                   ? "the Steam lobby is full or closed"
                   : "different session_id or incompatible G3 Together transport");
        RecordClientEnd(error, clientFlow.state());
        LeaveLobby();
        Skip(id, 30000);
        return;
    }
    if (metadata == connect::LobbyMetadata::Pending) {
        InspectLobby(id, "joined lobby", true);
        PollLobbyMetadata();
        return;
    }
    FinishLobbyEntry(id, api.GetLobbyOwner(matchmaking, id));
}

void WorkerRuntime::OnCallback(int callback, char const* p, int size)
{
    u64 a = 0, b = 0;
    unsigned state = 0;
    switch (callback) {
    case CbLobbyCreated:
        OnLobbyCreated(p, size);
        break;
    case CbLobbyEnter:
        OnLobbyEnter(p, size);
        break;
    case CbLobbyDataUpdate: {
        if (!p || size < static_cast<int>(offsetof(LobbyDataUpdateParam, success) + 1) || isHost) break;
        LobbyDataUpdateParam update{};
        std::memcpy(&update, p, offsetof(LobbyDataUpdateParam, success) + 1);
        a = update.lobby;
        b = update.member;
        if (a != candidateLobby || a != b) break;
        if (!update.success) Logf("connect: lobby %llu metadata update failed; still waiting for Steam", a);
        PollLobbyMetadata();
        break;
    }
    case CbLobbyChatUpdate: {
        if (!p || size < static_cast<int>(sizeof(LobbyChatUpdateParam))) break;
        LobbyChatUpdateParam chat{};
        std::memcpy(&chat, p, sizeof(chat));
        a = chat.lobby;
        b = chat.userChanged;
        state = chat.stateChange;
        if (a == lobby && (state & LobbyChatMemberLeftMask)) {
            if (Remote* r = Find(b)) {
                r->leaving = true;
                r->lobbyChange = state;
            } else if (isHost && connectionMode == connect::Mode::Invite && b == hostFlow.partner()) {
                OnHostLinkLost(b, "left the lobby before saying hello");
            }
        }
        break;
    }
    case CbLobbyJoinRequested: {
        if (!p || size < 8) break;
        LobbyJoinRequestedParam req{};
        std::memcpy(&req, p, size >= static_cast<int>(sizeof(req)) ? sizeof(req) : 8);
        char text[64];
        Format(text, "+connect_lobby %llu", req.lobby);
        OnAccepted(connect::ClientFlow::Source::LobbyJoin,
                   "invite accepted (GameLobbyJoinRequested_t 333)", req.friendId, text);
        break;
    }
    case CbRichJoinRequested: {
        if (!p || size < static_cast<int>(offsetof(RichJoinRequestedParam, connect))) break;
        RichJoinRequestedParam rich{};
        const size_t copyBytes = size < static_cast<int>(sizeof(rich))
            ? static_cast<size_t>(size) : sizeof(rich);
        std::memcpy(&rich, p, copyBytes);
        rich.connect[sizeof(rich.connect) - 1] = '\0';
        OnAccepted(connect::ClientFlow::Source::RichJoin,
                   "Join Game accepted (GameRichPresenceJoinRequested_t 337)", rich.friendId, rich.connect);
        break;
    }
    case CbLobbyInvite: {
        if (!p || size < 16) break;
        LobbyInviteParam invite{};
        std::memcpy(&invite, p, 16);
        OnLobbyInvite(invite.inviter, invite.lobby);
        break;
    }
    case CbConnectionStatusChanged:
        OnP2PStatusChanged(p, size);
        break;
    case CbSessionRequest: {
        if (!p || size < SteamIdentityBytes) break;
        a = api.IdentityGetSteamId(p);
        char const* accepted = nullptr;
        if (isHost && lobby && InLobby(a)) accepted = "lobby member";
        else if (!isHost && owner && a == owner) accepted = "the host";
        else if (!isHost && connectionMode == connect::Mode::Invite && clientFlow.host() == a && clientFlow.Active())
            accepted = "the host we asked to join";
        else if (isHost && connectionMode == connect::Mode::Invite && IsFriend(a)) accepted = "Steam friend";
        if (accepted) {
            api.AcceptSession(messages, p);
            Logf("connect: session with %llu (%s) accepted: %s", a, NameOf(a), accepted);
        } else {
            Logf("connect: session from %llu ignored (neither a lobby member nor a Steam friend asking to join)", a);
        }
        break;
    }
    case CbSessionFailed:
        if (!p || size < SteamIdentityBytes) break;
        a = api.IdentityGetSteamId(p);
        Logf("connect: session with %llu (%s) failed; dropping its relay (Steam callback=%d bytes=%d)",
             a, NameOf(a), CbSessionFailed, size);
        if (!isHost && connectionMode == connect::Mode::Invite) {
            const connect::ClientFlow::State before = clientFlow.state();
            const unsigned fx = clientFlow.SessionFailed(a);
            if (fx & connect::ClientFlow::Teardown) RecordClientEnd("Steam session failed (callback 1252)", before);
            AfterClient("session failed", before, fx);
        }
        Drop(Find(a), "session failed");
        break;
    default:
        break;
    }
}
}
