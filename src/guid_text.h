// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g3t::guid {
bool ParseBytes(char const* text, uint8_t out[16]);
bool ParseBytesExact(char const* text, uint8_t out[16]);
void FormatBytes(uint8_t const guid[16], char* out, size_t size);
}
