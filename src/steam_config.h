// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace g3t::steam::config {
// Valve ESteamNetworkingConfigScope: 1=Global, 2=SocketsInterface (ISteamNetworkingSockets*), 3=ListenSocket, 4=Connection.
constexpr int GlobalScope = 1, SocketsInterfaceScope = 2, ListenSocketScope = 3, ConnectionScope = 4, Int32 = 1;
constexpr int SendBufferSize = 9, SendRateMin = 10, SendRateMax = 11, NagleTime = 12;
constexpr int P2PTransportIceEnable = 104, IceEnableAll = 7;
constexpr int32_t BufferCapacity = 256 * 1024;
constexpr int32_t MinRateCapacity = 1 * 1024 * 1024;
constexpr int32_t MaxRateCapacity = 8 * 1024 * 1024;
constexpr int32_t Capacity = MaxRateCapacity;

struct Setting {
    int id;
    char const* name;
    int32_t capacity;
    bool exact = false;
};
constexpr Setting Settings[] = {
    {SendBufferSize, "SendBufferSize", BufferCapacity, true},
    {SendRateMin, "SendRateMin", MinRateCapacity, false},
    {SendRateMax, "SendRateMax", MaxRateCapacity, false},
    {NagleTime, "NagleTime", 0, false},
};

struct Read {
    int result = 0, type = 0;
    int32_t value = 0;
    bool Valid() const { return (result == 1 || result == 2) && type == Int32; }
};
struct Applied {
    Read before, after;
    int32_t requested = 0;
    bool exact = false;
    bool attempted = false, accepted = false;
    bool Verified() const
    {
        return before.Valid() && after.Valid() &&
            (!attempted || accepted) &&
            (exact ? after.value == requested : after.value >= requested);
    }
};

template<class Get, class Set>
Applied Apply(Setting const& setting, Get get, Set set)
{
    Applied applied;
    applied.exact = setting.exact;
    applied.before = get(setting.id);
    if (!applied.before.Valid()) return applied;
    // Right-size SendBufferSize to avoid multi-second standing queues while
    // preserving larger existing SendRateMin/Max ceilings.
    applied.requested = setting.exact ? setting.capacity
                                      : (std::max)(applied.before.value, setting.capacity);
    if (applied.requested != applied.before.value) {
        applied.attempted = true;
        applied.accepted = set(setting.id, applied.requested);
    }
    applied.after = get(setting.id);
    return applied;
}
}
