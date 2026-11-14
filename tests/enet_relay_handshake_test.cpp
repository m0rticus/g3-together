// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/enet_policy.h"
#include "../src/steam_send.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace g3t::steam;
using namespace g3t::steam::send;

static void CommandSizes()
{
    const size_t native[] = {
        0, sizeof(ENetProtocolAcknowledge), sizeof(ENetProtocolConnect),
        sizeof(ENetProtocolVerifyConnect), sizeof(ENetProtocolDisconnect),
        sizeof(ENetProtocolPing), sizeof(ENetProtocolSendReliable),
        sizeof(ENetProtocolSendUnreliable), sizeof(ENetProtocolSendFragment),
        sizeof(ENetProtocolSendUnsequenced), sizeof(ENetProtocolBandwidthLimit),
        sizeof(ENetProtocolThrottleConfigure), sizeof(ENetProtocolSendFragment)
    };
    static_assert(ENET_PROTOCOL_COMMAND_COUNT == 13, "update native command coverage");
    static_assert(sizeof(ENetProtocolConnect) == 48, "CONNECT includes its data word");
    static_assert(sizeof(ENetProtocolVerifyConnect) == 44, "VERIFY_CONNECT has no data word");
    static_assert(sizeof(ENetProtocolThrottleConfigure) == 16, "THROTTLE_CONFIGURE has three data words");
#ifdef _WIN32
    static_assert(sizeof(void*) == 4, "run against the production Win32 ABI");
#endif
    for (unsigned i = 0; i < ENET_PROTOCOL_COMMAND_COUNT; ++i) {
        if (EnetCommandHeaderSize(i) != native[i])
            std::fprintf(stderr, "command=%u classifier=%zu native=%zu\n",
                         i, EnetCommandHeaderSize(i), native[i]);
        assert(EnetCommandHeaderSize(i) == native[i]);
    }
    assert(EnetCommandHeaderSize(13) == 0 && EnetCommandHeaderSize(255) == 0);
}

static bool Same(ENetAddress const& a, ENetAddress const& b)
{
    return a.host == b.host && a.port == b.port;
}

static size_t Header(std::vector<uint8_t> const& plain)
{
    return plain[0] & 0x80u ? 4u : 2u;
}

struct Capture {
    unsigned connect = 0, verify = 0, compressedConnect = 0, compressedVerify = 0;
    unsigned ackOnly = 0, reliable = 0, unreliable = 0, definitions = 0, throttle = 0;
    unsigned outboundCompressed = 0, receivedDatagrams = 0, submittedDatagrams = 0;
    unsigned nonzeroConnect = 0;
};

static bool Observe(std::vector<uint8_t> const& plain, bool compressed, Capture& seen)
{
    const size_t header = Header(plain);
    bool onlyAck = true, criticalData = false;
    for (size_t at = header; at < plain.size();) {
        const unsigned command = plain[at] & ENET_PROTOCOL_COMMAND_MASK;
        const unsigned channel = plain[at + 1];
        size_t length = EnetCommandHeaderSize(command);
        assert(length && at + length <= plain.size());
        if (command == ENET_PROTOCOL_COMMAND_CONNECT) {
            ++seen.connect;
            seen.compressedConnect += compressed;
            assert(at == header && plain.size() == header + sizeof(ENetProtocolConnect));
            ENetProtocolConnect connect{};
            std::memcpy(&connect, plain.data() + at, sizeof(connect));
            assert(connect.header.channelID == 0xff);
            assert(ENET_NET_TO_HOST_32(connect.channelCount) == g3t::net::ChannelCount);
            assert(ENET_NET_TO_HOST_32(connect.mtu) == g3t::net::Mtu);
            const auto data = ENET_NET_TO_HOST_32(connect.data);
            assert(data == 0 || data == 0x1234abcdu);
            seen.nonzeroConnect += data != 0;
        } else if (command == ENET_PROTOCOL_COMMAND_VERIFY_CONNECT) {
            ++seen.verify;
            seen.compressedVerify += compressed;
            assert(at == header && plain.size() == header + sizeof(ENetProtocolVerifyConnect));
        } else if (command == ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE) {
            ++seen.throttle;
            ENetProtocolThrottleConfigure configure{};
            std::memcpy(&configure, plain.data() + at, sizeof(configure));
            assert(configure.header.channelID == 0xff);
            assert(ENET_NET_TO_HOST_32(configure.packetThrottleInterval) == 5000);
            assert(ENET_NET_TO_HOST_32(configure.packetThrottleAcceleration) == 0);
            assert(ENET_NET_TO_HOST_32(configure.packetThrottleDeceleration) == 0);
        } else if (command == ENET_PROTOCOL_COMMAND_SEND_RELIABLE) {
            ++seen.reliable;
            criticalData |= channel == g3t::net::ChannelReliable;
        } else if (command == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE) {
            ++seen.unreliable;
        }
        if ((command == ENET_PROTOCOL_COMMAND_SEND_RELIABLE ||
             command == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE) &&
            channel == g3t::net::ChannelDefinitions)
            ++seen.definitions;
        onlyAck &= command == ENET_PROTOCOL_COMMAND_ACKNOWLEDGE;
        if (command == ENET_PROTOCOL_COMMAND_SEND_RELIABLE)
            length += RelayReadUint16(plain.data() + at + 4);
        if (command == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE ||
            command == ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
            command == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED ||
            command == ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT)
            length += RelayReadUint16(plain.data() + at + 6);
        assert(at + length <= plain.size());
        at += length;
    }
    seen.ackOnly += onlyAck;
    return criticalData;
}

struct Relay {
    ENetSocket clientSide = ENET_SOCKET_NULL, serverSide = ENET_SOCKET_NULL;
    ENetAddress clientSideAddress{}, serverSideAddress{}, clientAddress{}, serverAddress{};
    bool haveClient = false, holdUnknown = false, criticalBeforeUnknown = false;
    bool impaired = false, refusedConnect = false, lostConnect = false, lostVerify = false;
    const std::vector<uint8_t> unknown{0, 0, 0x0d, 0, 0, 0};
    Codec codecs[2];
    Queue queues[2];
    Capture captures[2];
};

static bool PumpSide(Relay& relay, unsigned direction)
{
    ENetSocket const receiving = direction == 0 ? relay.clientSide : relay.serverSide;
    ENetSocket const sending = direction == 0 ? relay.serverSide : relay.clientSide;
    auto& codec = relay.codecs[direction];
    auto& queue = relay.queues[direction];
    auto& capture = relay.captures[direction];
    bool moved = false;
    for (unsigned i = 0; i < 128; ++i) {
        uint8_t bytes[RelayPayloadLimit];
        ENetBuffer buffer{sizeof(bytes), bytes};
        ENetAddress from{};
        int const length = enet_socket_receive(receiving, &from, &buffer, 1);
        assert(length >= 0);
        if (!length) break;
        moved = true;
        if (direction == 0) {
            if (relay.haveClient) assert(Same(from, relay.clientAddress));
            relay.clientAddress = from;
            relay.haveClient = true;
        } else {
            assert(relay.haveClient && Same(from, relay.serverAddress));
        }
        assert(length <= static_cast<int>(g3t::net::Mtu));
        std::vector<uint8_t> plain;
        EnetPacketParse decoded{}, pushed{};
        assert(codec.Decode(bytes, static_cast<size_t>(length), plain, &decoded));
        assert(decoded.valid && !decoded.compressed);
        const bool compressed = (bytes[0] & 0x40u) != 0;
        const bool critical = Observe(plain, compressed, capture);
        ++capture.receivedDatagrams;
        assert(queue.Push(codec, bytes, static_cast<size_t>(length), enet_time_get(), &pushed));
        assert(pushed.valid && pushed.firstCommand == decoded.firstCommand &&
               pushed.command == decoded.command && pushed.commandBytes == decoded.commandBytes);
        if (relay.impaired && direction == 0 && !relay.refusedConnect) {
            assert(capture.receivedDatagrams == 1 && capture.connect == 1 && !compressed);
            assert(decoded.firstCommand == ENET_PROTOCOL_COMMAND_CONNECT);
            assert(queue.Depth() == 1 && queue.Bytes() > 0);
            std::vector<uint8_t> refused;
            assert(queue.Drain([&](Item const& item) {
                assert(item.Flags() == 37 && item.reliable && !(item.bytes[0] & 0x40u));
                refused = item.bytes;
                return LimitExceeded;
            }) == LimitExceeded);
            assert(!refused.empty() && queue.Depth() == 1 && queue.Bytes() == refused.size());
            assert(queue.counters.failed[25] == 1 && queue.counters.retries == 1);
            relay.refusedConnect = true;
            assert(queue.Drain([&](Item const& item) {
                assert(item.Flags() == 37 && item.reliable && item.bytes == refused);
                relay.lostConnect = true;
                ++capture.submittedDatagrams;
                return Ok; // Accepted by Steam, then lost before the host receives it.
            }) == Ok);
            assert(relay.lostConnect && queue.Depth() == 0 && queue.Bytes() == 0);
            continue;
        }
        if (relay.holdUnknown && direction == 0 && !critical) continue;

        assert(queue.Drain([&](Item const& item) {
            assert(item.Flags() == 37);
            if (item.bytes == relay.unknown) {
                assert(relay.holdUnknown && relay.criticalBeforeUnknown && !item.reliable);
                relay.holdUnknown = false;
            } else {
                std::vector<uint8_t> output;
                EnetPacketParse parse{};
                assert(codec.Decode(item.bytes.data(), item.bytes.size(), output, &parse) && parse.valid);
                Capture outputCapture{};
                if (Observe(output, (item.bytes[0] & 0x40u) != 0, outputCapture))
                    relay.criticalBeforeUnknown = true;
                capture.outboundCompressed += (item.bytes[0] & 0x40u) != 0;
                if (relay.impaired && direction == 1 && outputCapture.verify && !relay.lostVerify) {
                    assert(relay.lostConnect && relay.captures[0].connect >= 2 &&
                           capture.verify == 1 && outputCapture.compressedVerify == 0 && item.reliable);
                    relay.lostVerify = true;
                    ++capture.submittedDatagrams;
                    return Ok; // Also lose the first accepted VERIFY_CONNECT.
                }
            }
            auto const& to = direction == 0 ? relay.serverAddress : relay.clientAddress;
            ENetBuffer out{item.bytes.size(), const_cast<uint8_t*>(item.bytes.data())};
            assert(enet_socket_send(sending, &to, &out, 1) == static_cast<int>(item.bytes.size()));
            ++capture.submittedDatagrams;
            return Ok;
        }) == Ok);
    }
    return moved;
}

struct State {
    unsigned connected = 0, disconnected = 0, received = 0, channelMask = 0;
    unsigned round = 0, other = 0;
    uint32_t connectData = 0;
    ENetPeer* peer = nullptr;
};

static std::vector<uint8_t> Payload(unsigned round, unsigned sender, unsigned channel)
{
    std::vector<uint8_t> data(64);
    for (unsigned i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(round * 37 + sender * 13 + channel * 7 + i % 4);
    return data;
}

static bool Service(ENetHost* host, State& state)
{
    ENetEvent event{};
    int result;
    bool active = false;
    while ((result = enet_host_service(host, &event, 0)) > 0) {
        active = true;
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            ++state.connected;
            state.peer = event.peer;
            state.connectData = event.data;
            assert(event.peer->channelCount == g3t::net::ChannelCount &&
                   event.peer->mtu == g3t::net::Mtu);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            ++state.disconnected;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            assert(event.channelID < g3t::net::ChannelCount);
            if (event.channelID == g3t::net::ChannelReliable)
                assert(event.packet->flags & ENET_PACKET_FLAG_RELIABLE);
            auto const expected = Payload(state.round, state.other, event.channelID);
            assert(event.packet->dataLength == expected.size());
            assert(std::memcmp(event.packet->data, expected.data(), expected.size()) == 0);
            assert((state.channelMask & (1u << event.channelID)) == 0);
            state.channelMask |= 1u << event.channelID;
            ++state.received;
            enet_packet_destroy(event.packet);
        }
    }
    assert(result == 0);
    return active;
}

static void Tick(ENetHost* client, State& cs, ENetHost* server, State& ss, Relay& relay)
{
    bool active = Service(client, cs);
    active = PumpSide(relay, 0) || active;
    active = Service(server, ss) || active;
    active = PumpSide(relay, 1) || active;
    if (!active) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

template<class Predicate>
static void Until(Predicate done, ENetHost* client, State& cs, ENetHost* server, State& ss, Relay& relay,
                  int seconds = 3)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done() && std::chrono::steady_clock::now() < deadline)
        Tick(client, cs, server, ss, relay);
    assert(done());
}

static void SendChannel(ENetHost* host, ENetPeer* peer, unsigned round, unsigned sender, unsigned channel)
{
    auto const data = Payload(round, sender, channel);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                             g3t::net::PacketFlags((channel & 1u) != 0));
    assert(packet && enet_peer_send(peer, static_cast<enet_uint8>(channel), packet) == 0);
    enet_host_flush(host);
}

static ENetSocket BindRelay(ENetAddress const& bind, ENetAddress& address)
{
    ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    assert(socket != ENET_SOCKET_NULL && enet_socket_bind(socket, &bind) == 0);
    assert(enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1) == 0);
    assert(enet_socket_get_address(socket, &address) == 0);
    return socket;
}

static void ImpairedHandshake(ENetHost* client, ENetHost* server, Relay& relay)
{
    relay.impaired = true;
    State cs{}, ss{};
    cs.round = ss.round = 1;
    cs.other = 1;
    ss.other = 0;
    ENetPeer* peer = g3t::net::ConnectTransportHost(client, &relay.clientSideAddress);
    assert(peer);
    Until([&] { return cs.connected == 1 && ss.connected == 1; }, client, cs, server, ss, relay, 10);
    assert(relay.refusedConnect && relay.lostConnect && relay.lostVerify);
    assert(relay.captures[0].connect >= 2 && relay.captures[0].compressedConnect == 0);
    assert(relay.captures[1].verify >= 2 && relay.captures[1].compressedVerify == 0);
    assert(peer == cs.peer && ss.peer && ss.connectData == 0);

    SendChannel(client, cs.peer, 1, 0, g3t::net::ChannelReliable);
    SendChannel(server, ss.peer, 1, 1, g3t::net::ChannelReliable);
    Until([&] {
        return cs.received == 1 && ss.received == 1 &&
               !cs.peer->reliableDataInTransit && !ss.peer->reliableDataInTransit &&
               !relay.queues[0].Depth() && !relay.queues[1].Depth();
    }, client, cs, server, ss, relay, 10);
    assert(cs.channelMask == (1u << g3t::net::ChannelReliable) &&
           ss.channelMask == (1u << g3t::net::ChannelReliable));
    assert(cs.connected == 1 && ss.connected == 1 && !cs.disconnected && !ss.disconnected);
    assert(cs.peer->state == ENET_PEER_STATE_CONNECTED && ss.peer->state == ENET_PEER_STATE_CONNECTED);
    for (unsigned direction = 0; direction < 2; ++direction) {
        auto const& queue = relay.queues[direction];
        assert(queue.Depth() == 0 && queue.Bytes() == 0 && queue.counters.unclassified == 0);
        assert(queue.counters.failed[25] == (direction == 0 ? 1u : 0u));
        assert(queue.counters.failed[16] == 0 && queue.counters.failed[41] == 0);
        assert(queue.counters.retries == (direction == 0 ? 1u : 0u));
    }
    std::printf("impaired connected=2 exact_reliable_payloads=2 connect=%u compressed_connect=%u "
                "verify=%u compressed_verify=%u result25=1 lost_connect=1 lost_verify=1 "
                "flags=37 unclassified=0 queues_empty=2\n",
                relay.captures[0].connect, relay.captures[0].compressedConnect,
                relay.captures[1].verify, relay.captures[1].compressedVerify);
}

static void DirectUdpCookiePunch()
{
    assert(enet_initialize() == 0);
    ENetAddress bindAny{ENET_HOST_ANY, 0};
    char const* error = nullptr;
    ENetHost* server = g3t::net::CreateTransportHost(&bindAny, 1, error);
    ENetHost* client = g3t::net::CreateTransportHost(&bindAny, 1, error);
    assert(server && client && !error);
    ENetAddress serverBound{}, clientBound{};
    assert(enet_socket_get_address(server->socket, &serverBound) == 0 && serverBound.port);
    assert(enet_socket_get_address(client->socket, &clientBound) == 0 && clientBound.port);
    ENetAddress serverLo{}, clientLo{};
    assert(enet_address_set_host_ip(&serverLo, "127.0.0.1") == 0);
    assert(enet_address_set_host_ip(&clientLo, "127.0.0.1") == 0);
    serverLo.port = serverBound.port;
    clientLo.port = clientBound.port;

    static uint8_t const cookie[16] = {0x91,0x42,0x7c,0x10,0xaa,0x55,0x33,0xef,
                                       0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0};
    static bool serverVerified = false, clientBidirectional = false;
    static unsigned uninvitedDropped = 0;
    static ENetAddress verifiedClient{}, verifiedServer{};
    serverVerified = clientBidirectional = false;
    uninvitedDropped = 0;

    server->intercept = [](ENetHost* h, ENetEvent*) -> int {
        if (h->receivedDataLength == 26 && std::memcmp(h->receivedData, "G3TPUNCH", 8) == 0) {
            if (std::memcmp(h->receivedData + 8, cookie, 16) == 0) {
                serverVerified = true;
                verifiedClient = h->receivedAddress;
                uint8_t reply[26] = {};
                std::memcpy(reply, "G3TPUNCH", 8);
                std::memcpy(reply + 8, cookie, 16);
                reply[24] = 0x02u;
                reply[25] = 1u;
                ENetBuffer b{sizeof(reply), reply};
                enet_socket_send(h->socket, &h->receivedAddress, &b, 1);
            }
            return 1;
        }
        if (serverVerified && Same(h->receivedAddress, verifiedClient)) return 0;
        ++uninvitedDropped;
        return 1;
    };
    client->intercept = [](ENetHost* h, ENetEvent*) -> int {
        if (h->receivedDataLength == 26 && std::memcmp(h->receivedData, "G3TPUNCH", 8) == 0) {
            if (std::memcmp(h->receivedData + 8, cookie, 16) == 0 && (h->receivedData[24] & 0x02u)) {
                clientBidirectional = true;
                verifiedServer = h->receivedAddress;
            }
            return 1;
        }
        return clientBidirectional && Same(h->receivedAddress, verifiedServer) ? 0 : 1;
    };

    // Uninvited UDP connect attempt from another socket must be dropped by intercept before ENet sees it.
    ENetHost* stranger = g3t::net::CreateTransportHost(&bindAny, 1, error);
    assert(stranger && !error);
    assert(g3t::net::ConnectTransportHost(stranger, &serverLo, 0xdeadbeefu) != nullptr);
    enet_host_flush(stranger);
    State cs{}, ss{}, xs{};
    for (int tick = 0; tick < 20; ++tick) {
        Service(stranger, xs);
        Service(server, ss);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(ss.connected == 0 && uninvitedDropped >= 1);

    // Cookie-authenticated punch opens the direct path with zero compression and full channel delivery.
    uint8_t probe[26] = {};
    std::memcpy(probe, "G3TPUNCH", 8);
    std::memcpy(probe + 8, cookie, 16);
    probe[24] = 0x01u;
    probe[25] = 2u;
    ENetBuffer pbuf{sizeof(probe), probe};
    assert(enet_socket_send(client->socket, &serverLo, &pbuf, 1) == 26);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!clientBidirectional && std::chrono::steady_clock::now() < deadline) {
        Service(server, ss);
        Service(client, cs);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(serverVerified && clientBidirectional);

    uint32_t tag = 0;
    std::memcpy(&tag, cookie, sizeof(tag));
    cs.round = ss.round = 7;
    cs.other = 1;
    ss.other = 0;
    ENetPeer* directPeer = g3t::net::ConnectTransportHost(client, &verifiedServer, tag);
    assert(directPeer);
    while ((cs.connected != 1 || ss.connected != 1) && std::chrono::steady_clock::now() < deadline) {
        Service(client, cs);
        Service(server, ss);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cs.connected == 1 && ss.connected == 1 && ss.connectData == tag);
    for (unsigned ch = 0; ch < g3t::net::ChannelCount; ++ch) {
        SendChannel(client, cs.peer, 7, 0, ch);
        SendChannel(server, ss.peer, 7, 1, ch);
    }
    while ((cs.received != 8 || ss.received != 8) && std::chrono::steady_clock::now() < deadline) {
        Service(client, cs);
        Service(server, ss);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(cs.channelMask == 0xff && ss.channelMask == 0xff);
    enet_host_destroy(stranger);
    enet_host_destroy(client);
    enet_host_destroy(server);
    enet_deinitialize();
    std::puts("direct ENet UDP cookie hole-punch and uninvited packet drop passed");
}

int ENET_CALLBACK main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "sizes") == 0) {
        CommandSizes();
        std::puts("all 13 native command sizes match");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "direct") == 0) {
        DirectUdpCookiePunch();
        return 0;
    }
    const bool impaired = argc == 2 && std::strcmp(argv[1], "impaired") == 0;
    assert(argc == 1 || impaired);
    static_assert(Ignored == 41 && Timeout == 16 && LimitExceeded == 25, "Steam EResult values");
    assert(enet_initialize() == 0);
    ENetAddress bind{ENET_HOST_ANY, 0};
    assert(enet_address_set_host_ip(&bind, "127.0.0.1") == 0);
    char const* error = nullptr;
    ENetHost* server = g3t::net::CreateTransportHost(&bind, 1, error);
    assert(server && !error);
    ENetHost* client = g3t::net::CreateTransportHost(nullptr, 1, error);
    assert(client && !error);
    for (auto host : {client, server}) {
        assert(host->channelLimit == g3t::net::ChannelCount && host->mtu == g3t::net::Mtu);
        assert(host->compressor.context && !host->compressor.compress && host->compressor.decompress);
    }
    Relay relay;
    assert(relay.codecs[0].Ready() && relay.codecs[1].Ready());
    relay.serverAddress = server->address;
    relay.clientSide = BindRelay(bind, relay.clientSideAddress);
    relay.serverSide = BindRelay(bind, relay.serverSideAddress);
    assert(!Same(relay.clientSideAddress, relay.serverSideAddress));
    if (impaired) {
        ImpairedHandshake(client, server, relay);
        enet_socket_destroy(relay.clientSide);
        enet_socket_destroy(relay.serverSide);
        enet_host_destroy(client);
        enet_host_destroy(server);
        enet_deinitialize();
        return 0;
    }
    for (unsigned round = 1; round <= 3; ++round) {
        State cs{}, ss{};
        cs.round = ss.round = round;
        cs.other = 1;
        ss.other = 0;
        ENetPeer* peer = round == 3
            ? enet_host_connect(client, &relay.clientSideAddress, g3t::net::ChannelCount, 0x1234abcdu)
            : g3t::net::ConnectTransportHost(client, &relay.clientSideAddress);
        assert(peer);
        Until([&] { return cs.connected == 1 && ss.connected == 1; }, client, cs, server, ss, relay);
        assert(peer == cs.peer && peer->state == ENET_PEER_STATE_CONNECTED);
        assert(ss.connectData == (round == 3 ? 0x1234abcdu : 0));
        Until([&] { return relay.captures[0].ackOnly + relay.captures[1].ackOnly > 0; },
              client, cs, server, ss, relay);
        enet_peer_throttle_configure(cs.peer, 5000, 0, 0);
        enet_peer_throttle_configure(ss.peer, 5000, 0, 0);
        enet_host_flush(client);
        enet_host_flush(server);
        Until([&] { return relay.captures[0].throttle >= round && relay.captures[1].throttle >= round; },
              client, cs, server, ss, relay);

        if (round == 1) {
            auto& queue = relay.queues[0];
            assert(queue.Depth() == 0);
            assert(queue.Push(relay.codecs[0], relay.unknown.data(), relay.unknown.size(), 1));
            for (int result : {LimitExceeded, Ignored}) {
                assert(queue.Drain([&](Item const& item) {
                    assert(item.Flags() == 37 && item.bytes == relay.unknown && !item.reliable);
                    return result;
                }) == result);
                assert(queue.Depth() == 1 && queue.Bytes() == relay.unknown.size());
            }
            relay.holdUnknown = true;
            SendChannel(client, cs.peer, round, 0, g3t::net::ChannelReliable);
            Until([&] { return !relay.holdUnknown; }, client, cs, server, ss, relay);
            assert(relay.criticalBeforeUnknown && queue.counters.unclassified == 1);
        }
        for (unsigned channel = 0; channel < g3t::net::ChannelCount; ++channel) {
            if (round != 1 || channel != g3t::net::ChannelReliable)
                SendChannel(client, cs.peer, round, 0, channel);
            SendChannel(server, ss.peer, round, 1, channel);
        }
        Until([&] { return cs.received == 8 && ss.received == 8; }, client, cs, server, ss, relay);
        assert(cs.channelMask == 0xff && ss.channelMask == 0xff);
        assert(!cs.disconnected && !ss.disconnected);
        enet_peer_disconnect(peer, 0);
        Until([&] { return cs.disconnected == 1 && ss.disconnected == 1; }, client, cs, server, ss, relay);
        std::printf("round=%u connected=2 payloads=16 channels=8 departure=2 data=%08x\n",
                    round, static_cast<unsigned>(ss.connectData));
    }
    assert(relay.captures[0].connect >= 3 && relay.captures[0].compressedConnect == 0);
    assert(relay.captures[0].nonzeroConnect >= 1);
    assert(relay.captures[1].verify >= 3 && relay.captures[1].compressedVerify == 0);
    for (unsigned direction = 0; direction < 2; ++direction) {
        auto const& c = relay.captures[direction];
        assert(c.reliable >= 12 && c.unreliable >= 12 && c.definitions >= 3 && c.throttle >= 3);
        assert(c.receivedDatagrams && c.submittedDatagrams && !c.outboundCompressed);
        assert(relay.queues[direction].Depth() == 0);
        std::printf("direction=%u packets=%u delivered=%u connect=%u compressed_connect=%u "
                    "verify=%u compressed_verify=%u ack_only=%u reliable=%u unreliable=%u definitions=%u throttle=%u\n",
                    direction, c.receivedDatagrams, c.submittedDatagrams, c.connect, c.compressedConnect,
                    c.verify, c.compressedVerify, c.ackOnly, c.reliable, c.unreliable, c.definitions, c.throttle);
    }
    assert(relay.queues[0].counters.failed[25] == 1 && relay.queues[0].counters.failed[41] == 1);
    assert(relay.queues[0].counters.failed[16] == 0);
    assert(relay.queues[0].counters.accepted[static_cast<unsigned>(Priority::Bulk)] >= 1);
    enet_socket_destroy(relay.clientSide);
    enet_socket_destroy(relay.serverSide);
    enet_host_destroy(client);
    enet_host_destroy(server);
    enet_deinitialize();
    std::puts("native ENet two-relay queued handshake/data/reconnect passed");
    return 0;
}
