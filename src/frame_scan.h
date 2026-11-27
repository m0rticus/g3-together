// SPDX-License-Identifier: GPL-3.0-or-later
// Shared per-frame cache of ROI NPCs and processed-range NPCs so non-NPC subsystems reuse one
// Entity::GetNPCs() / ProcessedEntities() pass per frame without duplicate engine array copies
// or repeated GUID / property-set lookups.
#pragma once

#include <Engine.h>
#include <Game.h>
#include <Script.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g3t::frame_scan {

using GuidKey = std::array<uint8_t, 16>;

struct GuidHash {
    size_t operator()(GuidKey const& key) const noexcept;
};

struct NpcEntry {
    eCEntity* entity = nullptr;
    GuidKey guid{};
    gCNPC_PS* npc = nullptr;
    gCScriptRoutine_PS* routine = nullptr;
    gCDialog_PS* dialog = nullptr;
};

void BeginFrame();
void EndFrame();
void Invalidate();
void OnWorldChange();
void OnEntityDestroyed(eCEntity const* entity);

// Cached Entity::GetNPCs() for the current frame (populated lazily on first query).
std::vector<NpcEntry> const& Npcs();
// Cached deduplicated union of Entity::GetNPCs() and NPCs in world::ProcessedEntities().
std::vector<NpcEntry> const& AllNpcs();
// O(1) membership check in the frame's Entity::GetNPCs() set (populates Npcs() if needed).
bool ContainsRoiNpc(eCEntity const* entity);

} // namespace g3t::frame_scan
