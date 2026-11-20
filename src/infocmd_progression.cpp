// SPDX-License-Identifier: GPL-3.0-or-later
#include "infocmd_internal.h"

#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"

#include <utility>

namespace g3t::infocmd::detail {
namespace {
std::map<GuidKey, npc_progression_policy::ObservedProgression> observedProgression;
std::set<GuidKey> syncedNpcs;
std::map<std::string, bool> syncedSectors;
std::map<GuidKey, net::WorldState> bufferedWorldStates;
std::map<std::string, bool> bufferedSectors;
std::set<GuidKey> activeDialogNpcs;
uint32_t bufferedEpoch = 0;
bool wasDialogRunning = false;
bool worldSeeded = false;

void SyncClientBufferEpoch()
{
    if (net::IsHost()) return;
    const uint32_t epoch = net::WorldEpoch();
    if (!net::Connected()) {
        bufferedWorldStates.clear();
        bufferedSectors.clear();
        bufferedEpoch = 0;
    } else if (epoch && bufferedEpoch != epoch) {
        if (bufferedEpoch) {
            bufferedWorldStates.clear();
            bufferedSectors.clear();
        }
        bufferedEpoch = epoch;
    }
}

void ObserveEntityProgression(Entity& e, eCEntity* hero, bool activeDialogPartner)
{
    eCEntity* inst = e.GetInstance();
    if (!inst) return;
    if (!inst->GetPropertySet(eEPropertySetType_NPC) && !inst->GetPropertySet(eEPropertySetType_Dialog)) return;
    const bool isHero = inst == hero;
    const bool isPuppet = remote::IsPuppet(inst);
    const bool suppressed = npcremoval::Suppressed(inst);
    if (isHero || isPuppet || suppressed) return;
    uint8_t guid[16];
    guid::CopyEntity(inst, guid);
    const GuidKey key = GuidKeyOf(guid);
    if (activeDialogPartner) activeDialogNpcs.insert(key);
    const auto nowObs = CaptureObserved(e);
    const bool canAuthor = npc_progression_policy::ShouldAuthorProgressionDiff(
        net::IsHost(), isHero, isPuppet, suppressed,
        npclease::LocalAuthority(inst), activeDialogPartner || talk::TalkingWithHero(inst));
    auto it = observedProgression.find(key);
    if (it == observedProgression.end()) {
        observedProgression.emplace(key, nowObs);
        if (!activeDialogPartner && net::IsHost() && npc_progression_policy::IsNonDefaultSeedCandidate(
                npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog),
                npc_progression_policy::UnpackDefeatedByPlayer(nowObs.npc))) {
            syncedNpcs.insert(key);
        }
        return;
    }
    if (it->second == nowObs) return;
    it->second = nowObs;
    if (!canAuthor) return;
    if (npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog))
        ApplyFriendMemory(e.GetName().GetText());
    if (net::IsHost()) syncedNpcs.insert(key);
    if (net::Connected()) QueueImmediateState(guid, true);
}
} // namespace

void NoteObservedProgression(GuidKey const& key, npc_progression_policy::ObservedProgression const& obs)
{
    observedProgression[key] = obs;
}

void NoteSyncedNpc(GuidKey const& key) { syncedNpcs.insert(key); }

void NoteSyncedSector(char const* sector, bool enabled)
{
    if (sector && *sector) syncedSectors[sector] = enabled;
}

void BufferClientNpcState(GuidKey const& key, net::WorldState const& s)
{
    SyncClientBufferEpoch();
    auto [it, inserted] = bufferedWorldStates.emplace(key, s);
    if (inserted) return;
    net::WorldState merged = s;
    if (merged.moved == 0 && it->second.moved != 0) {
        merged.moved = it->second.moved;
        std::memcpy(merged.position, it->second.position, sizeof(merged.position));
        std::memcpy(merged.rotation, it->second.rotation, sizeof(merged.rotation));
    }
    if (merged.routine[0] == '\0' && it->second.routine[0] != '\0')
        strncpy_s(merged.routine, it->second.routine, _TRUNCATE);
    it->second = merged;
}

void BufferClientSectorState(char const* sector, bool enabled)
{
    if (!sector || !*sector) return;
    SyncClientBufferEpoch();
    bufferedSectors[sector] = enabled;
}

void ResetProgressionState()
{
    observedProgression.clear();
    activeDialogNpcs.clear();
    wasDialogRunning = false;
    worldSeeded = false;
    if (net::IsHost()) {
        syncedNpcs.clear();
        syncedSectors.clear();
    } else {
        bufferedWorldStates.clear();
        bufferedSectors.clear();
        bufferedEpoch = net::WorldEpoch();
    }
}

npc_progression_policy::ObservedProgression CaptureObserved(Entity const& e)
{
    npc_progression_policy::ObservedProgression obs{};
    eCEntity* instance = e.GetInstance();
    if (!instance) return obs;
    if (instance->GetPropertySet(eEPropertySetType_Dialog)) {
        obs.dialog = npc_progression_policy::PackDialog(
            e.Dialog.TeachEnabled != GEFalse, e.Dialog.TradeEnabled != GEFalse,
            e.Dialog.PartyEnabled != GEFalse, e.Dialog.MobEnabled != GEFalse,
            e.Dialog.SlaveryEnabled != GEFalse, e.Dialog.TalkedToPlayer != GEFalse);
    }
    if (instance->GetPropertySet(eEPropertySetType_NPC)) {
        obs.npc = npc_progression_policy::PackNpcFlags(true, e.NPC.DefeatedByPlayer != GEFalse);
        obs.guard = static_cast<uint8_t>(static_cast<int>(e.NPC.GuardStatus));
        obs.npcClass = static_cast<uint8_t>(static_cast<int>(e.NPC.Class));
        obs.alignment = static_cast<uint8_t>(static_cast<int>(e.NPC.PoliticalAlignment));
        obs.immortal = e.NPC.Immortal ? 1 : 0;
    }
    return obs;
}

void ApplyFriendMemory(char const* entityName)
{
    eCEntity* rawHero = gCSession::GetInstance().IsGameRunning() ? gCSession::GetInstance().GetHero() : nullptr;
    if (!rawHero || !rawHero->GetPropertySet(eEPropertySetType_PlayerMemory)) return;
    Entity hero(rawHero);
    switch (npc_progression_policy::FriendMemoryForName(entityName)) {
    case npc_progression_policy::FriendMemoryFlag::Diego: hero.PlayerMemory.TalkedToDiego = GETrue; break;
    case npc_progression_policy::FriendMemoryFlag::Gorn: hero.PlayerMemory.TalkedToGorn = GETrue; break;
    case npc_progression_policy::FriendMemoryFlag::Milten: hero.PlayerMemory.TalkedToMilten = GETrue; break;
    case npc_progression_policy::FriendMemoryFlag::Lester: hero.PlayerMemory.TalkedToLester = GETrue; break;
    default: break;
    }
}

void QueueImmediateState(uint8_t const guid[16], bool progressionOnly)
{
    const uint32_t now = net::Now();
    for (Pending& p : pending) {
        if (!p.sector.empty() || std::memcmp(p.guid, guid, 16) != 0) continue;
        if (progressionOnly) {
            if (p.progressionOnly || static_cast<int32_t>(now - p.due) >= 0) return;
            continue;
        }
        p.progressionOnly = false;
        p.due = now;
        return;
    }
    Pending pend{};
    std::memcpy(pend.guid, guid, 16);
    pend.due = now;
    pend.progressionOnly = progressionOnly;
    pending.push_back(std::move(pend));
}

void SeedHostWorldState()
{
    if (worldSeeded || !net::IsHost() || !gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero())
        return;
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    if (!scene) return;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    auto& nodes = scene->m_mapEntities.GetNodes();
    for (GEInt i = 0; i < nodes.GetCount(); ++i) {
        for (auto* node = nodes[i]; node; node = node->m_pNext) {
            eCEntity* raw = node->m_Element;
            if (!raw || raw == hero || remote::IsPuppet(raw) || npcremoval::Suppressed(raw)) continue;
            if (!raw->GetPropertySet(eEPropertySetType_NPC)) continue;
            Entity npc(raw);
            const bool talked = raw->GetPropertySet(eEPropertySetType_Dialog) && npc.Dialog.TalkedToPlayer != GEFalse;
            const bool defeated = npc.NPC.DefeatedByPlayer != GEFalse;
            if (!npc_progression_policy::IsNonDefaultSeedCandidate(talked, defeated)) continue;
            uint8_t guid[16];
            guid::CopyEntity(raw, guid);
            const GuidKey key = GuidKeyOf(guid);
            syncedNpcs.insert(key);
            observedProgression[key] = CaptureObserved(npc);
            if (talked) ApplyFriendMemory(npc.GetName().GetText());
        }
    }
    worldSeeded = true;
}

void FlushBufferedClientStates()
{
    if (net::IsHost() || !gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero()) return;
    SyncClientBufferEpoch();
    for (auto const& [sector, status] : bufferedSectors) {
        world.SetSectorStatus(bCString(sector.c_str()), status);
        ++stats.sectorsApplied;
    }
    bufferedSectors.clear();
    for (auto it = bufferedWorldStates.begin(); it != bufferedWorldStates.end();) {
        Entity e = guid::Resolve(it->second.guid);
        if (!e.GetInstance()) { ++it; continue; }
        const net::WorldState copy = it->second;
        it = bufferedWorldStates.erase(it);
        Apply(copy, 1);
    }
}

void TrackLocalDialogAndProgression()
{
    if (!flags.sharing || !gCSession::GetInstance().IsGameRunning()) return;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    auto* mgr = gCInfoManager_PS::GetInstance();
    const bool running = mgr && mgr->AccessIsRunning();
    if (running) {
        eCEntity* const candidates[] = {mgr->GetDialogPartnerNPC(), mgr->GetCurrentSelf(), mgr->GetCurrentOther()};
        for (eCEntity* candidate : candidates) {
            if (!candidate) continue;
            Entity partner(candidate);
            ObserveEntityProgression(partner, hero, true);
        }
        wasDialogRunning = true;
    } else if (wasDialogRunning) {
        wasDialogRunning = false;
        for (GuidKey const& key : activeDialogNpcs) {
            Entity e = guid::Resolve(key.data());
            eCEntity* inst = e.GetInstance();
            if (!inst) continue;
            const auto nowObs = CaptureObserved(e);
            auto it = observedProgression.find(key);
            const bool hadPrior = it != observedProgression.end();
            const auto priorObs = hadPrior ? it->second : npc_progression_policy::ObservedProgression{};
            observedProgression[key] = nowObs;
            if (npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog))
                ApplyFriendMemory(e.GetName().GetText());
            const bool canAuthor = npc_progression_policy::ShouldAuthorProgressionDiff(
                net::IsHost(), inst == hero, remote::IsPuppet(inst), npcremoval::Suppressed(inst),
                npclease::LocalAuthority(inst), true);
            if (!canAuthor || !npc_progression_policy::ShouldPublishDialogCloseProgression(hadPrior, priorObs, nowObs))
                continue;
            if (net::IsHost()) syncedNpcs.insert(key);
            if (net::Connected()) QueueImmediateState(key.data(), true);
        }
        activeDialogNpcs.clear();
    }
    const bTObjArray<Entity> npcsList = Entity::GetNPCs();
    for (int i = 0; i < npcsList.GetCount(); ++i) {
        Entity npc = npcsList.GetAt(i);
        ObserveEntityProgression(npc, hero, false);
    }
}

} // namespace g3t::infocmd::detail

namespace g3t::infocmd {
void SyncToClients()
{
    using namespace detail;
    if (!flags.sharing || !net::IsHost() || !net::Connected() || !gCSession::GetInstance().IsGameRunning()) return;
    SeedHostWorldState();
    for (auto const& [sector, status] : syncedSectors) {
        net::WorldState s{};
        s.kind = 2;
        strncpy_s(s.routine, sector.c_str(), _TRUNCATE);
        s.enclave = status ? 1 : 0;
        net::SendWorldState(s, 0);
    }
    for (GuidKey const& key : syncedNpcs) {
        if (npcremoval::Suppressed(key.data())) continue;
        Entity e = guid::Resolve(key.data());
        if (!e.GetInstance() || e.GetInstance() == gCSession::GetInstance().GetHero() || remote::IsPuppet(e.GetInstance()))
            continue;
        net::WorldState s{};
        Snapshot(e, s, true);
        net::SendWorldState(s, 0);
    }
}
} // namespace g3t::infocmd
