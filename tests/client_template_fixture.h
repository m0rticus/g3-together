// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace template_fixture {
using Bytes = std::vector<uint8_t>;

inline Bytes Pattern(size_t size, uint32_t seed)
{
    Bytes bytes(size);
    for (auto& byte : bytes) {
        seed = seed * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(seed >> 24);
    }
    return bytes;
}

inline void Word(Bytes& bytes, size_t at, uint32_t value)
{
    assert(at + 4 <= bytes.size());
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

inline Bytes Header(size_t size, uint16_t token = 0x00af)
{
    assert(size >= 10);
    Bytes bytes = Pattern(size, 17);
    std::copy_n("GENOMESG", 8, bytes.begin());
    bytes[8] = static_cast<uint8_t>(token);
    bytes[9] = static_cast<uint8_t>(token >> 8);
    return bytes;
}

inline Bytes World(size_t size)
{
    assert(size >= 17 && size - 16 <= UINT32_MAX);
    Bytes bytes = Pattern(size, 23);
    std::copy_n("GENOMFLE", 8, bytes.begin());
    Word(bytes, 8, 0x12345678);
    Word(bytes, 12, static_cast<uint32_t>(size - 16));
    return bytes;
}
}
