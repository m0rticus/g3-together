// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/relaywire.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace g3t::steam;

static void Check(std::uint64_t lobby, char const* session,
                  void const* payload, std::size_t payloadSize)
{
    unsigned char packet[RelayMaxEnvelopeSize];
    std::size_t encodedSize = 0;
    assert(RelayEncode(lobby, session, payload, payloadSize, packet, sizeof(packet), &encodedSize));
    assert(encodedSize == RelayHeaderSize(session) + payloadSize);
    assert(encodedSize <= RelayMaxEnvelopeSize);
    for (std::size_t i = 0; i < 8; ++i)
        assert(packet[i] == static_cast<unsigned char>(lobby >> (8 * i)));
    assert(packet[8] == static_cast<unsigned char>(0x80u | (RelayHeaderSize(session) - 9)));
    if (payloadSize)
        assert(std::memcmp(packet + RelayHeaderSize(session), payload, payloadSize) == 0);
    void const* decodedPayload = nullptr;
    std::size_t decodedSize = 999;
    assert(RelayDecode(packet, encodedSize, lobby, session, &decodedPayload, &decodedSize));
    assert(decodedSize == payloadSize);
    if (payloadSize)
        assert(std::memcmp(decodedPayload, payload, payloadSize) == 0);
}

int main()
{
    unsigned char payload[RelayPayloadLimit];
    for (std::size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = static_cast<unsigned char>((i * 37u + 11u) & 0xffu);

    char session1[] = "A";
    char sessionDefault[] = "default";
    char session63[64];
    for (std::size_t i = 0; i < 63; ++i) session63[i] = i & 1 ? 'z' : 'A';
    session63[63] = '\0';

    std::uint64_t lobby = 0x0123456789abcdefULL;
    assert(RelayHeaderSize(sessionDefault) == 16);
    assert(RelayMaxHeaderSize == 72);
    for (char const* session : {session1, sessionDefault, session63}) {
        for (std::size_t length : {0u, 1u, 8u, 9u, 75u, 4096u})
            Check(lobby, session, length ? payload : nullptr, length);
    }

    for (char const control : {'H', 'W', 'P', 'Q', 'B'}) {
        unsigned char data[9] = {static_cast<unsigned char>(control)};
        Check(lobby, sessionDefault, data, control == 'P' || control == 'Q' ? 9u : 1u);
    }
    Check(lobby, sessionDefault, payload, RelayPayloadLimit);

    unsigned char packet[RelayMaxEnvelopeSize];
    std::size_t encodedSize = 0;
    assert(RelayEncode(lobby, sessionDefault, payload, 75, packet, sizeof(packet), &encodedSize));

    void const* out = nullptr;
    std::size_t outSize = 0;
    assert(!RelayDecode(nullptr, 0, lobby, sessionDefault, &out, &outSize));
    assert(!RelayDecode(packet, 0, lobby, sessionDefault, &out, &outSize));
    for (std::size_t length = 0; length < RelayHeaderSize(sessionDefault); ++length)
        assert(!RelayDecode(packet, length, lobby, sessionDefault, &out, &outSize));
    assert(!RelayDecode(packet, encodedSize, lobby + 1, sessionDefault, &out, &outSize));

    char pairLower[] = "Pair";
    char pairUpper[] = "pair";
    unsigned char mismatched[RelayMaxEnvelopeSize];
    assert(RelayEncode(lobby, pairLower, payload, 1, mismatched, sizeof(mismatched), &encodedSize));
    assert(!RelayDecode(mismatched, encodedSize, lobby, pairUpper, &out, &outSize));
    char prefix[] = "PairLong";
    assert(RelayEncode(lobby, pairLower, payload, 1, mismatched, sizeof(mismatched), &encodedSize));
    assert(!RelayDecode(mismatched, encodedSize, lobby, prefix, &out, &outSize));

    unsigned char malformedMarker[RelayMaxEnvelopeSize] = {};
    for (std::size_t i = 0; i < 8; ++i)
        malformedMarker[i] = static_cast<unsigned char>(lobby >> (8 * i));
    malformedMarker[8] = 0;
    assert(!RelayDecode(malformedMarker, 10, lobby, sessionDefault, &out, &outSize));
    malformedMarker[8] = 0xff;
    assert(!RelayDecode(malformedMarker, 10, lobby, sessionDefault, &out, &outSize));
    // Reserved bit 6 (0x40) must also be rejected even when session bytes match expectedSession.
    std::memcpy(malformedMarker + 9, sessionDefault, 7);
    malformedMarker[8] = 0xc7;
    assert(!RelayDecode(malformedMarker, 9 + 7, lobby, sessionDefault, &out, &outSize));
    malformedMarker[8] = 0xc1;
    assert(!RelayDecode(malformedMarker, 10, lobby, "a", &out, &outSize));

    for (char const* invalid : {"", "bad!", "bad session", "slash/name", "A."}) {
        assert(!RelayEncode(lobby, invalid, payload, 1, packet, sizeof(packet), &encodedSize));
    }
    char invalid63[64];
    std::memset(invalid63, 'x', 63);
    invalid63[63] = '\0';
    assert(RelayEncode(lobby, invalid63, payload, 1, packet, sizeof(packet), &encodedSize));
    invalid63[63] = '!';
    assert(!RelayEncode(lobby, invalid63, payload, 1, packet, sizeof(packet), &encodedSize));

    struct LegacyHeader {
        std::uint64_t lobby;
        char session[64];
    };
    LegacyHeader legacy{};
    legacy.lobby = lobby;
    std::memcpy(legacy.session, "default", 8);
    assert(!RelayDecode(&legacy, sizeof(legacy), lobby, sessionDefault, &out, &outSize));

    char empty[] = "";
    char nullSession[] = "default";
    assert(!RelayEncode(lobby, empty, payload, 1, packet, sizeof(packet), &encodedSize));
    assert(!RelayEncode(lobby, nullSession, nullptr, 1, packet, sizeof(packet), &encodedSize));
    assert(RelayEncode(lobby, nullSession, nullptr, 0, packet, sizeof(packet), &encodedSize));

    EnetPacketParse enet;
    unsigned char ack[] = {
        0x00,0x00,
        0x01,0x01,0x00,0x01,
        0x00,0x01,0x00,0x02
    };
    assert(ParseEnetDataPacket(ack, sizeof(ack), &enet));
    assert(!enet.compressed && enet.protocolBytes == 10 && enet.ackBytes == 8);

    unsigned char connect[4 + 48] = {};
    connect[0] = 0x80;
    connect[4] = 0x82;
    connect[5] = 0xff;
    connect[48] = 0x12; connect[49] = 0x34; connect[50] = 0xab; connect[51] = 0xcd;
    assert(!ParseEnetDataPacket(connect, 4 + 44, &enet));
    assert(!enet.valid && enet.firstCommand == 2 && enet.command == 2 &&
           enet.commandOffset == 4 && enet.commandBytes == 48 && enet.remainingBytes == 44);
    assert(ParseEnetDataPacket(connect, sizeof(connect), &enet));
    assert(enet.valid && enet.firstCommand == 2 && enet.command == 2 &&
           enet.commandOffset == 4 && enet.commandBytes == 48 && enet.protocolBytes == sizeof(connect));

    unsigned char reliable[19];
    std::memset(reliable, 0, sizeof(reliable));
    reliable[0] = 0x80; // sent-time ENet header
    reliable[1] = 0x00;
    reliable[4] = 0x06;
    reliable[5] = 0;
    reliable[6] = 0;
    reliable[7] = 1;
    reliable[8] = 0;
    reliable[9] = 9; // 9 payload bytes
    for (unsigned i = 10; i < sizeof(reliable); ++i) reliable[i] = static_cast<unsigned char>(i);
    assert(ParseEnetDataPacket(reliable, sizeof(reliable), &enet));
    assert(!enet.compressed && enet.protocolBytes == 10 && enet.ackBytes == 0);

    unsigned char unreliable[12];
    std::memset(unreliable, 0, sizeof(unreliable));
    unreliable[2] = 0x07;
    unreliable[4] = 0;
    unreliable[5] = 1;
    unreliable[8] = 0;
    unreliable[9] = 2;
    assert(!ParseEnetDataPacket(unreliable, 10, &enet)); // header2 + listed 8 bytes, but payload is 2
    assert(!enet.valid);

    unsigned char validUnreliable[12];
    std::memset(validUnreliable, 0, sizeof(validUnreliable));
    validUnreliable[2] = 0x07;
    validUnreliable[8] = 0;
    validUnreliable[9] = 2;
    assert(ParseEnetDataPacket(validUnreliable, sizeof(validUnreliable), &enet));
    assert(!enet.compressed && enet.protocolBytes == 10 && enet.ackBytes == 0);

    unsigned char fragment[34];
    std::memset(fragment, 0, sizeof(fragment));
    fragment[0] = 0x80;
    fragment[4] = 0x08;
    fragment[10] = 0;
    fragment[11] = 6;
    assert(ParseEnetDataPacket(fragment, sizeof(fragment), &enet));
    assert(!enet.compressed && enet.protocolBytes == 28 && enet.ackBytes == 0);

    unsigned char compressed[8];
    compressed[0] = 0x40;
    compressed[1] = 0x00;
    std::memset(compressed + 2, 0xa5, 6);
    assert(ParseEnetDataPacket(compressed, sizeof(compressed), &enet));
    assert(enet.valid && enet.compressed);
    assert(!ParseEnetDataPacket(compressed, 2, &enet));

    unsigned char truncated[] = {0x00, 0x00, 0x06, 0x00, 0x00};
    assert(!ParseEnetDataPacket(truncated, sizeof(truncated), &enet));
    unsigned char badCommand[] = {0x00, 0x00, 0x0d, 0x00, 0x00, 0x00};
    assert(!ParseEnetDataPacket(badCommand, sizeof(badCommand), &enet));
    unsigned char badLength[] = {0x00, 0x00, 0x06, 0x00, 0x00, 0x01, 0x00, 0xff};
    assert(!ParseEnetDataPacket(badLength, sizeof(badLength), &enet));

    return 0;
}
