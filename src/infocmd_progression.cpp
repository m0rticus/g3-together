// SPDX-License-Identifier: GPL-3.0-or-later
#include "infocmd_internal.h"

#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"

#include <utility>

namespace g3t::infocmd::detail {

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
        ++sectorsApplied;
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
    if (!sharing || !gCSession::GetInstance().IsGameRunning()) return;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    auto* mgr = gCInfoManager_PS::GetInstance();
    const bool running = mgr && mgr->AccessIsRunning();
    if (running) {
        auto notePartner = [&](eCEntity* candidate) {
            if (!candidate || candidate == hero || remote::IsPuppet(candidate)) return;
            if (!candidate->GetPropertySet(eEPropertySetType_NPC) && !candidate->GetPropertySet(eEPropertySetType_Dialog)) return;
            uint8_t guid[16];
            guid::CopyEntity(candidate, guid);
            const GuidKey key = GuidKeyOf(guid);
            activeDialogNpcs.insert(key);
            Entity e(candidate);
            const auto nowObs = CaptureObserved(e);
            auto it = observedProgression.find(key);
            if (it == observedProgression.end()) {
                observedProgression.emplace(key, nowObs);
            } else if (it->second != nowObs) {
                it->second = nowObs;
                if (npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog))
                    ApplyFriendMemory(e.GetName().GetText());
                if (net::IsHost()) syncedNpcs.insert(key);
                if (net::Connected()) QueueImmediateState(guid, true);
            }
        };
        notePartner(mgr->GetDialogPartnerNPC());
        notePartner(mgr->GetCurrentSelf());
        notePartner(mgr->GetCurrentOther());
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
        eCEntity* inst = npc.GetInstance();
        if (!inst) continue;
        const bool isHero = inst == hero;
        const bool isPuppet = remote::IsPuppet(inst);
        const bool suppressed = npcremoval::Suppressed(inst);
        if (isHero || isPuppet || suppressed) continue;
        uint8_t guid[16];
        guid::CopyEntity(inst, guid);
        const GuidKey key = GuidKeyOf(guid);
        const auto nowObs = CaptureObserved(npc);
        const bool canAuthor = npc_progression_policy::ShouldAuthorProgressionDiff(
            net::IsHost(), isHero, isPuppet, suppressed,
            npclease::LocalAuthority(inst), talk::TalkingWithHero(inst));
        auto it = observedProgression.find(key);
        if (it == observedProgression.end()) {
            observedProgression.emplace(key, nowObs);
            if (net::IsHost() && npc_progression_policy::IsNonDefaultSeedCandidate(
                    npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog),
                    npc_progression_policy::UnpackDefeatedByPlayer(nowObs.npc))) {
                syncedNpcs.insert(key);
            }
            continue;
        }
        if (it->second == nowObs) continue;
        it->second = nowObs;
        if (!canAuthor) continue;
        if (npc_progression_policy::UnpackTalkedToPlayer(nowObs.dialog))
            ApplyFriendMemory(npc.GetName().GetText());
        if (net::IsHost()) syncedNpcs.insert(key);
        if (net::Connected()) QueueImmediateState(guid, true);
    }
}

} // namespace g3t::infocmd::detail

namespace g3t::infocmd {
void SyncToClients()
{
    using namespace detail;
    if (!sharing || !net::IsHost() || !net::Connected() || !gCSession::GetInstance().IsGameRunning()) return;
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
