// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../src/config.h"
#include "../src/crashlog.h"
#include "../src/log.h"
#include "../src/saving_policy.h"

#include "../src/steam.cpp"
#include "../src/steam_published.cpp"
#include "../src/steam_p2p.cpp"
#include "../src/steam_flow.cpp"
#include "../src/steam_callbacks.cpp"
#include "../src/steam_legacy_auto.cpp"
#include "../src/steam_diagnostics.cpp"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
extern "C" void* ENET_CALLBACK enet_malloc(size_t size) { return std::malloc(size); }
extern "C" void ENET_CALLBACK enet_free(void* memory) { std::free(memory); }
extern "C" void ENET_CALLBACK enet_host_compress(ENetHost*, ENetCompressor const*) {}
namespace {
g3t::Config g_testConfig;
std::vector<std::string> g_logs;
}

namespace g3t {
Config const& GetConfig() { return g_testConfig; }
Config const& GetLocalConfig() { return g_testConfig; }

void Log(char const* text)
{
    if (text) g_logs.emplace_back(text);
}

void Logf(char const* format, ...)
{
    char buf[2048] = {};
    va_list args;
    va_start(args, format);
    FormatV(buf, sizeof(buf), format, args);
    va_end(args);
    g_logs.emplace_back(buf);
}

int FormatV(char* out, size_t size, char const* format, va_list args)
{
    if (!out || !size) return 0;
    const int n = std::vsnprintf(out, size, format, args);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    if (static_cast<size_t>(n) >= size) return static_cast<int>(size - 1);
    return n;
}

int Format(char* out, size_t size, char const* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = FormatV(out, size, format, args);
    va_end(args);
    return n;
}

namespace crashlog {
void PollWorldEntry() {}
void RefreshModules() {}
}
}

namespace {
using namespace g3t::steam;
#define TEST_CALL __cdecl

struct CapturedSocketSend
{
    uint32_t conn = 0;
    uint16_t lane = 0;
    int32_t flags = 0;
    std::vector<uint8_t> bytes;
};

struct CapturedMessagesSend
{
    u64 peer = 0;
    int flags = 0;
    int channel = 0;
    std::vector<uint8_t> bytes;
};

std::vector<CapturedSocketSend> g_socketSends;
std::vector<CapturedMessagesSend> g_messageSends;
bool g_failSendMessages = false;
int g_sendMessagesError = -8;
unsigned g_flushes = 0;
unsigned g_closedConns = 0;
unsigned g_connectP2PCalls = 0;
unsigned g_lockedConfigWriteAttempts = 0;
unsigned g_acceptedSessions = 0;
std::vector<u64> g_leftLobbies;
u64 g_lobbyMember = 0;
std::map<int, int32_t> g_globalConfig;
std::map<std::pair<uint32_t, int>, int32_t> g_listenSocketConfig;
std::map<std::pair<uint32_t, int>, int32_t> g_connConfig;
std::set<uint32_t> g_liveListenSockets;
std::set<uint32_t> g_liveConnections;
std::set<uint32_t> g_iceLockedConnections;

int32_t DefaultValveConfigValue(int id)
{
    switch (id) {
        case config::SendBufferSize: return 512 * 1024;
        case config::SendRateMin: return 128 * 1024;
        case config::SendRateMax: return 1024 * 1024;
        case config::NagleTime: return 5000;
        case config::P2PTransportIceEnable: return -1;
        default: return 0;
    }
}

int32_t InheritedGlobalValue(int id)
{
    auto it = g_globalConfig.find(id);
    return it != g_globalConfig.end() ? it->second : DefaultValveConfigValue(id);
}

// Mirrors Valve's CSteamNetworkingUtils::GetConfigValue / SetConfigValue scope contract:
// 1 = Global (object == 0), 2 = SocketsInterface (object MUST be ISteamNetworkingSockets*),
// 3 = ListenSocket (HSteamListenSocket), 4 = Connection (HSteamNetConnection).
int TEST_CALL FakeGetConfigValue(void*, int id, int scope, intptr_t object, int* outType, void* outValue, size_t* ioSize)
{
    assert(outType && ioSize);
    if (*ioSize < sizeof(int32_t) || !outValue) return -2;
    *outType = config::Int32;
    *ioSize = sizeof(int32_t);
    int32_t resolved = 0;
    int status = 1;
    if (scope == config::GlobalScope) {
        assert(object == 0);
        auto it = g_globalConfig.find(id);
        resolved = it != g_globalConfig.end() ? it->second : DefaultValveConfigValue(id);
    } else if (scope == config::SocketsInterfaceScope) {
        assert(object == reinterpret_cast<intptr_t>(rt.sockets));
        resolved = InheritedGlobalValue(id);
        status = 2;
    } else if (scope == config::ListenSocketScope) {
        const auto sock = static_cast<uint32_t>(object);
        assert(sock != 0 && g_liveListenSockets.count(sock) == 1);
        auto it = g_listenSocketConfig.find({sock, id});
        if (it != g_listenSocketConfig.end()) {
            resolved = it->second;
            status = 1;
        } else {
            resolved = InheritedGlobalValue(id);
            status = 2;
        }
    } else if (scope == config::ConnectionScope) {
        const auto conn = static_cast<uint32_t>(object);
        assert(conn != 0 && g_liveConnections.count(conn) == 1);
        auto it = g_connConfig.find({conn, id});
        if (it != g_connConfig.end()) {
            resolved = it->second;
            status = 1;
        } else if (rt.isHost && rt.listenSocketP2P && g_liveListenSockets.count(rt.listenSocketP2P) == 1) {
            auto lsIt = g_listenSocketConfig.find({rt.listenSocketP2P, id});
            resolved = lsIt != g_listenSocketConfig.end() ? lsIt->second : InheritedGlobalValue(id);
            status = 2;
        } else {
            resolved = InheritedGlobalValue(id);
            status = 2;
        }
    } else {
        assert(false && "invalid ESteamNetworkingConfigScope");
        return -1;
    }
    std::memcpy(outValue, &resolved, sizeof(resolved));
    return status;
}

sbool TEST_CALL FakeSetConfigValue(void*, int id, int scope, intptr_t object, int type, void const* value)
{
    assert(type == config::Int32 && value != nullptr);
    int32_t v = 0;
    std::memcpy(&v, value, sizeof(v));
    if (scope == config::GlobalScope) {
        assert(object == 0);
        g_globalConfig[id] = v;
        return 1;
    }
    if (scope == config::SocketsInterfaceScope) {
        // Real steamclient.dll casts object to CSteamNetworkingSockets* and dereferences it!
        assert(object == reinterpret_cast<intptr_t>(rt.sockets));
        return 1;
    }
    if (scope == config::ListenSocketScope) {
        const auto sock = static_cast<uint32_t>(object);
        assert(sock != 0 && g_liveListenSockets.count(sock) == 1);
        g_listenSocketConfig[{sock, id}] = v;
        return 1;
    }
    if (scope == config::ConnectionScope) {
        const auto conn = static_cast<uint32_t>(object);
        assert(conn != 0 && g_liveConnections.count(conn) == 1);
        if (id == config::P2PTransportIceEnable && g_iceLockedConnections.count(conn) == 1) {
            ++g_lockedConfigWriteAttempts;
            return 0;
        }
        g_connConfig[{conn, id}] = v;
        return 1;
    }
    assert(false && "invalid ESteamNetworkingConfigScope");
    return 0;
}

void TEST_CALL FakeFreeMessageData(void* raw)
{
    auto* msg = static_cast<OutboundSocketMessage*>(raw);
    if (!msg) return;
    delete[] static_cast<char*>(msg->data);
    msg->data = nullptr;
}

void TEST_CALL FakeReleaseMessage(void* raw)
{
    auto* msg = static_cast<OutboundSocketMessage*>(raw);
    if (!msg) return;
    if (msg->freeData) msg->freeData(msg);
    delete msg;
}

void* TEST_CALL FakeAllocateMessage(void*, int cbAllocateBuffer)
{
    auto* msg = new OutboundSocketMessage{};
    msg->data = cbAllocateBuffer > 0 ? new char[static_cast<size_t>(cbAllocateBuffer)] : nullptr;
    msg->size = cbAllocateBuffer;
    msg->freeData = &FakeFreeMessageData;
    msg->release = &FakeReleaseMessage;
    return msg;
}

void TEST_CALL FakeSendMessages(void*, int nMessages, void* const* pMessages, int64_t* pOutResult)
{
    for (int i = 0; i < nMessages; ++i) {
        auto* msg = static_cast<OutboundSocketMessage*>(pMessages[i]);
        assert(msg != nullptr);
        if (g_failSendMessages) {
            if (pOutResult) pOutResult[i] = g_sendMessagesError;
        } else {
            CapturedSocketSend cap;
            cap.conn = msg->conn;
            cap.lane = msg->lane;
            cap.flags = msg->flags;
            auto const* ptr = static_cast<uint8_t const*>(msg->data);
            cap.bytes.assign(ptr, ptr + msg->size);
            g_socketSends.push_back(std::move(cap));
            if (pOutResult) pOutResult[i] = 1;
        }
        if (msg->release) msg->release(msg);
    }
}

int TEST_CALL FakeFlushMessages(void*, uint32_t)
{
    ++g_flushes;
    return ResultOk;
}

sbool TEST_CALL FakeCloseConnection(void*, uint32_t conn, int, char const*, sbool)
{
    ++g_closedConns;
    g_liveConnections.erase(conn);
    g_iceLockedConnections.erase(conn);
    return 1;
}

uint32_t TEST_CALL FakeCreateListenSocketP2P(void*, int, int, void const*)
{
    const uint32_t sock = 0x00010000u; // Low integer handle that crashes if dereferenced as CSteamNetworkingSockets*.
    g_liveListenSockets.insert(sock);
    return sock;
}

sbool TEST_CALL FakeCloseListenSocket(void*, uint32_t sock)
{
    g_liveListenSockets.erase(sock);
    return 1;
}

int TEST_CALL FakeConfigureLanes(void*, uint32_t, int numLanes, int const*, uint16_t const*)
{
    assert(numLanes == LaneCount);
    return ResultOk;
}

int TEST_CALL FakeAcceptConnection(void*, uint32_t)
{
    return ResultOk;
}

uint32_t TEST_CALL FakeConnectP2P(void*, void const*, int, int, void const*)
{
    ++g_connectP2PCalls;
    const uint32_t conn = 500u + g_connectP2PCalls;
    g_liveConnections.insert(conn);
    g_iceLockedConnections.insert(conn); // Valve CheckInitICE locks P2P_Transport_ICE_Enable during ConnectP2P.
    return conn;
}

int TEST_CALL FakeSendToUser(void*, void const* identity, void const* pubData, uint32_t cubData, int nSendFlags, int nRemoteChannel)
{
    u64 peer = 0;
    std::memcpy(&peer, identity, sizeof(peer));
    CapturedMessagesSend cap;
    cap.peer = peer;
    cap.flags = nSendFlags;
    cap.channel = nRemoteChannel;
    auto const* ptr = static_cast<uint8_t const*>(pubData);
    cap.bytes.assign(ptr, ptr + cubData);
    g_messageSends.push_back(std::move(cap));
    return ResultOk;
}

void TEST_CALL FakeIdentitySetSteamId(void* self, u64 steamId)
{
    std::memcpy(self, &steamId, sizeof(steamId));
}

u64 TEST_CALL FakeIdentityGetSteamId(void const* self)
{
    u64 steamId = 0;
    std::memcpy(&steamId, self, sizeof(steamId));
    return steamId;
}

sbool TEST_CALL FakeGetConnectionInfo(void*, uint32_t, void* raw)
{
    auto* info = static_cast<diagnostics::ConnectionInfo*>(raw);
    if (!info) return 0;
    *info = diagnostics::ConnectionInfo{};
    info->state = diagnostics::Connected;
    info->flags = 0;
    // IPv4-mapped ::ffff:203.0.113.9:49999
    info->remoteAddress[10] = 0xff;
    info->remoteAddress[11] = 0xff;
    info->remoteAddress[12] = 203;
    info->remoteAddress[13] = 0;
    info->remoteAddress[14] = 113;
    info->remoteAddress[15] = 9;
    const uint16_t natPort = 49999;
    std::memcpy(info->remoteAddress + 16, &natPort, sizeof(natPort));
    strcpy_s(info->description, "#77 P2P ICE 203.0.113.9:49999");
    return 1;
}

int TEST_CALL FakeGetConnectionRealTimeStatus(void*, uint32_t, void* raw, int, void*)
{
    auto* status = static_cast<diagnostics::SessionStatus*>(raw);
    if (!status) return -1;
    *status = diagnostics::SessionStatus{};
    status->state = diagnostics::Connected;
    status->ping = 18;
    status->sendRate = 524288;
    status->pendingUnreliable = 64;
    return ResultOk;
}

int TEST_CALL FakeGetSessionConnectionInfo(void*, void const*, void* rawInfo, void* rawStatus)
{
    auto* info = static_cast<diagnostics::ConnectionInfo*>(rawInfo);
    auto* status = static_cast<diagnostics::SessionStatus*>(rawStatus);
    if (info) {
        *info = diagnostics::ConnectionInfo{};
        info->state = diagnostics::Connected;
        info->flags = diagnostics::Relayed;
        info->popRelay = 0x00667261u;
        strcpy_s(info->description, "P2P SDR");
    }
    if (status) {
        *status = diagnostics::SessionStatus{};
        status->state = diagnostics::Connected;
        status->ping = 34;
        status->sendRate = 131072;
    }
    return diagnostics::Connected;
}

char const* TEST_CALL FakeFriendPersonaName(void*, u64) { return "PartnerHero"; }
char const* TEST_CALL FakePersonaName(void*) { return "HostHero"; }
int TEST_CALL FakeFriendRelationship(void*, u64) { return 3; }
int TEST_CALL FakeMemberCount(void*, u64) { return g_lobbyMember ? 1 : 0; }
u64 TEST_CALL FakeMemberByIndex(void*, u64, int) { return g_lobbyMember; }
u64 TEST_CALL FakeCreateLobby(void*, int, int) { return 9001; }
sbool TEST_CALL FakeSetRichPresence(void*, char const*, char const*) { return 1; }
void TEST_CALL FakeLeaveLobby(void*, u64 lobby) { g_leftLobbies.push_back(lobby); }
sbool TEST_CALL FakeAcceptSession(void*, void const*) { ++g_acceptedSessions; return 1; }
sbool TEST_CALL FakeCloseSession(void*, void const*) { return 1; }

int DummyHandle = 1;

void ResetHarness()
{
    g_testConfig = g3t::Config{};
    strcpy_s(g_testConfig.sessionId, "thermos-sec1");
    g_testConfig.sessionIdValid = true;
    g_logs.clear();
    g_socketSends.clear();
    g_messageSends.clear();
    g_failSendMessages = false;
    g_flushes = 0;
    g_closedConns = 0;
    g_connectP2PCalls = 0;
    g_lockedConfigWriteAttempts = 0;
    g_acceptedSessions = 0;
    g_leftLobbies.clear();
    g_lobbyMember = 0;
    g_globalConfig.clear();
    g_listenSocketConfig.clear();
    g_connConfig.clear();
    g_liveListenSockets.clear();
    g_liveConnections.clear();
    g_iceLockedConnections.clear();

    rt.Reset();
    rt.sendCodec = std::make_unique<send::Codec>();
    assert(rt.sendCodec->Ready());
    rt.ready = true;
    rt.me = 76561198000000001ULL;
    rt.lobby = 9001;
    rt.isHost = true;
    rt.enetPort = 27015;
    rt.directUdpEnabled = true;
    rt.socketsLaneTransport = true;
    rt.sockets = &DummyHandle;
    rt.messages = &DummyHandle;
    rt.friends = &DummyHandle;
    rt.matchmaking = &DummyHandle;
    rt.networkingUtils = &DummyHandle;

    api = Api{};
    api.IdentitySetSteamId = &FakeIdentitySetSteamId;
    api.IdentityGetSteamId = &FakeIdentityGetSteamId;
    api.AllocateMessage = &FakeAllocateMessage;
    api.SendMessages = &FakeSendMessages;
    api.FlushMessagesOnConnection = &FakeFlushMessages;
    api.CloseConnection = &FakeCloseConnection;
    api.CreateListenSocketP2P = &FakeCreateListenSocketP2P;
    api.CloseListenSocket = &FakeCloseListenSocket;
    api.ConfigureConnectionLanes = &FakeConfigureLanes;
    api.AcceptConnection = &FakeAcceptConnection;
    api.ConnectP2P = &FakeConnectP2P;
    api.SetConfigValue = &FakeSetConfigValue;
    api.GetConfigValue = &FakeGetConfigValue;
    api.SendToUser = &FakeSendToUser;
    api.GetConnectionInfo = &FakeGetConnectionInfo;
    api.GetConnectionRealTimeStatus = &FakeGetConnectionRealTimeStatus;
    api.GetSessionConnectionInfo = &FakeGetSessionConnectionInfo;
    api.FriendPersonaName = &FakeFriendPersonaName;
    api.PersonaName = &FakePersonaName;
    api.FriendRelationship = &FakeFriendRelationship;
    api.MemberCount = &FakeMemberCount;
    api.MemberByIndex = &FakeMemberByIndex;
    api.CreateLobby = &FakeCreateLobby;
    api.SetRichPresence = &FakeSetRichPresence;
    api.LeaveLobby = &FakeLeaveLobby;
    api.AcceptSession = &FakeAcceptSession;
    api.CloseSession = &FakeCloseSession;

    std::lock_guard<std::mutex> lock(store.mutex);
    store.published = Published{};
    store.relayDrops.clear();
    store.relayCloses.clear();
}

std::vector<uint8_t> MakeUnreliableEnetDatagram(uint8_t channel, uint16_t seq, std::vector<uint8_t> const& payload)
{
    std::vector<uint8_t> out(2 + 8 + payload.size(), 0);
    out[2] = 7; // ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE
    out[3] = channel;
    out[6] = static_cast<uint8_t>(seq >> 8);
    out[7] = static_cast<uint8_t>(seq & 0xffu);
    out[8] = static_cast<uint8_t>(payload.size() >> 8);
    out[9] = static_cast<uint8_t>(payload.size() & 0xffu);
    std::memcpy(out.data() + 10, payload.data(), payload.size());
    return out;
}

bool LogsContain(char const* needle)
{
    for (auto const& line : g_logs)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

void TestZeroCopyV012LanesAndMessagesFallback()
{
    ResetHarness();
    const u64 peerId = 76561198000000002ULL;
    Remote* r = rt.Get(peerId);
    assert(r != nullptr);
    r->linked = true;
    r->p2pConn = 77;
    r->p2pConnected = true;
    r->p2pLanesConfigured = true;

    auto playerPkt = MakeUnreliableEnetDatagram(g3t::net::ChannelState, 1, {3, 10, 20, 30});
    auto defPkt = MakeUnreliableEnetDatagram(g3t::net::ChannelDefinitions, 1, {50, 1, 2, 3});
    auto posePayload = std::vector<uint8_t>(64, 0);
    posePayload[0] = 49; // MsgControlPose
    posePayload[25] = 1;
    posePayload[29] = 42;
    auto posePkt = MakeUnreliableEnetDatagram(g3t::net::ChannelPoses, 2, posePayload);
    auto bulkPkt = MakeUnreliableEnetDatagram(g3t::net::ChannelBulk, 1, {99, 7, 8, 9});

    // Queue in reverse priority order to prove priority lanes drain 0 -> 1 -> 2 -> 3!
    assert(rt.QueueDatagram(*r, bulkPkt.data(), static_cast<unsigned>(bulkPkt.size())));
    assert(rt.QueueDatagram(*r, posePkt.data(), static_cast<unsigned>(posePkt.size())));
    assert(rt.QueueDatagram(*r, defPkt.data(), static_cast<unsigned>(defPkt.size())));
    assert(rt.QueueDatagram(*r, playerPkt.data(), static_cast<unsigned>(playerPkt.size())));
    rt.DrainDatagrams(*r);

    assert(g_socketSends.size() == 4);
    assert(g_messageSends.empty());
    assert(g_flushes == 1);
    assert(g_socketSends[0].lane == 0 && g_socketSends[0].flags == (send::NoDelay | send::NoNagle));
    assert(g_socketSends[1].lane == 1 && g_socketSends[1].flags == (send::NoDelay | send::NoNagle));
    assert(g_socketSends[2].lane == 2 && g_socketSends[2].flags == (send::NoDelay | send::NoNagle));
    assert(g_socketSends[3].lane == 3 && g_socketSends[3].flags == (send::NoDelay | send::NoNagle));

    void const* unwrapped = nullptr;
    std::size_t unwrappedLen = 0;
    assert(rt.UnpackRelayEnvelope(reinterpret_cast<char const*>(g_socketSends[0].bytes.data()),
                                  static_cast<int>(g_socketSends[0].bytes.size()), unwrapped, unwrappedLen));
    assert(unwrappedLen > 0);
    // Non-retryable SendMessages failure must close P2P, keep Remote linked, and fall back to Messages immediately!
    g_socketSends.clear();
    g_failSendMessages = true;
    auto fallbackPkt = MakeUnreliableEnetDatagram(g3t::net::ChannelState, 3, {3, 99});
    assert(rt.QueueDatagram(*r, fallbackPkt.data(), static_cast<unsigned>(fallbackPkt.size())));
    rt.DrainDatagrams(*r);
    assert(r->linked);
    assert(r->p2pConn == 0 && !r->p2pConnected);
    assert(g_closedConns == 1);
    assert(g_messageSends.size() == 1);
    assert(g_messageSends[0].peer == peerId);
    assert(g_messageSends[0].channel == ChannelData);
    assert(LogsContain("falling back to Messages"));
    // Cooldown prevents immediate ConnectP2P spam on client right after close.
    rt.isHost = false;
    rt.OpenClientP2P(*r);
    assert(g_connectP2PCalls == 0);
    r->lastP2POpenAt -= (P2PReconnectCooldownMs + 50);
    rt.OpenClientP2P(*r);
    assert(g_connectP2PCalls == 1);
}

void TestActiveV012SendDiagnosticsVsMessagesFallback()
{
    ResetHarness();
    const u64 peerId = 76561198000000002ULL;
    Remote* r = rt.Get(peerId);
    r->linked = true;
    r->p2pConn = 88;
    r->p2pConnected = true;

    rt.SendDiagnostics(1000);
    assert(LogsContain("source=sockets-v012"));
    assert(LogsContain("send_rate_bytes_per_s=524288"));
    assert(LogsContain("transport=direct-ice"));
    assert(LogsContain("[redacted]"));

    g_logs.clear();
    r->p2pConn = 0;
    r->p2pConnected = false;
    rt.SendDiagnostics(6000);
    assert(LogsContain("source=messages"));
    assert(LogsContain("send_rate_bytes_per_s=131072"));
    assert(LogsContain("transport=relay-sdr"));
}

void TestDirectCandidateFilteringAndReflexivePort()
{
    ResetHarness();
    SetEnvironmentVariableA("G3T_DIRECT_UDP_ALLOW_LOOPBACK", "0");

    uint32_t ipZero = 0;
    uint32_t ipLoop = htonl(INADDR_LOOPBACK);
    uint32_t ipMulticast = htonl((239u << 24) | 1u);
    uint32_t ipBroadcast = 0xffffffffu;
    uint32_t ipLinkLocal = 0;
    inet_pton(AF_INET, "169.254.42.7", &ipLinkLocal);
    uint32_t ipPublic = 0;
    inet_pton(AF_INET, "198.51.100.7", &ipPublic);

    assert(!IsValidDirectCandidateEndpoint(ipZero, 27016, true));
    assert(!IsValidDirectCandidateEndpoint(ipMulticast, 27016, true));
    assert(!IsValidDirectCandidateEndpoint(ipBroadcast, 27016, true));
    assert(!IsValidDirectCandidateEndpoint(ipLinkLocal, 27016, true));
    assert(!IsValidDirectCandidateEndpoint(ipPublic, 0, true));
    assert(!IsValidDirectCandidateEndpoint(ipLoop, 27016, false));
    assert(IsValidDirectCandidateEndpoint(ipLoop, 27016, true));
    assert(IsValidDirectCandidateEndpoint(ipPublic, 27016, false));

    const u64 peerId = 76561198000000003ULL;
    g_lobbyMember = peerId;
    rt.lobby = 9001;
    Remote* r = rt.Get(peerId);
    r->linked = true;
    r->haveDirectCookie = true;

    // Build 'U' control packet with peer's ENet port 27016 (!= host's 27015) and 4 candidates (3 invalid + 1 valid).
    char uFrame[28 + 4 * 4] = {};
    uFrame[0] = 'U';
    std::memcpy(uFrame + 1, &r->generation, 8);
    std::memset(r->directCookie, 0xAB, sizeof(r->directCookie));
    std::memcpy(uFrame + 9, r->directCookie, 16);
    const uint16_t peerPort = 27016;
    std::memcpy(uFrame + 25, &peerPort, 2);
    uFrame[27] = 4;
    std::memcpy(uFrame + 28, &ipZero, 4);
    std::memcpy(uFrame + 32, &ipLoop, 4);
    std::memcpy(uFrame + 36, &ipMulticast, 4);
    std::memcpy(uFrame + 40, &ipPublic, 4);

    rt.OnControl(*r, uFrame, sizeof(uFrame));
    assert(r->directPeerPort == 27016);
    assert(r->directCandidateCount == 1);
    assert(r->directCandidates[0].hostIpv4 == ipPublic);
    assert(r->directCandidates[0].port == 27016);

    // Now fire P2P Connected callback AFTER 'U' arrived; reflexive IP 203.0.113.9 must pair with r->directPeerPort (27016),
    // NOT host rt.enetPort (27015) and NOT NAT port (49999)!
    r->p2pConn = 91;
    ConnectionStatusChangedParam cb{};
    cb.conn = 91;
    FakeGetConnectionInfo(nullptr, 91, &cb.info);
    FakeIdentitySetSteamId(cb.info.identity, peerId);
    cb.oldState = 1;
    rt.OnP2PStatusChanged(reinterpret_cast<char const*>(&cb), sizeof(cb));

    uint32_t expectedReflexive = 0;
    inet_pton(AF_INET, "203.0.113.9", &expectedReflexive);
    assert(r->p2pConnected);
    assert(r->directCandidateCount == 2);
    assert(r->directCandidates[1].hostIpv4 == expectedReflexive);
    assert(r->directCandidates[1].port == 27016);
    assert(r->directReady);

    // Even when 8 local/virtual NICs fill all 8 slots, the Steam-observed reflexive IPv4 replaces slot 7 instead of dropping!
    char uFull[28 + 8 * 4] = {};
    uFull[0] = 'U';
    std::memcpy(uFull + 1, &r->generation, 8);
    std::memcpy(uFull + 9, r->directCookie, 16);
    std::memcpy(uFull + 25, &peerPort, 2);
    uFull[27] = 8;
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t nicIp = htonl((10u << 24) | (0u << 16) | (i << 8) | 2u);
        std::memcpy(uFull + 28 + i * 4, &nicIp, 4);
    }
    rt.OnControl(*r, uFull, sizeof(uFull));
    assert(r->directCandidateCount == 8);
    assert(r->directCandidates[7].hostIpv4 == expectedReflexive);
    assert(r->directCandidates[7].port == 27016);
}

void TestDirectUdpHandshakeAcrossMismatchedGenerations()
{
    ResetHarness();
    SetEnvironmentVariableA("G3T_DIRECT_UDP_ALLOW_LOOPBACK", "1");

    const u64 hostId = 76561198000000001ULL;
    const u64 clientId = 76561198000000002ULL;

    // Step 1: Host has rotated generations (gen = 5) and sends its 'U' offer to the client.
    rt.isHost = true;
    rt.me = hostId;
    rt.lobby = 9001;
    rt.enetPort = 27015;
    rt.nextRelayGeneration = 4;
    Remote* hostView = rt.Get(clientId);
    assert(hostView && hostView->generation == 5);
    hostView->linked = true;
    hostView->haveDirectCookie = true;
    hostView->directOfferGeneration = hostView->generation;
    for (unsigned i = 0; i < 16; ++i) hostView->directCookie[i] = static_cast<uint8_t>(0x30 + i);
    g_messageSends.clear();
    rt.SendDirectUdpControl(*hostView);
    assert(g_messageSends.size() == 1);
    void const* hostPayload = nullptr;
    std::size_t hostPayloadBytes = 0;
    assert(RelayDecode(g_messageSends[0].bytes.data(), g_messageSends[0].bytes.size(),
                       9001, "thermos-sec1", &hostPayload, &hostPayloadBytes));
    const std::vector<char> hostU(static_cast<char const*>(hostPayload),
                                  static_cast<char const*>(hostPayload) + hostPayloadBytes);

    // Step 2: Fresh client runtime (local Remote::generation == 1 != 5) receives host's 'U' and sends its 'U' reply.
    WorkerRuntime savedHost = std::move(rt);
    rt.Reset();
    rt.nextRelayGeneration = 0;
    rt.ready = true;
    rt.isHost = false;
    rt.me = clientId;
    rt.owner = hostId;
    rt.lobby = 9001;
    rt.enetPort = 27016;
    rt.directUdpEnabled = true;
    rt.sockets = &DummyHandle;
    rt.messages = &DummyHandle;
    rt.friends = &DummyHandle;
    rt.matchmaking = &DummyHandle;
    rt.networkingUtils = &DummyHandle;
    Remote* clientView = rt.Get(hostId);
    assert(clientView && clientView->generation == 1);
    clientView->linked = true;
    g_messageSends.clear();
    rt.OnControl(*clientView, hostU.data(), static_cast<int>(hostU.size()));
    assert(clientView->haveDirectCookie && clientView->directOfferGeneration == 5 && clientView->directReady);
    rt.Publish();
    const auto clientOffers = DirectOffers();
    assert(clientOffers.size() == 1 && clientOffers[0].generation == 1 && clientOffers[0].ready);
    assert(HostRelayGeneration() == 1);
    assert(g_messageSends.size() == 1);
    void const* clientPayload = nullptr;
    std::size_t clientPayloadBytes = 0;
    assert(RelayDecode(g_messageSends[0].bytes.data(), g_messageSends[0].bytes.size(),
                       9001, "thermos-sec1", &clientPayload, &clientPayloadBytes));
    const std::vector<char> clientU(static_cast<char const*>(clientPayload),
                                    static_cast<char const*>(clientPayload) + clientPayloadBytes);

    // Step 3: Host (Remote::generation == 5) receives client's 'U' reply and MUST authenticate it and become directReady!
    rt = std::move(savedHost);
    Remote* restoredHostView = rt.Find(clientId);
    assert(restoredHostView && !restoredHostView->directReady);
    rt.OnControl(*restoredHostView, clientU.data(), static_cast<int>(clientU.size()));
    assert(restoredHostView->directReady && restoredHostView->directPeerPort == 27016);
    rt.Publish();
    const auto hostOffers = DirectOffers();
    assert(hostOffers.size() == 1 && hostOffers[0].generation == 5 && hostOffers[0].ready);

    SetEnvironmentVariableA("G3T_DIRECT_UDP_ALLOW_LOOPBACK", "0");
}

void TestDirectEndpointPinningAndInviteRejoinGenerationRotation()
{
    ResetHarness();
    SetEnvironmentVariableA("G3T_DIRECT_UDP_ALLOW_LOOPBACK", "0");

    const u64 clientId = 76561198000000004ULL;
    g_lobbyMember = clientId;
    rt.lobby = 0;
    rt.connectionMode = connect::Mode::Invite;
    const auto readyBefore = rt.hostFlow.state();
    rt.AfterHost("ready", readyBefore, rt.hostFlow.Ready(connect::Mode::Invite, Now()));

    // Drive HostFlow to Linked for clientId.
    connect::Frame join{};
    join.type = 'J';
    strcpy_s(join.session, "thermos-sec1");
    join.protocol = g3t::saving::Protocol;
    rt.OnJoinRequest(clientId, join);
    rt.lobby = 9001;
    const auto createdBefore = rt.hostFlow.state();
    rt.AfterHost("lobby created", createdBefore, rt.hostFlow.LobbyCreated(true, 1, Now()));

    Remote* r = rt.Get(clientId);
    r->linked = true;
    r->haveDirectCookie = true;
    for (unsigned i = 0; i < 16; ++i) r->directCookie[i] = static_cast<uint8_t>(i + 1);
    const uint64_t gen1 = r->generation;
    assert(gen1 >= 1);
    assert(rt.hostFlow.Admit(clientId));
    const auto admitBefore = rt.hostFlow.state();
    rt.AfterHost("hello", admitBefore, rt.hostFlow.Linked(clientId));
    assert(rt.hostFlow.state() == connect::HostFlow::State::Linked);
    rt.Publish();

    uint32_t ipLegit = 0, ipSpoof = 0, ipRejoin = 0;
    inet_pton(AF_INET, "198.51.100.20", &ipLegit);
    inet_pton(AF_INET, "203.0.113.99", &ipSpoof);
    inet_pton(AF_INET, "198.51.100.21", &ipRejoin);

    RegisterDirectPeerEndpoint(clientId, gen1, ipLegit, 27016);
    uint64_t outAcc = 0, outGen = 0;
    char outName[24] = {};
    assert(DirectPeerIdentity(ipLegit, 27016, outAcc, outName, outGen));
    assert(outAcc == clientId && outGen == gen1);

    // Pinning persists across subsequent worker Publish() snapshots!
    rt.Publish();
    assert(DirectPeerIdentity(ipLegit, 27016, outAcc, outName, outGen));

    // Spoofed rebind for the SAME (account, generation) is refused!
    RegisterDirectPeerEndpoint(clientId, gen1, ipSpoof, 31337);
    assert(!DirectPeerIdentity(ipSpoof, 31337, outAcc, outName, outGen));
    assert(DirectPeerIdentity(ipLegit, 27016, outAcc, outName, outGen));

    // Partner crashes and re-accepts invite: host receives G3T1J while Linked -> rotates partner & generation!
    rt.OnJoinRequest(clientId, join);
    assert(rt.hostFlow.state() == connect::HostFlow::State::Offered);
    auto drops = TakeRelayDrops();
    assert(drops.size() == 1 && drops[0].account == clientId && drops[0].generation == gen1);

    Remote* r2 = rt.Get(clientId);
    r2->linked = true;
    const uint64_t gen2 = r2->generation;
    assert(gen2 > gen1);
    rt.Publish();

    // Old endpoint is gone, and new generation can bind its fresh endpoint!
    assert(!DirectPeerIdentity(ipLegit, 27016, outAcc, outName, outGen));
    RegisterDirectPeerEndpoint(clientId, gen2, ipRejoin, 27018);
    assert(DirectPeerIdentity(ipRejoin, 27018, outAcc, outName, outGen));
    assert(outGen == gen2);
}

void TestClientForwardPortFilter()
{
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ResetHarness();
    rt.isHost = false;
    unsigned short relayPort = 0;
    rt.relay = LoopbackSocket(&relayPort);
    assert(rt.relay != INVALID_SOCKET && relayPort != 0);

    unsigned short enetBoundPort = 0;
    SOCKET enetSock = LoopbackSocket(&enetBoundPort);
    unsigned short strayPort = 0;
    SOCKET straySock = LoopbackSocket(&strayPort);
    assert(enetSock != INVALID_SOCKET && straySock != INVALID_SOCKET && enetBoundPort != strayPort);
    rt.enetPort = enetBoundPort;

    sockaddr_in relayAddr{};
    relayAddr.sin_family = AF_INET;
    relayAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relayAddr.sin_port = htons(relayPort);

    // Stray local packet from wrong port must NOT hijack rt.enetAddress.
    char const* strayPayload = "stray";
    assert(sendto(straySock, strayPayload, 5, 0, reinterpret_cast<sockaddr*>(&relayAddr), sizeof(relayAddr)) == 5);
    rt.Forward();
    assert(!rt.haveEnetAddress);

    // Legitimate packet from rt.enetPort records rt.enetAddress.
    char const* legitPayload = "legit";
    assert(sendto(enetSock, legitPayload, 5, 0, reinterpret_cast<sockaddr*>(&relayAddr), sizeof(relayAddr)) == 5);
    rt.Forward();
    assert(rt.haveEnetAddress && ntohs(rt.enetAddress.sin_port) == enetBoundPort);

    closesocket(straySock);
    closesocket(enetSock);
    closesocket(rt.relay);
    rt.relay = INVALID_SOCKET;
    WSACleanup();
}

void TestRealSteamConfigScopesOnHostAndClient()
{
    ResetHarness();
    static_assert(config::GlobalScope == 1);
    static_assert(config::SocketsInterfaceScope == 2);
    static_assert(config::ListenSocketScope == 3);
    static_assert(config::ConnectionScope == 4);

    // Pre-existing higher global send-rate ceilings must be preserved across global, listen-socket, and connection scopes.
    g_globalConfig[config::SendRateMin] = 2 * 1024 * 1024;
    g_globalConfig[config::SendRateMax] = 16 * 1024 * 1024;

    assert(rt.ConfigureNetworking());
    rt.TransportConfigDiagnostics();
    assert(g_globalConfig[config::P2PTransportIceEnable] == config::IceEnableAll);
    assert(g_globalConfig[config::SendBufferSize] == config::BufferCapacity);
    assert(g_globalConfig[config::SendRateMin] == 2 * 1024 * 1024);
    assert(g_globalConfig[config::SendRateMax] == 16 * 1024 * 1024);

    // Host startup creates P2P listen socket (0x00010000) and configures ListenSocketScope (3), NOT SocketsInterfaceScope (2)!
    rt.isHost = true;
    rt.listenSocketP2P = api.CreateListenSocketP2P(rt.sockets, P2PVirtualPort, 0, nullptr);
    assert(rt.listenSocketP2P == 0x00010000u);
    rt.ConfigureSocketOptions(config::ListenSocketScope, static_cast<intptr_t>(rt.listenSocketP2P));
    assert((g_listenSocketConfig[{rt.listenSocketP2P, config::P2PTransportIceEnable}] == config::IceEnableAll));

    size_t sz = sizeof(int32_t);
    int outType = 0;
    int32_t val = 0;
    assert(FakeGetConfigValue(rt.networkingUtils, config::SendBufferSize, config::ListenSocketScope,
                              static_cast<intptr_t>(rt.listenSocketP2P), &outType, &val, &sz) >= 1 &&
           val == config::BufferCapacity);
    assert(FakeGetConfigValue(rt.networkingUtils, config::SendRateMin, config::ListenSocketScope,
                              static_cast<intptr_t>(rt.listenSocketP2P), &outType, &val, &sz) >= 1 &&
           val == 2 * 1024 * 1024);
    assert(FakeGetConfigValue(rt.networkingUtils, config::SendRateMax, config::ListenSocketScope,
                              static_cast<intptr_t>(rt.listenSocketP2P), &outType, &val, &sz) >= 1 &&
           val == 16 * 1024 * 1024);

    // Invalid scope or zero handle is ignored safely before touching SetConfigValue.
    rt.ConfigureSocketOptions(config::SocketsInterfaceScope, static_cast<intptr_t>(rt.listenSocketP2P));
    rt.ConfigureSocketOptions(config::ListenSocketScope, 0);

    // Host incoming P2P connection accept uses ConnectionScope (4), respects locked ICE, and preserves larger ceilings.
    const u64 peerId = 76561198000000005ULL;
    g_lobbyMember = peerId;
    rt.lobby = 9001;
    const uint32_t incomingConn = 401u;
    g_liveConnections.insert(incomingConn);
    g_iceLockedConnections.insert(incomingConn);
    ConnectionStatusChangedParam incoming{};
    incoming.conn = incomingConn;
    incoming.info.listenSocket = rt.listenSocketP2P;
    incoming.info.state = 1; // Connecting
    FakeIdentitySetSteamId(incoming.info.identity, peerId);
    rt.OnP2PStatusChanged(reinterpret_cast<char const*>(&incoming), sizeof(incoming));

    Remote* hostPeer = rt.Find(peerId);
    assert(hostPeer && hostPeer->p2pConn == incomingConn && hostPeer->p2pLanesConfigured);
    assert(g_lockedConfigWriteAttempts == 0);
    assert(FakeGetConfigValue(rt.networkingUtils, config::SendRateMin, config::ConnectionScope,
                              static_cast<intptr_t>(incomingConn), &outType, &val, &sz) >= 1 &&
           val == 2 * 1024 * 1024);
    assert(FakeGetConfigValue(rt.networkingUtils, config::SendRateMax, config::ConnectionScope,
                              static_cast<intptr_t>(incomingConn), &outType, &val, &sz) >= 1 &&
           val == 16 * 1024 * 1024);

    // Client outgoing P2P connection uses ConnectionScope (4), respects locked ICE, and raises defaults when lower.
    ResetHarness();
    g_globalConfig[config::P2PTransportIceEnable] = config::IceEnableAll;
    rt.isHost = false;
    Remote* clientHostPeer = rt.Get(peerId);
    clientHostPeer->linked = true;
    rt.OpenClientP2P(*clientHostPeer);
    assert(clientHostPeer->p2pConn == 501u && clientHostPeer->p2pLanesConfigured);
    assert(g_lockedConfigWriteAttempts == 0);
    assert((g_connConfig[{501u, config::SendBufferSize}] == config::BufferCapacity));
    assert((g_connConfig[{501u, config::SendRateMin}] == config::MinRateCapacity));
    assert((g_connConfig[{501u, config::SendRateMax}] == config::MaxRateCapacity));
}

void TestCallbackBoundsAndDuplicateLobbyCleanup()
{
    ResetHarness();
    const u64 peerId = 76561198000000006ULL;
    g_lobbyMember = peerId;
    rt.lobby = 9001;
    Remote* r = rt.Get(peerId);
    r->linked = true;
    r->relayPort = 27999;
    rt.Publish();
    assert(IsPeerRelay(27999) && !IsPeerRelay(0) && !IsPeerRelay(28000));
    assert(PeerId(27999) == peerId && PeerId(0) == 0 && PeerId(28000) == 0);

    struct LobbyCreatedWire { int result; int pad; u64 lobby; } extra{ResultOk, 0, 9002};
    rt.OnLobbyCreated(reinterpret_cast<char const*>(&extra), sizeof(extra));
    assert(rt.lobby == 9001 && g_leftLobbies.size() == 1 && g_leftLobbies[0] == 9002);

    char truncated[SteamIdentityBytes - 1] = {};
    FakeIdentitySetSteamId(truncated, peerId);
    rt.OnCallback(CbSessionRequest, nullptr, SteamIdentityBytes);
    rt.OnCallback(CbSessionRequest, truncated, sizeof(truncated));
    assert(g_acceptedSessions == 0);
    struct TruncatedFailed { char id[SteamIdentityBytes - 4]; int reason; } badFail{};
    rt.OnCallback(CbSessionFailed, nullptr, SteamIdentityBytes + 4);
    rt.OnCallback(CbSessionFailed, reinterpret_cast<char const*>(&badFail), sizeof(badFail));
    assert(r->linked);
    g_logs.clear();
    Status(&g3t::Log);
    assert(LogsContain("STEAMSTATUS app=39500 ready=1"));
}
}

int main()
{
    TestZeroCopyV012LanesAndMessagesFallback();
    TestActiveV012SendDiagnosticsVsMessagesFallback();
    TestDirectCandidateFilteringAndReflexivePort();
    TestDirectUdpHandshakeAcrossMismatchedGenerations();
    TestDirectEndpointPinningAndInviteRejoinGenerationRotation();
    TestClientForwardPortFilter();
    TestRealSteamConfigScopesOnHostAndClient();
    TestCallbackBoundsAndDuplicateLobbyCleanup();
    std::puts("Steam v012 runtime: zero-copy lanes, Messages fallback, active v012 diagnostics, candidate validation, mismatched-gen handshake, endpoint pinning, rejoin rotation, port filtering, real Steam config scopes and callback bounds passed");
    return 0;
}
