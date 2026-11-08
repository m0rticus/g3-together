// SPDX-License-Identifier: GPL-3.0-or-later
// NPC relation (name colour) upkeep and diagnostics. See relation_policy.h for the rules.
#pragma once

#include <cstdint>

class eCEntity;

namespace g3t::relation {
// Once per frame: `relation:` transition lines when a player goes down or is revived, a summary and the nearest NPCs every
// 5 s (and 1 s and 4 s after a transition), `relation: change` lines for tracked NPCs, `relation: mismatch` lines.
void Tick();
// The attitude an NPC record this machine sends carries. A live NPC no script has evaluated yet (stored 0, the white name
// tag) is given the relation to this machine's player, as its next script would store; stored relations are never touched.
uint8_t Outgoing(eCEntity* npc, bool alive);
}
