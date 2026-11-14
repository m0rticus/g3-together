/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <enet/enet.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

static void codec_buffers(void)
{
    unsigned char input[1078], encoded[1200], segmented[1200], restored[1078];
    ENetBuffer flat, parts[8];
    size_t i, size, splitSize;
    void *context = enet_range_coder_create();
    assert(context);
    for (i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i % 13);
    flat.data = input;
    flat.dataLength = sizeof(input);
    size = enet_range_coder_compress(context, &flat, 1, sizeof(input), encoded, sizeof(encoded));
    assert(size > 0 && size < sizeof(input));
    for (i = 0; i < 8; ++i) {
        parts[i].data = i & 1 ? input : NULL;
        parts[i].dataLength = 0;
    }
    parts[2].data = input;
    parts[2].dataLength = 511;
    parts[5].data = input + 511;
    parts[5].dataLength = sizeof(input) - 511;
    splitSize = enet_range_coder_compress(context, parts, 8, sizeof(input), segmented, sizeof(segmented));
    assert(splitSize == size && !memcmp(encoded, segmented, size));
    assert(enet_range_coder_decompress(context, segmented, size, restored, sizeof(restored)) == sizeof(input));
    assert(!memcmp(input, restored, sizeof(input)));
    for (i = 0; i < 8; ++i) parts[i].dataLength = 0;
    assert(enet_range_coder_compress(context, parts, 8, sizeof(input), segmented, sizeof(segmented)) == 0);
    enet_range_coder_destroy(context);
}

static void codec_invalid(void)
{
    unsigned char input[64] = {1}, encoded[100];
    ENetBuffer buffer;
    void *context = enet_range_coder_create();
    assert(context);
    buffer.data = input;
    buffer.dataLength = sizeof(input);
    assert(enet_range_coder_compress(NULL, &buffer, 1, 64, encoded, 100) == 0);
    assert(enet_range_coder_compress(context, NULL, 1, 64, encoded, 100) == 0);
    assert(enet_range_coder_compress(context, &buffer, 0, 64, encoded, 100) == 0);
    assert(enet_range_coder_compress(context, &buffer, 1, 0, encoded, 100) == 0);
    assert(enet_range_coder_compress(context, &buffer, 1, 64, NULL, 100) == 0);
    assert(enet_range_coder_compress(context, &buffer, 1, 64, encoded, 0) == 0);
    buffer.data = NULL;
    assert(enet_range_coder_compress(context, &buffer, 1, 64, encoded, 100) == 0);
    assert(enet_range_coder_decompress(NULL, input, 64, encoded, 100) == 0);
    assert(enet_range_coder_decompress(context, NULL, 64, encoded, 100) == 0);
    assert(enet_range_coder_decompress(context, input, 0, encoded, 100) == 0);
    assert(enet_range_coder_decompress(context, input, 64, NULL, 100) == 0);
    assert(enet_range_coder_decompress(context, input, 64, encoded, 0) == 0);
    enet_range_coder_destroy(context);
}

static void codec_limit(void)
{
    unsigned char input[64], encoded[100], prefix[100], restored[64];
    ENetBuffer parts[2], flat;
    size_t size, prefixSize;
    void *context = enet_range_coder_create();
    assert(context);
    memset(input, 0x51, sizeof(input));
    flat.data = input;
    flat.dataLength = 31;
    parts[0].data = input;
    parts[0].dataLength = 16;
    parts[1].data = input + 16;
    parts[1].dataLength = 48;
    size = enet_range_coder_compress(context, parts, 2, 31, encoded, sizeof(encoded));
    prefixSize = enet_range_coder_compress(context, &flat, 1, 31, prefix, sizeof(prefix));
    assert(size > 0 && size == prefixSize && !memcmp(encoded, prefix, size));
    assert(enet_range_coder_decompress(context, encoded, size, restored, sizeof(restored)) == 31);
    assert(!memcmp(input, restored, 31));
    enet_range_coder_destroy(context);
}

static void transport_checkpoint(void)
{
    ENetAddress address = {ENET_HOST_ANY, 0};
    ENetHost *sender, *receiver;
    ENetPeer *peer;
    ENetEvent event;
    unsigned received = 0;
    enet_uint32 deadline;
    char payload[1072];
    assert(enet_initialize() == 0);
    sender = enet_host_create(&address, 1, 8, 0, 0);
    receiver = enet_host_create(&address, 1, 8, 0, 0);
    assert(sender && receiver);
    sender->mtu = receiver->mtu = 1200;
    assert(enet_host_compress_with_range_coder(sender) == 0);
    assert(enet_host_compress_with_range_coder(receiver) == 0);
    address.port = receiver->address.port;
    assert(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    peer = enet_host_connect(sender, &address, 8, 0);
    assert(peer);
    deadline = enet_time_get() + 2000;
    while (peer->state != ENET_PEER_STATE_CONNECTED && (int32_t)(deadline - enet_time_get()) > 0) {
        enet_host_service(sender, &event, 0);
        enet_host_service(receiver, &event, 0);
        if (peer->state != ENET_PEER_STATE_CONNECTED)
            enet_host_service(sender, &event, 1);
    }
    assert(peer->state == ENET_PEER_STATE_CONNECTED && peer->channelCount == 8);
    memset(payload, 0x51, sizeof(payload));
    {
        ENetPacket *pose = enet_packet_create(payload, sizeof(payload), 0);
        ENetPacket *checkpoint = enet_packet_create(NULL, 0, ENET_PACKET_FLAG_RELIABLE);
        assert(pose && checkpoint && checkpoint->data == NULL);
        assert(enet_peer_send(peer, 4, pose) == 0);
        assert(enet_peer_send(peer, 4, checkpoint) == 0);
    }
    sender->totalSentPackets = sender->totalSentData = 0;
    /* The reliable checkpoint leaves a trailing {0, NULL} compressor input. */
    enet_host_flush(sender);
    assert(sender->totalSentPackets == 1 && sender->totalSentData < sizeof(payload));
    deadline = enet_time_get() + 2000;
    while (received < 2 && (int32_t)(deadline - enet_time_get()) > 0) {
        if (enet_host_service(receiver, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
            assert(event.channelID == 4);
            if (event.packet->dataLength) {
                assert(event.packet->dataLength == sizeof(payload));
                assert(!memcmp(event.packet->data, payload, sizeof(payload)));
            } else {
                assert(event.packet->flags & ENET_PACKET_FLAG_RELIABLE);
            }
            enet_packet_destroy(event.packet);
            ++received;
        }
        enet_host_service(sender, &event, 0);
    }
    assert(received == 2 && peer->channels[4].outgoingUnreliableSequenceNumber == 0);
    enet_host_destroy(sender);
    enet_host_destroy(receiver);
    enet_deinitialize();
}

int ENET_CALLBACK main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "buffers")) codec_buffers();
    else if (!strcmp(argv[1], "invalid")) codec_invalid();
    else if (!strcmp(argv[1], "limit")) codec_limit();
    else if (!strcmp(argv[1], "checkpoint")) transport_checkpoint();
    else assert(0);
    return 0;
}
