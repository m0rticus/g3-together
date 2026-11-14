// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/netbudget.h"
#include <enet/enet.h>

#include <cassert>
#include <cstddef>
#include <cstring>

extern "C" size_t enet_protocol_command_size(enet_uint8)
{
    return 0;
}

extern "C" int enet_initialize(void)
{
    return 0;
}

extern "C" void enet_host_flush(ENetHost*)
{
}

namespace {
void QueueAndSum(size_t payloadBytes, enet_uint32 mtu, bool checksum,
                 enet_uint32 packetFlags, bool reliable, size_t expectedFragments)
{
    ENetHost host{};
    ENetPeer peer{};
    ENetChannel channel{};
    std::memset(&host, 0, sizeof(host));
    std::memset(&peer, 0, sizeof(peer));
    std::memset(&channel, 0, sizeof(channel));

    host.maximumPacketSize = ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
    host.checksum = checksum ? &enet_crc32 : nullptr;
    peer.host = &host;
    peer.channels = &channel;
    peer.channelCount = 1;
    peer.state = ENET_PEER_STATE_CONNECTED;
    peer.mtu = mtu;
    enet_list_clear(&peer.outgoingCommands);
    enet_list_clear(&peer.outgoingSendReliableCommands);
    enet_list_clear(&channel.incomingReliableCommands);
    enet_list_clear(&channel.incomingUnreliableCommands);

    ENetPacket* packet = enet_packet_create(nullptr, payloadBytes, packetFlags);
    assert(packet != nullptr);
    assert(packet->dataLength == payloadBytes);
    assert(enet_peer_send(&peer, 0, packet) == 0);

    ENetList* queued = reliable ? &peer.outgoingSendReliableCommands : &peer.outgoingCommands;
    ENetList* other = reliable ? &peer.outgoingCommands : &peer.outgoingSendReliableCommands;
    assert(enet_list_empty(other));

    size_t fragments = enet_list_size(queued);
    assert(fragments == expectedFragments);
    size_t fragmentPayloadBytes = 0;
    size_t maximumFragmentBytes = mtu - sizeof(ENetProtocolHeader) -
                                  sizeof(ENetProtocolSendFragment);
    if (checksum) maximumFragmentBytes -= sizeof(enet_uint32);

    while (!enet_list_empty(queued)) {
        ENetOutgoingCommand* sent =
            reinterpret_cast<ENetOutgoingCommand*>(enet_list_remove(enet_list_begin(queued)));
        assert(sent->packet == packet);
        assert(sent->fragmentLength <= maximumFragmentBytes);
        fragmentPayloadBytes += sent->fragmentLength;

        const enet_uint8 command = sent->command.header.command & ENET_PROTOCOL_COMMAND_MASK;
        if (reliable) {
            assert(command == (expectedFragments > 1
                ? ENET_PROTOCOL_COMMAND_SEND_FRAGMENT
                : ENET_PROTOCOL_COMMAND_SEND_RELIABLE));
        } else {
            assert(command == (expectedFragments > 1
                ? (packetFlags & ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT
                    ? ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT
                    : ENET_PROTOCOL_COMMAND_SEND_FRAGMENT)
                : ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE));
        }
        enet_free(sent);
    }

    assert(fragmentPayloadBytes == payloadBytes);
    assert(packet->dataLength == payloadBytes);
    packet->referenceCount = 0;
    enet_packet_destroy(packet);
}
}

int main()
{
    static_assert(sizeof(ENetProtocolHeader) == 4, "ENet header layout changed");
    static_assert(sizeof(ENetProtocolSendFragment) == 24, "ENet fragment layout changed");
    assert(1200u - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment) == 1172u);
    assert(1200u - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment)
           - sizeof(enet_uint32) == 1168u);

    QueueAndSum(1172, 1200, false, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 1);
    QueueAndSum(1173, 1200, false, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 2);
    QueueAndSum(2345, 1200, false, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 3);
    QueueAndSum(1172, 1200, false, ENET_PACKET_FLAG_RELIABLE, true, 1);
    QueueAndSum(1173, 1200, false, ENET_PACKET_FLAG_RELIABLE, true, 2);
    QueueAndSum(1168, 1200, true, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 1);
    QueueAndSum(1169, 1200, true, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 2);
    QueueAndSum(1172, 1172, false, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 2);
    QueueAndSum(1173, 1172, false, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, false, 2);

    assert(g3t::net::BudgetCharge(1172, 1200, 16, 0) == 1220);
    assert(g3t::net::BudgetCharge(1173, 1200, 16, 0) == 1269);
    assert(g3t::net::BudgetCharge(1169, 1200, 16, 4) == 1265);

    g3t::net::PeerBudget budget;
    g3t::net::BudgetConfig config;
    config.bytesPerSecond = 256u * 1024u;
    config.mtuBytes = 1200;
    budget.Reset(0, config);
    budget.CommitQueued(g3t::net::BudgetClass::NpcActive, 1269);
    assert(budget.Snapshot().chargedBytes == 1269);
    return 0;
}
