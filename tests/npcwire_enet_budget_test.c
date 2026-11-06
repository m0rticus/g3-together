/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <enet/enet.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* peer.c uses this only for its byte accounting; no socket or host is opened. */
size_t enet_protocol_command_size(enet_uint8 commandNumber)
{
    (void) commandNumber;
    return 0;
}

int enet_initialize(void) { return 0; }
void enet_host_flush(ENetHost * host) { (void) host; }

static void check(size_t size, enet_uint32 flags, enet_uint16 sequence,
                  enet_uint8 expectedCommand, int expectedCount)
{
    ENetHost host;
    ENetPeer peer;
    ENetChannel channel;
    ENetPacket packet;
    int count = 0;
    memset(&host, 0, sizeof(host));
    memset(&peer, 0, sizeof(peer));
    memset(&channel, 0, sizeof(channel));
    memset(&packet, 0, sizeof(packet));
    host.maximumPacketSize = 1024 * 1024;
    peer.host = &host;
    peer.state = ENET_PEER_STATE_CONNECTED;
    peer.channels = &channel;
    peer.channelCount = 1;
    peer.mtu = 1200;
    channel.outgoingUnreliableSequenceNumber = sequence;
    enet_list_clear(&peer.outgoingCommands);
    enet_list_clear(&peer.outgoingSendReliableCommands);
    packet.dataLength = size;
    packet.flags = flags;
    assert(enet_peer_send(&peer, 0, &packet) == 0);
    ENetList * lists[] = {&peer.outgoingCommands, &peer.outgoingSendReliableCommands};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        while (!enet_list_empty(lists[i])) {
            ENetOutgoingCommand * command = (ENetOutgoingCommand *) enet_list_remove(enet_list_begin(lists[i]));
            assert((command->command.header.command & ENET_PROTOCOL_COMMAND_MASK) == expectedCommand);
            assert(((command->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0) ==
                   (expectedCommand == ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
                    expectedCommand == ENET_PROTOCOL_COMMAND_SEND_RELIABLE));
            ++count;
            enet_free(command);
        }
    }
    assert(count == expectedCount);
}

int main(void)
{
    assert(sizeof(ENetProtocolHeader) == 4);
    assert(sizeof(ENetProtocolSendFragment) == 24);
    assert(1200 - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment) == 1172);
    check(1171, 0, 0, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE, 1);
    check(1172, 0, 0, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE, 1);
    check(1173, 0, 0, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, 2);
    check(1199, 0, 0, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, 2);
    check(1173, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, 0, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, 2);
    check(1200, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, 0, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, 2);
    check(2345, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, 0, ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT, 3);
    check(1173, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, 0xffff, ENET_PROTOCOL_COMMAND_SEND_FRAGMENT, 2);
    check(1172, 0, 0xffff, ENET_PROTOCOL_COMMAND_SEND_RELIABLE, 1);
    check(1172, ENET_PACKET_FLAG_RELIABLE, 0, ENET_PROTOCOL_COMMAND_SEND_RELIABLE, 1);
    return 0;
}
