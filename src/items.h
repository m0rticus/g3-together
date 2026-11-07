// SPDX-License-Identifier: GPL-3.0-or-later
// Loose world pickups are personal: the native take affects only this machine and is not broadcast.
// Consumed pickup GUIDs travel in the account's bounded personal-state record; no native world files are transferred.
#pragma once
#include "personal_store.h"

class gCScriptProcessingUnit;

namespace g3t::items {
void Start();
void AppendPersonal(saving::personal::Snapshot& snapshot);
bool RestorePersonal(saving::personal::Snapshot const& snapshot);
void TickPersonal();
void OnWorldChange();
void NoteOwnDrop(char const* name);  // log a personal weapon drop after knock-out (downed.cpp)
using LogFn = void (*)(char const*);
void List(LogFn log);                      // QA: world items within 30 m of the hero, then "ITEMS end"
void Take(char const* guid, LogFn log);    // QA: put a world item into the hero's inventory (as _AI_TakeItem does)
void Decay(char const* guid, LogFn log);   // QA: remove a world item with Entity::Decay
void Describe(char const* guid, LogFn log);  // QA: what the GUID resolves to now
bool BeforeTake(gCScriptProcessingUnit* spu);  // false: saved personal pickup is already consumed; do not grant it again
void AfterTake(gCScriptProcessingUnit* spu);
void Pick(char const* guid, LogFn log);  // QA: take an item the player's way (target + PS_Normal_Take)
void TookReport(LogFn log);
void Count(char const* templateName, LogFn log);  // QA: how many of these the hero carries
void Disable(LogFn log);  // legacy QA command; pickup synchronization is already disabled
unsigned Returned();  // always zero with personal pickups; keeps the enclave theft bookkeeping interface
unsigned TakesBegun();  // this hero's _AI_TakeItem calls on a world item
unsigned TakesDone();   // ... after which the item was in the hero's inventory
void Containers(LogFn log);  // QA: chests and corpses near the hero with their contents
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
}
