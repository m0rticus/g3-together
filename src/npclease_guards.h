// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <array>
#include <atomic>
#include <map>
#include <mutex>

namespace g3t::npclease {
using GuardKey = std::array<uint8_t, 16>;

struct AuthorityGuard {
    bool leased = false, locallyDriven = false;
    uint8_t owner = 0;
    uint32_t deadline = 0;
    bool processingAllowed = false;
    // A client-first owner never expires: the host handshake window cannot freeze a native NPC.
    bool persistent = false;
    bool hostClaimed = false;

    bool Local(uint32_t now) const { return !hostClaimed && locallyDriven && (persistent || !Reached(now, deadline)); }
    bool Processing(uint32_t now) const { return !hostClaimed && processingAllowed && (persistent || !Reached(now, deadline)); }
    bool Fenced(uint32_t now) const { return leased && !Local(now); }
};

// Native property/render callbacks read copies; never hold this lock across an engine call.
class GuardRegistry {
    mutable std::mutex mutex;
    std::map<GuardKey, AuthorityGuard> entries;
    std::atomic<size_t> count{0};
public:
    AuthorityGuard Get(GuardKey const& key) const
    {
        if (!count.load(std::memory_order_acquire)) return {};
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = entries.find(key);
        return found == entries.end() ? AuthorityGuard{} : found->second;
    }
    void Put(GuardKey const& key, AuthorityGuard const& guard)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries[key] = guard;
        count.store(entries.size(), std::memory_order_release);
    }
    void Erase(GuardKey const& key)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries.erase(key);
        count.store(entries.size(), std::memory_order_release);
    }
    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        count.store(0, std::memory_order_release);
    }
};
}
