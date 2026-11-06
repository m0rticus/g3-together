// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace g3t::latency {
constexpr uint32_t RelayPollMs = 1;      // independent of rendering; the OS can schedule later
constexpr uint32_t PingIntervalMs = 1000;
constexpr uint32_t PingExpiryMs = 5000;  // never display a stale pre-load/disconnected sample

struct Ping
{
    double pending = 0.0;
    int milliseconds = -1;
    uint32_t sampledAt = 0;
    bool awaiting = false;

    void Sent(double seconds) { pending = seconds; awaiting = true; }
    bool CanSend(double seconds) const { return !awaiting || (seconds - pending) * 1000.0 >= PingExpiryMs; }

    bool Reply(double echoed, double now, uint32_t ticks)
    {
        if (!awaiting || !std::isfinite(echoed) || !std::isfinite(now) || echoed != pending || now < echoed ||
            (now - echoed) * 1000.0 >= PingExpiryMs) return false;
        milliseconds = static_cast<int>(std::floor((now - echoed) * 1000.0 + 0.5));
        sampledAt = ticks;
        awaiting = false;
        return true;
    }

    int Value(uint32_t now) const
    {
        return milliseconds >= 0 && now - sampledAt < PingExpiryMs ? milliseconds : -1;
    }
};

struct Samples
{
    uint64_t sum = 0;
    uint32_t count = 0, maximum = 0;

    void Add(uint32_t value) { sum += value; ++count; maximum = (std::max)(maximum, value); }
    double Average() const { return count ? static_cast<double>(sum) / count : 0.0; }
};
}
