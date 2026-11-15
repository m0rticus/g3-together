// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class eCEntity;

namespace g3t::net { struct WorldState; }

namespace g3t::combat_hostility {

void Tick();
void SyncHostNpc(eCEntity* npc);
void SyncClientLeasedNpc(eCEntity* npc);
void ClearClientLeasedNpc(uint8_t const guid[16]);
void OnState(uint8_t from, net::WorldState const& state);
void OnWorldChange();
bool ResumeLeasedCombat(eCEntity* npc, uint8_t aiMode, uint8_t attitude, uint8_t attackReason, eCEntity* target);

bool IsSharedCombatHostile(eCEntity const* npc);
bool ShouldPromoteAttitude(eCEntity const* npc);
bool MirrorSharesCombatHostility(eCEntity const* npc);
uint8_t MirrorAttitude(eCEntity const* npc, uint8_t wanted);
int EffectiveAttitude(eCEntity const* npc, int baseAttitude);
uint8_t CoopTargetPlayer(eCEntity* target);
bool FocusPriorityHooked();

}  // namespace g3t::combat_hostility
