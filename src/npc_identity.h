// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

using NpcOpponents = std::unordered_map<uint32_t, eCEntity*>;

std::string FocusIdentity(eCEntity* entity);
bool FocusProcessed(eCEntity* entity);
std::string FocusRawIdentity(eCEntity* entity);
uint32_t FocusEntityFlags(eCEntity* entity);
Mirror const* FocusMirror(eCEntity* entity);
bool VerifyFocusQueueLayout(unsigned char* module);
bool ReadFocusCandidates(uint32_t serial, gCFocus_PS& focus, std::vector<eCEntity*>& candidates);
bool RelationOfImpl(eCEntity const* entity, RelationInfo& info);
void ReconcileClientPresence();
void ResetWorldNpcTables();

} // namespace g3t::npcs::detail
