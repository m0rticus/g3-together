// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "session.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::steam {

constexpr std::size_t RelayLobbySize = 8;
constexpr std::size_t RelayMarkerSize = 1;
constexpr std::size_t RelayMaxSessionSize = 63;
constexpr std::size_t RelayPayloadLimit = 4096;
constexpr std::size_t RelayMaxHeaderSize = RelayLobbySize + RelayMarkerSize + RelayMaxSessionSize;
constexpr std::size_t RelayMaxEnvelopeSize = RelayMaxHeaderSize + RelayPayloadLimit;

inline bool ValidRelaySession(char const* session)
{
    if (!session || !session[0]) return false;
    std::size_t length = 0;
    for (; session[length]; ++length) {
        char const c = session[length];
        if (length >= RelayMaxSessionSize ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return length != 0;
}

inline std::size_t RelayHeaderSize(char const* session)
{
    if (!ValidRelaySession(session)) return 0;
    std::size_t length = 0;
    while (session[length]) ++length;
    return RelayLobbySize + RelayMarkerSize + length;
}

inline bool RelayEncode(std::uint64_t lobby, char const* session,
                        void const* payload, std::size_t payloadSize,
                        void* packet, std::size_t packetCapacity,
                        std::size_t* encodedSize)
{
    if (encodedSize) *encodedSize = 0;
    if (!lobby || !packet) return false;
    std::size_t headerSize = RelayHeaderSize(session);
    if (!headerSize || payloadSize > RelayPayloadLimit ||
        (payloadSize && !payload) ||
        packetCapacity < headerSize + payloadSize)
        return false;
    std::size_t sessionSize = headerSize - RelayLobbySize - RelayMarkerSize;
    auto* out = static_cast<unsigned char*>(packet);
    for (std::size_t i = 0; i < RelayLobbySize; ++i)
        out[i] = static_cast<unsigned char>(lobby >> (8 * i));
    out[RelayLobbySize] = static_cast<unsigned char>(0x80u | sessionSize);
    for (std::size_t i = 0; i < sessionSize; ++i)
        out[RelayLobbySize + RelayMarkerSize + i] =
            static_cast<unsigned char>(static_cast<unsigned char>(session[i]));
    if (payloadSize)
        std::memcpy(out + headerSize, payload, payloadSize);
    if (encodedSize) *encodedSize = headerSize + payloadSize;
    return true;
}

inline bool RelayDecode(void const* packet, std::size_t packetSize,
                        std::uint64_t expectedLobby, char const* expectedSession,
                        void const** payload, std::size_t* payloadSize)
{
    if (payload) *payload = nullptr;
    if (payloadSize) *payloadSize = 0;
    if (!packet || packetSize < RelayLobbySize + RelayMarkerSize + 1 ||
        packetSize > RelayMaxEnvelopeSize ||
        !ValidRelaySession(expectedSession))
        return false;
    auto const* in = static_cast<unsigned char const*>(packet);
    std::uint64_t lobby = 0;
    for (std::size_t i = 0; i < RelayLobbySize; ++i)
        lobby |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    if (!lobby || lobby != expectedLobby) return false;
    unsigned marker = in[RelayLobbySize];
    if ((marker & 0x80u) == 0) return false;
    std::size_t sessionSize = marker & 0x3fu;
    if (!sessionSize || sessionSize > RelayMaxSessionSize) return false;
    std::size_t headerSize = RelayLobbySize + RelayMarkerSize + sessionSize;
    if (packetSize < headerSize || packetSize - headerSize > RelayPayloadLimit) return false;
    for (std::size_t i = 0; i < sessionSize; ++i) {
        char const c = static_cast<char>(in[RelayLobbySize + RelayMarkerSize + i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    std::size_t expectedSize = 0;
    while (expectedSession[expectedSize]) ++expectedSize;
    if (sessionSize != expectedSize ||
        std::memcmp(in + RelayLobbySize + RelayMarkerSize,
                    expectedSession, sessionSize) != 0)
        return false;
    if (payload) *payload = static_cast<void const*>(in + headerSize);
    if (payloadSize) *payloadSize = packetSize - headerSize;
    return true;
}

struct EnetPacketParse
{
    bool valid = false;
    bool compressed = false;
    std::size_t protocolBytes = 0;
    std::size_t ackBytes = 0;
    std::size_t packetBytes = 0, commandOffset = 0, commandBytes = 0, remainingBytes = 0;
    unsigned firstCommand = 0, command = 0;
};

inline constexpr std::size_t EnetCommandHeaderSize(unsigned command)
{
    // CONNECT includes its final application-data word; VERIFY_CONNECT does not.
    constexpr std::size_t sizes[] = {0, 8, 48, 44, 8, 4, 6, 8, 24, 8, 12, 16, 24};
    return command < sizeof(sizes) / sizeof(sizes[0]) ? sizes[command] : 0;
}

inline std::uint16_t RelayReadUint16(void const* bytes)
{
    auto const* p = static_cast<unsigned char const*>(bytes);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

inline bool ParseEnetDataPacket(void const* packet, std::size_t packetSize,
                                EnetPacketParse* result)
{
    if (!result) return false;
    *result = EnetPacketParse{};
    result->packetBytes = packetSize;
    if (!packet || packetSize < 2) return false;

    auto const* data = static_cast<unsigned char const*>(packet);
    std::uint16_t peerFlags = RelayReadUint16(data);
    std::size_t headerSize = (peerFlags & 0x8000u) ? 4u : 2u;
    if (packetSize < headerSize) return false;

    if (peerFlags & 0x4000u) {
        if (packetSize == headerSize) return false;
        result->valid = true;
        result->compressed = true;
        return true;
    }
    if (packetSize == headerSize) return false;

    result->protocolBytes = headerSize;
    std::size_t offset = headerSize;
    while (offset < packetSize) {
        result->commandOffset = offset;
        result->remainingBytes = packetSize - offset;
        result->commandBytes = 4;
        if (result->remainingBytes < 4) return false;
        unsigned command = data[offset] & 0x0fu;
        result->command = command;
        if (offset == headerSize) result->firstCommand = command;
        std::size_t commandSize = EnetCommandHeaderSize(command);
        result->commandBytes = commandSize;
        if (!commandSize) return false;
        if (packetSize - offset < commandSize) return false;

        std::size_t payloadSize = 0;
        if (command == 6) {
            payloadSize = RelayReadUint16(data + offset + 4);
            commandSize += payloadSize;
        } else if (command == 7 || command == 9 || command == 12) {
            payloadSize = RelayReadUint16(data + offset + 6);
            commandSize += payloadSize;
        } else if (command == 8) {
            payloadSize = RelayReadUint16(data + offset + 6);
            commandSize += payloadSize;
        }
        result->commandBytes = commandSize;
        if (packetSize - offset < commandSize) return false;

        result->protocolBytes += commandSize - payloadSize;
        if (command == 1) result->ackBytes += 8;
        offset += commandSize;
    }
    result->valid = true;
    return true;
}

}
