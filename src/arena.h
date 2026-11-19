// SPDX-License-Identifier: GPL-3.0-or-later
// P4, arena and sparring fights (35 quests): the game sends the opponent into the arena only on the machine whose dialog
// started the fight, and attacks only the local hero there (Entity::GetPlayer). The player who starts the fight takes the
// opponent over from the other machine while the fight lasts.
#pragma once

#include "net.h"
class eCEntity;

namespace g3t::arena {
void Tick();  // wrap arena scripts once the game runs; drive host/client bouts and release finished fighters
// Client: the host hands back an NPC from its save. True when it is the opponent of a running arena or sparring quest: it is
// given back to the host at once instead of becoming a companion (its fight starts again from the dialog).
bool Readopt(eCEntity* npc);
void OnWorldChange();  // the world (or save) was left: nothing is held any more
using LogFn = void (*)(char const*);
void Report(LogFn log);  // QA: ARENASYNC line
void Off(LogFn log);     // QA negative control: arenasyncoff, the opponent stays where it is simulated
void OnState(uint8_t from, net::WorldState const& state);
bool AllowClientQuestStatusOnHost(uint8_t from, char const* questName, int clientNewStatus);
void OnClientArenaQuestStatus(uint8_t from, char const* questName, int prevStatus, int newStatus);
void NoteQuestStatusFromHost(char const* questName, int newStatus);
void SyncArenaStatuses();
void TrackDuel(eCEntity* npc, uint8_t player);
uint8_t ChallengerForNpc(eCEntity const* npc);
}
