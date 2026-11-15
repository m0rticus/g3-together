// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
#include "steam_runtime.h"
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#include "config.h"
#include "enet_policy.h"
#include "log.h"
#include "relaywire.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace g3t::steam {
SOCKET LoopbackSocket(unsigned short* port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(local);
    u_long nonBlocking = 1;
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ioctlsocket(s, FIONBIO, &nonBlocking) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&net::PoseBurstBuffer), sizeof(net::PoseBurstBuffer)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&net::PoseBurstBuffer), sizeof(net::PoseBurstBuffer)) != 0) {
        Logf("steam: cannot configure the owned loopback relay socket (WSA %d)", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    net::DisableWindowsUdpConnReset(s);
    if (port) *port = ntohs(local.sin_port);
    return s;
}

unsigned GatherLocalIpv4s(uint32_t (&out)[8], bool allowLoopback)
{
    unsigned count = 0;
    auto addUnique = [&](uint32_t netIpv4) {
        if (!IsValidDirectCandidateEndpoint(netIpv4, 1, allowLoopback) || count >= 8) return;
        for (unsigned i = 0; i < count; ++i)
            if (out[i] == netIpv4) return;
        out[count++] = netIpv4;
    };
    if (allowLoopback) addUnique(htonl(INADDR_LOOPBACK));
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != INVALID_SOCKET) {
        alignas(SOCKET_ADDRESS_LIST) char listBuf[1024] = {};
        DWORD returned = 0;
        if (WSAIoctl(probe, SIO_ADDRESS_LIST_QUERY, nullptr, 0, listBuf, sizeof(listBuf),
                     &returned, nullptr, nullptr) == 0) {
            auto const* list = reinterpret_cast<SOCKET_ADDRESS_LIST const*>(listBuf);
            for (INT i = 0; i < list->iAddressCount && count < 8; ++i) {
                if (!list->Address[i].lpSockaddr ||
                    list->Address[i].iSockaddrLength < static_cast<INT>(sizeof(sockaddr_in)))
                    continue;
                auto const* sin = reinterpret_cast<sockaddr_in const*>(list->Address[i].lpSockaddr);
                if (sin->sin_family != AF_INET) continue;
                const uint32_t hostOrder = ntohl(sin->sin_addr.s_addr);
                if ((hostOrder >> 24) == 127) continue;
                addUnique(sin->sin_addr.s_addr);
            }
        }
        closesocket(probe);
    }
    return count;
}

void AddDirectCandidate(Remote& r, uint32_t netIpv4, uint16_t port, bool allowLoopback, bool replaceLastIfFull)
{
    if (!IsValidDirectCandidateEndpoint(netIpv4, port, allowLoopback)) return;
    for (unsigned i = 0; i < r.directCandidateCount; ++i)
        if (r.directCandidates[i].hostIpv4 == netIpv4 && r.directCandidates[i].port == port)
            return;
    if (r.directCandidateCount < 8) {
        r.directCandidates[r.directCandidateCount++] = DirectEndpoint{netIpv4, port};
    } else if (replaceLastIfFull) {
        r.directCandidates[7] = DirectEndpoint{netIpv4, port};
    }
}

void WorkerRuntime::ConfigureSocketOptions(int scope, intptr_t handle)
{
    if (!networkingUtils || !api.SetConfigValue || !handle ||
        (scope != config::ListenSocketScope && scope != config::ConnectionScope))
        return;
    auto readScoped = [&](int id) { return ReadNetworkingConfig(id, scope, handle); };
    auto writeScoped = [&](int id, int32_t value) {
        return api.SetConfigValue(networkingUtils, id, scope, handle, config::Int32, &value) != 0;
    };
    const config::Read iceBefore = readScoped(config::P2PTransportIceEnable);
    if (scope == config::ListenSocketScope || !iceBefore.Valid() || iceBefore.value != config::IceEnableAll)
        writeScoped(config::P2PTransportIceEnable, config::IceEnableAll);
    for (auto const& setting : config::Settings) {
        if (!setting.capacity && !setting.exact) continue;  // preserve-default settings (e.g. NagleTime)
        if (api.GetConfigValue) {
            const config::Applied applied = config::Apply(setting, readScoped, writeScoped);
            if (!applied.before.Valid()) writeScoped(setting.id, setting.capacity);
        } else {
            writeScoped(setting.id, setting.capacity);
        }
    }
}

bool WorkerRuntime::ConfigurePeerLanes(uint32_t conn)
{
    if (!socketsLaneTransport || !conn) return false;
    static int const priorities[LaneCount] = {0, 1, 2, 3};
    static uint16_t const weights[LaneCount] = {8, 4, 2, 1};
    const int rc = api.ConfigureConnectionLanes(sockets, conn, LaneCount, priorities, weights);
    if (rc != ResultOk)
        Logf("steam-send: ConfigureConnectionLanes conn=%u failed result=%d", conn, rc);
    return rc == ResultOk;
}

void WorkerRuntime::ClosePeerP2P(Remote& r)
{
    if (r.p2pConn && sockets && api.CloseConnection)
        api.CloseConnection(sockets, r.p2pConn, 0, "session ended", 0);
    r.p2pConn = 0;
    r.p2pConnected = r.p2pLanesConfigured = false;
}

void WorkerRuntime::OpenClientP2P(Remote& r)
{
    if (!socketsLaneTransport || isHost || r.p2pConn) return;
    const uint32_t now = Now();
    if (r.lastP2POpenAt && now - r.lastP2POpenAt < P2PReconnectCooldownMs) return;
    r.lastP2POpenAt = now ? now : 1;
    char identity[160];
    Identity(identity, r.id);
    r.p2pConn = api.ConnectP2P(sockets, identity, P2PVirtualPort, 0, nullptr);
    if (!r.p2pConn) {
        Logf("steam-send: ConnectP2P to %llu failed; keeping Messages fallback", r.id);
        return;
    }
    ConfigureSocketOptions(config::ConnectionScope, static_cast<intptr_t>(r.p2pConn));
    r.p2pLanesConfigured = ConfigurePeerLanes(r.p2pConn);
    Logf("steam-send: ConnectP2P peer=%llu conn=%u vport=%d lanes=%u ice=all",
         r.id, r.p2pConn, P2PVirtualPort, r.p2pLanesConfigured ? LaneCount : 0u);
}

void WorkerRuntime::SendDirectUdpControl(Remote& r)
{
    const uint64_t wireGen = isHost ? r.generation : r.directOfferGeneration;
    if (!directUdpEnabled || !enetPort || !r.haveDirectCookie || !wireGen) return;
    uint32_t localIps[8] = {};
    const unsigned ipCount = GatherLocalIpv4s(localIps);
    uint8_t frame[60] = {};
    frame[0] = 'U';
    std::memcpy(frame + 1, &wireGen, 8);
    std::memcpy(frame + 9, r.directCookie, 16);
    std::memcpy(frame + 25, &enetPort, 2);
    frame[27] = static_cast<uint8_t>(ipCount);
    for (unsigned i = 0; i < ipCount; ++i)
        std::memcpy(frame + 28 + i * 4, &localIps[i], 4);
    if (RawControlSend(r.id, SendReliable, frame, 28 + ipCount * 4) == ResultOk)
        r.lastDirectOfferSent = Now() ? Now() : 1;
}

void WorkerRuntime::RecordAcceptedItemTraffic(send::Item const& item, std::size_t encodedSize)
{
    const std::size_t size = item.bytes.size();
    ++traffic.txDatagrams;
    traffic.txBytes += size;
    traffic.txEnvelopeBytes += encodedSize;
    ++traffic.txDataDatagrams;
    traffic.txDataBytes += size;
    if (item.parsedEnet) {
        traffic.txEnetProtocolBytes += item.protocolBytes;
        traffic.txEnetAckBytes += item.ackBytes;
    } else {
        ++traffic.txEnetParseFailures;
    }
}

int WorkerRuntime::SubmitSocketEnvelope(Remote& r, void const* data, unsigned size, int flags,
                                        uint16_t lane, std::size_t& encodedSize)
{
    char const* session = GetLocalConfig().sessionId;
    if (!lobby || size > RelayPayloadLimit || !ValidRelaySession(session)) return 2;
    const std::size_t needed = RelayHeaderSize(session) + size;
    auto* msg = static_cast<SteamNetworkingMessage*>(
        api.AllocateMessage(networkingUtils, static_cast<int>(needed)));
    if (!msg || !msg->data) {
        if (msg) api.MessageRelease(msg);
        return 2;
    }
    if (!RelayEncode(lobby, session, data, size, msg->data, needed, &encodedSize)) {
        api.MessageRelease(msg);
        return 2;
    }
    msg->size = static_cast<int32_t>(encodedSize);
    msg->conn = r.p2pConn;
    msg->flags = flags & ~SendAutoRestart;
    msg->lane = lane < LaneCount ? lane : static_cast<uint16_t>(LaneCount - 1);
    void* batch[1] = {msg};
    int64_t numberOrResult = 0;
    api.SendMessages(sockets, 1, batch, &numberOrResult);
    return numberOrResult > 0 ? ResultOk
        : numberOrResult < 0 ? static_cast<int>(-numberOrResult) : 2;
}

int WorkerRuntime::SendMessagesEnvelope(u64 to, int flags, int channel,
                                        void const* data, unsigned size, std::size_t& encodedSize)
{
    char packet[RelayMaxEnvelopeSize];
    if (!lobby || size > RelayPayloadLimit) {
        Logf("steam: cannot send relay message (lobby=%llu bytes=%u)", lobby, size);
        return 2;
    }
    if (!RelayEncode(lobby, GetLocalConfig().sessionId, data, size, packet, sizeof(packet), &encodedSize)) {
        Logf("steam: cannot encode relay message (lobby=%llu bytes=%u)", lobby, size);
        return 2;
    }
    char identity[160];
    Identity(identity, to);
    const int result = api.SendToUser(messages, identity, packet, static_cast<unsigned>(encodedSize),
                                      flags | SendAutoRestart, channel);
    if (result != ResultOk) {
        ++sendFailures;
        const uint32_t now = Now();
        if (!lastSendFailure || now - lastSendFailure >= 5000) {
            lastSendFailure = now ? now : 1;
            Logf("steam: message to %llu failed (result=%d channel=%d bytes=%u failures=%u)",
                 to, result, channel, size, sendFailures);
        }
    }
    return result;
}

int WorkerRuntime::RawDataSend(Remote& r, send::Item const& item)
{
    const unsigned size = static_cast<unsigned>(item.bytes.size());
    if (socketsLaneTransport && r.p2pConn && r.p2pConnected && r.p2pLanesConfigured && api.AllocateMessage) {
        std::size_t encodedSize = 0;
        const int result = SubmitSocketEnvelope(r, item.bytes.data(), size, item.Flags(), item.Lane(), encodedSize);
        if (result == ResultOk) {
            ++(item.Flags() & SendReliable ? r.steamReliableOk : r.steamUnreliableOk);
            RecordAcceptedItemTraffic(item, encodedSize);
            return ResultOk;
        }
        ++r.steamDataFailed[result >= 0 && result < 128 ? result : 0];
        ++sendFailures;
        const uint32_t now = Now();
        if (!lastSendFailure || now - lastSendFailure >= 5000) {
            lastSendFailure = now ? now : 1;
            Logf("steam: P2P lane=%u message to %llu failed (result=%d bytes=%u failures=%u)",
                 static_cast<unsigned>(item.Lane() < LaneCount ? item.Lane() : LaneCount - 1),
                 r.id, result, size, sendFailures);
        }
        if (send::Retryable(result)) return result;
        Logf("steam-send: P2P lane non-retryable failure peer=%llu conn=%u result=%d; closing P2P lane and falling back to Messages",
             r.id, r.p2pConn, result);
        ClosePeerP2P(r);
        r.lastP2POpenAt = now ? now : 1;
    }
    std::size_t encodedSize = 0;
    const int result = SendMessagesEnvelope(r.id, item.Flags(), ChannelData, item.bytes.data(), size, encodedSize);
    if (result == ResultOk) {
        ++(item.Flags() & SendReliable ? r.steamReliableOk : r.steamUnreliableOk);
        RecordAcceptedItemTraffic(item, encodedSize);
    } else {
        ++r.steamDataFailed[result >= 0 && result < 128 ? result : 0];
    }
    return result;
}

int WorkerRuntime::RawControlSend(u64 to, int flags, void const* data, unsigned size)
{
    std::size_t encodedSize = 0;
    const int result = SendMessagesEnvelope(to, flags | SendNoNagle, ChannelControl, data, size, encodedSize);
    if (Remote* r = Find(to)) {
        if (result == ResultOk)
            ++(flags & SendReliable ? r->steamReliableOk : r->steamUnreliableOk);
        else
            ++r->steamControlFailed[result >= 0 && result < 128 ? result : 0];
    }
    if (result == ResultOk) {
        ++traffic.txDatagrams;
        traffic.txBytes += size;
        traffic.txEnvelopeBytes += encodedSize;
    }
    return result;
}

void WorkerRuntime::Control(u64 to, char what, bool reliable)
{
    char buffer[9] = {what};
    const double time = PingSeconds();
    std::memcpy(buffer + 1, &time, sizeof(time));
    const int result = RawControlSend(to, reliable ? SendReliable : SendUnreliable, buffer, what == 'P' ? 9 : 1);
    if (what == 'P' && result == ResultOk)
        if (Remote* r = Find(to)) r->ping.Sent(time);
}

u64 WorkerRuntime::Sender(char const* message)
{
    if (senderOffset) return api.IdentityGetSteamId(message + senderOffset);
    const u64 at16 = api.IdentityGetSteamId(message + 16), at12 = api.IdentityGetSteamId(message + 12);
    if (at16 || at12) {
        senderOffset = at16 ? 16 : 12;
        Logf("steam: a message's sender identity is at offset %d", senderOffset);
    }
    return at16 ? at16 : at12;
}

bool WorkerRuntime::UnpackRelayEnvelope(char const* raw, int rawSize, void const*& payload, std::size_t& payloadSize)
{
    if (!lobby || rawSize < 0) return false;
    if (!RelayDecode(raw, static_cast<std::size_t>(rawSize), lobby, GetLocalConfig().sessionId, &payload, &payloadSize))
        return false;
    ++traffic.rxDatagrams;
    traffic.rxBytes += payloadSize;
    traffic.rxEnvelopeBytes += static_cast<std::size_t>(rawSize);
    return true;
}

void WorkerRuntime::Receive(int channel)
{
    void* batch[32];
    for (int rounds = 0; rounds < 16; ++rounds) {
        const int count = api.ReceiveOnChannel(messages, channel, batch, 32);
        if (count < 0) ++receiveFailures;
        if (count <= 0) return;
        for (int i = 0; i < count; ++i) {
            auto const* msg = static_cast<SteamNetworkingMessage const*>(batch[i]);
            char const* data = static_cast<char const*>(msg->data);
            int size = msg->size;
            const u64 from = Sender(static_cast<char const*>(batch[i]));
            void const* payload = nullptr;
            std::size_t payloadSize = 0;
            const bool relayFrame = UnpackRelayEnvelope(data, size, payload, payloadSize);
            if (!relayFrame && channel == ChannelControl &&
                connect::IsFrame(data, size < 0 ? 0 : static_cast<std::size_t>(size))) {
                OnNegotiation(from, data, size);
                api.MessageRelease(batch[i]);
                continue;
            }
            if (!relayFrame) {
                ++isolatedDrops;
                api.MessageRelease(batch[i]);
                continue;
            }
            data = static_cast<char const*>(payload);
            size = static_cast<int>(payloadSize);
            Remote* r = !from || (!isHost && from != owner) ? nullptr
                : isHost && !Find(from) && !(lobby && InLobby(from)) ? nullptr : Get(from);
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

void WorkerRuntime::ReceiveP2P()
{
    if (!socketsLaneTransport || !api.ReceiveOnConnection) return;
    void* batch[32];
    for (auto& r : remotes) {
        if (!r.id || !r.p2pConn) continue;
        for (int rounds = 0; rounds < 16; ++rounds) {
            const int count = api.ReceiveOnConnection(sockets, r.p2pConn, batch, 32);
            if (count < 0) ++receiveFailures;
            if (count <= 0) break;
            for (int i = 0; i < count; ++i) {
                auto const* msg = static_cast<SteamNetworkingMessage const*>(batch[i]);
                char const* data = static_cast<char const*>(msg->data);
                const int size = msg->size;
                void const* payload = nullptr;
                std::size_t payloadSize = 0;
                if (UnpackRelayEnvelope(data, size, payload, payloadSize) && payloadSize > 0) {
                    r.heard = Now();
                    OnData(r, static_cast<char const*>(payload), static_cast<int>(payloadSize));
                } else {
                    ++isolatedDrops;
                }
                api.MessageRelease(batch[i]);
            }
            if (rounds == 15) ++receiveCaps;
        }
    }
}

void WorkerRuntime::OnControl(Remote& r, char const* data, int size)
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
            r.linkedAt = Now();
            CopyName(r.name, sizeof(r.name), api.FriendPersonaName(friends, r.id));
            Logf("steam: %s (%llu) linked to this game", Text(api.FriendPersonaName(friends, r.id)), r.id);
            if (connectionMode == connect::Mode::Invite) {
                const connect::HostFlow::State before = hostFlow.state();
                AfterHost("hello", before, hostFlow.Linked(r.id));
                Logf("connect: connected: partner %llu (%s) is linked to this game", r.id, NameOf(r.id));
            }
            if (directUdpEnabled && !r.haveDirectCookie) {
                if (BCryptGenRandom(nullptr, r.directCookie, sizeof(r.directCookie),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
                    r.haveDirectCookie = true;
                    r.directOfferGeneration = r.generation;
                }
            }
        }
        if (r.linked) {
            if (directUdpEnabled && r.haveDirectCookie) SendDirectUdpControl(r);
            Control(r.id, 'W', true);
        }
        break;
    case 'W':
        if (size != 1) break;
        if (!isHost && r.id == owner && !r.linked) {
            r.linked = true;
            r.linkedAt = clientLinkedAt = Now();
            clientHadLink = true;
            OpenClientP2P(r);
            Logf("steam: welcomed by the host %s; ENet goes over Steam now", hostName);
            if (connectionMode == connect::Mode::Invite) {
                const connect::ClientFlow::State before = clientFlow.state();
                AfterClient("welcomed", before, clientFlow.Welcomed(r.id));
                Logf("connect: connected: welcomed by host %llu (%s)", r.id, hostName);
            }
        }
        break;
    case 'U': {
        if (!directUdpEnabled || size < 28) break;
        uint64_t offerGen = 0;
        uint8_t cookie[16] = {};
        uint16_t peerPort = 0;
        const unsigned ipCount = static_cast<unsigned char>(data[27]);
        if (ipCount > 8 || size != static_cast<int>(28 + ipCount * 4)) break;
        std::memcpy(&offerGen, data + 1, 8);
        std::memcpy(cookie, data + 9, 16);
        std::memcpy(&peerPort, data + 25, 2);
        if (!peerPort || !offerGen) break;
        if (isHost) {
            if (!r.linked || !r.haveDirectCookie || offerGen != r.generation ||
                std::memcmp(cookie, r.directCookie, 16) != 0)
                break;
        } else {
            if (r.id != owner) break;
            r.directOfferGeneration = offerGen;
            std::memcpy(r.directCookie, cookie, 16);
            r.haveDirectCookie = true;
        }
        r.directPeerPort = peerPort;
        r.directCandidateCount = 0;
        const bool allowLoopback = AllowDirectLoopback();
        for (unsigned i = 0; i < ipCount; ++i) {
            uint32_t netIp = 0;
            std::memcpy(&netIp, data + 28 + i * 4, 4);
            AddDirectCandidate(r, netIp, peerPort, allowLoopback);
        }
        if (api.GetSessionConnectionInfo) {
            char identity[160];
            Identity(identity, r.id);
            diagnostics::ConnectionInfo info{};
            diagnostics::SessionStatus status{};
            if (api.GetSessionConnectionInfo(messages, identity, &info, &status) == diagnostics::Connected) {
                uint32_t reflexiveIp = 0;
                if (ExtractReflexiveIpv4(info, reflexiveIp, allowLoopback))
                    AddDirectCandidate(r, reflexiveIp, peerPort, allowLoopback, true);
            }
        }
        if (r.p2pConn && r.p2pConnected && api.GetConnectionInfo) {
            diagnostics::ConnectionInfo info{};
            uint32_t reflexiveIp = 0;
            if (api.GetConnectionInfo(sockets, r.p2pConn, &info) &&
                ExtractReflexiveIpv4(info, reflexiveIp, allowLoopback))
                AddDirectCandidate(r, reflexiveIp, peerPort, allowLoopback, true);
        }
        r.directReady = r.directCandidateCount != 0;
        if (!isHost && r.haveDirectCookie) SendDirectUdpControl(r);
        Logf("steam: direct UDP endpoint exchange peer=%llu port=%u candidates=%u ready=%u",
             r.id, static_cast<unsigned>(peerPort), r.directCandidateCount, r.directReady ? 1u : 0u);
        break;
    }
    case 'P':
        if (size == 9) {
            char pong[9];
            std::memcpy(pong, data, 9);
            pong[0] = 'Q';
            RawControlSend(r.id, SendUnreliable, pong, 9);
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

void WorkerRuntime::OnData(Remote& r, char const* data, int size)
{
    if (!r.linked || size <= 0) return;
    int sent = 0;
    if (isHost) {
        if (r.socket == INVALID_SOCKET) return;
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

bool WorkerRuntime::QueueDatagram(Remote& r, void const* data, unsigned size)
{
    try {
        EnetPacketParse parsed;
        if (sendCodec && r.pending.Push(*sendCodec, data, size, Now(), &parsed)) {
            auto const* bytes = static_cast<unsigned char const*>(data);
            const bool compressed = (bytes[0] & 0x40u) != 0;
            if (!r.firstEnetLogged) {
                r.firstEnetLogged = true;
                Logf("steam-send: first ENet datagram role=%s peer=%llu wire_bytes=%u decoded_bytes=%zu "
                     "compressed=%d command=%u command_header_bytes=%zu action=%s",
                     isHost ? "host" : "client", r.id, size, parsed.packetBytes, compressed ? 1 : 0,
                     parsed.firstCommand, EnetCommandHeaderSize(parsed.firstCommand),
                     parsed.valid ? "classified-and-queued" : "queued-as-unclassified-bulk");
            }
            if (!parsed.valid) {
                const unsigned kind = (compressed ? 16u : 0u) | (parsed.command & 15u);
                if (!r.unclassifiedLogged[kind]) {
                    r.unclassifiedLogged[kind] = true;
                    char hex[33] = {};
                    for (unsigned i = 0; i < (std::min)(size, 16u); ++i)
                        std::snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", bytes[i]);
                    Logf("steam-send: peer=%llu unclassified_enet_bytes=%u first_bytes=%s decoded_bytes=%zu "
                         "command=%u offset=%zu required=%zu remaining=%zu action=sent-as-bulk",
                         r.id, size, hex, parsed.packetBytes, parsed.command, parsed.commandOffset,
                         parsed.commandBytes, parsed.remainingBytes);
                }
            }
            return true;
        }
        Logf("steam-send: peer=%llu invalid_local_datagram_bytes=%u codec_ready=%d action=not-queued",
             r.id, size, sendCodec && sendCodec->Ready() ? 1 : 0);
    } catch (std::bad_alloc const&) {
        Logf("steam-send: peer=%llu action=close-session allocation-failed pending_bytes=%zu", r.id, r.pending.Bytes());
        Drop(&r, "Steam send queue allocation failed");
    }
    return false;
}

void WorkerRuntime::DrainDatagrams(Remote& r)
{
    try {
        const uint32_t now = Now();
        r.pending.EvictStaleUnreliablePoses(now);
        r.pending.counters.peakAge = (std::max)(r.pending.counters.peakAge, r.pending.OldestAge(now));
        bool flushedP2P = false;
        const int result = r.pending.Drain([&](send::Item const& item) {
            const int sent = RawDataSend(r, item);
            if (sent == ResultOk) {
                ++r.toSteam;
                flushedP2P = flushedP2P || (socketsLaneTransport && r.p2pConn && r.p2pConnected);
            }
            return sent;
        });
        if (flushedP2P && api.FlushMessagesOnConnection && r.p2pConn)
            api.FlushMessagesOnConnection(sockets, r.p2pConn);
        if (result != ResultOk && !send::Retryable(result)) {
            Logf("steam-send: peer=%llu action=close-session fatal_result=%d pending_datagrams=%zu pending_bytes=%zu",
                 r.id, result, r.pending.Depth(), r.pending.Bytes());
            char why[160];
            Format(why, "Steam data submission failed (result=%d)", result);
            Drop(&r, why);
        }
    } catch (std::bad_alloc const&) {
        Logf("steam-send: peer=%llu action=close-session allocation-failed pending_bytes=%zu", r.id, r.pending.Bytes());
        Drop(&r, "Steam send queue allocation failed");
    }
}

void WorkerRuntime::Forward()
{
    char datagram[4096];
    auto pumpSocket = [&](SOCKET s, Remote* peer, bool captureClientEndpoint) {
        for (int n = 0; n < 256; ++n) {
            sockaddr_in from{};
            int fromSize = sizeof(from);
            const int size = recvfrom(s, datagram, sizeof(datagram), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (size < 0 && WSAGetLastError() == WSAECONNRESET) continue;
            if (size <= 0) {
                if (size < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                    ++socketFailures;
                    socketError = WSAGetLastError();
                }
                break;
            }
            if ((isHost || enetPort) && ntohs(from.sin_port) != enetPort) continue;
            if (captureClientEndpoint) {
                enetAddress = from;
                haveEnetAddress = true;
            }
            if (!peer || !peer->linked) continue;
            if (!QueueDatagram(*peer, datagram, static_cast<unsigned>(size))) break;
            if (n == 255) ++forwardCaps;
        }
        if (peer && peer->linked) DrainDatagrams(*peer);
    };
    if (isHost) {
        for (auto& r : remotes) {
            if (!r.linked || r.socket == INVALID_SOCKET) continue;
            pumpSocket(r.socket, &r, false);
        }
        return;
    }
    if (relay == INVALID_SOCKET) return;
    Remote* host = owner ? Find(owner) : nullptr;
    pumpSocket(relay, host, true);
}

void WorkerRuntime::OnP2PStatusChanged(char const* p, int size)
{
    if (!socketsLaneTransport || size < static_cast<int>(offsetof(ConnectionStatusChangedParam, oldState) + 4)) return;
    ConnectionStatusChangedParam statusChanged{};
    std::memcpy(&statusChanged, p, offsetof(ConnectionStatusChangedParam, oldState) + 4);
    const uint32_t conn = statusChanged.conn;
    auto const& info = statusChanged.info;
    const u64 peerId = api.IdentityGetSteamId(info.identity);
    if (info.state == diagnostics::Connecting && isHost && listenSocketP2P && info.listenSocket == listenSocketP2P) {
        Remote* r = lobby && peerId && (InLobby(peerId) || Find(peerId)) ? Get(peerId) : nullptr;
        if (!r) {
            api.CloseConnection(sockets, conn, 0, "unauthorized peer", 0);
            return;
        }
        if (r->p2pConn && r->p2pConn != conn) ClosePeerP2P(*r);
        ConfigureSocketOptions(config::ConnectionScope, static_cast<intptr_t>(conn));
        const int rc = api.AcceptConnection(sockets, conn);
        if (rc == ResultOk) {
            r->p2pConn = conn;
            r->p2pConnected = false;
            r->p2pLanesConfigured = ConfigurePeerLanes(conn);
            Logf("steam-send: P2P connection accepted peer=%llu conn=%u lanes=%u ice=all",
                 peerId, conn, r->p2pLanesConfigured ? LaneCount : 0u);
        } else {
            Logf("steam-send: AcceptConnection peer=%llu conn=%u failed result=%d", peerId, conn, rc);
            api.CloseConnection(sockets, conn, 0, "accept failed", 0);
        }
    } else if (info.state == diagnostics::Connected) {
        if (Remote* r = Find(peerId); r && r->p2pConn == conn) {
            r->p2pConnected = true;
            if (!r->p2pLanesConfigured) r->p2pLanesConfigured = ConfigurePeerLanes(conn);
            uint32_t reflexiveIp = 0;
            const bool allowLoopback = AllowDirectLoopback();
            if (r->directPeerPort && ExtractReflexiveIpv4(info, reflexiveIp, allowLoopback)) {
                AddDirectCandidate(*r, reflexiveIp, r->directPeerPort, allowLoopback, true);
                r->directReady = r->directCandidateCount != 0;
            }
            Logf("steam-send: P2P lane session connected peer=%llu conn=%u lanes=%u relayed=%u",
                 peerId, conn, r->p2pLanesConfigured ? LaneCount : 0u,
                 (info.flags & diagnostics::Relayed) ? 1u : 0u);
        }
    } else if (info.state == diagnostics::ClosedByPeer || info.state == diagnostics::ProblemDetectedLocally) {
        if (Remote* r = Find(peerId); r && r->p2pConn == conn) {
            Logf("steam-send: P2P lane session closed peer=%llu conn=%u state=%d reason=%d",
                 peerId, conn, info.state, info.endReason);
            ClosePeerP2P(*r);
            const uint32_t now = Now();
            r->lastP2POpenAt = now ? now : 1;
        } else if (conn) {
            api.CloseConnection(sockets, conn, 0, nullptr, 0);
        }
    }
}
}
