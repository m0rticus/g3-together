// SPDX-License-Identifier: GPL-3.0-or-later
// One experience for everyone (owner 2 Nov.: host and client get exactly the same XP at any given time). Every
// experience a hero earns by the game's own GiveXP (kills, quests, dialogs, whatever the source) counts for all the
// players: each machine tells the others its running total of what its hero earned, and the others add what they have
// not counted yet. The number is cumulative, so a lost or repeated message changes nothing. A machine hearing from a
// player for the first time (a join, a restart) is lifted to that player's total, and the lift is no gain of its own.
#pragma once

#include <cstdint>

namespace g3t::net { struct WorldState; }

namespace g3t::xpshare {
void Tick();                                  // sends this machine's total, applies what is waiting
void NoteGiveXp(int gained);                  // spells.cpp: the game's GiveXP raised the local hero's experience by this much
void OnState(net::WorldState const& s);       // kind 19: another player's total
void OnWorldChange();                         // a load: the hero is another, lift it to the others again
using LogFn = void (*)(char const*);
void Report(LogFn log);                       // QA: XPSHARE line
void Off(LogFn log);                          // QA negative control (dialogxpoff): experience stays with whoever earned it
}
