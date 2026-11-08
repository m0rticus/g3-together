// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace g3t::steam::config {
constexpr int GlobalScope = 1, Int32 = 1;
constexpr int SendBufferSize = 9, SendRateMin = 10, SendRateMax = 11, NagleTime = 12;
constexpr int32_t Capacity = 4 * 1024 * 1024;

struct Setting {
    int id;
    char const* name;
    int32_t capacity;
};
constexpr Setting Settings[] = {
    {SendBufferSize, "SendBufferSize", Capacity},
    {SendRateMin, "SendRateMin", Capacity},
    {SendRateMax, "SendRateMax", Capacity},
    {NagleTime, "NagleTime", 0},
};

struct Read {
    int result = 0, type = 0;
    int32_t value = 0;
    bool Valid() const { return (result == 1 || result == 2) && type == Int32; }
};
struct Applied {
    Read before, after;
    int32_t requested = 0;
    bool attempted = false, accepted = false;
    bool Verified() const
    {
        return before.Valid() && after.Valid() &&
            (!attempted || accepted) && after.value >= requested;
    }
};

template<class Get, class Set>
Applied Apply(Setting const& setting, Get get, Set set)
{
    Applied applied;
    applied.before = get(setting.id);
    if (!applied.before.Valid()) return applied;
    applied.requested = (std::max)(applied.before.value, setting.capacity);
    // Keep larger existing values. Valve's public sender has no bandwidth probe
    // cycle: raising only the maximum can leave its initial rate at the minimum.
    if (applied.requested != applied.before.value) {
        applied.attempted = true;
        applied.accepted = set(setting.id, applied.requested);
    }
    applied.after = get(setting.id);
    return applied;
}
}
