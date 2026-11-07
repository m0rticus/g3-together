// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace g3t::npclease {
constexpr uint32_t PresentationGraceMs = 350;

inline bool PresentationFresh(bool connected, bool released, uint32_t age, uint32_t interval)
{
    return connected && !released && age <= std::max(PresentationGraceMs, interval * 4);
}
}
