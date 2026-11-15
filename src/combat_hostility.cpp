// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_hostility.h"

#include "ai.h"
#include "body.h"
#include "guards.h"
#include "guid.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "relation_policy.h"
#include "remote.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace g3t::combat_hostility {
namespace {

using GuidKey = std::array<uint8_t, 16>;

struct KeyHash {
    size_t operator()(GuidKey const& key) const noexcept { return net::HashGuid(key.data()); }
};

constexpr uint8_t WorldStateKind = 23;
constexpr uintptr_t FocusPriorityRva = 0xAA4A0;

gFScript originalFocusPriority = nullptr;
bool focusPrioritySearched = false;
bool focusPriorityNative = false;

std::unordered_set<eCEntity const*> promotedNpcs;
std::unordered_map<GuidKey, uint8_t, KeyHash> sentState;
std::unordered_map<GuidKey, std::array<bool, net::MaxPlayers>, KeyHash> remoteHostile;

GuidKey KeyOf(eCEntity const* npc)
{
    GuidKey key{};
    if (npc) guid::CopyEntity(const_cast<eCEntity*>(npc), key.data());
    return key;
}

bool ValidNpc(eCEntity const* entity)
{
    return entity && entity->GetPropertySet(eEPropertySetType_NPC) &&
           entity->GetPropertySet(eEPropertySetType_ScriptRoutine);
}

uint8_t LocalCoopId()
{
    const uint8_t id = net::LocalId();
    return id ? id : uint8_t(1);
}

uint8_t CoopPlayerOfEntity(eCEntity const* entity)
{
    if (!entity) return 0;
    if (entity == remote::ControlledBody() || entity == gCSession::GetInstance().GetHero()) return LocalCoopId();
    return static_cast<uint8_t>(remote::PuppetOwner(entity));
}

uint8_t CoopCompanionOwner(eCEntity const* entity)
{
    if (!ValidNpc(entity)) return 0;
    eCEntity* mutableEntity = const_cast<eCEntity*>(entity);
    if (const uint8_t owner = npcs::CompanionOwner(mutableEntity)) return owner;
    if (auto const* rec = npcs::CombatRecord(entity)) {
        if (rec->partyPlayer != 0) return rec->partyPlayer;
        if (rec->guidePlayer != 0) return rec->guidePlayer;
    }
    if (entity->GetPropertySet(eEPropertySetType_Party)) {
        eCEntity* leader = Entity(mutableEntity).Party.GetPartyLeader().GetInstance();
        if (const uint8_t owner = CoopPlayerOfEntity(leader)) return owner;
    }
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (hero && hero->GetPropertySet(eEPropertySetType_Party) &&
        Entity(hero).Party.GetPartyLeader().GetInstance() == entity)
        return LocalCoopId();
    eCEntity* controlled = remote::ControlledBody();
    if (controlled && controlled != hero && controlled->GetPropertySet(eEPropertySetType_Party) &&
        Entity(controlled).Party.GetPartyLeader().GetInstance() == entity)
        return LocalCoopId();
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        eCEntity* puppet = remote::PuppetOf(id);
        if (puppet && puppet->GetPropertySet(eEPropertySetType_Party) &&
            Entity(puppet).Party.GetPartyLeader().GetInstance() == entity)
            return id;
    }
    return 0;
}

uint8_t CoopOwnerOf(eCEntity const* entity)
{
    if (const uint8_t player = CoopPlayerOfEntity(entity)) return player;
    return CoopCompanionOwner(entity);
}

bool IsCoopCompanion(eCEntity const* entity)
{
    return CoopPlayerOfEntity(entity) == 0 && CoopCompanionOwner(entity) != 0;
}

bool IsCoopParticipant(eCEntity const* entity)
{
    return CoopOwnerOf(entity) != 0;
}

relation::CombatRole RoleOf(eCEntity const* entity)
{
    if (CoopPlayerOfEntity(entity) != 0) return relation::CombatRole::PlayerOrPuppet;
    if (CoopCompanionOwner(entity) != 0) return relation::CombatRole::CoopCompanion;
    return relation::CombatRole::OrdinaryNpc;
}

bool HasRemoteHostile(GuidKey const& key)
{
    auto const found = remoteHostile.find(key);
    if (found == remoteHostile.end()) return false;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (found->second[id]) return true;
    }
    return false;
}

void ClearRemoteHostileOnHost(GuidKey const& key)
{
    auto const found = remoteHostile.find(key);
    if (found == remoteHostile.end()) return;
    if (net::Connected()) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            if (!found->second[id]) continue;
            net::WorldState clearMsg{};
            clearMsg.kind = WorldStateKind;
            std::memcpy(clearMsg.guid, key.data(), 16);
            clearMsg.npc = 0;
            clearMsg.guard = id;
            net::SendWorldState(clearMsg, 0);
        }
    }
    remoteHostile.erase(found);
}

relation::SharedCombatInput BuildInput(eCEntity const* npc)
{
    relation::SharedCombatInput in{};
    if (!ValidNpc(npc)) return in;
    Entity self(const_cast<eCEntity*>(npc));
    in.alive = !self.IsDead();
    in.down = self.IsDown();
    in.role = RoleOf(npc);
    in.storedAttitude = static_cast<int>(static_cast<gEAttitude>(self.NPC.AttitudeToPlayer2));
    in.mirrored = npcs::IsMirrored(npc);
    const GuidKey key = KeyOf(npc);
    in.syncedSharedHostile = HasRemoteHostile(key);

    if (in.mirrored) {
        if (auto const* rec = npcs::CombatRecord(npc)) {
            in.aiMode = rec->aiMode;
            in.combatState = (rec->flags & 4u) ? 1u : 0u;
            in.attackReason = rec->attackReason;
            const uint8_t targetPlayer = (rec->flags >> 3) & 7u;
            if (targetPlayer != 0) {
                in.targetsCoopSide = true;
            } else {
                eCEntity* opponent = nullptr;
                if (npcs::CombatTarget(const_cast<eCEntity*>(npc), opponent) && IsCoopParticipant(opponent))
                    in.targetsCoopSide = true;
            }
        } else {
            in.aiMode = body::AIModeOf(const_cast<eCEntity*>(npc));
        }
        return in;
    }

    in.aiMode = static_cast<int>(static_cast<gEAIMode>(self.Routine.AIMode));
    in.combatState = static_cast<uint32_t>(self.NPC.CombatState);
    in.lastFight = static_cast<int>(static_cast<gELastFightAgainstPlayer>(self.NPC.LastFightAgainstPlayer));
    in.attackReason = static_cast<int>(static_cast<gEAttackReason>(self.NPC.AttackReason));
    in.clientLeasedEngagement = net::IsHost() && npclease::LeasedCombatHostile(npc);

    const bool active = in.aiMode == gEAIMode_Combat || in.combatState != 0 ||
                        in.lastFight == gELastFightAgainstPlayer_Running;
    if (active) {
        eCEntity* target = self.NPC.GetCurrentTarget().GetInstance();
        eCEntity* alt = self.NPC.GetAlternativeTarget().GetInstance();
        eCEntity* attacker = self.NPC.GetCurrentAttacker().GetInstance();
        in.targetsCoopSide = IsCoopParticipant(target) || IsCoopParticipant(alt) || IsCoopParticipant(attacker);
    }
    return in;
}

void PublishTransition(GuidKey const& key, bool active, int attackReason)
{
    if (!net::Connected()) return;
    const uint8_t next = active ? 1 : 0;
    auto const prev = sentState.find(key);
    if (prev == sentState.end() && !active) return;
    if (prev != sentState.end() && prev->second == next) return;
    sentState[key] = next;

    net::WorldState s{};
    s.kind = WorldStateKind;
    std::memcpy(s.guid, key.data(), 16);
    s.npc = next;
    s.guard = net::LocalId();
    s.npcClass = active ? static_cast<uint8_t>(attackReason & 0xFF) : 0;
    net::SendWorldState(s, 0);
}

void SyncNativeEntity(eCEntity* npc)
{
    if (!ValidNpc(npc) || npcs::IsMirrored(npc)) return;
    if (npc == gCSession::GetInstance().GetHero() || npc == remote::ControlledBody() || remote::IsPuppet(npc))
        return;

    const GuidKey key = KeyOf(npc);
    if (net::IsHost() && !npclease::RemoteOwner(npc) && HasRemoteHostile(key)) {
        ClearRemoteHostileOnHost(key);
    }
    const relation::SharedCombatInput in = BuildInput(npc);
    const bool hostileNow = relation::ActiveSharedCombatHostility(in);

    if (hostileNow) {
        promotedNpcs.insert(npc);
        if (in.storedAttitude != relation::Hostile && in.storedAttitude != relation::Predator &&
            in.storedAttitude != relation::Panic)
            Entity(npc).NPC.AttitudeToPlayer2 = gEAttitude_Hostile;
        if (net::IsHost() || npclease::LocalAuthority(npc))
            PublishTransition(key, true, in.attackReason);
        return;
    }

    if (promotedNpcs.erase(npc) > 0) {
        int truth = -1;
        if (ai::LocalAttitude(npc, truth) && relation::ValidAttitude(truth) && truth > relation::None)
            Entity(npc).NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(truth);
    }
    if (net::IsHost() || npclease::LocalAuthority(npc))
        PublishTransition(key, false, 0);
}

bool PlayerInCombatFocusMode()
{
    eCEntity* body = remote::ControlledBody();
    if (!body || !body->GetPropertySet(eEPropertySetType_ScriptRoutine)) return false;
    Entity player(body);
    const bCString task = player.Routine.GetCurrentTask();
    char const* name = task.GetText();
    if (name && (!std::strcmp(name, "PS_Melee") || !std::strcmp(name, "PS_Ranged") ||
                 !std::strcmp(name, "PS_Magic")))
        return true;
    if (auto* focus = static_cast<gCFocus_PS*>(body->GetPropertySet(eEPropertySetType_Focus))) {
        const int mode = static_cast<int>(focus->GetCombatMode());
        if (mode >= 2 && mode <= 4) return true;
    }
    return false;
}

GEInt GE_STDCALL FilterFocusEntityPriority(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown())
        return originalFocusPriority ? originalFocusPriority(spu, self, other, args) : 0;
    eCEntity* candidate = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    if (ValidNpc(candidate) && PlayerInCombatFocusMode()) {
        const relation::SharedCombatInput sc = BuildInput(candidate);
        relation::FocusPriorityInput fi{};
        fi.alive = sc.alive;
        fi.down = sc.down;
        fi.combatTask = true;
        fi.mirrored = sc.mirrored;
        fi.sharedHostile = relation::ActiveSharedCombatHostility(sc);
        fi.targetsCoopSide = sc.targetsCoopSide;
        fi.inCombat = sc.aiMode == gEAIMode_Combat || sc.combatState != 0 ||
                      sc.lastFight == gELastFightAgainstPlayer_Running;
        fi.outsideBout = relation::BoutAttackReason(sc.attackReason);
        fi.role = sc.role;
        fi.attitude = sc.storedAttitude;
        const int overridePriority = relation::CombatFocusPriorityOverride(fi);
        if (overridePriority >= 0) return overridePriority;
    }
    return originalFocusPriority ? originalFocusPriority(spu, self, other, args) : 0;
}

void InstallHooks()
{
    if (focusPrioritySearched || !gCSession::GetInstance().IsGameRunning()) return;
    focusPrioritySearched = true;
    gSScript* script = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("GetFocusEntityPriority"), script) ||
        !script || !script->m_funcScript) {
        Log("relation: GetFocusEntityPriority not found; mid-combat focus override unavailable");
        return;
    }
    const gFScript native = script->m_funcScript;
    originalFocusPriority = native;
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    auto* code = module ? reinterpret_cast<unsigned char*>(module) + FocusPriorityRva : nullptr;
    static const unsigned char head[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0xfc, 0x03, 0x00, 0x00};
    if (code && reinterpret_cast<void*>(native) == code && !std::memcmp(code, head, sizeof(head))) {
        if (void* trampoline = ai::JumpTo(code, reinterpret_cast<void*>(&FilterFocusEntityPriority))) {
            originalFocusPriority = reinterpret_cast<gFScript>(trampoline);
            focusPriorityNative = true;
        }
    }
    script->m_funcScript = &FilterFocusEntityPriority;
    ai::TrackPointer(reinterpret_cast<void*>(&script->m_funcScript), reinterpret_cast<void*>(native),
                     reinterpret_cast<void*>(&FilterFocusEntityPriority), "combat_hostility GetFocusEntityPriority");
    Logf("relation: GetFocusEntityPriority wrapped (%s)", focusPriorityNative ? "native jump and script table" : "script table only");
}

void PruneDisconnectedPeers()
{
    if (!net::Connected()) {
        remoteHostile.clear();
        sentState.clear();
        return;
    }
    for (auto it = remoteHostile.begin(); it != remoteHostile.end();) {
        bool any = net::IsHost() ? false : it->second[1];
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            const bool alive = net::IsHost() ? net::PeerJoined(id)
                                             : (id != net::LocalId() && net::Remotes()[id].connected);
            if (it->second[id] && !alive) it->second[id] = false;
            any = any || it->second[id];
        }
        if (!any) it = remoteHostile.erase(it);
        else ++it;
    }
}

}  // namespace

void Tick()
{
    if (ai::ShuttingDown() || !gCSession::GetInstance().IsGameRunning()) return;
    InstallHooks();
    PruneDisconnectedPeers();

    std::unordered_set<eCEntity const*> visited;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* npc = all.GetAt(i).GetInstance();
        if (!ValidNpc(npc) || npcremoval::Suppressed(npc) || !visited.insert(npc).second) continue;
        if (!npcs::IsMirrored(npc)) SyncNativeEntity(npc);
    }
    if (auto const* processed = world::ProcessedEntities()) {
        for (GEU16 i = 0; i < processed->GetCount(); ++i) {
            eCEntity* npc = processed->GetAt(i);
            if (!ValidNpc(npc) || npcremoval::Suppressed(npc) || !visited.insert(npc).second) continue;
            if (!npcs::IsMirrored(npc)) SyncNativeEntity(npc);
        }
    }
}

void SyncHostNpc(eCEntity* npc)
{
    if (!net::IsHost() || ai::ShuttingDown()) return;
    SyncNativeEntity(npc);
}

void SyncClientLeasedNpc(eCEntity* npc)
{
    if (net::IsHost() || ai::ShuttingDown() || !npclease::LocalAuthority(npc)) return;
    SyncNativeEntity(npc);
}

void ClearClientLeasedNpc(uint8_t const guid[16])
{
    if (net::IsHost() || !guid) return;
    GuidKey key{};
    std::memcpy(key.data(), guid, 16);
    PublishTransition(key, false, 0);
}

void OnState(uint8_t from, net::WorldState const& state)
{
    if (state.kind != WorldStateKind) return;
    uint8_t sender = 0;
    const bool active = state.npc != 0;
    if (net::IsHost()) {
        if (from < 2 || from >= net::MaxPlayers) return;
        if (active) {
            if (eCEntity* npc = npcs::NpcByGuid(state.guid); npc && npclease::RemoteOwner(npc) != from) return;
        }
        sender = from;
    } else {
        if (from != 1) return;
        sender = (state.guard >= 1 && state.guard < net::MaxPlayers) ? state.guard : uint8_t(1);
        if (sender == net::LocalId()) return;
    }
    GuidKey key{};
    std::memcpy(key.data(), state.guid, 16);
    if (active) {
        remoteHostile[key][sender] = true;
    } else {
        auto found = remoteHostile.find(key);
        if (found != remoteHostile.end()) {
            found->second[sender] = false;
            bool any = false;
            for (uint8_t id = 1; id < net::MaxPlayers; ++id) any = any || found->second[id];
            if (!any) remoteHostile.erase(found);
        }
    }
    if (net::IsHost()) {
        net::WorldState relayed = state;
        relayed.guard = from;
        net::SendWorldState(relayed, from);
        if (eCEntity* npc = npcs::NpcByGuid(state.guid)) SyncNativeEntity(npc);
    }
}

void OnWorldChange()
{
    promotedNpcs.clear();
    sentState.clear();
    remoteHostile.clear();
}

bool ResumeLeasedCombat(eCEntity* npc, uint8_t aiMode, uint8_t attitude, uint8_t attackReason, eCEntity* target)
{
    if (!npc || aiMode != gEAIMode_Combat || relation::BoutAttackReason(attackReason) || !target) return false;
    if (attitude == relation::Hostile) Entity(npc).NPC.AttitudeToPlayer2 = gEAttitude_Hostile;
    const int reason = attackReason ? attackReason : static_cast<int>(gEAttackReason_ReactToDamage);
    guards::Acting player(remote::IsPuppet(target) ? target : nullptr, false);
    GetScriptAdminExt().CallScript(bCString("AssessTarget"), npc, target, reason);
    return true;
}

bool IsSharedCombatHostile(eCEntity const* npc)
{
    return relation::ActiveSharedCombatHostility(BuildInput(npc));
}

bool ShouldPromoteAttitude(eCEntity const* npc)
{
    if (!ValidNpc(npc) || npcs::IsMirrored(npc)) return false;
    const relation::SharedCombatInput in = BuildInput(npc);
    return in.storedAttitude != relation::Predator && in.storedAttitude != relation::Panic &&
           relation::ActiveSharedCombatHostility(in);
}

bool MirrorSharesCombatHostility(eCEntity const* npc)
{
    if (!ValidNpc(npc) || !npcs::IsMirrored(npc)) return false;
    auto const* rec = npcs::CombatRecord(npc);
    const bool haveRecord = rec != nullptr;
    const uint8_t hostAttitude = haveRecord ? rec->attitude : relation::Unset;
    const bool inCombat = haveRecord && (rec->aiMode == gEAIMode_Combat || (rec->flags & 4u) != 0);
    return relation::MirrorSharesCombatHostility(true, true, haveRecord, hostAttitude, inCombat,
                                                 HasRemoteHostile(KeyOf(npc)));
}

uint8_t MirrorAttitude(eCEntity const* npc, uint8_t wanted)
{
    if (!ValidNpc(npc) || !npcs::IsMirrored(npc)) return wanted;
    if (wanted == relation::Hostile || wanted == relation::Predator || wanted == relation::Panic) return wanted;
    return IsSharedCombatHostile(npc) ? static_cast<uint8_t>(relation::Hostile) : wanted;
}

int EffectiveAttitude(eCEntity const* npc, int baseAttitude)
{
    if (!ValidNpc(npc)) return baseAttitude;
    return relation::EffectiveAttitude(baseAttitude, BuildInput(npc));
}

bool FocusPriorityHooked()
{
    return focusPriorityNative;
}

uint8_t CoopTargetPlayer(eCEntity* target)
{
    return CoopOwnerOf(target);
}

}  // namespace g3t::combat_hostility
