// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "worldentry.h"
#include <cstdint>

namespace g3t::crashlog {
// Production world-entry progress: written on the engine thread, polled without engine access.
void WorldEntryWait(char const* step);
class WorldEntryScope {
    worldentry::Step previous;
    bool active = false;
  public:
    explicit WorldEntryScope(char const* operation);
    ~WorldEntryScope();
    void Complete();
    WorldEntryScope(WorldEntryScope const&) = delete;
    WorldEntryScope& operator=(WorldEntryScope const&) = delete;
};

struct WorldEntryState {
    worldentry::Step gate;
    worldentry::Step operation;
    uint32_t epoch = 0;
    uint32_t welcome = 0;
    unsigned long threadId = 0;
};
bool ReadWorldEntry(WorldEntryState& out);
bool WorldEntryReportDue(uint32_t now, worldentry::Step const& operation, worldentry::Step const& gate);

enum class CacheKind { Cameras, Prefetchers, PrivateEntities, Spheres, Anchors, ProcessingRange, Puppets, Disabled, Summons, Leases, Count };
// Engine thread publishes values only; reporting never follows a native pointer on a worker.
void CacheObjects(CacheKind kind, uint32_t count, void const* first);
void LogCacheInventory(char const* phase);
void NativeStack(char const* reason);
}
