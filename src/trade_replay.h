// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::trade::replay {
inline bool SellerEpoch(uint64_t requested, uint64_t current)
{
    return requested && requested == current;
}

inline bool SellerAccount(bool host, uint64_t expected, uint64_t current)
{
    return !host || (expected && expected == current);
}
}
