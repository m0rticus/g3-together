// SPDX-License-Identifier: GPL-3.0-or-later
// No game over while a partner can help: when the game would let the local hero die, it is knocked out instead
// and waits until a partner pays a healing potion to help it up. When nobody can help (alone, everybody down),
// the hero dies as in the single-player game.
#pragma once
#include <cstdint>

class eCEntity;

namespace g3t::downed {
// Called by the AI wrapper before an AI state runs for the local hero; true when it replaced a death state.
bool RedirectDeath(eCEntity* self, char const* state);
bool GuardState(eCEntity* self, char const* state); // held: knockout only; terminal: one native entry, then corpse loop
void InstallTerminalGuards();
void ObserveTerminal(eCEntity* self, char const* source);
// Once per frame: a knocked-out hero stays down while a partner could help; a partner who talks to its puppet and
// chooses "Ich werde dir helfen." pays a healing potion and stands it up (owner 23.10.). Active, alive partners count
// anywhere; nobody able to help for 5 s after genuine absence: the hero dies. Other players are told
// when someone goes down or dies.
void Tick();
void OnWorldChange();                 // reset terminal death, but carry an unfinished downed life through loading
void Revived(int byPlayer, uint32_t sequence); // a paid revive for this particular downed life
void HelperHasNoPotion(int byPlayer); // network: that partner stood next to us without one
bool Held();             // the local hero lies knocked out, waiting for help
bool Terminal();         // the local hero's life is in the native terminal death presentation
bool CombatUnavailable(eCEntity* entity); // player life truth, never the puppet's interpolated pose/native mode
bool HarmProtected(eCEntity* entity); // co-op held or paid recovery; never blocks terminal death or loading
bool RejectHarm(eCEntity* victim, eCEntity* attacker, char const* path);
bool RejectReaction(eCEntity* victim, char const* state, char const* path);
void LogGroundEvent(eCEntity* victim, eCEntity* attacker, char const* path, int before, int after, char const* decision);
bool SyntheticProne(eCEntity* victim); // restore vanilla's exact pose gate on defender-local synthetic hits only
uint32_t UnavailablePlayers(); // player-id bits; stale held packets do not make a player eligible again
void LogCombatDecision(eCEntity* npc, eCEntity* player, char const* decision, bool transition = false);
uint32_t Sequence();      // zero while not waiting; carried by player state and paid revive requests
void Restore(bool waiting, char const* source); // saves may restore a downed life, never release an existing one
void NoteBlow();         // combat.cpp: a blow on the local hero came from an NPC that means to kill (judged on its owner's machine)
bool NativeDown();       // ... or knocked out by the game's own rules (a guard, a duel): it stands up by itself; a winner may rob it
using LogFn = void (*)(char const*);
void Off(LogFn log);     // QA negative control: let the hero die again
void HoldOff(LogFn log); // QA negative control: the hero gets up by itself again
void PotionOff(LogFn log); // QA negative control: helping up costs nothing
void LethalOnlyOff(LogFn log);  // QA negative control: every knock-out waits for help, not only an intercepted death
void GivePotion(char const* argument, LogFn log);  // QA: "Template[-Amount]" into the hero's inventory ("-0" takes all away)
void Potions(LogFn log); // QA: POTION lines for the hero's potions
void Report(LogFn log);  // QA: DOWNED line
bool ObserveDeathState(eCEntity* self, bool entered, bool successful = true); // false refuses duplicate native entry
void AutoHelpOn(LogFn log);     // QA: standing next to a downed partner for 3 s helps it up again (as before 23.10.)
void HelpOptionOff(LogFn log);  // QA negative control: a downed partner's puppet offers no help option
void HelpLineSaid();  // infocmd.cpp: the line "Ich werde dir helfen." of the help option is being spoken
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
}
