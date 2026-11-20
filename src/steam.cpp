// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
// Steam transport lifecycle, worker thread and published engine-thread readers; see steam.h,
// steam_abi.h, steam_runtime.h, steam_p2p.cpp, steam_flow.cpp and steam_diagnostics.cpp.
#include "steam_runtime.h"

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#include "config.h"
#include "crashlog.h"
#include "log.h"
#include "relaywire.h"
#include "saving_policy.h"
#include "session.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <process.h>

namespace g3t::steam {
Api api;
WorkerRuntime rt;
PublishedStore store;

namespace {
Export const Exports[] = {
    {reinterpret_cast<void**>(&api.InitFlat), "SteamAPI_InitFlat"},
    {reinterpret_cast<void**>(&api.Shutdown), "SteamAPI_Shutdown"},
    {reinterpret_cast<void**>(&api.ClientUtils), "SteamAPI_SteamUtils_v010"},
    {reinterpret_cast<void**>(&api.AppId), "SteamAPI_ISteamUtils_GetAppID"},
    {reinterpret_cast<void**>(&api.GetHSteamPipe), "SteamAPI_GetHSteamPipe"},
    {reinterpret_cast<void**>(&api.DispatchInit), "SteamAPI_ManualDispatch_Init"},
    {reinterpret_cast<void**>(&api.DispatchRunFrame), "SteamAPI_ManualDispatch_RunFrame"},
    {reinterpret_cast<void**>(&api.DispatchNext), "SteamAPI_ManualDispatch_GetNextCallback"},
    {reinterpret_cast<void**>(&api.DispatchFree), "SteamAPI_ManualDispatch_FreeLastCallback"},
    {reinterpret_cast<void**>(&api.DispatchResult), "SteamAPI_ManualDispatch_GetAPICallResult"},
    {reinterpret_cast<void**>(&api.User), "SteamAPI_SteamUser_v023"},
    {reinterpret_cast<void**>(&api.UserSteamId), "SteamAPI_ISteamUser_GetSteamID"},
    {reinterpret_cast<void**>(&api.Matchmaking), "SteamAPI_SteamMatchmaking_v009"},
    {reinterpret_cast<void**>(&api.CreateLobby), "SteamAPI_ISteamMatchmaking_CreateLobby"},
    {reinterpret_cast<void**>(&api.JoinLobby), "SteamAPI_ISteamMatchmaking_JoinLobby"},
    {reinterpret_cast<void**>(&api.LeaveLobby), "SteamAPI_ISteamMatchmaking_LeaveLobby"},
    {reinterpret_cast<void**>(&api.SetLobbyData), "SteamAPI_ISteamMatchmaking_SetLobbyData"},
    {reinterpret_cast<void**>(&api.GetLobbyData), "SteamAPI_ISteamMatchmaking_GetLobbyData"},
    {reinterpret_cast<void**>(&api.RequestLobbyData), "SteamAPI_ISteamMatchmaking_RequestLobbyData"},
    {reinterpret_cast<void**>(&api.GetLobbyOwner), "SteamAPI_ISteamMatchmaking_GetLobbyOwner"},
    {reinterpret_cast<void**>(&api.MemberCount), "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers"},
    {reinterpret_cast<void**>(&api.MemberByIndex), "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex"},
    {reinterpret_cast<void**>(&api.Friends), "SteamAPI_SteamFriends_v018"},
    {reinterpret_cast<void**>(&api.PersonaName), "SteamAPI_ISteamFriends_GetPersonaName"},
    {reinterpret_cast<void**>(&api.FriendCount), "SteamAPI_ISteamFriends_GetFriendCount"},
    {reinterpret_cast<void**>(&api.FriendByIndex), "SteamAPI_ISteamFriends_GetFriendByIndex"},
    {reinterpret_cast<void**>(&api.FriendGamePlayed), "SteamAPI_ISteamFriends_GetFriendGamePlayed"},
    {reinterpret_cast<void**>(&api.FriendPersonaName), "SteamAPI_ISteamFriends_GetFriendPersonaName"},
    {reinterpret_cast<void**>(&api.SetRichPresence), "SteamAPI_ISteamFriends_SetRichPresence"},
    {reinterpret_cast<void**>(&api.Messages), "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"},
    {reinterpret_cast<void**>(&api.SendToUser), "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"},
    {reinterpret_cast<void**>(&api.ReceiveOnChannel), "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"},
    {reinterpret_cast<void**>(&api.AcceptSession), "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"},
    {reinterpret_cast<void**>(&api.CloseSession), "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser"},
    {reinterpret_cast<void**>(&api.IdentitySetSteamId), "SteamAPI_SteamNetworkingIdentity_SetSteamID64"},
    {reinterpret_cast<void**>(&api.IdentityGetSteamId), "SteamAPI_SteamNetworkingIdentity_GetSteamID64"},
    {reinterpret_cast<void**>(&api.MessageRelease), "SteamAPI_SteamNetworkingMessage_t_Release"},
    {reinterpret_cast<void**>(&api.Utils), "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"},
    {reinterpret_cast<void**>(&api.InitRelayNetworkAccess), "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"},
};
Export const OptionalExports[] = {
    {reinterpret_cast<void**>(&api.FriendRelationship), "SteamAPI_ISteamFriends_GetFriendRelationship"},
    {reinterpret_cast<void**>(&api.SetConfigValue), "SteamAPI_ISteamNetworkingUtils_SetConfigValue"},
    {reinterpret_cast<void**>(&api.GetConfigValue), "SteamAPI_ISteamNetworkingUtils_GetConfigValue"},
    {reinterpret_cast<void**>(&api.GetSessionConnectionInfo), "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo"},
    {reinterpret_cast<void**>(&api.AllocateMessage), "SteamAPI_ISteamNetworkingUtils_AllocateMessage"},
    {reinterpret_cast<void**>(&api.Sockets), "SteamAPI_SteamNetworkingSockets_SteamAPI_v012"},
    {reinterpret_cast<void**>(&api.CreateListenSocketP2P), "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P"},
    {reinterpret_cast<void**>(&api.ConnectP2P), "SteamAPI_ISteamNetworkingSockets_ConnectP2P"},
    {reinterpret_cast<void**>(&api.AcceptConnection), "SteamAPI_ISteamNetworkingSockets_AcceptConnection"},
    {reinterpret_cast<void**>(&api.CloseConnection), "SteamAPI_ISteamNetworkingSockets_CloseConnection"},
    {reinterpret_cast<void**>(&api.CloseListenSocket), "SteamAPI_ISteamNetworkingSockets_CloseListenSocket"},
    {reinterpret_cast<void**>(&api.ConfigureConnectionLanes), "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes"},
    {reinterpret_cast<void**>(&api.SendMessages), "SteamAPI_ISteamNetworkingSockets_SendMessages"},
    {reinterpret_cast<void**>(&api.FlushMessagesOnConnection), "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection"},
    {reinterpret_cast<void**>(&api.ReceiveOnConnection), "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection"},
    {reinterpret_cast<void**>(&api.GetConnectionInfo), "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo"},
    {reinterpret_cast<void**>(&api.GetConnectionRealTimeStatus), "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus"},
    {reinterpret_cast<void**>(&api.FakeSteamAbi), "FakeSteam_GetAbiInfoV1"},
};

unsigned __stdcall TransportThread(void*)
{
    for (;;) {
        const DWORD stop = WaitForSingleObject(store.stopEvent, 0);
        if (stop == WAIT_OBJECT_0) break;
        if (stop == WAIT_FAILED) {
            rt.ready = false;
            rt.Fail("transport stop event failed");
            break;
        }
        crashlog::PollWorldEntry();
        rt.PumpOnce();
        HANDLE events[] = {store.stopEvent, store.wakeEvent, store.pollTimer};
        if (store.pollTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(latency::RelayPollMs) * 10000;
            if (!SetWaitableTimer(store.pollTimer, &due, 0, nullptr, nullptr, FALSE)) {
                rt.ready = false;
                rt.Fail("transport timer could not be armed");
                break;
            }
        }
        const DWORD result = WaitForMultipleObjects(store.pollTimer ? 3 : 2, events, FALSE,
                                                    store.pollTimer ? INFINITE : latency::RelayPollMs);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) {
            rt.ready = false;
            rt.Fail("transport wait failed");
            break;
        }
    }
    return 0;
}
}

void WorkerRuntime::Identity(char (&buffer)[160], u64 id) const
{
    std::memset(buffer, 0, sizeof(buffer));
    api.IdentitySetSteamId(buffer, id);
}

bool WorkerRuntime::Fail(char const* text)
{
    strncpy_s(error, text, _TRUNCATE);
    Logf("steam: not used: %s", error);
    if (!isHost && clientFlow.Active()) OnClientLinkLost(clientFlow.host(), text);
    Publish();
    return false;
}

bool WorkerRuntime::Load()
{
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("G3T_STEAM_DLL", path, MAX_PATH) == 0 || !*path) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&Start), &self) ||
            !GetModuleFileNameA(self, path, MAX_PATH))
            return Fail("no game folder");
        for (int part = 0; part < 2; ++part) {
            char* separator = std::strrchr(path, '\\');
            if (separator) *separator = '\0';
        }
        strcat_s(path, "\\G3Together\\steam_api.dll");
    }
    if (!SetEnvironmentVariableA("SteamAppId", "39500") || !SetEnvironmentVariableA("SteamGameId", "39500"))
        return Fail("Gothic 3 Steam identity could not be selected");
    HMODULE dll = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        Logf("steam: %s not loaded (%lu)", path, GetLastError());
        return Fail("steam_api.dll not found (G3Together folder in the game folder)");
    }
    for (auto const& e : Exports) {
        FARPROC function = GetProcAddress(dll, e.name);
        if (!function) {
            char text[160];
            Format(text, "steam_api.dll too old (no %s)", e.name);
            return Fail(text);
        }
        *e.slot = reinterpret_cast<void*>(function);
    }
    for (auto const& e : OptionalExports) {
        FARPROC function = GetProcAddress(dll, e.name);
        *e.slot = reinterpret_cast<void*>(function);
        if (!function) Logf("steam: optional %s not exported by this steam_api.dll", e.name);
    }
    Logf("steam: loaded %s", path);
    return true;
}

void WorkerRuntime::PumpOnce()
{
    if (!ready) return;
    const uint32_t began = Now();
    if (lastPump) pollGaps.Add(began - lastPump);
    lastPump = began;
    SteamFpScope floatingPoint("callbacks");
    api.DispatchRunFrame(pipe);
    DispatchCallbackMsg message{};
    while (api.DispatchNext(pipe, &message)) {
        if (message.callback == CbCallCompleted && message.param &&
            message.size >= static_cast<int>(sizeof(CallCompletedParam))) {
            CallCompletedParam completed{};
            std::memcpy(&completed, message.param, sizeof(completed));
            char result[512];
            sbool failed = 0;
            if (completed.size <= sizeof(result) &&
                api.DispatchResult(pipe, completed.call, result, static_cast<int>(completed.size),
                                   completed.callback, &failed)) {
                if (!failed) {
                    OnCallback(completed.callback, result, static_cast<int>(completed.size));
                } else if (completed.callback == CbLobbyCreated) {
                    LobbyCreatedParam fail{};
                    fail.result = 2;
                    OnLobbyCreated(reinterpret_cast<char const*>(&fail), static_cast<int>(sizeof(fail)));
                } else if (completed.callback == CbLobbyEnter && !isHost && lobby && !owner) {
                    LobbyEnterParam fail{};
                    fail.lobby = lobby;
                    fail.response = 5;
                    OnLobbyEnter(reinterpret_cast<char const*>(&fail), static_cast<int>(sizeof(fail)));
                }
            }
        } else {
            OnCallback(message.callback, message.param, message.size);
        }
        api.DispatchFree(pipe);
    }
    std::vector<RelayClose> closes;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        closes.swap(store.relayCloses);
        for (auto const& close : closes)
            InvalidatePublishedLocked(store, isHost, close.account, close.generation);
    }
    if (isHost) {
        for (auto const& close : closes)
            if (Remote* r = Find(close.account); r && r->generation == close.generation)
                Drop(r, close.reason);
    }
    if (isHost && !lobby && connectionMode == connect::Mode::Auto && Now() - lastCreate > 15000)
        CreateLobbyAuto();
    if (store.disconnectRequested.exchange(false) && !isHost &&
        connectionMode == connect::Mode::Invite && clientFlow.Active()) {
        ClientEnet enet;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(store.mutex);
            enet = store.clientEnet;
            generation = store.disconnectGeneration;
            store.disconnectGeneration = 0;
        }
        const Remote* activeHost = owner ? Find(owner) : nullptr;
        if (generation && activeHost && activeHost->linked && activeHost->generation == generation)
            OnClientLinkLost(clientFlow.host(), enet.reason[0] ? enet.reason : "ENet connection ended (no event reason)");
    }
    if (!isHost) {
        PollLobbyMetadata();
        if (connectionMode == connect::Mode::Auto) Search();
        else ClientTick();
    } else if (connectionMode == connect::Mode::Invite) {
        HostTick();
    }
    ReceiveP2P();
    Receive(ChannelData);
    Receive(ChannelControl);
    for (auto& r : remotes)
        if (r.leaving) {
            char why[160];
            Format(why, "left the Steam lobby (callback %d flags=0x%x)", CbLobbyChatUpdate, r.lobbyChange);
            Drop(&r, why);
        }
    Forward();
    const uint32_t now = Now();
    if (!lastNames || now - lastNames >= 1000) {
        lastNames = now ? now : 1;
        CopyName(playerName, sizeof(playerName), api.PersonaName(friends));
        CopyName(persona, sizeof(persona), api.PersonaName(friends));
        for (auto& r : remotes)
            if (r.linked) CopyName(r.name, sizeof(r.name), api.FriendPersonaName(friends, r.id));
        if (!isHost && owner) CopyName(hostName, sizeof(hostName), api.FriendPersonaName(friends, owner));
    }
    if (!isHost && owner) {
        Remote* host = Find(owner);
        if (host && !host->linked && now - lastHello > 1000) {
            Control(owner, 'H', true);
            lastHello = now;
        }
        if (host && host->linked && !host->p2pConn) OpenClientP2P(*host);
    }
    for (auto& r : remotes) {
        if (!r.id) continue;
        if (now - r.heard > (r.linked ? SilenceMs : 60000)) {
            Drop(&r, r.linked ? "silent for 180 s" : "not linked after 60 s");
            continue;
        }
        if (r.linked && now - r.lastPing > latency::PingIntervalMs && r.ping.CanSend(PingSeconds())) {
            Control(r.id, 'P', false);
            r.lastPing = now;
        }
    }
    if (!lastDiagnostics || now - lastDiagnostics >= 5000) {
        lastDiagnostics = now ? now : 1;
        SendDiagnostics(now);
        Publish();
        Status(&Log);
        pollGaps = {};
        pumpWork = {};
    } else {
        Publish();
    }
    pumpWork.Add(Now() - began);
}

bool Start(bool host, unsigned short port)
{
    if (rt.tried) return Ready();
    SteamFpScope floatingPoint("startup");
    rt.tried = true;
    rt.traffic = {};
    char const* session = GetLocalConfig().sessionId;
    rt.traffic.envelopeBytes = static_cast<uint32_t>(RelayHeaderSize(session));
    rt.isHost = host;
    rt.enetPort = port;
    if (!GetLocalConfig().sessionIdValid || !ValidSessionId(session))
        return rt.Fail("invalid session_id: use 1-63 ASCII letters, digits, '_' or '-'");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return rt.Fail("winsock not started");
    rt.winsockStarted = true;
    rt.sendCodec = std::make_unique<send::Codec>();
    if (!rt.sendCodec->Ready()) return rt.Fail("Steam send compression context allocation failed");
    if (!rt.Load()) return false;
    crashlog::RefreshModules();
    char text[1024] = "";
    const int result = api.InitFlat(text);
    if (result != 0) {
        Logf("steam: InitFlat %d: %s", result, text);
        return rt.Fail(result == 2 ? "Steam is not running"
            : result == 3 ? "the Steam client is too old"
            : *text ? text : "Steam did not start");
    }
    rt.apiInitialized = true;
    crashlog::RefreshModules();
    void* clientUtils = api.ClientUtils();
    if (!clientUtils || api.AppId(clientUtils) != GothicAppId)
        return rt.Fail("Steam initialized with an unexpected game AppID");
    Logf("steam: initialized Gothic 3 AppID %u", GothicAppId);
    api.DispatchInit();
    rt.pipe = api.GetHSteamPipe();
    rt.user = api.User();
    rt.matchmaking = api.Matchmaking();
    rt.friends = api.Friends();
    rt.networkingUtils = api.Utils();
    if (!rt.ConfigureNetworking()) return rt.Fail("Steam networking capacity configuration failed; see steam-send config");
    rt.TransportConfigDiagnostics();
    rt.messages = api.Messages();
    rt.sockets = api.Sockets ? api.Sockets() : nullptr;
    rt.socketsLaneTransport = rt.sockets && api.CreateListenSocketP2P && api.ConnectP2P && api.AcceptConnection &&
                              api.CloseConnection && api.CloseListenSocket && api.ConfigureConnectionLanes &&
                              api.SendMessages && api.ReceiveOnConnection && api.AllocateMessage;
    char directEnv[16] = {};
    const DWORD directEnvLen = GetEnvironmentVariableA("G3T_DIRECT_UDP", directEnv, sizeof(directEnv));
    if (directEnvLen > 0 && directEnv[0] == '0') rt.directUdpEnabled = false;
    else if (directEnvLen > 0 && directEnv[0] == '1') rt.directUdpEnabled = rt.enetPort != 0;
    else rt.directUdpEnabled = (api.FakeSteamAbi == nullptr) && (rt.enetPort != 0);
    if (!rt.user || !rt.matchmaking || !rt.friends || !rt.messages) return rt.Fail("Steam interfaces missing");
    if (rt.networkingUtils) api.InitRelayNetworkAccess(rt.networkingUtils);
    if (rt.socketsLaneTransport && rt.isHost) {
        rt.listenSocketP2P = api.CreateListenSocketP2P(rt.sockets, P2PVirtualPort, 0, nullptr);
        if (rt.listenSocketP2P) {
            rt.ConfigureSocketOptions(config::ListenSocketScope, static_cast<intptr_t>(rt.listenSocketP2P));
            Logf("steam-send: ISteamNetworkingSockets v012 P2P listener active vport=%d lanes=%d ice=all",
                 P2PVirtualPort, LaneCount);
        } else {
            rt.socketsLaneTransport = false;
            Log("steam-send: CreateListenSocketP2P failed; falling back to ISteamNetworkingMessages");
        }
    } else if (rt.socketsLaneTransport) {
        Logf("steam-send: ISteamNetworkingSockets v012 P2P client lanes=%d ready", LaneCount);
    }
    Logf("steam: transport capabilities direct_udp=%u sockets_v012_lanes=%u fake_steam=%u enet_port=%u",
         rt.directUdpEnabled ? 1u : 0u, rt.socketsLaneTransport ? 1u : 0u, api.FakeSteamAbi ? 1u : 0u,
         static_cast<unsigned>(rt.enetPort));
    rt.me = api.UserSteamId(rt.user);
    if (!rt.me) return rt.Fail("Steam did not provide a SteamID64");
    char const* steamName = api.PersonaName(rt.friends);
    if (!steamName || !*steamName) return rt.Fail("Steam did not provide a player name");
    CopyName(rt.persona, sizeof(rt.persona), steamName);
    CopyName(rt.playerName, sizeof(rt.playerName), steamName);
    if (!*rt.playerName) return rt.Fail("Steam player name does not fit the network protocol");
    rt.ready = true;
    Logf("steam: ready as Gothic 3 (%u), %s (%llu), %s", GothicAppId, rt.persona, rt.me, rt.isHost ? "host" : "client");
    auto const connectionMode = GetLocalConfig().connectionAuto ? connect::Mode::Auto : connect::Mode::Invite;
    rt.connectionMode = connectionMode;
    auto& hostFlow = rt.hostFlow;
    if (connectionMode == connect::Mode::Invite)
        Log("connect: mode=invite: nothing connects, searches the friends list or opens a lobby by itself; it all starts when a Steam invite / Join Game is accepted");
    else
        Log("connect: mode=auto (legacy): the host opens a lobby at startup and the client searches its friends list by itself");
    if (rt.isHost) {
        const connect::HostFlow::State before = hostFlow.state();
        const unsigned fx = hostFlow.Ready(connectionMode, Now());
        if (connectionMode == connect::Mode::Invite)
            Logf("connect: host ready: no lobby until a friend accepts the invite and its join request arrives; session_id=%s protocol=%u",
                 session, static_cast<unsigned>(saving::Protocol));
        rt.AfterHost("steam ready", before, fx);
    } else {
        rt.relay = LoopbackSocket(&rt.relayPort);
        if (rt.relay == INVALID_SOCKET) {
            rt.ready = false;
            return rt.Fail("no local relay socket");
        }
        rt.wantedLobby = _strtoui64(GetLocalConfig().steamLobby, nullptr, 10);
        Logf("steam: relay on 127.0.0.1:%u%s", rt.relayPort, rt.wantedLobby ? " (steam_lobby set)" : "");
        rt.clientFlow.Configure(rt.me, session);
        rt.clientFlow.Ready();
        rt.StartupAccept();
    }
    store.stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    store.wakeEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!store.stopEvent || !store.wakeEvent) {
        rt.ready = false;
        return rt.Fail("transport events could not be created");
    }
    store.pollTimer = CreateWaitableTimerExA(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!store.pollTimer) {
        const DWORD failure = GetLastError();
        if (failure != ERROR_INVALID_PARAMETER) {
            rt.ready = false;
            return rt.Fail("transport timer could not be created");
        }
        Log("steam: high-resolution timers unavailable on this Windows version; using timed wait (see poll_gap diagnostics)");
    }
    rt.Publish();
    store.worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &TransportThread, nullptr, 0, nullptr));
    if (!store.worker) {
        rt.ready = false;
        return rt.Fail("transport worker could not be started");
    }
    Logf("steam: independent transport worker poll_ms=%u timer=%s; ping=QPC control RTT",
         latency::RelayPollMs, store.pollTimer ? "highres" : "legacy");
    return true;
}

void Pump() { if (store.wakeEvent) SetEvent(store.wakeEvent); }

void Stop()
{
    if (store.worker) {
        SetEvent(store.stopEvent);
        if (WaitForSingleObject(store.worker, INFINITE) != WAIT_OBJECT_0) {
            Logf("steam: transport worker join failed (Windows error %lu); resources retained", GetLastError());
            return;
        }
        CloseHandle(store.worker);
        store.worker = nullptr;
    }
    if (store.wakeEvent) CloseHandle(store.wakeEvent);
    if (store.stopEvent) CloseHandle(store.stopEvent);
    if (store.pollTimer) CloseHandle(store.pollTimer);
    store.wakeEvent = store.stopEvent = store.pollTimer = nullptr;
    SteamFpScope floatingPoint("shutdown");
    if (rt.apiInitialized) {
        if (!rt.isHost && (rt.owner || rt.clientFlow.Active()))
            rt.RecordClientEnd("local transport shutdown", rt.clientFlow.state());
        if (rt.isHost) {
            rt.SetPresence("connect", "");
            rt.SetPresence("status", "");
        }
        rt.Forward();
        rt.LeaveLobby();
        if (rt.listenSocketP2P && rt.sockets && api.CloseListenSocket) {
            api.CloseListenSocket(rt.sockets, rt.listenSocketP2P);
            rt.listenSocketP2P = 0;
        }
        Log("steam: stopped");
    }
    if (rt.relay != INVALID_SOCKET) closesocket(rt.relay);
    rt.relay = INVALID_SOCKET;
    const bool wasInitialized = rt.apiInitialized;
    const bool wasWinsock = rt.winsockStarted;
    rt.sendCodec.reset();
    rt.Reset();
    if (wasInitialized) api.Shutdown();
    if (wasWinsock) WSACleanup();
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.relayCloses.clear();
        store.clientEnet = {};
        store.disconnectRequested.store(false);
        store.published = Published{};
    }
}
}

#ifdef G3T_REALISM_SOURCE
#include "steam_published.cpp"
#include "steam_callbacks.cpp"
#endif
