// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_hostility.h"

#include "ai.h"
#include "body.h"
#include "enclaves.h"
#include "fightmem.h"
#include "guards.h"
#include "guid.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "occupancy.h"
#include "patch.h"
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
constexpr uint8_t RemoteInactive = 0;
constexpr uint8_t RemoteNonLethal = 1;
constexpr uint8_t RemoteLethal = 2;
constexpr uintptr_t FocusPriorityRva = 0xAA4A0;

struct RemoteCombatEntry {
    uint8_t state = RemoteInactive;
    uint8_t attackReason = 0;
};

gFScript originalFocusPriority = nullptr;
bool focusPrioritySearched = false;
bool focusPriorityNative = false;

std::unordered_set<GuidKey, KeyHash> promotedNpcs;
std::unordered_map<GuidKey, uint8_t, KeyHash> sentState;
std::unordered_map<GuidKey, std::array<RemoteCombatEntry, net::MaxPlayers>, KeyHash> remoteHostile;

GuidKey KeyOf(uint8_t const guid[16])
{
    GuidKey key{};
    if (guid) std::memcpy(key.data(), guid, 16);
    return key;
}

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
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (id == LocalCoopId()) continue;
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
        if (found->second[id].state != RemoteInactive) return true;
    }
    return false;
}

bool HasRemoteLethal(GuidKey const& key)
{
    auto const found = remoteHostile.find(key);
    if (found == remoteHostile.end()) return false;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (found->second[id].state == RemoteLethal) return true;
    }
    return false;
}

int RemoteAttackReason(GuidKey const& key)
{
    auto const found = remoteHostile.find(key);
    if (found == remoteHostile.end()) return 0;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (found->second[id].state != RemoteInactive && found->second[id].attackReason != 0)
            return found->second[id].attackReason;
    }
    return 0;
}

eCEntity* ResolveCoopPlayerBody(eCEntity const* participant)
{
    const uint8_t owner = CoopOwnerOf(participant);
    if (owner == 0) return nullptr;
    if (owner == LocalCoopId()) {
        eCEntity* controlled = remote::ControlledBody();
        return controlled ? controlled : gCSession::GetInstance().GetHero();
    }
    return remote::PuppetOf(owner);
}

void ClearRemoteHostileOnHost(GuidKey const& key)
{
    auto const found = remoteHostile.find(key);
    if (found == remoteHostile.end()) return;
    if (net::Connected()) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            if (found->second[id].state == RemoteInactive) continue;
            net::WorldState clearMsg{};
            clearMsg.kind = WorldStateKind;
            std::memcpy(clearMsg.guid, key.data(), 16);
            clearMsg.npc = RemoteInactive;
            clearMsg.guard = id;
            net::SendWorldState(clearMsg, 0);
        }
    }
    remoteHostile.erase(found);
}

relation::SharedCombatInput BuildInput(eCEntity const* npc, int knownBaseAttitude = -1)
{
    relation::SharedCombatInput in{};
    if (!ValidNpc(npc)) return in;
    eCEntity* mutableNpc = const_cast<eCEntity*>(npc);
    Entity self(mutableNpc);
    in.alive = !self.IsDead();
    in.down = self.IsDown();
    in.role = RoleOf(npc);
    in.storedAttitude = static_cast<int>(static_cast<gEAttitude>(self.NPC.AttitudeToPlayer2));
    in.mirrored = npcs::IsMirrored(npc);
    const GuidKey key = KeyOf(npc);
    const bool remoteActive = HasRemoteHostile(key);
    const bool remoteLethal = HasRemoteLethal(key);
    const bool humanoid = relation::HumanoidCombatSpecies(
        static_cast<int>(static_cast<gESpecies>(self.NPC.Species)));
    in.lethal = !humanoid || remoteLethal;

    if (in.mirrored) {
        auto const* rec = npcs::CombatRecord(npc);
        if (rec) {
            in.aiMode = rec->aiMode;
            in.combatState = (rec->flags & 4u) ? 1u : 0u;
            in.attackReason = rec->attackReason ? rec->attackReason : RemoteAttackReason(key);
            if (!relation::NonCombatAiMode(in.aiMode)) {
                const uint8_t targetPlayer = (rec->flags >> 3) & 7u;
                if (targetPlayer != 0) {
                    in.targetsCoopSide = true;
                } else {
                    eCEntity* opponent = nullptr;
                    if (npcs::CombatTarget(mutableNpc, opponent) && IsCoopParticipant(opponent))
                        in.targetsCoopSide = true;
                }
            }
        } else {
            in.aiMode = body::AIModeOf(mutableNpc);
            in.attackReason = RemoteAttackReason(key);
        }
        const bool inCombat = !relation::NonCombatAiMode(in.aiMode) &&
                              (in.aiMode == gEAIMode_Combat || in.combatState != 0) &&
                              !relation::BoutAttackReason(in.attackReason);
        in.syncedSharedHostile = relation::MirrorSharesCombatHostility(
            true, true, rec != nullptr, rec ? rec->attitude : relation::Unset,
            inCombat, remoteActive, in.targetsCoopSide);
        const int hostAttitude = (rec && relation::ValidAttitude(rec->attitude))
                                     ? static_cast<int>(rec->attitude)
                                     : relation::None;
        in.lethal = relation::IsLethalSharedCombat(humanoid, false, in.attackReason, hostAttitude, remoteLethal);
        return in;
    }

    const bool remoteLeasedOnHost = net::IsHost() && npclease::RemoteOwner(npc) != 0;
    in.syncedSharedHostile = remoteActive;
    in.aiMode = static_cast<int>(static_cast<gEAIMode>(self.Routine.AIMode));
    in.combatState = static_cast<uint32_t>(self.NPC.CombatState);
    in.lastFight = static_cast<int>(static_cast<gELastFightAgainstPlayer>(self.NPC.LastFightAgainstPlayer));
    in.attackReason = remoteLeasedOnHost ? RemoteAttackReason(key)
                                         : static_cast<int>(static_cast<gEAttackReason>(self.NPC.AttackReason));
    in.clientLeasedEngagement = net::IsHost() && npclease::LeasedCombatHostile(npc);

    eCEntity* target = nullptr;
    eCEntity* alt = nullptr;
    eCEntity* attacker = nullptr;
    const bool active = !relation::NonCombatAiMode(in.aiMode) &&
                        (in.aiMode == gEAIMode_Combat || in.combatState != 0);
    if (active && !remoteLeasedOnHost) {
        target = self.NPC.GetCurrentTarget().GetInstance();
        alt = self.NPC.GetAlternativeTarget().GetInstance();
        attacker = self.NPC.GetCurrentAttacker().GetInstance();
        in.targetsCoopSide = IsCoopParticipant(target) || IsCoopParticipant(alt) ||
                             (!target && !alt && IsCoopParticipant(attacker));
    }

    if (relation::ActiveSharedCombatHostility(in)) {
        int baseAttitude = knownBaseAttitude;
        if (!relation::ValidAttitude(baseAttitude) || baseAttitude <= relation::None) {
            int truth = -1;
            if (ai::LocalAttitude(mutableNpc, truth) && relation::ValidAttitude(truth) && truth > relation::None)
                baseAttitude = truth;
            else
                baseAttitude = relation::None;
        }
        bool meansToKill = false;
        if (!remoteLeasedOnHost && humanoid && !remoteLethal &&
            !relation::LethalCombatAttackReason(in.attackReason) &&
            baseAttitude != relation::Hostile && baseAttitude != relation::Predator) {
            eCEntity* playerBody = ResolveCoopPlayerBody(target);
            if (!playerBody) playerBody = ResolveCoopPlayerBody(alt);
            if (!playerBody) playerBody = ResolveCoopPlayerBody(attacker);
            if (!playerBody) {
                eCEntity* controlled = remote::ControlledBody();
                playerBody = controlled ? controlled : gCSession::GetInstance().GetHero();
            }
            meansToKill = guards::MeansToKill(mutableNpc, playerBody);
        }
        in.lethal = relation::IsLethalSharedCombat(humanoid, meansToKill, in.attackReason, baseAttitude, remoteLethal);
    }
    return in;
}

void PublishTransition(GuidKey const& key, uint8_t next, int attackReason)
{
    if (!net::Connected()) return;
    auto const prev = sentState.find(key);
    if (prev == sentState.end() && next == RemoteInactive) return;
    if (prev != sentState.end() && prev->second == next) return;
    sentState[key] = next;

    net::WorldState s{};
    s.kind = WorldStateKind;
    std::memcpy(s.guid, key.data(), 16);
    s.npc = next;
    s.guard = net::LocalId();
    s.npcClass = next != RemoteInactive ? static_cast<uint8_t>(attackReason & 0xFF) : 0;
    net::SendWorldState(s, 0);
}

void RestorePromotedEntityAttitude(eCEntity* npc, GuidKey const& key)
{
    if (promotedNpcs.erase(key) == 0 || !ValidNpc(npc)) return;
    Entity self(npc);
    if (self.IsDead()) return;
    int truth = -1;
    if (ai::LocalAttitude(npc, truth) && relation::ValidAttitude(truth) && truth > relation::None)
        self.NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(truth);
}

void SyncNativeEntity(eCEntity* npc)
{
    if (!ValidNpc(npc) || npcs::IsMirrored(npc)) return;
    if (npc == gCSession::GetInstance().GetHero() || npc == remote::ControlledBody() || remote::IsPuppet(npc))
        return;

    const GuidKey key = KeyOf(npc);
    const bool localSimAuthority = (net::IsHost() && !npclease::RemoteOwner(npc)) || npclease::LocalAuthority(npc);
    if (net::IsHost() && !npclease::RemoteOwner(npc) && HasRemoteHostile(key)) {
        ClearRemoteHostileOnHost(key);
    } else if (!net::IsHost() && npclease::LocalAuthority(npc)) {
        remoteHostile.erase(key);
    }
    const relation::SharedCombatInput in = BuildInput(npc);
    const bool hostileNow = relation::ActiveSharedCombatHostility(in);

    if (hostileNow) {
        int truth = -1;
        const bool haveTruth = ai::LocalAttitude(npc, truth) && relation::ValidAttitude(truth) &&
                               truth > relation::None;
        const int base = haveTruth ? truth : in.storedAttitude;
        const int targetAttitude = relation::EffectiveAttitude(base, in);
        if (!haveTruth || targetAttitude != truth)
            promotedNpcs.insert(key);
        if (in.storedAttitude != targetAttitude && relation::ValidAttitude(targetAttitude) &&
            targetAttitude > relation::None) {
            Entity(npc).NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(targetAttitude);
        }
        if (localSimAuthority)
            PublishTransition(key, in.lethal ? RemoteLethal : RemoteNonLethal, in.attackReason);
        return;
    }

    RestorePromotedEntityAttitude(npc, key);
    if (localSimAuthority)
        PublishTransition(key, RemoteInactive, 0);
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
        fi.inCombat = !relation::NonCombatAiMode(sc.aiMode) &&
                      (sc.aiMode == gEAIMode_Combat || sc.combatState != 0);
        fi.inBout = relation::BoutAttackReason(sc.attackReason);
        fi.role = sc.role;
        fi.attitude = sc.storedAttitude;
        const int overridePriority = relation::CombatFocusPriorityOverride(fi);
        if (overridePriority >= 0) return overridePriority;
    }
    // Script_Game.dll GetFocusEntityPriority (0xAA4A0) rejects world interactables whose Interaction.User
    // is still populated; scrub any stale user first so the native script evaluates true usability.
    occupancy::ClearStaleUser(candidate, "focus");
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
    if (code && reinterpret_cast<void*>(native) == code &&
        (!std::memcmp(code, head, sizeof(head)) || patch::IsPatchedByUs(code, head, 6))) {
        if (!std::memcmp(code, head, sizeof(head))) {
            if (void* trampoline = ai::JumpTo(code, reinterpret_cast<void*>(&FilterFocusEntityPriority))) {
                originalFocusPriority = reinterpret_cast<gFScript>(trampoline);
                focusPriorityNative = true;
            }
        } else {
            focusPriorityNative = true;
        }
    }
    script->m_funcScript = &FilterFocusEntityPriority;
    ai::TrackPointer(reinterpret_cast<void*>(&script->m_funcScript), reinterpret_cast<void*>(native),
                     reinterpret_cast<void*>(&FilterFocusEntityPriority), "combat_hostility GetFocusEntityPriority");
    Logf("relation: GetFocusEntityPriority wrapped (%s)", focusPriorityNative ? "native jump and script table" : "script table only");
}

void ExpireCalmMirroredHostilityOnClient()
{
    if (net::IsHost() || !net::Connected()) return;
    for (auto& [key, peers] : remoteHostile) {
        if (peers[1].state == RemoteInactive) continue;
        eCEntity* npc = npcs::NpcByGuid(key.data());
        if (!npc || !npcs::IsMirrored(npc)) continue;
        auto const* rec = npcs::CombatRecord(npc);
        if (!rec) continue;
        const bool inCombat = !relation::NonCombatAiMode(rec->aiMode) &&
                              (rec->aiMode == gEAIMode_Combat || (rec->flags & 4u) != 0) &&
                              !relation::BoutAttackReason(rec->attackReason);
        if (!relation::MirrorSharesCombatHostility(true, true, true, rec->attitude, inCombat, true, false) &&
            !inCombat)
            peers[1] = {};
    }
}

void PruneDisconnectedPeers()
{
    if (!net::Connected()) {
        remoteHostile.clear();
        sentState.clear();
        return;
    }
    ExpireCalmMirroredHostilityOnClient();
    for (auto it = remoteHostile.begin(); it != remoteHostile.end();) {
        bool any = net::IsHost() ? false : (it->second[1].state != RemoteInactive);
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            const bool alive = net::IsHost() ? net::PeerJoined(id)
                                             : (id != net::LocalId() && net::Remotes()[id].connected);
            if (it->second[id].state != RemoteInactive && !alive) it->second[id] = {};
            any = any || (it->second[id].state != RemoteInactive);
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

void OnClientLeaseConfirmed(uint8_t const guid[16])
{
    if (net::IsHost() || !guid || ai::ShuttingDown()) return;
    const GuidKey key = KeyOf(guid);
    sentState.erase(key);
    remoteHostile.erase(key);
    if (eCEntity* npc = npcs::NpcByGuid(guid))
        SyncClientLeasedNpc(npc);
}

void ClearClientLeasedNpc(uint8_t const guid[16])
{
    if (net::IsHost() || !guid) return;
    const GuidKey key = KeyOf(guid);
    remoteHostile.erase(key);
    RestorePromotedEntityAttitude(npcs::NpcByGuid(guid), key);
    PublishTransition(key, RemoteInactive, 0);
}

void NoteImportedAttitude(eCEntity* npc, uint8_t importedAttitude)
{
    if (!ValidNpc(npc)) return;
    const GuidKey key = KeyOf(npc);
    if ((importedAttitude != relation::Hostile && importedAttitude != relation::Angry) ||
        Entity(npc).IsDead()) {
        promotedNpcs.erase(key);
        return;
    }
    int truth = -1;
    if (ai::LocalAttitude(npc, truth) && relation::ValidAttitude(truth) &&
        truth > relation::None && truth != importedAttitude && truth != relation::Hostile &&
        truth != relation::Predator && truth != relation::Panic) {
        promotedNpcs.insert(key);
    }
}

void OnState(uint8_t from, net::WorldState const& state)
{
    if (state.kind != WorldStateKind) return;
    uint8_t sender = 0;
    const bool active = state.npc != RemoteInactive;
    const GuidKey key = KeyOf(state.guid);
    if (net::IsHost()) {
        if (from < 2 || from >= net::MaxPlayers) return;
        if (active) {
            if (npclease::RemoteOwner(state.guid) != from) return;
        } else {
            auto const existing = remoteHostile.find(key);
            if (existing == remoteHostile.end() || existing->second[from].state == RemoteInactive) return;
        }
        sender = from;
    } else {
        if (from != 1) return;
        sender = (state.guard >= 1 && state.guard < net::MaxPlayers) ? state.guard : uint8_t(1);
        if (sender == net::LocalId()) return;
    }
    if (active) {
        const uint8_t level = (state.npc >= RemoteLethal ||
                               relation::LethalCombatAttackReason(state.npcClass))
                                  ? RemoteLethal
                                  : RemoteNonLethal;
        remoteHostile[key][sender] = {level, state.npcClass};
    } else {
        auto found = remoteHostile.find(key);
        if (found != remoteHostile.end()) {
            found->second[sender] = {};
            bool any = false;
            for (uint8_t id = 1; id < net::MaxPlayers; ++id)
                any = any || (found->second[id].state != RemoteInactive);
            if (!any) remoteHostile.erase(found);
        }
    }
    if (net::IsHost()) {
        net::WorldState relayed = state;
        relayed.guard = from;
        net::SendWorldState(relayed, from);
        if (eCEntity* npc = npcs::NpcByGuid(state.guid))
            SyncNativeEntity(npc);
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
    if (attitude == relation::Hostile || attitude == relation::Angry) {
        NoteImportedAttitude(npc, attitude);
        Entity(npc).NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(attitude);
    }
    const int reason = attackReason ? attackReason : static_cast<int>(gEAttackReason_ReactToDamage);
    guards::Acting player(remote::IsPuppet(target) ? target : nullptr, false);
    GetScriptAdminExt().CallScript(bCString("AssessTarget"), npc, target, reason);
    SyncNativeEntity(npc);
    return true;
}

bool IsCoopParticipant(eCEntity const* entity)
{
    return CoopOwnerOf(entity) != 0;
}

bool IsSharedCombatHostile(eCEntity const* npc)
{
    return relation::ActiveSharedCombatHostility(BuildInput(npc));
}

bool MirrorSharesCombatHostility(eCEntity const* npc)
{
    if (!ValidNpc(npc) || !npcs::IsMirrored(npc)) return false;
    const relation::SharedCombatInput in = BuildInput(npc);
    return in.mirrored && relation::ActiveSharedCombatHostility(in);
}

uint8_t MirrorAttitude(eCEntity const* npc, uint8_t wanted)
{
    if (!ValidNpc(npc) || !npcs::IsMirrored(npc)) return wanted;
    eCEntity* mutableNpc = const_cast<eCEntity*>(npc);
    const relation::SharedCombatInput in = BuildInput(npc);
    const bool sharedHostile = relation::ActiveSharedCombatHostility(in);
    const bool useLocalRelation = !relation::MirrorUsesHostAttitude(
        enclaves::Together(), npcs::CompanionOwner(mutableNpc),
        fightmem::HasLocalOverride(mutableNpc), sharedHostile);
    int local = -1;
    const bool haveLocal = useLocalRelation && ai::LocalAttitude(mutableNpc, local) &&
                           relation::ValidAttitude(local) && local > relation::None;
    if (!sharedHostile) {
        return haveLocal ? static_cast<uint8_t>(local) : wanted;
    }
    auto const* rec = npcs::CombatRecord(npc);
    int base = (rec && relation::ValidAttitude(rec->attitude) && rec->attitude > relation::None)
                   ? static_cast<int>(rec->attitude)
                   : static_cast<int>(wanted);
    if (wanted == relation::Predator || wanted == relation::Panic)
        base = wanted;
    if (haveLocal && (local == relation::Hostile || local == relation::Predator))
        base = local;
    return static_cast<uint8_t>(relation::EffectiveAttitude(base, in));
}

int EffectiveAttitude(eCEntity const* npc, int baseAttitude)
{
    if (!ValidNpc(npc)) return baseAttitude;
    const relation::SharedCombatInput in = BuildInput(npc, baseAttitude);
    const int effective = relation::EffectiveAttitude(baseAttitude, in);
    if (!in.mirrored && effective != baseAttitude && !Entity(const_cast<eCEntity*>(npc)).IsDead())
        promotedNpcs.insert(KeyOf(npc));
    return effective;
}

bool FocusPriorityHooked()
{
    return focusPriorityNative;
}

uint8_t CoopTargetPlayer(eCEntity* target)
{
    if (!net::IsHost() && !net::LocalId()) return 0;
    return CoopOwnerOf(target);
}

}  // namespace g3t::combat_hostility
