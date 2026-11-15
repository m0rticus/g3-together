// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace g3t::saving {

inline uint32_t CrcByte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    return crc;
}

inline uint32_t Crc32(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) crc = CrcByte(crc, data[i]);
    return crc ^ 0xffffffffu;
}

inline uint32_t Crc32Zeroed(uint8_t const* data, size_t size, size_t zeroStart, size_t zeroLen)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = (i >= zeroStart && i < zeroStart + zeroLen) ? 0 : data[i];
        crc = CrcByte(crc, b);
    }
    return crc ^ 0xffffffffu;
}

inline void PutLe(std::vector<uint8_t>& out, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline void PutLeAt(uint8_t* dst, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) dst[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline bool GetLe(uint8_t const* data, size_t size, size_t& at, uint64_t& value, unsigned count)
{
    if (at > size || count > size - at) return false;
    value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(data[at++]) << (8 * i);
    return true;
}

inline void PutF32(std::vector<uint8_t>& out, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    PutLe(out, bits, 4);
}

inline bool GetF32(uint8_t const* data, size_t size, size_t& at, float& value)
{
    uint64_t raw = 0;
    if (!GetLe(data, size, at, raw, 4)) return false;
    const uint32_t bits = static_cast<uint32_t>(raw);
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

} // namespace g3t::saving
