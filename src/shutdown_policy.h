// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::shutdown {
constexpr uint32_t PollMs = 100;
constexpr uint32_t GraphicsGraceMs = 1500;
constexpr uint32_t GraphicsStallMs = 1000;
constexpr uint32_t DiagnosticMs = 12000;

inline bool DeviceReleaseSite(uint8_t const* code, size_t size)
{
    constexpr uint8_t expected[] = {0x8b, 0x46, 0x08, 0x8b, 0x08, 0x8b, 0x51, 0x08,
                                    0x50, 0xff, 0xd2, 0x85, 0xc0, 0x77, 0xf1};
    return size >= sizeof(expected) && !std::memcmp(code, expected, sizeof(expected));
}

inline bool DriverWaitSite(uint8_t const* code, size_t size)
{
    constexpr uint8_t before[] = {0x68, 0xe8, 0x03, 0x00, 0x00, 0x53, 0x55, 0xff, 0x15};
    constexpr uint8_t after[] = {0x39, 0x7e, 0x48, 0x72, 0xee};
    return size >= 18 && !std::memcmp(code, before, sizeof(before)) &&
           !std::memcmp(code + 13, after, sizeof(after));
}

struct GraphicsWait {
    uint32_t object = 0, completed = 0, produced = 0, retired = 0;
    bool Pending() const { return completed < produced - retired; }
    bool operator==(GraphicsWait const& other) const
    {
        return object == other.object && completed == other.completed &&
               produced == other.produced && retired == other.retired;
    }
};

// These are bounded stack candidates, not an unwind. Require the verified nested
// wait's caller/arguments before the native final Release return.
inline uint32_t WaitingObject(uint32_t const* stack, size_t words, uint32_t conditionReturn,
                              uint32_t driverReturn, uint32_t deviceReturn)
{
    if (!conditionReturn || !driverReturn || !deviceReturn) return 0;
    for (size_t i = 0; i < words; ++i) {
        if (stack[i] != conditionReturn) continue;
        for (size_t j = i + 1; j + 3 < words; ++j) {
            if (stack[j] != driverReturn || stack[j + 1] < 8 ||
                stack[j + 1] > UINT32_MAX - 4 || stack[j + 2] != stack[j + 1] + 4 ||
                stack[j + 3] != 1000) continue;
            for (size_t k = j + 4; k < words; ++k)
                if (stack[k] == deviceReturn) return stack[j + 1] - 8;
        }
    }
    return 0;
}

class ExitPolicy {
    GraphicsWait previous{};
    uint32_t since = 0;
    bool observing = false;
  public:
    bool Observe(uint32_t now, bool cleanupReady, uint32_t cleanupSince,
                 bool matched, GraphicsWait const& wait)
    {
        if (!cleanupReady || !matched || !wait.object || !wait.Pending()) {
            observing = false;
            return false;
        }
        if (!observing || !(wait == previous)) {
            previous = wait;
            since = now;
            observing = true;
        }
        return now - cleanupSince >= GraphicsGraceMs && now - since >= GraphicsStallMs;
    }
};
}
