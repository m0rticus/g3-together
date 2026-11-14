// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "steam.h"
#include "steam_abi.h"
#include "steam_config.h"
#include "steam_connect.h"
#include "steam_diagnostics.h"
#include "steam_send.h"
#include "latency.h"
#include "net.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace g3t::steam {
inline constexpr uint32_t SilenceMs = 180000;
inline constexpr uint32_t ScanMs = 3000;
inline constexpr uint32_t JoinWaitMs = 15000;
inline constexpr uint32_t MetadataRequestRetryMs = 1000;
inline constexpr uint32_t P2PReconnectCooldownMs = 2000;
inline constexpr int MaxRemotes = net::MaxPlayers;

inline double PingSeconds()
{
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) / frequency;
}

inline uint32_t Now()
{
    return static_cast<uint32_t>(static_cast<uint64_t>(PingSeconds() * 1000.0));
}

inline char const* Text(char const* text)
{
    return text ? text : "";
}

inline void CopyName(char* out, size_t size, char const* source)
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

inline uint32_t CookieTag(uint8_t const cookie[16])
{
    uint32_t tag = 0;
    std::memcpy(&tag, cookie, sizeof(tag));
    return tag ? tag : 0x67337431u;
}

inline bool AllowDirectLoopback()
{
    char allowLoopback[8] = {};
    return GetEnvironmentVariableA("G3T_DIRECT_UDP_ALLOW_LOOPBACK", allowLoopback, sizeof(allowLoopback)) > 0 &&
           allowLoopback[0] == '1';
}

// DirectEndpoint::hostIpv4 follows ENetAddress::host (network byte order); port is host byte order.
inline bool IsValidDirectCandidateEndpoint(uint32_t netIpv4, uint16_t hostPort, bool allowLoopback)
{
    if (!netIpv4 || netIpv4 == 0xffffffffu || !hostPort) return false;
    const uint32_t hostOrder = ntohl(netIpv4);
    const uint8_t first = static_cast<uint8_t>(hostOrder >> 24);
    if (first == 0 || first >= 224) return false;  // reject 0.0.0.0/8, multicast (224/4), and reserved/broadcast (240/4)
    if (first == 127) {
        if (!allowLoopback) return false;
        const uint32_t hostPart = hostOrder & 0x00ffffffu;
        if (hostPart == 0 || hostPart == 0x00ffffffu) return false;
    }
    return true;
}

inline bool ExtractReflexiveIpv4(diagnostics::ConnectionInfo const& info, uint32_t& netIpv4, bool allowLoopback = false)
{
    if (info.flags & diagnostics::Relayed) return false;
    for (unsigned i = 0; i < 10; ++i)
        if (info.remoteAddress[i] != 0) return false;
    if (info.remoteAddress[10] != 0xff || info.remoteAddress[11] != 0xff) return false;
    std::memcpy(&netIpv4, info.remoteAddress + 12, 4);
    return IsValidDirectCandidateEndpoint(netIpv4, 1, allowLoopback);
}

struct Remote
{
    u64 id = 0;
    uint64_t generation = 0;
    char name[24] = {};
    bool linked = false;
    bool leaving = false;
    uint32_t createdAt = 0, linkedAt = 0, lobbyChange = 0;
    bool firstEnetLogged = false, unclassifiedLogged[32] = {};
    uint32_t heard = 0, lastPing = 0;
    latency::Ping ping;
    SOCKET socket = INVALID_SOCKET;
    unsigned short relayPort = 0;
    uint32_t p2pConn = 0;
    uint32_t lastP2POpenAt = 0;
    bool p2pConnected = false, p2pLanesConfigured = false;
    bool haveDirectCookie = false, directReady = false;
    uint8_t directCookie[16] = {};
    DirectEndpoint directCandidates[8] = {};
    unsigned directCandidateCount = 0;
    uint16_t directPeerPort = 0;
    uint32_t lastDirectOfferSent = 0;
    unsigned toEnet = 0, toSteam = 0;
    send::Queue pending;
    uint64_t steamReliableOk = 0, steamUnreliableOk = 0, steamFailed[128] = {};
};

struct Skipped { u64 lobby = 0; uint32_t until = 0; };
struct Label { u64 id = 0; uint32_t at = 0; char text[64] = ""; };

struct ClientEnet
{
    unsigned state = 0, event = 0;
    uint32_t data = 0, elapsedMs = 0, sampledAt = 0;
    char reason[160] = {};
};

struct RelayClose
{
    uint64_t account = 0;
    uint64_t generation = 0;
    char reason[160] = {};
};

struct PublishedPeer
{
    unsigned short port = 0;
    u64 account = 0;
    uint64_t generation = 0;
    char name[24] = {};
    DirectOffer directOffer{};
    DirectEndpoint verifiedDirect{};
    bool directPinned = false;
};

struct Published
{
    bool ready = false, hostLinked = false;
    uint64_t hostGeneration = 0;
    u64 lobby = 0;
    int linked = 0, ping = -1;
    uint32_t pingSampleAt = 0;
    PublishedPeer peers[MaxRemotes] = {};
    char playerName[24] = {};
    char hostName[64] = {}, error[256] = {}, status[900] = {};
    Connection connection;
    TrafficCounters traffic;
};

struct PublishedStore
{
    std::mutex mutex;
    Published published;
    ClientEnet clientEnet;
    std::vector<RelayDrop> relayDrops;
    std::vector<RelayClose> relayCloses;
    std::atomic<bool> disconnectRequested{false};
    HANDLE worker = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE pollTimer = nullptr;
};

struct WorkerRuntime
{
    bool ready = false, tried = false, winsockStarted = false, apiInitialized = false;
    bool socketsLaneTransport = false, directUdpEnabled = false;
    uint32_t listenSocketP2P = 0;
    char error[256] = "";
    HSteamPipe pipe = 0;
    void *user = nullptr, *matchmaking = nullptr, *friends = nullptr, *messages = nullptr, *sockets = nullptr;
    void* networkingUtils = nullptr;
    u64 me = 0;
    char persona[64] = "";
    char playerName[24] = "";
    unsigned sendFailures = 0;
    config::Applied networkingConfig[4] = {};
    bool networkingConfigured = false;
    unsigned isolatedDrops = 0;
    uint32_t lastSendFailure = 0, lastDiagnostics = 0, lastNames = 0;

    bool isHost = false;
    unsigned short enetPort = 0;
    u64 lobby = 0, owner = 0;
    uint32_t lastCreate = 0;
    Remote remotes[MaxRemotes] = {};
    uint64_t nextRelayGeneration = 0;
    int senderOffset = 0;

    SOCKET relay = INVALID_SOCKET;
    unsigned short relayPort = 0;
    sockaddr_in enetAddress{};
    bool haveEnetAddress = false;
    u64 wantedLobby = 0;
    bool wantedWaitLogged = false;
    u64 pendingLobby = 0;
    u64 candidateLobby = 0;
    uint32_t candidateAt = 0, candidateRequestAt = 0;
    bool candidateJoined = false, candidatePendingLogged = false;
    char candidateReason[64] = "";
    uint32_t joinedAt = 0, lastScan = 0, lastHello = 0;
    char hostName[64] = "";
    Skipped skipped[16] = {};
    bool searchLogged = false;

    connect::Mode connectionMode = connect::Mode::Invite;
    connect::ClientFlow clientFlow;
    connect::HostFlow hostFlow;
    Label labels[16] = {};
    char presenceStatus[96] = "", presenceConnect[256] = "";
    bool connectPublished = false;
    uint32_t lastPresence = 0;
    u64 requestedHost = 0;
    uint32_t clientAttemptAt = 0, clientLinkedAt = 0;
    bool clientHadLink = false, clientEndRecorded = false, clientEndedImmediately = false;
    char clientEndReason[160] = {};

    uint32_t lastPump = 0;
    latency::Samples pollGaps, pumpWork;
    unsigned receiveCaps = 0, forwardCaps = 0, socketFailures = 0, receiveFailures = 0, invalidPongs = 0;
    int socketError = 0;
    TrafficCounters traffic;
    std::unique_ptr<send::Codec> sendCodec;

    void Reset()
    {
        const uint64_t preservedGeneration = nextRelayGeneration;
        *this = WorkerRuntime{};
        nextRelayGeneration = preservedGeneration;
    }

    // Core helpers & lifecycle (steam.cpp)
    void Identity(char (&buffer)[160], u64 id) const;
    void Publish();
    bool Fail(char const* text);
    bool Load();
    void PumpOnce();

    // P2P, Direct UDP & datagram transport (steam_p2p.cpp)
    void ConfigureSocketOptions(int scope, intptr_t handle);
    bool ConfigurePeerLanes(uint32_t conn);
    void ClosePeerP2P(Remote& r);
    void OpenClientP2P(Remote& r);
    void OnP2PStatusChanged(char const* p, int size);
    void SendDirectUdpControl(Remote& r);
    void RecordAcceptedItemTraffic(send::Item const& item, std::size_t encodedSize);
    int SubmitSocketEnvelope(Remote& r, void const* data, unsigned size, int flags, uint16_t lane, std::size_t& encodedSize);
    int RawDataSend(Remote& r, send::Item const& item);
    int RawSend(u64 to, int flags, int channel, void const* data, unsigned size, send::Item const* preParsedItem = nullptr);
    void Control(u64 to, char what, bool reliable);
    u64 Sender(char const* message);
    bool UnpackRelayEnvelope(char const* raw, int rawSize, void const*& payload, std::size_t& payloadSize);
    void Receive(int channel);
    void ReceiveP2P();
    void OnControl(Remote& r, char const* data, int size);
    void OnData(Remote& r, char const* data, int size);
    bool QueueDatagram(Remote& r, void const* data, unsigned size);
    void DrainDatagrams(Remote& r);
    void Forward();

    // Lobby, invite/auto flow & callbacks (steam_flow.cpp)
    Remote* Find(u64 id);
    Remote* Get(u64 id);
    int CountLinked() const;
    bool InLobby(u64 id) const;
    void ResetClientEnd();
    void RecordClientEnd(char const* why, connect::ClientFlow::State before);
    void Drop(Remote* r, char const* why);
    void RotatePartnerForRejoin(u64 id);
    void Skip(u64 id, uint32_t forMs);
    bool IsSkipped(u64 id) const;
    bool CreateLobby();
    void CreateLobbyAuto();
    void Join(u64 id, char const* why);
    void InspectLobby(u64 id, char const* why, bool joined = false);
    connect::LobbyMetadata ReadLobbyMetadata(u64 id) const;
    void LeaveLobby();
    char const* NameOf(u64 id);
    bool IsFriend(u64 id) const;
    u64 FriendByName(char const* name) const;
    int SendFrame(u64 to, char const* what, char const* frame, size_t size);
    bool SetPresence(char const* key, char const* value);
    void PublishPresence();
    void SendJoinRequest(bool resend);
    void TearDownClient();
    void LogClientChange(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet);
    void FinishLobbyEntry(u64 id, u64 lobbyOwner);
    void AfterClient(char const* event, connect::ClientFlow::State before, unsigned fx, bool quiet = false);
    void PollLobbyMetadata();
    void SendOffer(u64 partner);
    void SendDecline(u64 to, connect::Why why, u64 arg);
    void AfterHost(char const* event, connect::HostFlow::State before, unsigned fx, bool quiet = false);
    void OnAccepted(connect::ClientFlow::Source source, char const* what, u64 friendId, char const* raw);
    void OnLobbyInvite(u64 inviter, u64 lobbyId);
    void OnJoinRequest(u64 from, connect::Frame const& frame);
    void OnNegotiation(u64 from, char const* data, int size);
    void OnClientLinkLost(u64 id, char const* why);
    void OnHostLinkLost(u64 id, char const* why);
    void ClientTick();
    void HostTick();
    void ParseLegacyConnect(char const* text);
    void StartupAccept();
    void FillConnection(Connection& out);
    void OnLobbyCreated(char const* p, int size);
    void OnLobbyEnter(char const* p, int size);
    void OnCallback(int callback, char const* p, int size);
    void Search();

    // Networking config & diagnostics (steam_diagnostics.cpp)
    config::Read ReadNetworkingConfig(int id) const;
    bool ConfigureNetworking();
    void TransportConfigDiagnostics() const;
    void SendDiagnostics(uint32_t now);
};

SOCKET LoopbackSocket(unsigned short* port);
void AddDirectCandidate(Remote& r, uint32_t netIpv4, uint16_t port, bool allowLoopback = AllowDirectLoopback());
unsigned GatherLocalIpv4s(uint32_t (&out)[8], bool allowLoopback = AllowDirectLoopback());
void InvalidatePublishedLocked(PublishedStore& target, bool isHostRole, u64 account, uint64_t generation);

extern Api api;
extern WorkerRuntime rt;
extern PublishedStore store;
inline Remote (&remotes)[MaxRemotes] = rt.remotes;
}
