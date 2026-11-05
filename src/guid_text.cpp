// SPDX-License-Identifier: GPL-3.0-or-later
#include "guid_text.h"

#include "log.h"

#include <cstdio>
#include <cstring>

namespace g3t::guid {
bool ParseBytes(char const* text, uint8_t out[16])
{
    if (!text || !out) return false;
    unsigned d1 = 0, d2 = 0, d3 = 0, b[8] = {};
    if (sscanf_s(text, "%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X",
                 &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    out[0] = static_cast<uint8_t>(d1 & 0xff);
    out[1] = static_cast<uint8_t>((d1 >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((d1 >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((d1 >> 24) & 0xff);
    out[4] = static_cast<uint8_t>(d2 & 0xff);
    out[5] = static_cast<uint8_t>((d2 >> 8) & 0xff);
    out[6] = static_cast<uint8_t>(d3 & 0xff);
    out[7] = static_cast<uint8_t>((d3 >> 8) & 0xff);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(b[i]);
    return true;
}

bool ParseBytesExact(char const* text, uint8_t out[16])
{
    return text && out && std::strlen(text) == 36 && ParseBytes(text, out);
}

void FormatBytes(uint8_t const guid[16], char* out, size_t size)
{
    uint32_t d1 = static_cast<uint32_t>(guid[0]) |
                  (static_cast<uint32_t>(guid[1]) << 8) |
                  (static_cast<uint32_t>(guid[2]) << 16) |
                  (static_cast<uint32_t>(guid[3]) << 24);
    uint16_t d2 = static_cast<uint16_t>(guid[4] | (guid[5] << 8));
    uint16_t d3 = static_cast<uint16_t>(guid[6] | (guid[7] << 8));
    g3t::Format(out, size, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                d1, d2, d3, guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}
}
