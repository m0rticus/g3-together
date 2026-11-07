// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/enet_policy.h"
#include "../src/net.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <vector>

std::vector<size_t> datagramSizes;
extern "C" int g3t_test_socket_send(ENetSocket, ENetAddress const*, ENetBuffer const* buffers, size_t count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) bytes += buffers[i].dataLength;
    datagramSizes.push_back(bytes);
    return static_cast<int>(bytes);
}

namespace {
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
    } else {
        assert(false);
    }
}
