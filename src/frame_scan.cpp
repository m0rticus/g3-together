// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_scan.h"

#include "guid.h"
#include "net.h"
#include "world.h"

#include <unordered_set>

namespace g3t::frame_scan {
namespace {

bool npcsReady = false;
bool allNpcsReady = false;
std::vector<NpcEntry> npcsCache;
std::vector<NpcEntry> allNpcsCache;
std::unordered_set<eCEntity const*> roiSeen;
std::unordered_set<eCEntity const*> allSeen;

NpcEntry BuildEntry(eCEntity* entity)
{
    NpcEntry entry{};
    entry.entity = entity;
    guid::CopyEntity(entity, entry.guid.data());
    entry.npc = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
    entry.routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    entry.dialog = static_cast<gCDialog_PS*>(entity->GetPropertySet(eEPropertySetType_Dialog));
    return entry;
}

void PopulateNpcs()
{
    npcsCache.clear();
    roiSeen.clear();
    if (gCSession::GetInstance().IsGameRunning()) {
        const bTObjArray<Entity> all = Entity::GetNPCs();
        npcsCache.reserve(static_cast<size_t>(all.GetCount()));
        for (int i = 0; i < all.GetCount(); ++i) {
            eCEntity* entity = all.GetAt(i).GetInstance();
            if (!entity || !roiSeen.insert(entity).second) continue;
            npcsCache.push_back(BuildEntry(entity));
        }
    }
    npcsReady = true;
}

void PopulateAllNpcs()
{
    if (!npcsReady) PopulateNpcs();
    allNpcsCache = npcsCache;
    allSeen = roiSeen;
    if (auto const* processed = world::ProcessedEntities()) {
        for (GEU16 i = 0; i < processed->GetCount(); ++i) {
            eCEntity* entity = processed->GetAt(i);
            if (!entity || allSeen.count(entity)) continue;
            auto* npc = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
            auto* routine = npc ? static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) : nullptr;
            if (!npc || !routine) continue;
            allSeen.insert(entity);
            NpcEntry entry{};
            entry.entity = entity;
            guid::CopyEntity(entity, entry.guid.data());
            entry.npc = npc;
            entry.routine = routine;
            entry.dialog = static_cast<gCDialog_PS*>(entity->GetPropertySet(eEPropertySetType_Dialog));
            allNpcsCache.push_back(entry);
        }
    }
    allNpcsReady = true;
}

} // namespace

size_t GuidHash::operator()(GuidKey const& key) const noexcept
{
    return net::HashGuid(key.data());
}

void BeginFrame()
{
    npcsReady = false;
    allNpcsReady = false;
}

void EndFrame()
{
    npcsReady = false;
    allNpcsReady = false;
    npcsCache.clear();
    allNpcsCache.clear();
    roiSeen.clear();
    allSeen.clear();
}

void Invalidate()
{
    npcsReady = false;
    allNpcsReady = false;
}

void OnWorldChange()
{
    EndFrame();
}

void OnEntityDestroyed(eCEntity const* entity)
{
    if (!entity) return;
    if (roiSeen.count(entity) || allSeen.count(entity)) Invalidate();
}

std::vector<NpcEntry> const& Npcs()
{
    if (!npcsReady) PopulateNpcs();
    return npcsCache;
}

std::vector<NpcEntry> const& AllNpcs()
{
    if (!allNpcsReady) PopulateAllNpcs();
    return allNpcsCache;
}

bool ContainsRoiNpc(eCEntity const* entity)
{
    if (!entity) return false;
    if (!npcsReady) PopulateNpcs();
    return roiSeen.count(entity) != 0;
}

} // namespace g3t::frame_scan
