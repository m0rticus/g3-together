// SPDX-License-Identifier: GPL-3.0-or-later
// Animals coming back (P3, plan to 90 %): the game's Respawn (Script_Game, called from OnEnterProcessingRange for a dead
// NPC that enters this machine's processing range) rolls once 120 game hours have passed since its last fight. A success
// stands it up again at its spawn point, a failure takes it out of the world. Only the host rolls;
// clients replay the resulting new NPC incarnation or reliable removal without running respawn scripts.
#pragma once

#include <cstdint>

namespace g3t::respawn {
void Tick();  // installs the jump into the game's Respawn once its scripts are registered
using LogFn = void (*)(char const*);
void Report(LogFn log);  // QA: RESPAWNSYNC line
void Off(LogFn log);     // QA negative control (respawnoff): bypass host respawn bookkeeping
}
