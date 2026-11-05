// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace g3t {
constexpr size_t SessionIdSize = 64;

inline bool ValidSessionId(char const* id)
{
    size_t n = 0;
    for (; id[n]; ++n) {
        const char c = id[n];
        if (n >= SessionIdSize - 1 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return n != 0;
}
}
