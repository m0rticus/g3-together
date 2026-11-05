// SPDX-License-Identifier: GPL-3.0-or-later
// The client's info manager owns its dialog UI and choices. The host runs the NPC talk state against that player's
// puppet and publishes its animation; the client partner remains a passive mirror.
#pragma once

#include <cstdint>

class eCEntity;

namespace g3t::talk {
void Start();
void Tick();
// Host: a client's dialog partner (one of the host's NPCs, held here) makes a gesture there: the same motion here (kind 15).
void OnGesture(uint8_t from, uint8_t const guid[16], uint32_t motion, float const rotation[4]);
bool TalkingWithHero(eCEntity const* entity);  // client: this mirrored NPC is in a dialog with our hero
bool IsHeld(eCEntity const* entity);           // host: held for a remote player's dialog
bool InGesture(eCEntity const* entity);        // host: a held NPC plays a gesture of the client's dialog now
void OnWorldChange();                          // forget held NPCs and the dialog partner (their entities are gone)
using LogFn = void (*)(char const*);
void Choose(int index, LogFn log);  // QA: pick the index-th available dialog option
void ChooseInfo(char const* name, LogFn log);  // QA: run a named option in the running dialog
void Report(LogFn log);             // QA: TALKSYNC line
void DisableHold(LogFn log);        // QA negative control (host): ignore dialog notices
void GesturesOff(LogFn log);        // QA negative control (client): the partner's gestures are not sent
}
