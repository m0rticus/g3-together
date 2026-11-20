// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
#include "steam_runtime.h"

#include "config.h"
#include "log.h"
#include "player_identity.h"
#include "saving_policy.h"

#include <cstdlib>
#include <cstring>

namespace g3t::steam {
Remote* WorkerRuntime::Find(u64 id)
{
    for (auto& r : remotes) if (r.id == id) return &r;
    return nullptr;
}

Remote* WorkerRuntime::Get(u64 id)
{
    if (Remote* r = Find(id)) return r;
    for (auto& r : remotes)
        if (!r.id) {
            r = Remote{};
            r.id = id;
            r.generation = ++nextRelayGeneration;
            r.createdAt = r.heard = Now();
            return &r;
        }
    return nullptr;
}

int WorkerRuntime::CountLinked() const
{
    int count = 0;
    for (auto const& r : remotes) count += r.linked ? 1 : 0;
    return count;
}

bool WorkerRuntime::InLobby(u64 id) const
{
    const int count = api.MemberCount(matchmaking, lobby);
    for (int i = 0; i < count; ++i)
        if (api.MemberByIndex(matchmaking, lobby, i) == id) return true;
    return false;
}

void WorkerRuntime::ResetClientEnd()
{
    clientAttemptAt = Now();
    clientLinkedAt = 0;
    clientHadLink = clientEndRecorded = clientEndedImmediately = false;
    clientEndReason[0] = '\0';
    std::lock_guard<std::mutex> lock(store.mutex);
    store.clientEnet = {};
}

void WorkerRuntime::RecordClientEnd(char const* why, connect::ClientFlow::State before)
{
    if (isHost || clientEndRecorded) return;
    clientEndRecorded = true;
    CopyName(clientEndReason, sizeof(clientEndReason), why);
    const uint32_t now = Now();
    const uint32_t elapsed = now - (clientHadLink ? clientLinkedAt : clientAttemptAt);
    clientEndedImmediately = connect::LinkEndedImmediately(clientHadLink, elapsed);
    ClientEnet enet;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        enet = store.clientEnet;
    }
    const Remote* r = Find(owner ? owner : requestedHost);
    Logf("connect: client link ended: reason=%s steam_state=%s enet_state=%s(%u) enet_event=%u event_data=%u "
         "elapsed_ms=%u enet_elapsed_ms=%u immediate=%d arg=%llu lobby=%llu relay_port=%u generation=%llu "
         "to_enet=%u to_steam=%u",
         clientEndReason, connect::StateName(before), net::EnetPeerStateName(enet.state), enet.state, enet.event, enet.data,
         elapsed, enet.elapsedMs + (enet.sampledAt ? now - enet.sampledAt : 0), clientEndedImmediately ? 1 : 0,
         clientFlow.arg(), lobby, relayPort, r ? r->generation : 0, r ? r->toEnet : 0, r ? r->toSteam : 0);
}

void WorkerRuntime::ReleaseRemote(Remote& r, char const* dropReason, bool sendBye, bool closeSession)
{
    if (!r.id) return;
    if ((isHost && (r.relayPort || r.linked)) || (!isHost && r.id == owner && r.linked)) {
        RelayDrop drop;
        drop.account = r.id;
        drop.generation = r.generation;
        drop.port = r.relayPort;
        strncpy_s(drop.reason, dropReason, _TRUNCATE);
        std::lock_guard<std::mutex> lock(store.mutex);
        InvalidatePublishedLocked(store, isHost, drop.account, drop.generation);
        store.relayDrops.push_back(drop);
    }
    if (sendBye) Control(r.id, 'B', true);
    ClosePeerP2P(r);
    if (closeSession) {
        char identity[160];
        Identity(identity, r.id);
        api.CloseSession(messages, identity);
    }
    if (r.socket != INVALID_SOCKET) closesocket(r.socket);
    r = Remote{};
}

void WorkerRuntime::Drop(Remote* r, char const* why)
{
    if (!r || !r->id) return;
    const u64 id = r->id;
    const bool wasHost = !isHost && id == owner;
    if (wasHost) RecordClientEnd(why, clientFlow.state());
    if (isHost)
        Logf("connect: host link ended: peer=%llu reason=%s steam_state=%s elapsed_ms=%u "
             "stage=%s lobby_callback=%u lobby_flags=0x%x enet_port=%u relay_port=%u to_enet=%u to_steam=%u",
             id, why, connect::StateName(hostFlow.state()), Now() - (r->linked ? r->linkedAt : r->createdAt),
             r->toEnet || r->toSteam ? "enet-traffic" : "before-enet", r->lobbyChange ? CbLobbyChatUpdate : 0,
             r->lobbyChange, enetPort, r->relayPort, r->toEnet, r->toSteam);
    if (r->pending.Depth())
        Logf("steam-send: peer=%llu action=close-session pending_datagrams=%zu pending_bytes=%zu reason=%s",
             id, r->pending.Depth(), r->pending.Bytes(), why);
    Logf("steam: %llu dropped (%s; %u datagrams to ENet, %u to Steam)", r->id, why, r->toEnet, r->toSteam);
    ReleaseRemote(*r, why, false, true);
    if (wasHost) LeaveLobby();
    if (connectionMode != connect::Mode::Invite) return;
    if (wasHost) OnClientLinkLost(id, why);
    else if (isHost) OnHostLinkLost(id, why);
}

void WorkerRuntime::RotatePartnerForRejoin(u64 id)
{
    Remote* r = Find(id);
    if (!r || !r->id) return;
    Logf("connect: rotating relay generation for rejoining partner=%llu prev_gen=%llu relay_port=%u",
         id, static_cast<unsigned long long>(r->generation), static_cast<unsigned>(r->relayPort));
    rejoinPartner = id;
    ReleaseRemote(*r, "partner rejoined", false, false);
}

bool WorkerRuntime::CreateLobby()
{
    lastCreate = Now();
    const int members = connectionMode == connect::Mode::Invite ? static_cast<int>(connect::MaxPartners) + 1 : net::MaxPlayers;
    const u64 call = api.CreateLobby(matchmaking, LobbyFriendsOnly, members);
    if (!call) {
        Logf("connect: Steam rejected the CreateLobby request");
        return false;
    }
    Logf("connect: CreateLobby called (friends-only, %d members); waiting for Steam's answer", members);
    return true;
}

void WorkerRuntime::Join(u64 id, char const* why)
{
    LeaveLobby();
    if (connectionMode == connect::Mode::Auto) ResetClientEnd();
    lobby = id;
    owner = 0;
    joinedAt = Now();
    api.JoinLobby(matchmaking, id);
    Logf("steam: joining lobby %llu (%s)", id, why);
}

void WorkerRuntime::InspectLobby(u64 id, char const* why, bool joined)
{
    candidateLobby = id;
    candidateAt = candidateRequestAt = Now();
    candidateJoined = joined;
    candidatePendingLogged = false;
    strncpy_s(candidateReason, why, _TRUNCATE);
    Logf("connect: reading the metadata of lobby %llu (%s)", id, why);
    if (!api.RequestLobbyData(matchmaking, id))
        Logf("steam: could not request lobby %llu metadata", id);
}

connect::LobbyMetadata WorkerRuntime::ReadLobbyMetadata(u64 id) const
{
    return connect::CheckLobbyMetadata(Text(api.GetLobbyData(matchmaking, id, LobbyTag)), LobbyVersion,
                                       Text(api.GetLobbyData(matchmaking, id, "session_id")),
                                       GetLocalConfig().sessionId);
}

void WorkerRuntime::LeaveLobby()
{
    if (!lobby) return;
    if (!isHost && owner) RecordClientEnd("local lobby closed", clientFlow.state());
    for (auto& r : remotes)
        if (r.id && (isHost || r.id == owner))
            ReleaseRemote(r, "lobby closed", false, true);
    rejoinPartner = 0;
    api.LeaveLobby(matchmaking, lobby);
    if (isHost && connectionMode == connect::Mode::Auto) {
        api.SetRichPresence(friends, "connect", "");
        api.SetRichPresence(friends, "status", "");
    }
    Logf("steam: left lobby %llu", lobby);
    lobby = owner = 0;
    hostName[0] = '\0';
    searchLogged = false;
}

// ---- invite-driven connection (steam_connect.h) ----

char const* WorkerRuntime::NameOf(u64 id)
{
    if (!id || !friends) return "";
    Label* slot = &labels[0];
    for (auto& l : labels) {
        if (l.id == id) { slot = &l; break; }
        if (l.at < slot->at) slot = &l;
    }
    const uint32_t now = Now();
    if (slot->id != id) {
        *slot = Label{};
        slot->id = id;
    }
    if (!slot->text[0] && (!slot->at || now - slot->at > 2000)) {
        slot->at = now ? now : 1;
        CopyName(slot->text, sizeof(slot->text), api.FriendPersonaName(friends, id));
    }
    return slot->text[0] ? slot->text : "a friend";
}

bool WorkerRuntime::IsFriend(u64 id) const
{
    if (!id || id == me) return false;
    if (api.FriendRelationship) return api.FriendRelationship(friends, id) == FriendRelationshipFriend;
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i)
        if (api.FriendByIndex(friends, i, FriendFlagImmediate) == id) return true;
    return false;
}

u64 WorkerRuntime::FriendByName(char const* name) const
{
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        const u64 id = api.FriendByIndex(friends, i, FriendFlagImmediate);
        if (!_stricmp(Text(api.FriendPersonaName(friends, id)), name)) return id;
    }
    return 0;
}

int WorkerRuntime::SendFrame(u64 to, char const* what, char const* frame, size_t size)
{
    char identity[160];
    Identity(identity, to);
    const int result = api.SendToUser(messages, identity, frame, static_cast<unsigned>(size),
                                      SendReliable | SendNoNagle | SendAutoRestart, ChannelControl);
    Logf("connect: sent %s to %llu (%s) result=%d%s", what, to, NameOf(to), result,
         result == ResultOk ? "" : " (Steam did not accept the message)");
    return result;
}

bool WorkerRuntime::SetPresence(char const* key, char const* value)
{
    if (!friends) return false;
    const bool ok = api.SetRichPresence(friends, key, value) != 0;
    Logf("connect: rich presence %s=\"%s\" ok=%d", key, value, ok ? 1 : 0);
    return ok;
}

void WorkerRuntime::PublishPresence()
{
    if (!isHost || connectionMode != connect::Mode::Invite || !apiInitialized) return;
    lastPresence = Now();
    if (!connectPublished) {
        if (!connect::FormatConnect(presenceConnect, sizeof(presenceConnect), me, GetLocalConfig().sessionId)) {
            strcpy_s(error, "cannot build the Join Game connect string");
            Logf("connect: %s (host %llu, session %s)", error, me, GetLocalConfig().sessionId);
            return;
        }
        if (SetPresence("connect", presenceConnect)) {
            connectPublished = true;
            error[0] = '\0';
        } else {
            strcpy_s(error, "Steam did not accept the Join Game rich presence");
            Logf("connect: %s; trying again in 5 s", error);
        }
    }
    using S = connect::HostFlow::State;
    const S state = hostFlow.state();
    char const* status = state == S::Linked ? "G3 Together: playing with a partner"
        : state == S::Opening || state == S::Offered ? "G3 Together: a partner is connecting"
        : "G3 Together: open for a partner (Join Game)";
    if (std::strcmp(status, presenceStatus) != 0 && SetPresence("status", status))
        strcpy_s(presenceStatus, status);
}

void WorkerRuntime::SendJoinRequest(bool resend)
{
    const u64 host = clientFlow.host();
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeJoin(frame, sizeof(frame), GetLocalConfig().sessionId, saving::Protocol);
    if (!host || !size) {
        Logf("connect: cannot send a join request (host=%llu)", host);
        return;
    }
    requestedHost = host;
    SendFrame(host, resend ? "join request (again, no answer yet)" : "join request", frame, size);
}

void WorkerRuntime::TearDownClient()
{
    candidateLobby = 0;
    if (lobby) LeaveLobby();
    if (requestedHost) {
        char identity[160];
        Identity(identity, requestedHost);
        api.CloseSession(messages, identity);
        requestedHost = 0;
    }
    owner = 0;
}

void WorkerRuntime::LogClientChange(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet)
{
    using F = connect::ClientFlow;
    const F::State after = clientFlow.state();
    if (after == F::State::Declined && before != after)
        Logf("connect: client %s -> %s on %s: %s (arg=%llu%s%s)", connect::StateName(before), connect::StateName(after), event,
             connect::WhyName(clientFlow.why()), clientFlow.arg(),
             *clientFlow.hostSession() ? ", host session " : "", clientFlow.hostSession());
    else if (after != before)
        Logf("connect: client %s -> %s on %s", connect::StateName(before), connect::StateName(after), event);
    else if ((fx & F::Ignored) && !quiet)
        Logf("connect: client %s: %s ignored (nothing changes)", connect::StateName(before), event);
}

void WorkerRuntime::FinishLobbyEntry(u64 id, u64 lobbyOwner)
{
    if (!lobbyOwner) {
        Logf("connect: lobby %llu has no owner after entry", id);
        strcpy_s(error, "the Steam lobby has no owner");
        RecordClientEnd("the Steam lobby has no owner", clientFlow.state());
        LeaveLobby();
        Skip(id, 30000);
        return;
    }
    error[0] = '\0';
    owner = lobbyOwner;
    CopyName(hostName, sizeof(hostName), api.FriendPersonaName(friends, owner));
    if (Remote* r = Get(owner)) r->heard = Now();
    lastHello = 0;
    Logf("connect: in lobby %llu of %s (%llu); saying hello", id, hostName, owner);
}

void WorkerRuntime::AfterClient(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet)
{
    using F = connect::ClientFlow;
    LogClientChange(event, before, fx, quiet);
    if (fx & F::Teardown) {
        RecordClientEnd(clientFlow.state() == F::State::Declined ? connect::WhyName(clientFlow.why()) :
                        !std::strcmp(event, "accept") ? "another Steam invite accepted" : event, before);
        TearDownClient();
    }
    if (!std::strcmp(event, "accept") && (fx & (F::SendRequest | F::InspectLobby))) ResetClientEnd();
    if (fx & F::SendRequest) SendJoinRequest(false);
    if (fx & F::InspectLobby) {
        InspectLobby(clientFlow.lobby(), "lobby invite");
        PollLobbyMetadata();
    }
    if (fx & F::JoinLobby) {
        Logf("connect: JoinLobby(%llu)", clientFlow.lobby());
        Join(clientFlow.lobby(), "accepted invite");
    }
    if (fx & F::AwaitMetadata) {
        InspectLobby(clientFlow.lobby(), "joined lobby", true);
        PollLobbyMetadata();
    }
    if (fx & F::HelloReady) FinishLobbyEntry(clientFlow.lobby(), clientFlow.host());
}

void WorkerRuntime::PollLobbyMetadata()
{
    if (!candidateLobby) return;
    const u64 id = candidateLobby;
    const uint32_t now = Now();
    const auto metadata = ReadLobbyMetadata(id);
    if (connectionMode == connect::Mode::Auto && candidatePendingLogged &&
        now - candidateAt >= connect::LobbyMetadataWaitMs) {
        Logf("connect: lobby %llu metadata timed out after %u ms; Steam did not deliver both keys", id, now - candidateAt);
        RecordClientEnd("Steam lobby metadata timed out", clientFlow.state());
        candidateLobby = 0;
        if (candidateJoined && lobby == id) LeaveLobby();
        Skip(id, 30000);
        strcpy_s(error, "Steam did not deliver the lobby's data in time; accept the invite again");
        lastScan = now - ScanMs;
        return;
    }
    if (metadata == connect::LobbyMetadata::Pending) {
        if (!candidatePendingLogged) {
            Logf("connect: lobby %llu metadata pending (g3together=\"%s\" session_id=\"%s\"), waiting up to %u ms", id,
                 Text(api.GetLobbyData(matchmaking, id, LobbyTag)), Text(api.GetLobbyData(matchmaking, id, "session_id")),
                 connect::LobbyMetadataWaitMs);
            candidatePendingLogged = true;
        }
        if (connectionMode == connect::Mode::Invite) {
            const auto before = clientFlow.state();
            const unsigned fx = clientFlow.LobbyChecked(id, metadata, now);
            if ((fx & connect::ClientFlow::Teardown) && clientFlow.why() == connect::Why::MetadataTimeout)
                Logf("connect: lobby %llu metadata timed out after %u ms; Steam did not deliver both keys", id, now - candidateAt);
            AfterClient("lobby metadata pending", before, fx, true);
            if (fx & connect::ClientFlow::Teardown) return;
        }
        if (now - candidateRequestAt >= MetadataRequestRetryMs) {
            candidateRequestAt = now;
            if (!api.RequestLobbyData(matchmaking, id))
                Logf("steam: could not refresh lobby %llu metadata", id);
        }
        return;
    }

    if (candidatePendingLogged)
        Logf("connect: lobby %llu metadata arrived after %u ms: %s", id, now - candidateAt,
             metadata == connect::LobbyMetadata::Match ? "ok" : "different");
    else
        Logf("connect: lobby %llu metadata: %s", id, metadata == connect::LobbyMetadata::Match ? "ok" : "different");
    const bool joined = candidateJoined;
    candidateLobby = 0;
    if (connectionMode == connect::Mode::Invite) {
        const auto before = clientFlow.state();
        const unsigned fx = clientFlow.LobbyChecked(id, metadata, now);
        if ((fx & connect::ClientFlow::Teardown) && clientFlow.why() == connect::Why::MetadataTimeout)
            Logf("connect: lobby %llu metadata timed out after %u ms; Steam did not deliver both keys in time", id, now - candidateAt);
        AfterClient("lobby metadata", before, fx);
    } else if (metadata == connect::LobbyMetadata::Match) {
        if (joined) FinishLobbyEntry(id, api.GetLobbyOwner(matchmaking, id));
        else Join(id, candidateReason);
    } else {
        Logf("steam: skipping lobby %llu: different session_id or incompatible transport (wanted %s)",
             id, GetLocalConfig().sessionId);
        if (joined && lobby == id) LeaveLobby();
        Skip(id, 30000);
        if (wantedLobby == id || !std::strcmp(candidateReason, "accepted through Steam"))
            strcpy_s(error, "different session_id or incompatible G3 Together transport");
        lastScan = now - ScanMs;
    }
}

void WorkerRuntime::SendOffer(u64 partner)
{
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeOffer(frame, sizeof(frame), lobby, GetLocalConfig().sessionId);
    if (!lobby || !size) {
        Logf("connect: cannot send the lobby offer (lobby=%llu)", lobby);
        return;
    }
    SendFrame(partner, "lobby offer", frame, size);
}

void WorkerRuntime::SendDecline(u64 to, connect::Why why, u64 arg)
{
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeDecline(frame, sizeof(frame), why, GetLocalConfig().sessionId, arg);
    Logf("connect: declining %llu (%s): %s", to, NameOf(to), connect::WhyName(why));
    if (!to || !size) return;
    SendFrame(to, "decline", frame, size);
}

void WorkerRuntime::AfterHost(char const* event, connect::HostFlow::State before, unsigned fx, bool quiet)
{
    using H = connect::HostFlow;
    const H::State after = hostFlow.state();
    if (after != before)
        Logf("connect: host %s -> %s on %s", connect::StateName(before), connect::StateName(after), event);
    else if ((fx & H::Ignored) && !quiet)
        Logf("connect: host %s: %s ignored (nothing changes)", connect::StateName(before), event);
    if (fx & H::RotatePartner) RotatePartnerForRejoin(hostFlow.partner());
    if (fx & H::SendDecline) SendDecline(hostFlow.declineTo(), hostFlow.declineWhy(), hostFlow.declineArg());
    if (fx & H::Teardown) LeaveLobby();
    if (fx & H::CreateLobby) {
        if (connectionMode == connect::Mode::Auto) {
            CreateLobbyAuto();
        } else if (!CreateLobby()) {
            const H::State rejected = hostFlow.state();
            AfterHost("CreateLobby rejected", rejected, hostFlow.LobbyCreated(false, 2, Now()));
        }
    }
    if (fx & H::SendOffer) SendOffer(hostFlow.partner());
    PublishPresence();
}

namespace {
void DescribeTarget(connect::Target const& target, char (&out)[160])
{
    if (target.kind == connect::Target::Kind::Host) Format(out, "host %llu, session %s", target.id, target.session);
    else if (target.kind == connect::Target::Kind::Lobby) Format(out, "lobby %llu", target.id);
    else Format(out, "%s", target.malformed ? "malformed G3 Together connect string" : "not a G3 Together connect string");
}
}

void WorkerRuntime::OnAccepted(connect::ClientFlow::Source source, char const* what, u64 friendId, char const* raw)
{
    const connect::Target target = connect::ParseConnect(raw);
    char described[160];
    DescribeTarget(target, described);
    Logf("connect: %s: friend=%llu (%s) connect=\"%.200s\" -> %s", what, friendId, NameOf(friendId), raw ? raw : "", described);
    if (isHost) {
        Log("connect: ignored: this copy is the host (invites are for the client)");
        return;
    }
    if (connectionMode == connect::Mode::Auto) {
        if (target.kind == connect::Target::Kind::Lobby) pendingLobby = target.id;
        else Log("connect: connection=auto only follows +connect_lobby; the friends-list search goes on");
        return;
    }
    const connect::ClientFlow::State before = clientFlow.state();
    AfterClient("accept", before, clientFlow.Accept(source, friendId, target, Now()));
}

void WorkerRuntime::OnLobbyInvite(u64 inviter, u64 lobbyId)
{
    Logf("connect: lobby invite from %llu (%s) lobby=%llu: not joining by itself, the player has to accept it in Steam",
         inviter, NameOf(inviter), lobbyId);
    if (isHost || connectionMode != connect::Mode::Invite) return;
    const connect::ClientFlow::State before = clientFlow.state();
    AfterClient("lobby invite", before, clientFlow.LobbyInvite(inviter, lobbyId));
}

void WorkerRuntime::OnJoinRequest(u64 from, connect::Frame const& frame)
{
    connect::HostFlow::Join join;
    join.from = from;
    join.friendOk = IsFriend(from);
    join.sessionOk = !std::strcmp(frame.session, GetLocalConfig().sessionId);
    join.protocol = frame.protocol;
    Logf("connect: join request from %llu (%s): session=%s protocol=%u friend=%d", from, NameOf(from),
         frame.session, frame.protocol, join.friendOk ? 1 : 0);
    const connect::HostFlow::State before = hostFlow.state();
    AfterHost("join request", before, hostFlow.Request(join, saving::Protocol, Now()));
}

void WorkerRuntime::OnNegotiation(u64 from, char const* data, int size)
{
    connect::Frame frame;
    if (!from || size <= 0 || !connect::DecodeFrame(data, static_cast<size_t>(size), &frame)) {
        Logf("connect: malformed negotiation frame from %llu (%d bytes) dropped", from, size);
        return;
    }
    if (isHost) {
        if (frame.type == 'J') OnJoinRequest(from, frame);
        else Logf("connect: frame '%c' from %llu ignored: the host only answers join requests", frame.type, from);
        return;
    }
    if (connectionMode != connect::Mode::Invite) {
        Logf("connect: frame '%c' from %llu ignored: connection=auto", frame.type, from);
        return;
    }
    const connect::ClientFlow::State before = clientFlow.state();
    if (frame.type == 'L') {
        if (std::strcmp(frame.session, GetLocalConfig().sessionId)) {
            Logf("connect: offer from %llu names session %s, not ours (%s); ignored",
                 from, frame.session, GetLocalConfig().sessionId);
            return;
        }
        Logf("connect: lobby offer from %llu (%s): lobby=%llu", from, NameOf(from), frame.lobby);
        AfterClient("lobby offer", before, clientFlow.Offer(from, frame.lobby, Now()));
    } else if (frame.type == 'N') {
        const connect::Why why = connect::DeclineWhy(frame.code);
        Logf("connect: host %llu (%s) declined the join request: %s (code %c)", from, NameOf(from), connect::WhyName(why), frame.code);
        AfterClient("host declined", before, clientFlow.HostDeclined(from, why, frame.number, frame.code == 'S' ? frame.session : nullptr));
    } else {
        Logf("connect: frame '%c' from %llu ignored: a client only expects offers and declines", frame.type, from);
    }
}

void WorkerRuntime::OnClientLinkLost(u64 id, char const* why)
{
    const connect::ClientFlow::State before = clientFlow.state();
    const unsigned fx = clientFlow.LinkLost(id);
    if (fx & connect::ClientFlow::Teardown) RecordClientEnd(why, before);
    if (clientFlow.state() == connect::ClientFlow::State::Lost)
        Logf("connect: connection to %llu (%s) ended (%s); nothing reconnects by itself: accept their invite again",
             id, NameOf(id), why);
    AfterClient("link lost", before, fx);
}

void WorkerRuntime::OnHostLinkLost(u64 id, char const* why)
{
    const connect::HostFlow::State before = hostFlow.state();
    const unsigned fx = hostFlow.LinkLost(id);
    if (fx & connect::HostFlow::Teardown)
        Logf("connect: partner %llu (%s) is gone (%s); lobby closed, waiting for the next invite", id, NameOf(id), why);
    AfterHost("link lost", before, fx);
}

void WorkerRuntime::ClientTick()
{
    const uint32_t now = Now();
    if (clientFlow.ResendDue(now)) SendJoinRequest(true);
    const connect::ClientFlow::State before = clientFlow.state();
    const u64 waitingLobby = clientFlow.lobby();
    const unsigned fx = clientFlow.Tick(now);
    if ((fx & connect::ClientFlow::Teardown) && clientFlow.why() == connect::Why::MetadataTimeout)
        Logf("connect: lobby %llu metadata timed out after %u ms; Steam did not deliver both keys",
             waitingLobby, now - candidateAt);
    AfterClient("timer", before, fx, true);
}

void WorkerRuntime::HostTick()
{
    const uint32_t now = Now();
    const connect::HostFlow::State before = hostFlow.state();
    AfterHost("timer", before, hostFlow.Tick(now), true);
    if (!connectPublished && now - lastPresence > 5000) PublishPresence();
}

void WorkerRuntime::StartupAccept()
{
    char const* line = GetCommandLineA();
    Logf("connect: command line: %.300s", line ? line : "");
    const connect::Target target = connect::ParseConnect(line);
    if (connectionMode == connect::Mode::Auto) {
        if (target.kind == connect::Target::Kind::Lobby) pendingLobby = target.id;
        return;
    }
    if (target.kind != connect::Target::Kind::None || target.malformed) {
        OnAccepted(connect::ClientFlow::Source::CommandLine, "Steam started the game with a connect string",
                   target.kind == connect::Target::Kind::Host ? target.id : 0, line);
        return;
    }
    Log("connect: no G3 Together connect string on the command line");
    if (wantedLobby) {
        char text[64];
        Format(text, "+connect_lobby %llu", wantedLobby);
        OnAccepted(connect::ClientFlow::Source::Manual, "steam_lobby in g3together.ini (manual join, one attempt)", 0, text);
        return;
    }
    char const* manualHost = GetLocalConfig().steamHost;
    if (*manualHost) {
        uint64_t parsedId = 0;
        u64 id = 0;
        if (identity::SteamId(manualHost, parsedId)) {
            if (IsFriend(parsedId)) id = parsedId;
        } else {
            id = FriendByName(manualHost);
        }
        char text[128];
        if (id && connect::FormatConnect(text, sizeof(text), id, GetLocalConfig().sessionId)) {
            OnAccepted(connect::ClientFlow::Source::Manual, "steam_host in g3together.ini (manual join, one attempt)", id, text);
            return;
        }
        Logf("connect: steam_host '%s' is not on your Steam friends list", manualHost);
    }
    Log("connect: waiting for an invite (accept a Steam invite or Join Game on the host; nothing connects by itself)");
}

void WorkerRuntime::FillConnection(Connection& out)
{
    out.automatic = connectionMode == connect::Mode::Auto;
    out.host = isHost;
    out.hostState = hostFlow.state();
    out.clientState = clientFlow.state();
    out.why = clientFlow.why();
    out.arg = clientFlow.arg();
    strcpy_s(out.hostSession, clientFlow.hostSession());
    out.lobbyInvitePending = !isHost && clientFlow.Idle() && clientFlow.lobbyInvitedBy() != 0;
    strcpy_s(out.endReason, clientEndReason);
    out.endedImmediately = clientEndedImmediately;
    const u64 who = isHost ? hostFlow.partner() : out.lobbyInvitePending ? clientFlow.lobbyInvitedBy() : clientFlow.inviter();
    CopyName(out.name, sizeof(out.name), NameOf(who));
}
}
