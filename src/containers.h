// SPDX-License-Identifier: GPL-3.0-or-later
// Personal chest/corpse loot and NPC merchant inventories. Native local generation/transfers are retained;
// remote inventory messages never overwrite loot or roll back the hero. Direct player trades remain in trade.cpp.
#pragma once

#include <cstdint>
#include <vector>

class eCEntity;

namespace g3t::containers {
bool IsPersonalInventory(eCEntity* entity);  // world inventory, not the hero, an item or another player's puppet
bool PersonalHooksReady();  // true only when every required native/script guard is installed
void PrepareCorpseLoot(eCEntity* entity);  // ensure personal loot is generated and captured on newly dead/KO'd NPCs
bool CapturePersonal(std::vector<uint8_t>& out);  // false leaves out unchanged
bool RestorePersonal(std::vector<uint8_t> const& bytes);  // empty bytes clear the ledger without changing native inventory
void Start();
void Tick();
void OnWorldChange();  // forget every container state of the world before (load, world change)
}
