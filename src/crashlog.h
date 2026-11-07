// SPDX-License-Identifier: GPL-3.0-or-later
// QA diagnostic for crashes (owner, 20.10.: the host crashed in Script.dll Entity::AttachTo while the raid was ended;
// the game's own crash log has no usable call stack). With QA enabled, an access violation is logged to g3together.log
// before the game handles it: registers and every value on the stack that points into the game's modules or ours, as
// module+RVA, for the game's modules with the nearest function name (exports, and the scripts of Script_Game.dll).
// First chance, so exceptions the game catches itself show up too; at most a few are logged.
#pragma once

#include "worldentry.h"
#include <cstdint>

namespace g3t::crashlog {
using LogFn = void (*)(char const*);
// Production world-entry progress: written on the engine thread, polled without engine access.
void WorldEntryWait(char const* step);
void PollWorldEntry();
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
void Tick();              // installs the handler once QA is enabled; takes the script names once a game runs
void EndStep(char const* step); // cache a static-lifetime step name; safe to report without reading game objects
void NamesOff(LogFn log); // QA negative control (crashnamesoff): crash lines without function names
void Probe(LogFn log);    // QA crashprobe: a caught access violation in Engine.dll, CRASHPROBE line
void Stage(char const* name);
void BeginShutdown();
bool Shutdown();
void StartWatchdog();
void CancelWatchdog();
enum class CacheKind { Cameras, Prefetchers, PrivateEntities, Spheres, Anchors, ProcessingRange, Puppets, Disabled, Summons, Leases, Count };
// Engine thread publishes values only; reporting never follows a native pointer on a worker.
void CacheObjects(CacheKind kind, uint32_t count, void const* first);
void LogCacheInventory(char const* phase);
void NativeStack(char const* reason);
}
