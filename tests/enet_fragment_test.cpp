// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/enet_policy.h"
#include "../src/net.h"
#include "../src/steam_send.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <vector>

std::vector<size_t> datagramSizes;
namespace {
bool relayActive = false;
bool directSockets = false;
ENetSocket senderSocket = ENET_SOCKET_NULL;
g3t::steam::send::Codec relayCodec;
g3t::steam::send::Queue toReceiver, toSender;
}
extern "C" int g3t_test_socket_send(ENetSocket socket, ENetAddress const* address, ENetBuffer const* buffers, size_t count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) bytes += buffers[i].dataLength;
    datagramSizes.push_back(bytes);
    if (relayActive) {
        std::vector<uint8_t> packet;
        for (size_t i = 0; i < count; ++i) {
            auto const* data = static_cast<uint8_t const*>(buffers[i].data);
            packet.insert(packet.end(), data, data + buffers[i].dataLength);
        }
        auto& queue = socket == senderSocket ? toReceiver : toSender;
        assert(queue.Push(relayCodec, packet.data(), packet.size(), enet_time_get()));
    } else if (directSockets) {
        return enet_socket_send(socket, address, buffers, count);
    }
    return static_cast<int>(bytes);
}

namespace {
void ReliableSteamRelay()
{
    using namespace g3t::steam::send;
    ENetAddress address{ENET_HOST_ANY, 0};
    assert(enet_initialize() == 0);
    directSockets = true;
    auto* sender = enet_host_create(&address, 1, 8, 0, 0);
    auto* receiver = enet_host_create(&address, 1, 8, 0, 0);
    assert(sender && receiver);
    sender->mtu = receiver->mtu = 1200;
    assert(enet_host_compress_with_range_coder(sender) == 0);
    assert(enet_host_compress_with_range_coder(receiver) == 0);
    address.port = receiver->address.port;
    assert(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    auto* peer = enet_host_connect(sender, &address, 8, 0);
    assert(peer);
    ENetEvent event{};
    const auto deadline = enet_time_get() + 5000;
    // Establish directly: the separate CONNECT-classifier regression owns that path.
    while (peer->state != ENET_PEER_STATE_CONNECTED &&
           static_cast<int32_t>(deadline - enet_time_get()) > 0) {
        enet_host_service(sender, &event, 0);
        enet_host_service(receiver, &event, 0);
        if (peer->state != ENET_PEER_STATE_CONNECTED)
            enet_host_service(sender, &event, 1);
    }
    assert(peer->state == ENET_PEER_STATE_CONNECTED);
    enet_host_flush(receiver);
    enet_host_service(sender, &event, 0);
    senderSocket = sender->socket;
    relayActive = true;
    const std::vector<uint8_t> action(120, 0x5a), save(4000, 0x36);
    auto* packet = enet_packet_create(action.data(), action.size(), ENET_PACKET_FLAG_RELIABLE);
    assert(packet && enet_peer_send(peer, 1, packet) == 0);
    packet = enet_packet_create(save.data(), save.size(), ENET_PACKET_FLAG_RELIABLE);
    assert(packet && enet_peer_send(peer, 6, packet) == 0);
    bool lostAction = false, refused = false;
    unsigned actions = 0, saves = 0, steamReliable = 0;
    while ((actions != 1 || saves != 1 || peer->reliableDataInTransit) &&
           static_cast<int32_t>(deadline - enet_time_get()) > 0) {
        enet_host_service(sender, &event, 1);
        toReceiver.Drain([&](Item const& item) {
            if (!refused) { refused = true; return LimitExceeded; }
            assert(!(item.Flags() & Reliable));
            steamReliable += (item.Flags() & Reliable) != 0;
            std::vector<uint8_t> plain;
            assert(relayCodec.Decode(item.bytes.data(), item.bytes.size(), plain));
            const size_t header = plain[0] & 0x80u ? 4 : 2;
            if (!lostAction && plain[header + 1] == 1) {
                lostAction = true;
                return Ok; // Steam accepted it; ordinary unreliable network loss follows.
            }
            ENetBuffer buffer{item.bytes.size(), const_cast<uint8_t*>(item.bytes.data())};
            assert(enet_socket_send(sender->socket, &address, &buffer, 1) == static_cast<int>(buffer.dataLength));
            return Ok;
        });
        while (enet_host_service(receiver, &event, 0) > 0) {
            if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
            assert(event.packet->flags & ENET_PACKET_FLAG_RELIABLE);
            if (event.channelID == 1) {
                assert(event.packet->dataLength == action.size());
                assert(!std::memcmp(event.packet->data, action.data(), action.size()));
                ++actions;
            } else {
                assert(event.channelID == 6 && event.packet->dataLength == save.size());
                assert(!std::memcmp(event.packet->data, save.data(), save.size()));
                ++saves;
            }
            enet_packet_destroy(event.packet);
        }
        enet_host_flush(receiver);
        toSender.Drain([&](Item const& item) {
            assert(!(item.Flags() & Reliable));
            ENetBuffer buffer{item.bytes.size(), const_cast<uint8_t*>(item.bytes.data())};
            ENetAddress target{peer->address.host, sender->address.port};
            assert(enet_socket_send(receiver->socket, &target, &buffer, 1) == static_cast<int>(buffer.dataLength));
            return Ok;
        });
    }
    assert(refused && lostAction && actions == 1 && saves == 1 && steamReliable == 0);
    assert(!peer->reliableDataInTransit);
    assert(toReceiver.Depth() == 0 && toSender.Depth() == 0);
    relayActive = false;
    directSockets = false;
    enet_host_destroy(sender);
    enet_host_destroy(receiver);
    enet_deinitialize();
}

void Queue(size_t size, enet_uint32 flags, enet_uint8 command, bool reliable, unsigned fragments,
           enet_uint32 mtu = 1200, bool checksum = false, enet_uint16 epoch = 0)
{
    // Pure queue construction: no ENet initialization, sockets, service or traffic.
    ENetHost host{};
    host.maximumPacketSize = ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
    host.checksum = checksum ? &enet_crc32 : nullptr;
    ENetChannel channel{};
    channel.outgoingUnreliableSequenceNumber = epoch;
    ENetPeer peer{};
    peer.host = &host;
    peer.channels = &channel;
    peer.channelCount = 1;
    peer.state = ENET_PEER_STATE_CONNECTED;
    peer.mtu = mtu;
    enet_list_clear(&peer.outgoingCommands);
    enet_list_clear(&peer.outgoingSendReliableCommands);
    ENetPacket* packet = enet_packet_create(nullptr, size, flags);
    assert(packet && enet_peer_send(&peer, 0, packet) == 0);
    ENetList* queued = reliable ? &peer.outgoingSendReliableCommands : &peer.outgoingCommands;
    ENetList* other = reliable ? &peer.outgoingCommands : &peer.outgoingSendReliableCommands;
    assert(enet_list_empty(other) && enet_list_size(queued) == fragments);
    assert(packet->referenceCount == fragments);
    assert(channel.outgoingReliableSequenceNumber == (reliable ? fragments : 0));
    while (!enet_list_empty(queued)) {
        auto* sent = static_cast<ENetOutgoingCommand*>(enet_list_remove(enet_list_begin(queued)));
        assert((sent->command.header.command & ENET_PROTOCOL_COMMAND_MASK) == command);
        assert(((sent->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0) == reliable);
        assert(sent->packet == packet);
        --packet->referenceCount;
        enet_free(sent);
    }
    assert(!packet->referenceCount);
    enet_packet_destroy(packet);
}
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    constexpr size_t legacyBatch = 1199;
    static_assert(legacyBatch == 1199);
    static_assert(g3t::net::NpcDatagramBudget == 1172);
    static_assert(g3t::net::NpcDatagramBudget < legacyBatch);
    static_assert(sizeof(ENetProtocolHeader) + sizeof(ENetProtocolSendFragment) == 28);
    const auto bestEffort = g3t::net::PacketFlags(false);
    const auto reliable = g3t::net::PacketFlags(true);
    if (!std::strcmp(argv[1], "boundary")) {
        Queue(legacyBatch, 0, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, true, 2);  // reproduce the old implicit promotion
        Queue(legacyBatch, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, false, 2);
        for (size_t size : {611u, 1172u})
            Queue(size, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE, false, 1);
        for (size_t size : {1173u, 1174u, 1200u})
            Queue(size, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, false, 2);
    } else if (!std::strcmp(argv[1], "reliable")) {
        Queue(611, reliable, ENET_PROTOCOL_COMMAND_SEND_RELIABLE, true, 1);
        Queue(legacyBatch, reliable, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, true, 2);
        Queue(10000, reliable, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, true, 9);
        Queue(10000, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, false, 9);
    } else if (!std::strcmp(argv[1], "negotiated")) {
        Queue(972, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE, false, 1, 1000);
        Queue(973, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, false, 2, 1000);
        Queue(1168, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE, false, 1, 1200, true);
        Queue(1169, bestEffort, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, false, 2, 1200, true);
    } else if (!std::strcmp(argv[1], "epoch")) {
        // ENet's own reliable epoch rollover is intentional, unlike an unflagged near-MTU snapshot.
        Queue(legacyBatch, bestEffort, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, true, 2, 1200, false, 0xffff);
        Queue(611, bestEffort, ENET_PROTOCOL_COMMAND_SEND_RELIABLE, true, 1, 1200, false, 0xffff);
    } else if (!std::strcmp(argv[1], "packing")) {
        ENetHost host{};
        ENetPeer peer{};
        ENetChannel channel{};
        host.peers = &peer;
        host.peerCount = 1;
        host.mtu = 1200;
        host.maximumPacketSize = ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
        enet_list_clear(&host.dispatchQueue);
        peer.host = &host;
        enet_list_clear(&peer.acknowledgements);
        enet_list_clear(&peer.sentReliableCommands);
        enet_list_clear(&peer.outgoingCommands);
        enet_list_clear(&peer.outgoingSendReliableCommands);
        enet_list_clear(&peer.dispatchedCommands);
        enet_peer_reset(&peer);
        peer.channels = &channel;
        peer.channelCount = 1;
        peer.state = ENET_PEER_STATE_CONNECTED;
        peer.mtu = 1200;
        peer.packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
        // Four 450-byte logical poses must share exactly two datagrams.
        // Only the socket-send call is mocked; the real ENet packer runs.
        for (int i = 0; i < 4; ++i) {
            auto* packet = enet_packet_create(nullptr, 450, bestEffort);
            assert(packet && enet_peer_send(&peer, 0, packet) == 0);
        }
        enet_host_flush(&host);
        assert(datagramSizes.size() == 2);
        for (size_t bytes : datagramSizes) assert(bytes >= 916 && bytes <= 1200);
        assert(host.totalSentPackets == 2);
    } else if (!std::strcmp(argv[1], "steam")) {
        ReliableSteamRelay();
    } else if (!std::strcmp(argv[1], "pose-order")) {
        using namespace g3t::steam::send;
        assert(enet_initialize() == 0);
        directSockets = true;
        ENetAddress bindAddress{ENET_HOST_ANY, 0};
        char const* err = nullptr;
        auto* sender = g3t::net::CreateTransportHost(&bindAddress, 1, err);
        auto* receiver = g3t::net::CreateTransportHost(&bindAddress, 1, err);
        assert(sender && receiver && !err);
#ifdef _WIN32
        assert(g3t::net::DisableWindowsUdpConnReset(sender->socket));
        assert(g3t::net::DisableWindowsUdpConnReset(receiver->socket));
#endif
        ENetAddress target{0, receiver->address.port};
        assert(enet_address_set_host_ip(&target, "127.0.0.1") == 0);
        auto* peer = g3t::net::ConnectTransportHost(sender, &target, 0x12345678u);
        assert(peer);
        ENetEvent event{};
        const auto deadline = enet_time_get() + 5000;
        while (peer->state != ENET_PEER_STATE_CONNECTED &&
               static_cast<int32_t>(deadline - enet_time_get()) > 0) {
            enet_host_service(sender, &event, 0);
            enet_host_service(receiver, &event, 0);
            if (peer->state != ENET_PEER_STATE_CONNECTED)
                enet_host_service(sender, &event, 1);
        }
        assert(peer->state == ENET_PEER_STATE_CONNECTED);
        enet_host_flush(receiver);
        enet_host_service(sender, &event, 0);

        toReceiver = g3t::steam::send::Queue{};
        senderSocket = sender->socket;
        relayActive = true;

        auto makeControlPose = [](uint16_t npcId, uint8_t sample) {
            std::vector<uint8_t> p(120, sample);
            p[0] = 49; // MsgControlPose
            p[25] = 1; p[26] = 0; p[27] = 0; p[28] = 0;
            p[29] = static_cast<uint8_t>(npcId & 0xffu);
            p[30] = static_cast<uint8_t>(npcId >> 8);
            p[31] = sample;
            return p;
        };
        const auto sendPose = [&](uint16_t npcId, uint8_t sample) {
            auto payload = makeControlPose(npcId, sample);
            auto* pkt = enet_packet_create(payload.data(), payload.size(), 0);
            assert(pkt && enet_peer_send(peer, g3t::net::ChannelPoses, pkt) == 0);
            enet_host_flush(sender);
        };
        // Queue A(v1), B(v1), then A(v2). Tail-append replacement must deliver B(v1) BEFORE A(v2)
        // so ENet's incomingUnreliableSequenceNumber accepts both NPCs' poses.
        sendPose(10, 1);
        sendPose(20, 1);
        sendPose(10, 2);
        assert(toReceiver.Depth() == 2);
        assert(toReceiver.Drain([&](Item const& item) {
            ENetBuffer buffer{item.bytes.size(), const_cast<uint8_t*>(item.bytes.data())};
            assert(enet_socket_send(sender->socket, &target, &buffer, 1) == static_cast<int>(buffer.dataLength));
            return Ok;
        }) == Ok);

        std::vector<std::pair<uint16_t, uint8_t>> delivered;
        while (delivered.size() < 2 && enet_host_service(receiver, &event, 10) > 0) {
            if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
            assert(event.channelID == g3t::net::ChannelPoses);
            assert(event.packet->dataLength >= 32);
            const uint16_t npcId = static_cast<uint16_t>(event.packet->data[29] | (uint16_t(event.packet->data[30]) << 8));
            const uint8_t sample = event.packet->data[31];
            delivered.emplace_back(npcId, sample);
            enet_packet_destroy(event.packet);
        }
        assert(enet_host_service(receiver, &event, 0) == 0);
        assert(delivered.size() == 2);
        assert(delivered[0].first == 20 && delivered[0].second == 1);
        assert(delivered[1].first == 10 && delivered[1].second == 2);

        relayActive = false;
        directSockets = false;
        enet_host_destroy(sender);
        enet_host_destroy(receiver);
        enet_deinitialize();
    } else {
        assert(false);
    }
}
