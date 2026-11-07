/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <enet/enet.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    ENetAddress address = {ENET_HOST_ANY, 0};
    ENetHost *sender, *receiver;
    ENetPeer *peer;
    ENetEvent event;
    unsigned i, received = 0;
    enet_uint32 deadline;
    char payload[66];
    assert(enet_initialize() == 0);
    sender = enet_host_create(&address, 1, 7, 0, 0);
    receiver = enet_host_create(&address, 1, 7, 0, 0);
    assert(sender && receiver);
    sender->mtu = receiver->mtu = 1200;
    address.port = receiver->address.port;
    assert(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    peer = enet_host_connect(sender, &address, 7, 0);
    assert(peer);
    deadline = enet_time_get() + 2000;
    while (peer->state != ENET_PEER_STATE_CONNECTED && (int32_t)(deadline - enet_time_get()) > 0) {
        enet_host_service(receiver, &event, 1);
        enet_host_service(sender, &event, 1);
    }
    assert(peer->state == ENET_PEER_STATE_CONNECTED && peer->channelCount == 7);
    memset(payload, 0x51, sizeof(payload));
    sender->totalSentPackets = sender->totalSentData = 0;
    for (i = 0; i < 12; ++i) {
        ENetPacket *packet = enet_packet_create(payload, sizeof(payload), i & 1 ? ENET_PACKET_FLAG_RELIABLE : 0);
        assert(packet && enet_peer_send(peer, i % 7, packet) == 0);
    }
    /* No per-message flush: ENet coalesces reliability and channels itself. */
    assert(sender->totalSentPackets == 0);
    enet_host_flush(sender);
    assert(sender->totalSentPackets == 1 && sender->totalSentData <= 1200);
    deadline = enet_time_get() + 2000;
    while (received < 12 && (int32_t)(deadline - enet_time_get()) > 0) {
        if (enet_host_service(receiver, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
            assert(event.packet->dataLength == sizeof(payload));
            assert(!memcmp(event.packet->data, payload, sizeof(payload)));
            enet_packet_destroy(event.packet);
            ++received;
        }
        enet_host_service(sender, &event, 0);
    }
    assert(received == 12);
    enet_host_destroy(sender);
    enet_host_destroy(receiver);
    enet_deinitialize();
    return 0;
}
