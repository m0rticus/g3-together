// SPDX-License-Identifier: GPL-3.0-or-later
// Host campaign death journal. Only the host restores recorded deaths after processing-range entry;
// clients replay the host's NPC life/HP/animation state without running death tasks.
#pragma once

class eCEntity;
#include <cstdio>

namespace g3t::journal {
void Start();
void Tick();                               // once per frame
void OnWorldChange();                      // entities of the old world are gone
// The campaign's journal in the host's save sidecar (persist.cpp): one line per death.
unsigned Write(FILE* file);
unsigned Read(FILE* file);                 // replaces the journal
void Clear();
bool Pending(eCEntity const* entity);      // a recorded death not applied here yet (host: do not mirror it alive)
// The NPC stood up again (respawn.cpp): its recorded death is over, a new death is reported again.
void NewLife(eCEntity const* entity);
void NewLife(unsigned char const guid[16]);  // the same by GUID, for an NPC not in this part of the world now
using LogFn = void (*)(char const*);
void Off(LogFn log);                       // QA negative control: report no deaths
void QuestCountOff(LogFn log);             // QA negative control: the host does not count clients' kills for quests
void Report(LogFn log);                    // QA: JOURNAL line
void Entry(char const* name, LogFn log);   // QA: JENTRY line for a named NPC
}
