// SPDX-License-Identifier: GPL-3.0-or-later
// QA diagnostic for crashes (owner, 20.10.: the host crashed in Script.dll Entity::AttachTo while the raid was ended;
// the game's own crash log has no usable call stack). With QA enabled, an access violation is logged to g3together.log
// before the game handles it: registers and every value on the stack that points into the game's modules or ours, as
// module+RVA, for the game's modules with the nearest function name (exports, and the scripts of Script_Game.dll).
// First chance, so exceptions the game catches itself show up too; at most a few are logged.
#pragma once

#include "shutdown_watchdog.h"
#include "worldentry_diag.h"
#include <cstddef>
#include <cstdint>

namespace g3t::crashlog {
void PollWorldEntry();
void Install();                 // installs minidump + first-chance crash logging before subsystem startup
void RefreshModules();          // resolves newly loaded dynamic modules (e.g. steam_api.dll, steamclient.dll)
void Tick();                    // ensures installation and takes script names once a game runs
void EndStep(char const* step); // cache a static-lifetime step name; safe to report without reading game objects
void BeginShutdown();
bool Shutdown();
bool WriteProcessDump(char const* path);
bool Where(uintptr_t address, char* out, size_t size);
}
