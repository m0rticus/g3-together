// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
// Steam transport, see steam.h. steam_api.dll (32-bit, Steamworks.NET Standalone 2025.164.1, SHA-256
// fdafb7c65ec7d4182b42bf92cc8671c68e4dc547cc6e613899d2618a47eb65da) is loaded at run time and driven through its flat C
// API with manual callback dispatch: no Steamworks headers or import libraries. The flat API table, the lobby and session
// handling and the hello/welcome/ping link are adapted from Jericho Co-op's native/steam_net.c (MIT, Copyright (c) 2026
// Jericho Co-op contributors, thirdparty/licenses/jericho-coop-LICENSE), which a real two-account test confirmed
// (2026-10-21: the sender identity at offset 12 in the 32-bit DLL, LobbyCreated twice, no overlay in a D3D9 game).
//
// On top of NetworkingMessages:
//   channel 1 (control): 'H' hello (client -> lobby owner until welcomed), 'W' welcome, 'P'+time ping and 'Q'+time pong
//            every second (round trip for the status), 'B' bye. Before a lobby exists (invite-driven connection, see
//            steam_connect.h) the same channel carries the plain ASCII join request/offer/decline frames "G3T1J/L/N".
//   channel 0 (data): one ENet datagram per message, sent unreliable (ENet does its own reliability).
// Nothing connects by itself (connection = invite, the default): the host publishes Join Game rich presence and opens its
// friends-only lobby only for a friend's join request; the client connects only after its player accepts a Steam invite.
// Every step is logged as "connect: ..." (connection = auto keeps the old automatic discovery for the stand-in/tests).
// The relay: a client's ENet connects to a 127.0.0.1 socket of this file, which passes each datagram to the host's
// SteamID. On the host each linked friend gets its own 127.0.0.1 socket that talks to the host's ENet port, so ENet sees
// every friend as a separate local peer (coop-anything's HXCoop pattern) and needs no change. The host's ENet still
// listens only on loopback and admits sockets belonging to linked Steam peers.
#include <winsock2.h>
#include <windows.h>

#include "steam.h"
#include "player_identity.h"
#include "relaywire.h"
#include "saving_policy.h"
#include "steam_connect.h"

#include "config.h"
#include "crashlog.h"
#include "latency.h"
#include "log.h"
#include "net.h"

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <float.h>
#include <mutex>
#include <process.h>
#include <xmmintrin.h>

namespace g3t::steam {
namespace {
using u64 = unsigned long long;
using sbool = unsigned char;  // C++ bool, returned in AL
using HSteamPipe = int;

unsigned short FpuControl()
{
    unsigned short value;
    __asm fnstcw value
    return value;
}

// Real Steam startup changes x87 precision from the engine's single precision to double precision.
class SteamFpScope
{
    unsigned short control = FpuControl();
    unsigned mxcsr = _mm_getcsr();
    char const* operation;
  public:
    explicit SteamFpScope(char const* name) : operation(name)
    {
        const unsigned short libraryControl = static_cast<unsigned short>((control & ~_MCW_PC) | _PC_53);
        __asm fldcw libraryControl
    }
    ~SteamFpScope()
    {
        const unsigned short after = FpuControl();
        const unsigned sseAfter = _mm_getcsr();
        static thread_local unsigned changes = 0;
        if ((after != control || ((sseAfter ^ mxcsr) & ~0x3fu)) && ++changes <= 8)
            Logf("steam: restoring engine FP state after %s: x87=%04x->%04x mxcsr=%08x->%08x",
                 operation, after, control, sseAfter, mxcsr);
        const unsigned short saved = control;
        __asm fldcw saved
        _mm_setcsr(mxcsr);
    }
    SteamFpScope(SteamFpScope const&) = delete;
    SteamFpScope& operator=(SteamFpScope const&) = delete;
};

// The flat API: plain __cdecl functions (the mod's own default is __stdcall, /Gz), the interface first.
struct Api
{
    int(__cdecl* InitFlat)(char* error);
    void(__cdecl* Shutdown)();
    void*(__cdecl* ClientUtils)();
    uint32_t(__cdecl* AppId)(void*);
    HSteamPipe(__cdecl* GetHSteamPipe)();
    void(__cdecl* DispatchInit)();
    void(__cdecl* DispatchRunFrame)(HSteamPipe);
    sbool(__cdecl* DispatchNext)(HSteamPipe, void* message);
    void(__cdecl* DispatchFree)(HSteamPipe);
    sbool(__cdecl* DispatchResult)(HSteamPipe, u64 call, void* buffer, int size, int callback, sbool* failed);
    void*(__cdecl* User)();
    u64(__cdecl* UserSteamId)(void*);
    void*(__cdecl* Matchmaking)();
    u64(__cdecl* CreateLobby)(void*, int type, int maxMembers);
    u64(__cdecl* JoinLobby)(void*, u64 lobby);
    void(__cdecl* LeaveLobby)(void*, u64 lobby);
    sbool(__cdecl* SetLobbyData)(void*, u64 lobby, char const* key, char const* value);
    char const*(__cdecl* GetLobbyData)(void*, u64 lobby, char const* key);
    sbool(__cdecl* RequestLobbyData)(void*, u64 lobby);
    u64(__cdecl* GetLobbyOwner)(void*, u64 lobby);
    int(__cdecl* MemberCount)(void*, u64 lobby);
    u64(__cdecl* MemberByIndex)(void*, u64 lobby, int index);
    void*(__cdecl* Friends)();
    char const*(__cdecl* PersonaName)(void*);
    int(__cdecl* FriendCount)(void*, int flags);
    u64(__cdecl* FriendByIndex)(void*, int index, int flags);
    sbool(__cdecl* FriendGamePlayed)(void*, u64 user, void* info);
    char const*(__cdecl* FriendPersonaName)(void*, u64 user);
    sbool(__cdecl* SetRichPresence)(void*, char const* key, char const* value);
    int(__cdecl* FriendRelationship)(void*, u64 user);  // optional: older stand-ins lack it; 3 = friend
    void*(__cdecl* Messages)();
    int(__cdecl* SendToUser)(void*, void const* identity, void const* data, unsigned size, int flags, int channel);
    int(__cdecl* ReceiveOnChannel)(void*, int channel, void** messages, int max);
    sbool(__cdecl* AcceptSession)(void*, void const* identity);
    sbool(__cdecl* CloseSession)(void*, void const* identity);
    void(__cdecl* IdentitySetSteamId)(void* identity, u64 id);
    u64(__cdecl* IdentityGetSteamId)(void const* identity);
    void(__cdecl* MessageRelease)(void* message);
    void*(__cdecl* Utils)();
    void(__cdecl* InitRelayNetworkAccess)(void*);
} api;

struct Export { void** slot; char const* name; };
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
};

// Callback ids and the 32-bit layouts (playbook, "Struct layouts"): read with memcpy at these offsets.
enum : int {
    CbLobbyJoinRequested = 333,  // GameLobbyJoinRequested_t { u64 lobby; u64 friend; }: an invite accepted in the game
    CbRichJoinRequested = 337,   // GameRichPresenceJoinRequested_t { u64 friend; char connect[256]; }: "Join game"
    CbLobbyInvite = 503,         // LobbyInvite_t { u64 inviter; u64 lobby; u64 game; }: an invite arrived, not accepted yet
    CbLobbyEnter = 504,          // LobbyEnter_t { u64 lobby; u32 perms; bool locked; u32 response at 16; } 1 = success
    CbLobbyDataUpdate = 505,     // LobbyDataUpdate_t { u64 lobby; u64 member; bool success; }
    CbLobbyChatUpdate = 506,     // LobbyChatUpdate_t { u64 lobby; u64 changed; u64 by; u32 state at 24; }
    CbLobbyCreated = 513,        // LobbyCreated_t { int result; u64 lobby at 8; }
    CbCallCompleted = 703,       // SteamAPICallCompleted_t { u64 call; int callback; u32 size; }
    CbSessionRequest = 1251,     // SteamNetworkingMessagesSessionRequest_t { identity at 0; }
    CbSessionFailed = 1252,      // SteamNetworkingMessagesSessionFailed_t { connection info, its identity at 0; }
};
enum : int { SendUnreliable = 0, SendNoNagle = 1, SendReliable = 8, SendAutoRestart = 32 };
enum : int { ResultOk = 1 };
enum : int { ChannelData = 0, ChannelControl = 1 };
constexpr int LobbyFriendsOnly = 1;
constexpr int FriendFlagImmediate = 4;
constexpr uint32_t GothicAppId = 39500;
char const LobbyTag[] = "g3together";
char const LobbyVersion[] = "3";  // compact LE lobby + marker/length + exact ASCII session relay framing
// The transport worker continues during native loads; allow ENet's shorter bounded timeout to decide network loss.
// A friend who quits leaves the lobby, which ends the link at once.
constexpr uint32_t SilenceMs = 180000;
constexpr uint32_t ScanMs = 3000;       // client: how often the friends list is searched for a host
constexpr uint32_t JoinWaitMs = 15000;  // client: a lobby that does not answer the join in this time is skipped for a while
constexpr int MaxRemotes = net::MaxPlayers;

struct Remote
{
    u64 id = 0;
    uint64_t generation = 0;
    char name[24] = {};
    bool linked = false;   // host: said hello while in the lobby; client: the host welcomed us
    bool leaving = false;
    uint32_t heard = 0, lastPing = 0;
    latency::Ping ping;
    SOCKET socket = INVALID_SOCKET;  // host: this friend's 127.0.0.1 socket to the host's ENet port
    unsigned short relayPort = 0;
    unsigned toEnet = 0, toSteam = 0;
};

struct Skipped { u64 lobby; uint32_t until; };  // until 0: for good (not a G3 Together lobby)

bool ready = false, tried = false, winsockStarted = false, apiInitialized = false;
char error[256] = "";
HSteamPipe pipe = 0;
void *user = nullptr, *matchmaking = nullptr, *friends = nullptr, *messages = nullptr;
u64 me = 0;
char persona[64] = "";
char playerName[24] = "";
unsigned sendFailures = 0;
unsigned isolatedDrops = 0;
uint32_t lastSendFailure = 0, lastDiagnostics = 0;

bool isHost = false;
unsigned short enetPort = 0;
u64 lobby = 0, owner = 0;
uint32_t lastCreate = 0;
Remote remotes[MaxRemotes];
uint64_t nextRelayGeneration = 0;
int senderOffset = 0;  // where a message's sender identity sits: 12 in the real 32-bit DLL, 16 by the headers; 0: unknown

// Client
SOCKET relay = INVALID_SOCKET;   // 127.0.0.1:<RelayPort>, the address the client's ENet connects to
unsigned short relayPort = 0;
sockaddr_in enetAddress{};       // where the client's ENet sends from (learnt from its first datagram)
bool haveEnetAddress = false;
u64 wantedLobby = 0;             // from steam_lobby in the INI or +connect_lobby on the command line
bool wantedWaitLogged = false;
u64 pendingLobby = 0;            // an invite or "Join game" accepted through Steam
u64 candidateLobby = 0;          // metadata must match before calling JoinLobby, including invites and explicit numbers
uint32_t candidateAt = 0;
char candidateReason[64] = "";
uint32_t joinedAt = 0, lastScan = 0, lastHello = 0;
char hostName[64] = "";
Skipped skipped[16] = {};
bool searchLogged = false;

// The invite-driven connection (steam_connect.h): both flows live on the transport worker like everything below.
connect::Mode connectionMode = connect::Mode::Invite;
connect::ClientFlow clientFlow;
connect::HostFlow hostFlow;
struct Label { u64 id = 0; uint32_t at = 0; char text[64] = ""; } labels[3];  // display names of the people the flows name
char presenceStatus[96] = "", presenceConnect[256] = "";
bool connectPublished = false;  // host: the Join Game rich presence (connect) was accepted by Steam
uint32_t lastPresence = 0;
u64 requestedHost = 0;          // client: the host a join request went to (its session is closed when the attempt ends)
std::atomic<bool> disconnectRequested{false};  // engine thread -> worker: the ENet connection ended, end the link too

// Only the transport worker owns the API, sockets and remotes after startup.
// Engine-thread readers copy published state, never wait on a Steam call or inspect its mutable buffers.
struct Published
{
    bool ready = false, hostLinked = false;
    uint64_t hostGeneration = 0;
    u64 lobby = 0;
    int linked = 0, ping = -1;
    uint32_t pingSampleAt = 0;
    unsigned short ports[MaxRemotes] = {};
    u64 accounts[MaxRemotes] = {};
    uint64_t generations[MaxRemotes] = {};
    char names[MaxRemotes][24] = {}, playerName[24] = {};
    char hostName[64] = {}, error[256] = {}, status[900] = {};
    Connection connection;
    TrafficCounters traffic;
} published;
std::mutex publishedMutex;
std::vector<RelayDrop> relayDrops;
struct RelayClose { uint64_t account, generation; };
std::vector<RelayClose> relayCloses;
HANDLE worker = nullptr, stopEvent = nullptr, wakeEvent = nullptr, pollTimer = nullptr;
uint32_t lastPump = 0;
latency::Samples pollGaps, pumpWork;
unsigned receiveCaps = 0, forwardCaps = 0, socketFailures = 0, receiveFailures = 0, invalidPongs = 0;
int socketError = 0;
TrafficCounters traffic;

double PingSeconds()
{
    static const LONGLONG frequency = [] { LARGE_INTEGER value; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) / frequency;
}
// Keep relay intervals/ages on QPC too, independent of an occluded game's millisecond timer resolution.
uint32_t Now() { return static_cast<uint32_t>(static_cast<uint64_t>(PingSeconds() * 1000.0)); }

void FillConnection(Connection& out);  // after the Steam helpers below

int CountLinked()
{
    int count = 0;
    for (auto const& r : remotes) count += r.linked ? 1 : 0;
    return count;
}

void Publish()
{
    Published next;
    const uint32_t now = Now();
    next.ready = ready;
    next.lobby = lobby;
    next.linked = CountLinked();
    next.traffic = traffic;
    strcpy_s(next.playerName, playerName);
    next.traffic.envelopeBytes = static_cast<uint32_t>(RelayHeaderSize(GetConfig().sessionId));
    for (auto const& r : remotes)
        if (r.linked && r.ping.Value(now) > next.ping) {
            next.ping = r.ping.Value(now);
            next.pingSampleAt = r.ping.sampledAt;
        }
    strcpy_s(next.hostName, hostName);
    strcpy_s(next.error, error);
    FillConnection(next.connection);
    int n = Format(next.status, "STEAMSTATUS app=%u ready=%d role=%s lobby=%llu owner=%llu host=%s linked=%d ping=%d "
                   "send_failures=%u offset=%d error=%s session_id=%s isolated_drops=%u "
                   "poll_timer=%s poll_gap_avg_ms=%.2f poll_gap_max_ms=%u pump_work_max_ms=%u receive_caps=%u forward_caps=%u "
                   "socket_failures=%u socket_error=%d receive_failures=%u invalid_pongs=%u",
                   GothicAppId, ready ? 1 : 0, isHost ? "host" : "client", lobby, owner, hostName, next.linked, next.ping,
                   sendFailures, senderOffset, *error ? error : "-", GetConfig().sessionId, isolatedDrops,
                   pollTimer ? "highres" : "legacy", pollGaps.Average(), pollGaps.maximum, pumpWork.maximum, receiveCaps, forwardCaps,
                   socketFailures, socketError, receiveFailures, invalidPongs);
    n += Format(next.status + n, sizeof(next.status) - n, " connect_mode=%s connect_state=%s why=%s",
                connectionMode == connect::Mode::Auto ? "auto" : "invite",
                isHost ? connect::StateName(hostFlow.state()) : connect::StateName(clientFlow.state()),
                isHost ? "-" : connect::WhyName(clientFlow.why()));
    for (int i = 0; i < MaxRemotes; ++i) {
        auto const& r = remotes[i];
        if (!r.id) continue;
        if (r.linked) {
            next.ports[i] = r.relayPort; next.accounts[i] = r.id; next.generations[i] = r.generation;
            strcpy_s(next.names[i], r.name);
        }
        if (!isHost && r.id == owner) {
            next.hostLinked = r.linked;
            if (r.linked) next.hostGeneration = r.generation;
        }
        n += Format(next.status + n, sizeof(next.status) - n, " peer=%llu:%d:%u:%u:%d ping_age_ms=%u", r.id,
                    r.linked ? 1 : 0, r.toEnet, r.toSteam, r.ping.Value(now),
                    r.ping.milliseconds < 0 ? 0u : now - r.ping.sampledAt);
    }
    std::lock_guard<std::mutex> lock(publishedMutex);
    published = next;
}

char const* Text(char const* text) { return text ? text : ""; }  // Steam's strings, never null for the CRT

void CopyName(char* out, size_t size, char const* source)
{
    size_t length = std::strlen(Text(source));
    if (length >= size) {
        length = size - 1;
        while (length && (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80) --length;
    }
    std::memcpy(out, Text(source), length);
    for (size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(out[i]) < 32 || out[i] == 127) out[i] = '_';
    out[length] = '\0';
}

void Identity(char (&buffer)[160], u64 id)
{
    std::memset(buffer, 0, sizeof(buffer));  // a SteamNetworkingIdentity is 136 bytes; filled only through SetSteamID64
    api.IdentitySetSteamId(buffer, id);
}

int RawSend(u64 to, int flags, int channel, void const* data, unsigned size)
{
    char packet[RelayMaxEnvelopeSize];
    if (!lobby || size > RelayPayloadLimit) {
        Logf("steam: cannot send relay message (lobby=%llu bytes=%u)", lobby, size);
        return 2;
    }
    std::size_t encodedSize = 0;
    if (!RelayEncode(lobby, GetConfig().sessionId, data, size, packet, sizeof(packet), &encodedSize)) {
        Logf("steam: cannot encode relay message (lobby=%llu bytes=%u)", lobby, size);
        return 2;
    }
    char identity[160];
    Identity(identity, to);
    const int result = api.SendToUser(messages, identity, packet, static_cast<unsigned>(encodedSize),
                                      flags | SendNoNagle | SendAutoRestart, channel);
    if (result != ResultOk) {
        ++sendFailures;
        const uint32_t now = Now();
        if (!lastSendFailure || now - lastSendFailure >= 5000) {
            lastSendFailure = now ? now : 1;
            Logf("steam: message to %llu failed (result=%d channel=%d bytes=%u failures=%u)", to, result, channel, size, sendFailures);
        }
    } else {
        ++traffic.txDatagrams;
        traffic.txBytes += size;
        traffic.txEnvelopeBytes += encodedSize;
        if (channel == ChannelData) {
            ++traffic.txDataDatagrams;
            traffic.txDataBytes += size;
            EnetPacketParse enet;
            if (!ParseEnetDataPacket(data, size, &enet))
                ++traffic.txEnetParseFailures;
            else if (enet.compressed)
                ++traffic.txEnetUnparsedDatagrams;
            else {
                traffic.txEnetProtocolBytes += enet.protocolBytes;
                traffic.txEnetAckBytes += enet.ackBytes;
            }
        }
    }
    return result;
}

void Control(u64 to, char what, bool reliable)
{
    char buffer[9] = {what};
    const double time = PingSeconds();
    std::memcpy(buffer + 1, &time, sizeof(time));
    const int result = RawSend(to, reliable ? SendReliable : SendUnreliable, ChannelControl, buffer, what == 'P' ? 9 : 1);
    if (what == 'P' && result == ResultOk)
        for (auto& r : remotes) if (r.id == to) r.ping.Sent(time);
}

SOCKET LoopbackSocket(unsigned short* port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(local);
    u_long nonBlocking = 1;
    constexpr int RelayBuffer = 4 * 1024 * 1024;
    // Windows reports an earlier datagram to a closed port as WSAECONNRESET on the next receive; turn that off.
    DWORD off = FALSE, bytes = 0;
    constexpr DWORD UdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ioctlsocket(s, FIONBIO, &nonBlocking) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&RelayBuffer), sizeof(RelayBuffer)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&RelayBuffer), sizeof(RelayBuffer)) != 0) {
        Logf("steam: cannot configure the owned loopback relay socket (WSA %d)", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    WSAIoctl(s, UdpConnReset, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    if (port) *port = ntohs(local.sin_port);
    return s;
}

Remote* Find(u64 id)
{
    for (auto& r : remotes) if (r.id == id) return &r;
    return nullptr;
}

Remote* Get(u64 id)
{
    if (Remote* r = Find(id)) return r;
    for (auto& r : remotes)
        if (!r.id) {
            r = Remote{};
            r.id = id;
            r.generation = ++nextRelayGeneration;
            r.heard = Now();
            return &r;
        }
    return nullptr;
}

bool InLobby(u64 id)
{
    const int count = api.MemberCount(matchmaking, lobby);
    for (int i = 0; i < count; ++i)
        if (api.MemberByIndex(matchmaking, lobby, i) == id) return true;
    return false;
}

void LeaveLobby();
void OnClientLinkLost(u64 id, char const* why);
void OnHostLinkLost(u64 id, char const* why);

// publishedMutex must be held: an engine reader must not authenticate an already-closing relay.
void InvalidatePublished(u64 account, uint64_t generation)
{
    for (int i = 0; i < MaxRemotes; ++i) {
        if (published.accounts[i] != account || published.generations[i] != generation) continue;
        published.ports[i] = 0;
        published.accounts[i] = published.generations[i] = 0;
        published.names[i][0] = '\0';
        if (isHost && published.linked > 0) --published.linked;
    }
    if (!isHost && published.hostGeneration == generation) {
        published.hostLinked = false;
        published.hostGeneration = 0;
    }
}

void Drop(Remote* r, char const* why)
{
    if (!r || !r->id) return;
    const u64 id = r->id;
    Logf("steam: %llu dropped (%s; %u datagrams to ENet, %u to Steam)", r->id, why, r->toEnet, r->toSteam);
    if ((isHost && r->relayPort) || (!isHost && r->id == owner && r->linked)) {
        RelayDrop drop;
        drop.account = id;
        drop.generation = r->generation;
        drop.port = r->relayPort;
        strncpy_s(drop.reason, why, _TRUNCATE);
        std::lock_guard<std::mutex> lock(publishedMutex);
        InvalidatePublished(drop.account, drop.generation);
        relayDrops.push_back(drop);
    }
    char identity[160];
    Identity(identity, r->id);
    api.CloseSession(messages, identity);
    if (r->socket != INVALID_SOCKET) closesocket(r->socket);
    const bool wasHost = !isHost && r->id == owner;
    *r = Remote{};
    if (wasHost) LeaveLobby();  // the engine thread consumes the queued relay drop
    if (connectionMode != connect::Mode::Invite) return;
    if (wasHost) OnClientLinkLost(id, why);
    else if (isHost) OnHostLinkLost(id, why);
}

void Skip(u64 id, uint32_t forMs)
{
    Skipped* slot = &skipped[0];
    for (auto& s : skipped) {
        if (s.lobby == id) { slot = &s; break; }
        if (!s.lobby || (s.until && static_cast<int32_t>(s.until - slot->until) < 0)) slot = &s;
    }
    slot->lobby = id;
    slot->until = forMs ? (Now() + forMs) | 1 : 0;
}

bool IsSkipped(u64 id)
{
    for (auto& s : skipped)
        if (s.lobby == id) return !s.until || static_cast<int32_t>(s.until - Now()) > 0;
    return false;
}

// ---- lobby ----

bool CreateLobby()
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

// connection = auto (legacy): the lobby exists from the start and is retried every 15 s.
void CreateLobbyAuto()
{
    if (!CreateLobby()) {
        strcpy_s(error, "Steam rejected the lobby creation request");
        Logf("steam: %s", error);
        return;
    }
    Log("steam: creating a friends-only lobby");
}

void Join(u64 id, char const* why)
{
    LeaveLobby();
    lobby = id;
    owner = 0;
    joinedAt = Now();
    api.JoinLobby(matchmaking, id);
    Logf("steam: joining lobby %llu (%s)", id, why);
}

bool InspectLobby(u64 id, char const* why)
{
    candidateLobby = id;
    candidateAt = Now();
    strncpy_s(candidateReason, why, _TRUNCATE);
    Logf("connect: reading the metadata of lobby %llu (%s)", id, why);
    if (!api.RequestLobbyData(matchmaking, id)) {
        Logf("steam: could not request lobby %llu metadata", id);
        candidateLobby = 0;
        Skip(id, 30000);
        if (connectionMode == connect::Mode::Auto) strcpy_s(error, "Steam could not read the lobby's session ID");
        return false;
    }
    return true;
}

bool MatchesSession(u64 id)
{
    if (std::strcmp(Text(api.GetLobbyData(matchmaking, id, LobbyTag)), LobbyVersion)) return false;
    return !std::strcmp(Text(api.GetLobbyData(matchmaking, id, "session_id")), GetConfig().sessionId);
}

void LeaveLobby()
{
    if (!lobby) return;
    for (auto& r : remotes)
        if (r.id && (isHost || r.id == owner)) {
            if ((isHost && r.relayPort) || (!isHost && r.id == owner && r.linked)) {
                RelayDrop drop;
                drop.account = r.id;
                drop.generation = r.generation;
                drop.port = r.relayPort;
                strcpy_s(drop.reason, "lobby closed");
                std::lock_guard<std::mutex> lock(publishedMutex);
                InvalidatePublished(drop.account, drop.generation);
                relayDrops.push_back(drop);
            }
            Control(r.id, 'B', true);
            char identity[160];
            Identity(identity, r.id);
            api.CloseSession(messages, identity);
            if (r.socket != INVALID_SOCKET) closesocket(r.socket);
            r = Remote{};
        }
    api.LeaveLobby(matchmaking, lobby);
    if (isHost && connectionMode == connect::Mode::Auto) {  // legacy: its Join Game string names this lobby (invite mode keeps its own)
        api.SetRichPresence(friends, "connect", "");
        api.SetRichPresence(friends, "status", "");
    }
    Logf("steam: left lobby %llu", lobby);
    lobby = owner = 0;
    hostName[0] = '\0';
    searchLogged = false;
}

// ---- invite-driven connection (steam_connect.h) ----

// Display label only (the persona name Steam knows); nobody is ever identified by it.
char const* NameOf(u64 id)
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
    if (!slot->text[0] && (!slot->at || now - slot->at > 2000)) {  // Steam may not know the name yet; ask again later
        slot->at = now ? now : 1;
        CopyName(slot->text, sizeof(slot->text), api.FriendPersonaName(friends, id));
    }
    return slot->text[0] ? slot->text : "a friend";
}

bool IsFriend(u64 id)
{
    if (!id || id == me) return false;
    if (api.FriendRelationship) return api.FriendRelationship(friends, id) == 3;  // k_EFriendRelationshipFriend
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i)
        if (api.FriendByIndex(friends, i, FriendFlagImmediate) == id) return true;
    return false;
}

u64 FriendByName(char const* name)
{
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        const u64 id = api.FriendByIndex(friends, i, FriendFlagImmediate);
        if (!_stricmp(Text(api.FriendPersonaName(friends, id)), name)) return id;
    }
    return 0;
}

// A negotiation frame (steam_connect.h): plain ASCII on the control channel, reliable, before any lobby exists.
int SendFrame(u64 to, char const* what, char const* frame, size_t size)
{
    char identity[160];
    Identity(identity, to);
    const int result = api.SendToUser(messages, identity, frame, static_cast<unsigned>(size),
                                      SendReliable | SendNoNagle | SendAutoRestart, ChannelControl);
    Logf("connect: sent %s to %llu (%s) result=%d%s", what, to, NameOf(to), result, result == ResultOk ? "" : " (Steam did not accept the message)");
    return result;
}

bool SetPresence(char const* key, char const* value)
{
    if (!friends) return false;
    const bool ok = api.SetRichPresence(friends, key, value) != 0;
    Logf("connect: rich presence %s=\"%s\" ok=%d", key, value, ok ? 1 : 0);
    return ok;
}

// Host: the Join Game rich presence stays published for the whole run (also while a partner is connected, so the same friend can
// accept again after a drop); only the status text follows the flow.
void PublishPresence()
{
    if (!isHost || connectionMode != connect::Mode::Invite || !apiInitialized) return;
    lastPresence = Now();
    if (!connectPublished) {
        if (!connect::FormatConnect(presenceConnect, sizeof(presenceConnect), me, GetConfig().sessionId)) {
            strcpy_s(error, "cannot build the Join Game connect string");
            Logf("connect: %s (host %llu, session %s)", error, me, GetConfig().sessionId);
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
    if (std::strcmp(status, presenceStatus) != 0 && SetPresence("status", status)) strcpy_s(presenceStatus, status);
}

void SendJoinRequest(bool resend)
{
    const u64 host = clientFlow.host();
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeJoin(frame, sizeof(frame), GetConfig().sessionId, saving::Protocol);
    if (!host || !size) {
        Logf("connect: cannot send a join request (host=%llu)", host);
        return;
    }
    requestedHost = host;
    SendFrame(host, resend ? "join request (again, no answer yet)" : "join request", frame, size);
}

void TearDownClient()
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

void LogClientChange(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet)
{
    using F = connect::ClientFlow;
    const F::State after = clientFlow.state();
    if (after == F::State::Declined && before != after)
        Logf("connect: client %s -> %s on %s: %s (arg=%llu%s%s)", connect::StateName(before), connect::StateName(after), event,
             connect::WhyName(clientFlow.why()), clientFlow.arg(), *clientFlow.hostSession() ? ", host session " : "", clientFlow.hostSession());
    else if (after != before)
        Logf("connect: client %s -> %s on %s", connect::StateName(before), connect::StateName(after), event);
    else if ((fx & F::Ignored) && !quiet)
        Logf("connect: client %s: %s ignored (nothing changes)", connect::StateName(before), event);
}

void AfterClient(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet = false)
{
    using F = connect::ClientFlow;
    LogClientChange(event, before, fx, quiet);
    if (fx & F::Teardown) TearDownClient();
    if (fx & F::SendRequest) SendJoinRequest(false);
    if (fx & F::InspectLobby) {
        const u64 inspected = clientFlow.lobby();
        if (!InspectLobby(inspected, "lobby invite")) {
            const F::State pending = clientFlow.state();
            AfterClient("lobby metadata request failed", pending, clientFlow.LobbyChecked(inspected, false, Now()));
        }
    }
    if (fx & F::JoinLobby) {
        Logf("connect: JoinLobby(%llu)", clientFlow.lobby());
        Join(clientFlow.lobby(), "accepted invite");
    }
}

void SendOffer(u64 partner)
{
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeOffer(frame, sizeof(frame), lobby, GetConfig().sessionId);
    if (!lobby || !size) {
        Logf("connect: cannot send the lobby offer (lobby=%llu)", lobby);
        return;
    }
    SendFrame(partner, "lobby offer", frame, size);
}

void SendDecline(u64 to, connect::Why why, u64 arg)
{
    char frame[connect::FrameMax + 1];
    const size_t size = connect::EncodeDecline(frame, sizeof(frame), why, GetConfig().sessionId, arg);
    Logf("connect: declining %llu (%s): %s", to, NameOf(to), connect::WhyName(why));
    if (!to || !size) return;
    SendFrame(to, "decline", frame, size);
}

void AfterHost(char const* event, connect::HostFlow::State before, unsigned fx, bool quiet = false)
{
    using H = connect::HostFlow;
    const H::State after = hostFlow.state();
    if (after != before)
        Logf("connect: host %s -> %s on %s", connect::StateName(before), connect::StateName(after), event);
    else if ((fx & H::Ignored) && !quiet)
        Logf("connect: host %s: %s ignored (nothing changes)", connect::StateName(before), event);
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

void DescribeTarget(connect::Target const& target, char (&out)[160])
{
    if (target.kind == connect::Target::Kind::Host) Format(out, "host %llu, session %s", target.id, target.session);
    else if (target.kind == connect::Target::Kind::Lobby) Format(out, "lobby %llu", target.id);
    else Format(out, "%s", target.malformed ? "malformed G3 Together connect string" : "not a G3 Together connect string");
}

// The player accepted something in Steam (or the INI/command line says so): the only way a client starts connecting.
void OnAccepted(connect::ClientFlow::Source source, char const* what, u64 friendId, char const* raw)
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

void OnLobbyInvite(u64 inviter, u64 lobbyId)
{
    Logf("connect: lobby invite from %llu (%s) lobby=%llu: not joining by itself, the player has to accept it in Steam",
         inviter, NameOf(inviter), lobbyId);
    if (isHost || connectionMode != connect::Mode::Invite) return;
    const connect::ClientFlow::State before = clientFlow.state();
    AfterClient("lobby invite", before, clientFlow.LobbyInvite(inviter, lobbyId));
}

void OnJoinRequest(u64 from, connect::Frame const& frame)
{
    connect::HostFlow::Join join;
    join.from = from;
    join.friendOk = IsFriend(from);
    join.sessionOk = !std::strcmp(frame.session, GetConfig().sessionId);
    join.protocol = frame.protocol;
    Logf("connect: join request from %llu (%s): session=%s protocol=%u friend=%d", from, NameOf(from), frame.session, frame.protocol,
         join.friendOk ? 1 : 0);
    const connect::HostFlow::State before = hostFlow.state();
    AfterHost("join request", before, hostFlow.Request(join, saving::Protocol, Now()));
}

void OnNegotiation(u64 from, char const* data, int size)
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
        if (std::strcmp(frame.session, GetConfig().sessionId)) {
            Logf("connect: offer from %llu names session %s, not ours (%s); ignored", from, frame.session, GetConfig().sessionId);
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

void OnClientLinkLost(u64 id, char const* why)
{
    const connect::ClientFlow::State before = clientFlow.state();
    const unsigned fx = clientFlow.LinkLost(id);
    if (clientFlow.state() == connect::ClientFlow::State::Lost)
        Logf("connect: connection to %llu (%s) ended (%s); nothing reconnects by itself: accept their invite again", id, NameOf(id), why);
    AfterClient("link lost", before, fx);
}

void OnHostLinkLost(u64 id, char const* why)
{
    const connect::HostFlow::State before = hostFlow.state();
    const unsigned fx = hostFlow.LinkLost(id);
    if (fx & connect::HostFlow::Teardown)
        Logf("connect: partner %llu (%s) is gone (%s); lobby closed, waiting for the next invite", id, NameOf(id), why);
    AfterHost("link lost", before, fx);
}

void ClientTick()
{
    const uint32_t now = Now();
    if (clientFlow.ResendDue(now)) SendJoinRequest(true);
    const connect::ClientFlow::State before = clientFlow.state();
    AfterClient("timer", before, clientFlow.Tick(now), true);
}

void HostTick()
{
    const uint32_t now = Now();
    const connect::HostFlow::State before = hostFlow.state();
    AfterHost("timer", before, hostFlow.Tick(now), true);
    if (!connectPublished && now - lastPresence > 5000) PublishPresence();
}

void ParseLegacyConnect(char const* text);

// Client, once Steam is ready. Steam may have started the game with the host's connect string on the command line (Join Game /
// an invite accepted while the game was closed); the INI's steam_lobby / steam_host are explicit manual joins. One attempt.
void StartupAccept()
{
    char const* line = GetCommandLineA();
    Logf("connect: command line: %.300s", line ? line : "");
    if (connectionMode == connect::Mode::Auto) {
        ParseLegacyConnect(line);
        return;
    }
    const connect::Target target = connect::ParseConnect(line);
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
    if (*GetConfig().steamHost) {
        const u64 id = FriendByName(GetConfig().steamHost);
        char text[96];
        if (id && connect::FormatConnect(text, sizeof(text), id, GetConfig().sessionId)) {
            OnAccepted(connect::ClientFlow::Source::Manual, "steam_host in g3together.ini (manual join, one attempt)", id, text);
            return;
        }
        Logf("connect: steam_host '%s' is not on your Steam friends list", GetConfig().steamHost);
    }
    Log("connect: waiting for an invite (accept a Steam invite or Join Game on the host; nothing connects by itself)");
}

void FillConnection(Connection& out)
{
    out.automatic = connectionMode == connect::Mode::Auto;
    out.host = isHost;
    out.hostState = hostFlow.state();
    out.clientState = clientFlow.state();
    out.why = clientFlow.why();
    out.arg = clientFlow.arg();
    strcpy_s(out.hostSession, clientFlow.hostSession());
    out.lobbyInvitePending = !isHost && clientFlow.Idle() && clientFlow.lobbyInvitedBy() != 0;
    const u64 who = isHost ? hostFlow.partner() : out.lobbyInvitePending ? clientFlow.lobbyInvitedBy() : clientFlow.inviter();
    CopyName(out.name, sizeof(out.name), NameOf(who));
}

void OnLobbyCreated(char const* p)
{
    int result = 0;
    u64 id = 0;
    std::memcpy(&result, p, 4);
    std::memcpy(&id, p + 8, 8);
    if (!isHost || (lobby && lobby == id)) return;  // arrives twice on real Steam: as the call result and as a broadcast
    Logf("connect: lobby create result=%d lobby=%llu", result, id);
    const bool invite = connectionMode == connect::Mode::Invite;
    if (invite && hostFlow.state() != connect::HostFlow::State::Opening) {
        // Nobody is waiting for it (the request timed out or was withdrawn): close it again.
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
    if (!api.SetLobbyData(matchmaking, id, "session_id", GetConfig().sessionId) ||
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
    Logf("connect: lobby %llu open (friends-only); session_id=%s (case-sensitive; only matching sessions can connect)", id,
         GetConfig().sessionId);
    if (invite) {
        const connect::HostFlow::State before = hostFlow.state();
        AfterHost("lobby created", before, hostFlow.LobbyCreated(true, result, Now()));
        return;
    }
    char text[64];
    Format(text, "+connect_lobby %llu", id);
    if (!SetPresence("status", "G3 Together") || !SetPresence("connect", text)) Log("steam: could not publish Join game rich presence");
    Logf("steam: lobby %llu open; friends join over Steam (friends list: Join game, or steam_lobby = %llu)", id, id);
}

void OnLobbyEnter(char const* p)
{
    u64 id = 0;
    unsigned response = 0;
    std::memcpy(&id, p, 8);
    std::memcpy(&response, p + 16, 4);
    if (isHost || id != lobby || owner) return;
    const bool joined = response == 1;
    const bool matches = joined && MatchesSession(id);
    Logf("connect: LobbyEnter lobby=%llu response=%u metadata=%s", id, response, !joined ? "-" : matches ? "ok" : "different session_id or transport");
    if (connectionMode == connect::Mode::Invite) {
        const u64 lobbyOwner = joined ? api.GetLobbyOwner(matchmaking, id) : 0;
        const connect::ClientFlow::State before = clientFlow.state();
        const unsigned fx = clientFlow.LobbyEntered(id, joined, matches, lobbyOwner, Now());
        if (fx & connect::ClientFlow::Ignored) {  // not the lobby the flow is joining any more
            api.LeaveLobby(matchmaking, id);
            lobby = 0;
        } else if (!(fx & connect::ClientFlow::Teardown)) {
            error[0] = '\0';
            owner = lobbyOwner;
            CopyName(hostName, sizeof(hostName), api.FriendPersonaName(friends, owner));
            if (Remote* r = Get(owner)) r->heard = Now();
            lastHello = 0;
            Logf("connect: in lobby %llu of %s (%llu); saying hello", id, hostName, owner);
        }
        AfterClient("lobby entered", before, fx);
        return;
    }
    if (!joined || !matches) {
        Logf("steam: lobby %llu refused (response %u, incompatible session ID or transport)", id, response);
        Format(error, response != 1 ? "the Steam lobby is full or closed" : "different session_id or incompatible G3 Together transport");
        api.LeaveLobby(matchmaking, id);
        lobby = 0;
        Skip(id, 30000);
        return;
    }
    error[0] = '\0';
    owner = api.GetLobbyOwner(matchmaking, id);
    CopyName(hostName, sizeof(hostName), api.FriendPersonaName(friends, owner));
    if (Remote* r = Get(owner)) r->heard = Now();
    lastHello = 0;
    Logf("steam: in lobby %llu of %s (%llu); saying hello", id, hostName, owner);
}

// connection = auto: only the legacy "+connect_lobby <id>" is followed, by the friends-list search.
void ParseLegacyConnect(char const* text)
{
    char const* key = text ? std::strstr(text, "+connect_lobby ") : nullptr;
    if (key) pendingLobby = _strtoui64(key + 15, nullptr, 10);
}

void OnCallback(int callback, char const* p, int size)
{
    u64 a = 0, b = 0;
    unsigned state = 0;
    switch (callback) {
    case CbLobbyCreated:
        OnLobbyCreated(p);
        break;
    case CbLobbyEnter:
        OnLobbyEnter(p);
        break;
    case CbLobbyDataUpdate:
        if (size < 17 || isHost) break;
        std::memcpy(&a, p, 8);
        std::memcpy(&b, p + 8, 8);
        if (a != candidateLobby || a != b) break;
        candidateLobby = 0;
        if (connectionMode == connect::Mode::Invite) {
            const bool matches = p[16] && MatchesSession(a);
            Logf("connect: lobby %llu metadata: success=%d g3together=%s session_id=%s (ours %s) -> %s", a, p[16] ? 1 : 0,
                 Text(api.GetLobbyData(matchmaking, a, LobbyTag)), Text(api.GetLobbyData(matchmaking, a, "session_id")),
                 GetConfig().sessionId, matches ? "a G3 Together lobby of this session" : "refused");
            const connect::ClientFlow::State before = clientFlow.state();
            AfterClient("lobby metadata", before, clientFlow.LobbyChecked(a, matches, Now()));
            break;
        }
        if (p[16] && MatchesSession(a)) {
            error[0] = '\0';
            Join(a, candidateReason);
        } else {
            Logf("steam: skipping lobby %llu: different session_id or incompatible transport (wanted %s)", a, GetConfig().sessionId);
            Skip(a, 30000);
            if (wantedLobby == a || !std::strcmp(candidateReason, "accepted through Steam"))
                strcpy_s(error, "different session_id or incompatible G3 Together transport");
            lastScan = Now() - ScanMs;
        }
        break;
    case CbLobbyChatUpdate:
        std::memcpy(&a, p, 8);
        std::memcpy(&b, p + 8, 8);
        std::memcpy(&state, p + 24, 4);
        if (a == lobby && (state & 0x1e)) {
            if (Remote* r = Find(b)) r->leaving = true;
            else if (isHost && connectionMode == connect::Mode::Invite && b == hostFlow.partner())
                OnHostLinkLost(b, "left the lobby before saying hello");  // no relay exists yet to drop
        }
        break;
    case CbLobbyJoinRequested: {  // the player accepted a lobby invite
        char text[64];
        std::memcpy(&a, p, 8);
        if (size >= 16) std::memcpy(&b, p + 8, 8);
        Format(text, "+connect_lobby %llu", a);
        OnAccepted(connect::ClientFlow::Source::LobbyJoin, "invite accepted (GameLobbyJoinRequested_t 333)", b, text);
        break;
    }
    case CbRichJoinRequested: {  // the player pressed Join Game / accepted an invite while the game runs
        char text[257] = {};
        if (size < 8) break;
        std::memcpy(&a, p, 8);
        std::memcpy(text, p + 8, size - 8 < 256 ? static_cast<size_t>(size - 8) : 256);
        OnAccepted(connect::ClientFlow::Source::RichJoin, "Join Game accepted (GameRichPresenceJoinRequested_t 337)", a, text);
        break;
    }
    case CbLobbyInvite:
        if (size < 16) break;
        std::memcpy(&a, p, 8);
        std::memcpy(&b, p + 8, 8);
        OnLobbyInvite(a, b);
        break;
    case CbSessionRequest: {
        a = api.IdentityGetSteamId(p);
        // Host: members of its lobby, and (invite mode) Steam friends, who may only ask to join. Strangers never get a session
        // (code review 28.10. P2: anyone's session was accepted and took one of the slots).
        char const* accepted = nullptr;
        if (isHost && lobby && InLobby(a)) accepted = "lobby member";
        else if (!isHost && owner && a == owner) accepted = "the host";
        else if (!isHost && connectionMode == connect::Mode::Invite && clientFlow.host() == a && clientFlow.Active()) accepted = "the host we asked to join";
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
        a = api.IdentityGetSteamId(p);
        Logf("connect: session with %llu (%s) failed; dropping its relay", a, NameOf(a));
        if (!isHost && connectionMode == connect::Mode::Invite) {
            const connect::ClientFlow::State before = clientFlow.state();
            AfterClient("session failed", before, clientFlow.SessionFailed(a));
        }
        Drop(Find(a), "session failed");
        break;
    default:
        break;
    }
}

// ---- messages ----

void OnControl(Remote& r, char const* data, int size)
{
    if (!data || size <= 0) return;
    switch (data[0]) {
    case 'H':
        if (size != 1) break;
        if (isHost && lobby && !r.linked && InLobby(r.id)) {
            if (connectionMode == connect::Mode::Invite && !hostFlow.Admit(r.id)) {
                Logf("connect: hello from %llu (%s) refused: not the invited partner", r.id, NameOf(r.id));
                break;
            }
            if (CountLinked() + 1 >= net::MaxPlayers) {
                Logf("steam: %llu refused, the game is full", r.id);
                break;
            }
            r.socket = LoopbackSocket(&r.relayPort);
            if (r.socket == INVALID_SOCKET) {
                Logf("steam: no local socket for %llu (%d)", r.id, WSAGetLastError());
                break;
            }
            r.linked = true;
            CopyName(r.name, sizeof(r.name), api.FriendPersonaName(friends, r.id));
            Logf("steam: %s (%llu) linked to this game", Text(api.FriendPersonaName(friends, r.id)), r.id);
            if (connectionMode == connect::Mode::Invite) {
                const connect::HostFlow::State before = hostFlow.state();
                AfterHost("hello", before, hostFlow.Linked(r.id));
                Logf("connect: connected: partner %llu (%s) is linked to this game", r.id, NameOf(r.id));
            }
        }
        if (r.linked) Control(r.id, 'W', true);
        break;
    case 'W':
        if (size != 1) break;
        if (!isHost && r.id == owner && !r.linked) {
            r.linked = true;
            Logf("steam: welcomed by the host %s; ENet goes over Steam now", hostName);
            if (connectionMode == connect::Mode::Invite) {
                const connect::ClientFlow::State before = clientFlow.state();
                AfterClient("welcomed", before, clientFlow.Welcomed(r.id));
                Logf("connect: connected: welcomed by host %llu (%s)", r.id, hostName);
            }
        }
        break;
    case 'P':
        if (size == 9) {
            char pong[9];
            std::memcpy(pong, data, 9);
            pong[0] = 'Q';
            RawSend(r.id, SendUnreliable, ChannelControl, pong, 9);
        }
        break;
    case 'Q':
        if (size == 9) {
            double sent = 0;
            std::memcpy(&sent, data + 1, 8);
            if (!r.ping.Reply(sent, PingSeconds(), Now())) ++invalidPongs;
        }
        break;
    case 'B':
        if (size == 1) Drop(&r, "said bye");
        break;
    default:
        break;
    }
}

void OnData(Remote& r, char const* data, int size)
{
    if (!r.linked || size <= 0) return;
    int sent = 0;
    if (isHost) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(enetPort);
        sent = sendto(r.socket, data, size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    } else if (haveEnetAddress) {
        sent = sendto(relay, data, size, 0, reinterpret_cast<sockaddr*>(&enetAddress), sizeof(enetAddress));
    } else return;
    if (sent != size) { ++socketFailures; socketError = WSAGetLastError(); return; }
    ++r.toEnet;
}

u64 Sender(char const* message)
{
    if (senderOffset) return api.IdentityGetSteamId(message + senderOffset);
    // GetSteamID64 answers 0 unless the identity's type field says SteamID, so the wrong offset reads 0 (Jericho's order).
    const u64 at16 = api.IdentityGetSteamId(message + 16), at12 = api.IdentityGetSteamId(message + 12);
    if (at16 || at12) {
        senderOffset = at16 ? 16 : 12;
        Logf("steam: a message's sender identity is at offset %d", senderOffset);
    }
    return at16 ? at16 : at12;
}

void Receive(int channel)
{
    void* batch[32];
    for (int rounds = 0; rounds < 16; ++rounds) {
        const int count = api.ReceiveOnChannel(messages, channel, batch, 32);
        if (count < 0) ++receiveFailures;
        if (count <= 0) return;
        for (int i = 0; i < count; ++i) {
            char const* m = static_cast<char const*>(batch[i]);
            char const* data = nullptr;
            int size = 0;
            std::memcpy(&data, m, sizeof(data));  // SteamNetworkingMessage_t (32-bit): data at 0, size at 4
            std::memcpy(&size, m + 4, sizeof(size));
            const u64 from = Sender(m);
            void const* payload = nullptr;
            std::size_t payloadSize = 0;
            const bool relayFrame = lobby && size >= 0 && RelayDecode(data, static_cast<std::size_t>(size), lobby,
                                                                      GetConfig().sessionId, &payload, &payloadSize);
            if (!relayFrame && channel == ChannelControl && connect::IsFrame(data, size < 0 ? 0 : static_cast<std::size_t>(size))) {
                OnNegotiation(from, data, size);  // join request / offer / decline: no lobby (or a different one) yet
                api.MessageRelease(batch[i]);
                continue;
            }
            if (!relayFrame) {
                ++isolatedDrops;
                api.MessageRelease(batch[i]);
                continue;
            }
            ++traffic.rxDatagrams;
            traffic.rxBytes += payloadSize;
            traffic.rxEnvelopeBytes += static_cast<std::size_t>(size);
            data = static_cast<char const*>(payload);
            size = static_cast<int>(payloadSize);
            // Host: a slot only for a member of its lobby (checked before Get reserves one), or one it has already.
            Remote* r = !from || (!isHost && from != owner) ? nullptr : isHost && !Find(from) && !(lobby && InLobby(from)) ? nullptr : Get(from);
            if (r && size > 0) {
                r->heard = Now();
                if (channel == ChannelControl) OnControl(*r, data, size);
                else OnData(*r, data, size);
            }
            api.MessageRelease(batch[i]);
        }
    }
    ++receiveCaps;
}

// ENet -> Steam: what ENet sent to the relay sockets since the last Pump.
void Forward()
{
    char datagram[4096];
    sockaddr_in from{};
    int fromSize = sizeof(from);
    if (isHost) {
        for (auto& r : remotes) {
            if (!r.linked || r.socket == INVALID_SOCKET) continue;
            for (int n = 0; n < 256; ++n) {
                fromSize = sizeof(from);
                const int size = recvfrom(r.socket, datagram, sizeof(datagram), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
                if (size < 0 && WSAGetLastError() == WSAECONNRESET) continue;
                if (size <= 0) {
                    if (size < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                        ++socketFailures; socketError = WSAGetLastError();
                    }
                    break;
                }
                if (ntohs(from.sin_port) != enetPort) continue;
                RawSend(r.id, SendUnreliable, ChannelData, datagram, static_cast<unsigned>(size));
                ++r.toSteam;
                if (n == 255) ++forwardCaps;
            }
        }
        return;
    }
    if (relay == INVALID_SOCKET) return;
    Remote* host = owner ? Find(owner) : nullptr;
    for (int n = 0; n < 256; ++n) {
        fromSize = sizeof(from);
        const int size = recvfrom(relay, datagram, sizeof(datagram), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (size < 0 && WSAGetLastError() == WSAECONNRESET) continue;
        if (size <= 0) {
            if (size < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                ++socketFailures; socketError = WSAGetLastError();
            }
            break;
        }
        enetAddress = from;
        haveEnetAddress = true;
        if (!host || !host->linked) continue;  // no host yet: ENet keeps trying to connect until there is one
        RawSend(owner, SendUnreliable, ChannelData, datagram, static_cast<unsigned>(size));
        ++host->toSteam;
        if (n == 255) ++forwardCaps;
    }
}

// Client without a lobby: an accepted invite, the INI's lobby, else the first friend hosting a G3 Together game.
void Search()
{
    if (lobby) {
        if (!owner && Now() - joinedAt > JoinWaitMs) {
            Logf("steam: lobby %llu did not answer the join; trying others", lobby);
            Skip(lobby, 30000);
            api.LeaveLobby(matchmaking, lobby);
            lobby = 0;
        }
        return;
    }
    if (candidateLobby) {
        if (Now() - candidateAt > JoinWaitMs) {
            Logf("steam: lobby %llu did not answer the metadata request", candidateLobby);
            Skip(candidateLobby, 30000);
            candidateLobby = 0;
            strcpy_s(error, "Steam did not answer the lobby session ID request");
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
    // steam_lobby set: that lobby only. Refused or silent, it is tried again after its pause; the friends' games are not
    // searched instead (code review 28.10. P2: that could join another friend's host).
    if (wantedLobby) {
        if (!IsSkipped(wantedLobby)) InspectLobby(wantedLobby, "steam_lobby");
        else if (!wantedWaitLogged) {
            wantedWaitLogged = true;
            Logf("steam: steam_lobby %llu refused or did not answer; waiting for it (no other host)", wantedLobby);
        }
        return;
    }
    char const* wantedHost = GetConfig().steamHost;
    const int count = api.FriendCount(friends, FriendFlagImmediate);
    for (int i = 0; i < count; ++i) {
        const u64 id = api.FriendByIndex(friends, i, FriendFlagImmediate);
        // FriendGameInfo_t { CGameID game; u32 ip; u16 port, queryPort; CSteamID lobby at 16; }
        struct { u64 game; uint32_t ip; uint16_t port, queryPort; u64 lobby; } info{};
        if (!api.FriendGamePlayed(friends, id, &info) || (info.game & 0xffffff) != GothicAppId || !info.lobby) continue;
        if (IsSkipped(info.lobby)) continue;
        char const* name = Text(api.FriendPersonaName(friends, id));
        uint64_t wantedId = 0;
        if (*wantedHost && (identity::SteamId(wantedHost, wantedId) ? id != wantedId : _stricmp(name, wantedHost) != 0)) continue;
        InspectLobby(info.lobby, *name ? name : "a friend");
        return;
    }
    if (!searchLogged) {
        Logf("steam: no friend hosting a G3 Together game yet%s%s; looking every %u s", *wantedHost ? " named " : "", wantedHost,
             ScanMs / 1000);
        searchLogged = true;
    }
}

bool Fail(char const* text)
{
    strncpy_s(error, text, _TRUNCATE);
    Logf("steam: not used: %s", error);
    Publish();
    return false;
}

void PumpOnce();
unsigned __stdcall TransportThread(void*)
{
    for (;;) {
        const DWORD stop = WaitForSingleObject(stopEvent, 0);
        if (stop == WAIT_OBJECT_0) break;
        if (stop == WAIT_FAILED) {
            ready = false;
            Fail("transport stop event failed");
            break;
        }
        crashlog::PollWorldEntry();
        PumpOnce();
        HANDLE events[] = {stopEvent, wakeEvent, pollTimer};
        if (pollTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(latency::RelayPollMs) * 10000;  // relative 100 ns units
            if (!SetWaitableTimer(pollTimer, &due, 0, nullptr, nullptr, FALSE)) {
                ready = false;
                Fail("transport timer could not be armed");
                break;
            }
        }
        const DWORD result = WaitForMultipleObjects(pollTimer ? 3 : 2, events, FALSE, pollTimer ? INFINITE : latency::RelayPollMs);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) {
            ready = false;
            Fail("transport wait failed");
            break;
        }
    }
    return 0;
}

// <game>\G3Together\steam_api.dll (the mod's own copy, never one the game may have), or G3T_STEAM_DLL (QA: the
// stand-in tools/fakesteam, Steam over 127.0.0.1).
bool Load()
{
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("G3T_STEAM_DLL", path, MAX_PATH) == 0 || !*path) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&Load), &self) ||
            !GetModuleFileNameA(self, path, MAX_PATH))
            return Fail("no game folder");
        for (int part = 0; part != 2; ++part) {
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
        if (!function) Logf("steam: optional %s not exported by this steam_api.dll; friendship is checked through the friends list instead", e.name);
    }
    Logf("steam: loaded %s", path);
    return true;
}
}

bool Start(bool host, unsigned short port)
{
    if (tried) return Ready();
    SteamFpScope floatingPoint("startup");
    tried = true;
    traffic = {};
    traffic.envelopeBytes = static_cast<uint32_t>(RelayHeaderSize(GetConfig().sessionId));
    isHost = host;
    enetPort = port;
    if (!GetConfig().sessionIdValid || !ValidSessionId(GetConfig().sessionId))
        return Fail("invalid session_id: use 1-63 ASCII letters, digits, '_' or '-'");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return Fail("winsock not started");
    winsockStarted = true;
    if (!Load()) return false;
    char text[1024] = "";
    const int result = api.InitFlat(text);
    if (result != 0) {
        Logf("steam: InitFlat %d: %s", result, text);
        return Fail(result == 2 ? "Steam is not running" : result == 3 ? "the Steam client is too old" : *text ? text : "Steam did not start");
    }
    apiInitialized = true;
    void* clientUtils = api.ClientUtils();
    if (!clientUtils || api.AppId(clientUtils) != GothicAppId) return Fail("Steam initialized with an unexpected game AppID");
    Logf("steam: initialized Gothic 3 AppID %u", GothicAppId);
    api.DispatchInit();
    pipe = api.GetHSteamPipe();
    user = api.User();
    matchmaking = api.Matchmaking();
    friends = api.Friends();
    messages = api.Messages();
    if (!user || !matchmaking || !friends || !messages) return Fail("Steam interfaces missing");
    if (void* utils = api.Utils()) api.InitRelayNetworkAccess(utils);  // the relay network is ready by the first message
    me = api.UserSteamId(user);
    if (!me) return Fail("Steam did not provide a SteamID64");
    char const* steamName = api.PersonaName(friends);
    if (!steamName || !*steamName) return Fail("Steam did not provide a player name");
    CopyName(persona, sizeof(persona), steamName);
    CopyName(playerName, sizeof(playerName), steamName);
    if (!*playerName) return Fail("Steam player name does not fit the network protocol");
    ready = true;
    Logf("steam: ready as Gothic 3 (%u), %s (%llu), %s", GothicAppId, persona, me, isHost ? "host" : "client");
    connectionMode = GetConfig().connectionAuto ? connect::Mode::Auto : connect::Mode::Invite;
    if (connectionMode == connect::Mode::Invite)
        Log("connect: mode=invite: nothing connects, searches the friends list or opens a lobby by itself; it all starts when a Steam invite / Join Game is accepted");
    else
        Log("connect: mode=auto (legacy): the host opens a lobby at startup and the client searches its friends list by itself");
    if (isHost) {
        const connect::HostFlow::State before = hostFlow.state();
        const unsigned fx = hostFlow.Ready(connectionMode, Now());
        if (connectionMode == connect::Mode::Invite)
            Logf("connect: host ready: no lobby until a friend accepts the invite and its join request arrives; session_id=%s protocol=%u",
                 GetConfig().sessionId, static_cast<unsigned>(saving::Protocol));
        AfterHost("steam ready", before, fx);
    } else {
        relay = LoopbackSocket(&relayPort);
        if (relay == INVALID_SOCKET) {
            ready = false;
            return Fail("no local relay socket");
        }
        wantedLobby = _strtoui64(GetConfig().steamLobby, nullptr, 10);
        Logf("steam: relay on 127.0.0.1:%u%s", relayPort, wantedLobby ? " (steam_lobby set)" : "");
        clientFlow.Configure(me, GetConfig().sessionId);
        clientFlow.Ready();
        StartupAccept();
    }
    stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    wakeEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent || !wakeEvent) {
        ready = false;
        return Fail("transport events could not be created");
    }
    // Unlike a timed wait/timeBeginPeriod, this retains sub-frame scheduling for an occluded Windows 11 game.
    pollTimer = CreateWaitableTimerExA(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!pollTimer) {
        const DWORD failure = GetLastError();
        if (failure != ERROR_INVALID_PARAMETER) {
            ready = false;
            return Fail("transport timer could not be created");
        }
        Log("steam: high-resolution timers unavailable on this Windows version; using timed wait (see poll_gap diagnostics)");
    }
    Publish();
    worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &TransportThread, nullptr, 0, nullptr));
    if (!worker) {
        ready = false;
        return Fail("transport worker could not be started");
    }
    Logf("steam: independent transport worker poll_ms=%u timer=%s; ping=QPC control RTT",
         latency::RelayPollMs, pollTimer ? "highres" : "legacy");
    return true;
}

unsigned short RelayPort() { return relayPort; }

bool IsPeerRelay(unsigned short port)
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    if (!published.ready || !isHost || !port) return false;
    for (auto const linkedPort : published.ports) if (linkedPort == port) return true;
    return false;
}

uint64_t PeerId(unsigned short port)
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    if (!published.ready || !isHost || !port) return 0;
    for (int i = 0; i < MaxRemotes; ++i)
        if (published.ports[i] == port) return published.accounts[i];
    return 0;
}
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24])
{
    uint64_t generation = 0;
    return PeerIdentity(port, account, name, generation);
}
namespace {
bool ReadPeerIdentityLocked(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!published.ready || !isHost || !port) return false;
    for (int i = 0; i < MaxRemotes; ++i)
        if (published.ports[i] == port && published.accounts[i]) {
            account = published.accounts[i];
            generation = published.generations[i];
            strcpy_s(name, published.names[i]);
            return true;
        }
    return false;
}
}
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return ReadPeerIdentityLocked(port, account, name, generation);
}
bool ClaimPeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    if (!ReadPeerIdentityLocked(port, account, name, generation) || account == me) return false;
    relayCloses.erase(std::remove_if(relayCloses.begin(), relayCloses.end(),
        [account, generation](RelayClose const& request) {
            return request.account == account && request.generation == generation;
        }), relayCloses.end());
    return true;
}

std::vector<RelayDrop> TakeRelayDrops()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    std::vector<RelayDrop> result;
    result.swap(relayDrops);
    return result;
}

void RequestHostRelayClose(uint64_t account, uint64_t generation)
{
    if (!account || !generation) return;
    {
        std::lock_guard<std::mutex> lock(publishedMutex);
        relayCloses.push_back({account, generation});
    }
    if (wakeEvent) SetEvent(wakeEvent);
}

namespace {
void PumpOnce()
{
    if (!ready) return;
    const uint32_t began = Now();
    if (lastPump) pollGaps.Add(began - lastPump);
    lastPump = began;
    SteamFpScope floatingPoint("callbacks");
    api.DispatchRunFrame(pipe);
    struct { int user; int callback; char* param; int size; } message{};  // CallbackMsg_t
    while (api.DispatchNext(pipe, &message)) {
        if (message.callback == CbCallCompleted) {
            u64 call = 0;
            int callback = 0;
            unsigned size = 0;
            std::memcpy(&call, message.param, 8);
            std::memcpy(&callback, message.param + 8, 4);
            std::memcpy(&size, message.param + 12, 4);
            char result[512];
            sbool failed = 0;
            if (size <= sizeof(result) && api.DispatchResult(pipe, call, result, static_cast<int>(size), callback, &failed) && !failed)
                OnCallback(callback, result, static_cast<int>(size));
        } else {
            OnCallback(message.callback, message.param, message.size);
        }
        api.DispatchFree(pipe);
    }
    std::vector<RelayClose> closes;
    {
        std::lock_guard<std::mutex> lock(publishedMutex);
        closes.swap(relayCloses);
        for (auto const& close : closes)
            InvalidatePublished(close.account, close.generation);
    }
    if (isHost) for (auto const& close : closes)
        if (Remote* r = Find(close.account); r && r->generation == close.generation)
            Drop(r, "ENet connection ended");
    if (isHost && !lobby && connectionMode == connect::Mode::Auto && Now() - lastCreate > 15000)
        CreateLobbyAuto();  // the first try failed: again every 15 s (legacy mode only)
    if (disconnectRequested.exchange(false) && !isHost && connectionMode == connect::Mode::Invite && clientFlow.Active())
        OnClientLinkLost(clientFlow.host(), "the ENet connection ended");
    if (!isHost) {
        if (connectionMode == connect::Mode::Auto) Search();
        else ClientTick();
    } else if (connectionMode == connect::Mode::Invite) {
        HostTick();
    }
    // Data before control: a friend who quits sends ENet's goodbye and then 'B'; the other order closed its socket first.
    Receive(ChannelData);
    Receive(ChannelControl);
    // Lobby departure callbacks can precede a queued ENet goodbye. Drain it before closing that peer's relay.
    for (auto& r : remotes)
        if (r.leaving) Drop(&r, "left the lobby");
    Forward();
    const uint32_t now = Now();
    static uint32_t lastNames = 0;
    if (now - lastNames >= 1000) {
        lastNames = now;
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
    }
    for (auto& r : remotes) {
        if (!r.id) continue;
        // A stranger's session (any Steam user may write to a host) or a hello that never got through goes after 60 s.
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
        Publish();
        Status(&Log);
        pollGaps = {};
        pumpWork = {};
    } else {
        Publish();
    }
    pumpWork.Add(Now() - began);
}
}

void Pump() { if (wakeEvent) SetEvent(wakeEvent); }

void Stop()
{
    if (worker) {
        SetEvent(stopEvent);
        // Never close the worker's sockets/API underneath it, including during engine shutdown.
        if (WaitForSingleObject(worker, INFINITE) != WAIT_OBJECT_0) {
            Logf("steam: transport worker join failed (Windows error %lu); resources retained", GetLastError());
            return;
        }
        CloseHandle(worker);
        worker = nullptr;
    }
    if (wakeEvent) CloseHandle(wakeEvent);
    if (stopEvent) CloseHandle(stopEvent);
    if (pollTimer) CloseHandle(pollTimer);
    wakeEvent = stopEvent = pollTimer = nullptr;
    SteamFpScope floatingPoint("shutdown");
    if (apiInitialized) {
        if (isHost) {  // not joinable any more once the game is closed
            SetPresence("connect", "");
            SetPresence("status", "");
        }
        Forward();  // ENet's goodbye to the others
        LeaveLobby();
        Log("steam: stopped");
    }
    if (relay != INVALID_SOCKET) closesocket(relay);
    relay = INVALID_SOCKET;
    ready = false;
    if (apiInitialized) {
        api.Shutdown();
        apiInitialized = false;
    }
    if (winsockStarted) {
        WSACleanup();
        winsockStarted = false;
    }
    {
        std::lock_guard<std::mutex> lock(publishedMutex);
        relayCloses.clear();
    }
    Publish();
}

bool Ready() { std::lock_guard<std::mutex> lock(publishedMutex); return published.ready; }
uint64_t LobbyId() { std::lock_guard<std::mutex> lock(publishedMutex); return published.lobby; }
char const* PlayerName()
{
    thread_local char copy[24];
    std::lock_guard<std::mutex> lock(publishedMutex);
    strcpy_s(copy, published.playerName);
    return copy;
}
uint64_t PlayerId() { return me; }
int Ping()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.ping >= 0 && Now() - published.pingSampleAt < latency::PingExpiryMs ? published.ping : -1;
}
char const* Error()
{
    thread_local char copy[sizeof(error)];
    std::lock_guard<std::mutex> lock(publishedMutex);
    strcpy_s(copy, published.error);
    return copy;
}
char const* HostName()
{
    thread_local char copy[sizeof(hostName)];
    std::lock_guard<std::mutex> lock(publishedMutex);
    strcpy_s(copy, published.hostName);
    return copy;
}

bool HostLinked()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.hostLinked;
}

uint64_t HostRelayGeneration()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.hostGeneration;
}

bool RequestClientDisconnect()
{
    if (isHost || connectionMode != connect::Mode::Invite) return false;
    disconnectRequested.store(true);
    if (wakeEvent) SetEvent(wakeEvent);
    return true;
}

Connection Flow()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.connection;
}

int LinkedCount()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.linked;
}

TrafficCounters Traffic()
{
    std::lock_guard<std::mutex> lock(publishedMutex);
    return published.traffic;
}

void Status(void (*log)(char const*))
{
    char line[sizeof(published.status)];
    {
        std::lock_guard<std::mutex> lock(publishedMutex);
        strcpy_s(line, published.status);
    }
    log(line);
}
}
