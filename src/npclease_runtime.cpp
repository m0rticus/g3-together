// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_runtime.h"
#include "npclease_client.h"
#include "npclease_save.h"
#include "npclease_guards.h"
#include "npclease_presentation.h"
#include "npclease_admission.h"
#include "npcown_policy.h"

#include "ai.h"
#include "body.h"
#include "config.h"
#include "guid.h"
#include "journal.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "npcremoval.h"
#include "persist.h"
#include "remote.h"
#include "talk.h"
#include "world.h"
#include "crashlog.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace g3t::npclease {
namespace {
struct Entry {
    Lease lease;
    Eligibility interest;
    ClientWindow window;
    uint32_t lastRequest = 0, lastState = 0, lastControl = 0, localDeadline = 0;
    uint32_t echo = 0, welcome = 0, world = 0, recallAt = 0;
    uint32_t previousGeneration = 0;
    uint32_t appliedRevision = 0;
    uint32_t presentationFloor = 0;
    uint32_t unavailableAt = 0;
    uint32_t lastHit = 0;
    uint32_t activationAt = 0, ownerReportAt = 0, rangeChangedAt = 0, processableChangedAt = 0;
    eCEntity* observedEntity = nullptr;
    bool observedRange = false, observedProcessable = false;
    bool nativeActivated = false;
    bool hasPresentationFloor = false;
    uint8_t previousOwner = 0;
    eCEntity* applied = nullptr;
    bool active = false, prepared = false, cut = false, committed = false, hasBaseline = false;
    bool captureFailed = false;
    bool adopted = false;
    // Client-first ownership: `local` = this client drives the NPC natively right now (host handshake independent);
    // `confirmed` = the host granted an epoch for it (Commit seen).
    bool local = false, confirmed = false;
    uint32_t ownedAt = 0, yieldedAt = 0, cutAt = 0, hostSeenAt = 0, requestEvery = 500, retryAt = 0;
    char const* cutReason = nullptr;
    eCEntity* ownedEntity = nullptr;
    bool silentLogged = false;
    char lastTask[48]{};
    uint32_t reactionAt = 0;
    char sentRoutine[64]{};
};
using GuidKey = std::array<uint8_t, 16>;
struct GuidHash {
    size_t operator()(GuidKey const& key) const { return net::HashGuid(key.data()); }
};
std::unordered_map<GuidKey, Entry, GuidHash> entries;
GuardRegistry guardStates;
uint32_t authoritySerial = 0, scanAt = 0, reportAt = 0, seenWorld = 0, seenWelcome = 0;
unsigned granted = 0, recalled = 0, recovered = 0, refused = 0, stateSent = 0, stateReceived = 0;
unsigned contactsRefused = 0, nativeDeaths = 0, repeatedDeaths = 0;
unsigned discovered = 0;
unsigned scanSamples = 0, scanTotalMs = 0, scanMaxMs = 0;
bool installed = false, installFailed = false;
gFScript originalKill = nullptr;
using QuestFn = GEBool(__fastcall*)(gCQuestManager_PS*, void*, eCEntity const&);
QuestFn originalQuest = nullptr;
thread_local eCEntity* ownerDeath = nullptr;
// Per-GUID refusal/waiting reasons: logged once per GUID and reason, counted once; the 10 Hz scan only re-asserts them.
npcown::RefusalLedger refusals, waiting;
npcown::TransitionLog transitions;
npcown::OwnStats ownTotals;
uint32_t sessionReadyAt = 0, ownReportAt = 0;

GuidKey Key(uint8_t const guid[16])
{
    GuidKey result;
    std::memcpy(result.data(), guid, result.size());
    return result;
}
GuidKey Key(eCEntity const* entity)
{
    uint8_t identity[16];
    guid::CopyEntity(const_cast<eCEntity*>(entity), identity);
    return Key(identity);
}
Entry* Find(eCEntity const* entity)
{
    if (entries.empty() || !entity || !entity->GetPropertySet(eEPropertySetType_NPC)) return nullptr;
    const auto it = entries.find(Key(entity));
    return it == entries.end() ? nullptr : &it->second;
}
bool Player(eCEntity const* entity)
{
    return entity && (entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody() ||
                      remote::IsPuppet(entity) || entity->GetPropertySet(eEPropertySetType_PlayerMemory));
}
bool HostWins(GuidKey const& key)
{
    return npcs::HostStreamClaimed(key.data()) && npcs::HostStreamRecords(key.data()) >= 2;
}
bool CapacityAvailable()
{
    if (entries.size() < MaxLeases) return true;
    ++refused;
    static uint32_t last = 0;
    const auto now = net::Now();
    if (!last || now - last >= 5000) {
        Logf("FAIL npclease: canonical cache full (%u); untracked wildlife remains host-owned", MaxLeases);
        last = now ? now : 1;
    }
    return false;
}
bool Running()
{
    return !ai::ShuttingDown() && !persist::NativeLoading() && !persist::HeroPending() &&
           gCSession::GetInstance().IsGameRunning() && !gCSession::GetInstance().IsPaused() &&
           remote::ControlledBody() && net::Connected();
}
void PublishGuard(GuidKey const& key, Entry const& entry)
{
    if (!entry.local && (!entry.lease.epoch || entry.lease.phase == Phase::Host)) {
        guardStates.Erase(key);
        return;
    }
    AuthorityGuard guard;
    guard.leased = true;
    // A client-first owner is independent of the host round trip, the lease window, pauses and checkpoints: only the world
    // identity matters. Pausing or saving must never make an owned NPC passive (the passive path clears its task).
    guard.processingAllowed = !net::IsHost() && entry.local && entry.active && entry.world == net::WorldEpoch() &&
        entry.welcome == net::WelcomeSerial() && !ai::ShuttingDown();
    guard.persistent = entry.local;
    guard.locallyDriven = guard.processingAllowed && entry.nativeActivated;
    guard.deadline = entry.window.deadline;
    guard.owner = net::IsHost() && entry.lease.ActiveLeases() ? entry.lease.owner : 0;
    guardStates.Put(key, guard);
}
AuthorityGuard ReadGuard(eCEntity const* entity)
{
    if (!entity) return {};
    return guardStates.Get(Key(entity));
}
char const* SpeciesName(Entity& npc)
{
    switch (static_cast<gESpecies>(npc.NPC.Species)) {
    case gESpecies_Wolf: return "Wolf";
    case gESpecies_Boar: return "Boar";
    case gESpecies_Deer: return "Deer";
    case gESpecies_Scavenger: return "Scavenger";
    case gESpecies_Golem: return "Golem";
    case gESpecies_Demon: return "Demon";
    case gESpecies_Troll: return "Troll";
    case gESpecies_Minecrawler: return "Minecrawler";
    case gESpecies_Sabertooth: return "Sabertooth";
    case gESpecies_Shadowbeast: return "Shadowbeast";
    case gESpecies_Bison: return "Bison";
    case gESpecies_Rhino: return "Rhino";
    case gESpecies_Ripper: return "Ripper";
    case gESpecies_Lurker: return "Lurker";
    case gESpecies_Varan: return "Varan";
    case gESpecies_Snapper: return "Snapper";
    case gESpecies_Alligator: return "Alligator";
    case gESpecies_Trex: return "Trex";
    case gESpecies_FireVaran: return "FireVaran";
    case gESpecies_Bloodfly: return "Bloodfly";
    case gESpecies_Lizard: return "Lizard";
    case gESpecies_Meatbug: return "Meatbug";
    case gESpecies_Rabbit: return "Rabbit";
    case gESpecies_Molerat: return "Molerat";
    case gESpecies_Snake: return "Snake";
    case gESpecies_Spider: return "Spider";
    case gESpecies_Turtle: return "Turtle";
    case gESpecies_Gargoyle: return "Gargoyle";
    case gESpecies_SwampLurker: return "SwampLurker";
    case gESpecies_Ogre: return "Ogre";
    case gESpecies_Scorpion: return "Scorpion";
    case gESpecies_FireGolem: return "FireGolem";
    case gESpecies_IceGolem: return "IceGolem";
    case gESpecies_Stalker: return "Stalker";
    case gESpecies_Chicken: return "Chicken";
    case gESpecies_Pig: return "Pig";
    case gESpecies_Cow: return "Cow";
    default: return nullptr;
    }
}
NativeClass ClassOf(Entity& npc)
{
    return SpeciesName(npc) ? NativeClass::Creature : NativeClass::Ordinary;
}

// Summons, party members and owned actors follow a player and stay with that player's machine; trade/teach/dialogue/quest
// actors, immortals, named actors and equipped NPCs are ordinary client citizens.
bool FollowerRole(eCEntity* entity)
{
    if (entity->GetPropertySet(eEPropertySetType_Interaction) &&
        Entity(entity).Interaction.GetOwner().GetInstance()) return true;
    auto* party = static_cast<gCParty_PS*>(entity->GetPropertySet(eEPropertySetType_Party));
    if (!party) return false;
    Entity npc(entity);
    return npc.Party.GetPartyLeader().GetInstance() || npc.Party.Waiting ||
        static_cast<gEPartyMemberType>(npc.Party.PartyMemberType) != gEPartyMemberType_None ||
        party->GetMembers().GetCount() != 0;
}

npcown::RoleFacts RoleOf(eCEntity* entity, bool prototype)
{
    npcown::RoleFacts role;
    role.removed = npcremoval::Suppressed(entity);
    if (!prototype) {
        role.hero = entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody() ||
            entity->GetPropertySet(eEPropertySetType_PlayerMemory);
        role.remotePuppet = remote::IsPuppet(entity);
        role.duplicateIdentity = npcs::RedundantClientEntity(entity);
    }
    role.complete = entity->GetPropertySet(eEPropertySetType_NPC) &&
        entity->GetPropertySet(eEPropertySetType_DamageReceiver) &&
        entity->GetPropertySet(eEPropertySetType_ScriptRoutine) &&
        entity->GetPropertySet(eEPropertySetType_CharacterMovement) &&
        entity->GetPropertySet(eEPropertySetType_Inventory) &&
        entity->GetPropertySet(eEPropertySetType_Navigation) && (prototype || entity->GetCreator() != nullptr);
    role.itemOrSpell = entity->GetPropertySet(eEPropertySetType_Magic) || entity->GetPropertySet(eEPropertySetType_Item);
    role.summonOrFollower = role.complete && FollowerRole(entity);
    return role;
}

char const* EligibilityReason(eCEntity* entity, char const* = nullptr, bool prototype = false)
{
    if (!entity) return "native NPC instance unavailable";
    return npcown::Exclusion(RoleOf(entity, prototype));
}
bool Eligible(eCEntity* entity)
{
    return !EligibilityReason(entity);
}
char const* DiscoveryReason(Checkpoint const& state)
{
    if (!Valid(state)) return "invalid discovery checkpoint";
    _GUID raw;
    std::memcpy(&raw, state.templateId, sizeof(raw));
    eCTemplateEntityProxy proxy;
    proxy.SetEntity(guid::Property(raw));
    auto* prototype = proxy.GetEntity();
    if (!prototype || !prototype->GetPropertySet(eEPropertySetType_NPC)) return "native discovery template unavailable";
    return EligibilityReason(prototype, state.name, true);
}
bool ValidDiscovery(Checkpoint const& state)
{
    return !DiscoveryReason(state);
}
NativeClass ClassOf(Checkpoint const& state)
{
    _GUID raw;
    std::memcpy(&raw, state.templateId, sizeof(raw));
    eCTemplateEntityProxy proxy;
    proxy.SetEntity(guid::Property(raw));
    auto* prototype = proxy.GetEntity();
    if (!prototype || !prototype->GetPropertySet(eEPropertySetType_NPC)) return NativeClass::Unsupported;
    Entity npc(prototype);
    return ClassOf(npc);
}
bool Processed(eCEntity const* entity)
{
    if (!entity || !entity->IsEnabled() || entity->IsProcessingDisabled()) return false;
    // GetNPCs is the frame-cached interaction ROI, not the native processing inventory.
    const auto* all = world::ProcessedEntities();
    if (!all) return false;
    for (GEU16 i = 0; i < all->GetCount(); ++i) if (all->GetAt(i) == entity) return true;
    return false;
}
bool NativeReady(eCEntity* entity)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    return Processed(entity) && world::ProcessingOf(entity).processable && animation && animation->GetActor();
}
ClientReadiness Readiness(eCEntity* entity, Entry const&, uint32_t)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    ClientReadiness result{Running(), GetConfig().npcAuthorityFallback, persist::CheckpointBusy(),
        Processed(entity), animation && animation->GetActor(), Eligible(entity)};
    result.processingEnabled = entity && entity->IsEnabled() && !entity->IsProcessingDisabled();
    result.processable = world::ProcessingOf(entity).processable;
    auto* hero = remote::ControlledBody();
    if (!net::IsHost()) {
        result.insideRange = entity && hero &&
            LeaseProcessingRange(true, (entity->GetWorldPosition() - hero->GetWorldPosition()).GetMagnitude());
    }
    return result;
}
bool CanResumePresentation(eCEntity* entity)
{
    std::vector<pose::Bone> bones;
    uint8_t flags = 0;
    return NativeReady(entity) && npcplayback::CapturePose(entity, bones, flags, nullptr);
}
bool TransferableStatus(Checkpoint const& state)
{
    constexpr uint32_t supported = gEStatusEffect_Burning | gEStatusEffect_Poisoned | gEStatusEffect_Diseased;
    return state.dead || (state.status & ~supported) == 0;
}
uint8_t TargetPlayer(eCEntity* target)
{
    if (!target) return 0;
    if (target == remote::ControlledBody() || target == gCSession::GetInstance().GetHero()) return net::LocalId();
    return static_cast<uint8_t>(remote::PuppetOwner(target));
}
bool CaptureNative(eCEntity* entity, Checkpoint& state)
{
    if (!Eligible(entity)) return false;
    Entity npc(entity);
    const auto& id = entity->GetTemplateID().GetCore();
    std::memcpy(state.templateId, &id, 16);
    const auto& p = entity->GetWorldPosition();
    state.position[0] = p.GetX(); state.position[1] = p.GetY(); state.position[2] = p.GetZ();
    const auto q = entity->GetWorldRotation();
    state.rotation[0] = q.GetX(); state.rotation[1] = q.GetY(); state.rotation[2] = q.GetZ(); state.rotation[3] = q.GetW();
    state.hp = npc.DamageReceiver.HitPoints; state.maxHp = npc.DamageReceiver.HitPointsMax;
    state.mana = npc.DamageReceiver.ManaPoints; state.maxMana = npc.DamageReceiver.ManaPointsMax;
    state.stamina = npc.DamageReceiver.StaminaPoints; state.maxStamina = npc.DamageReceiver.StaminaPointsMax;
    state.level = npc.NPC.Level;
    state.dead = npc.IsDead() ? 1 : 0;
    state.defeated = npc.NPC.DefeatedByPlayer ? 1 : 0;
    state.attitude = static_cast<uint8_t>(static_cast<gEAttitude>(npc.NPC.AttitudeToPlayer2));
    state.aiMode = static_cast<uint8_t>(static_cast<gEAIMode>(npc.Routine.AIMode));
    state.action = static_cast<uint8_t>(static_cast<gEAction>(npc.Routine.Action));
    state.attackReason = static_cast<uint8_t>(static_cast<gEAttackReason>(npc.NPC.AttackReason));
    state.targetPlayer = TargetPlayer(npc.NPC.GetCurrentTarget().GetInstance());
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    strncpy_s(state.name, npc.GetName().GetText(), _TRUNCATE);
    strncpy_s(state.routine, static_cast<bCString const&>(routine->GetRoutine()).GetText(), _TRUNCATE);
    npcs::LeaseBaseline(entity, state.routine, sizeof(state.routine), state.defeated);
    auto* properties = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
    state.status = properties->GetStatusEffects();
    const uint32_t seconds = Entity::GetTimeStampInSeconds();
    const uint32_t stamps[] = {properties->GetTimeStampFrozen(), properties->GetTimeStampBurning(),
        properties->GetTimeStampPoisoned(), properties->GetTimeStampDiseased(), properties->GetTimeStampTransformed()};
    for (size_t i = 0; i < 5; ++i) state.statusAge[i] = seconds - stamps[i];
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    state.busy = static_cast<uint8_t>(npc.IsDown() || npc.Routine.AIMode == gEAIMode_Combat ||
        npc.Routine.AIMode == gEAIMode_Talk || body::LocallyAttacking(entity) ||
        (target && !TargetPlayer(target)) || talk::IsHeld(entity) || state.status != 0);
    if (state.dead) {
        state.hp = 0;
        auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
        state.busy = animation && animation->GetRagDollEnabled() && animation->GetRagDollController();
    }
    return Valid(state);
}
void Stop(eCEntity* entity)
{
    if (!entity || Player(entity) || !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;
    npcplayback::RememberNativePresentation(entity);
    Entity npc(entity);
    npc.Routine.FullStop();
    npc.Routine.StopAICombatMove();
    npc.CharacterMovement.StopMovement();
    Entity none;
    npc.NPC.SetCurrentTarget(none);
    npc.NPC.SetAlternativeTarget(none);
    npc.NPC.SetCurrentAttacker(none);
    npc.NPC.SetLastAttacker(none);
    npc.NPC.SetSpellTarget(none);
    npc.SetAlignmentTarget(none);
    npc.DamageReceiver.SetLastInflictor(none);
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    routine->AccessCurrentTask() = bCString();
    routine->AccessCurrentState() = bCString();
    routine->GetSPU().SetOtherEntity(nullptr);
}
bool Import(eCEntity* entity, Checkpoint const& state)
{
    if (!Eligible(entity) || !Valid(state) ||
        std::memcmp(&entity->GetTemplateID().GetCore(), state.templateId, 16)) return false;
    Entity npc(entity);
    const bool wasDead = npc.IsDead();
    Stop(entity);
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    static_cast<bCString&>(routine->AccessRoutine()) = bCString(state.routine);
    const bCMatrix pose = body::Pose(*entity, state.position, state.rotation);
    entity->SetToWorldMatrix(pose);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody))) rigid->SetPose(pose);
    npc.DamageReceiver.HitPointsMax = state.maxHp; npc.DamageReceiver.HitPoints = state.hp;
    npc.DamageReceiver.ManaPointsMax = state.maxMana; npc.DamageReceiver.ManaPoints = state.mana;
    npc.DamageReceiver.StaminaPointsMax = state.maxStamina; npc.DamageReceiver.StaminaPoints = state.stamina;
    npc.NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(state.attitude);
    npc.NPC.Level = state.level;
    npc.NPC.AttackReason = static_cast<gEAttackReason>(state.attackReason);
    npc.NPC.DefeatedByPlayer = state.defeated != 0;
    npc.NPC.CombatState = 0;
    npc.Routine.AIMode = state.dead ? gEAIMode_Dead : gEAIMode_Abandoned;
    npc.Routine.Action = static_cast<gEAction>(state.action);
    auto* properties = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
    properties->AccessStatusEffects() = state.status;
    const uint32_t seconds = Entity::GetTimeStampInSeconds();
    properties->AccessTimeStampFrozen() = seconds - state.statusAge[0];
    properties->AccessTimeStampBurning() = seconds - state.statusAge[1];
    properties->AccessTimeStampPoisoned() = seconds - state.statusAge[2];
    properties->AccessTimeStampDiseased() = seconds - state.statusAge[3];
    properties->AccessTimeStampTransformed() = seconds - state.statusAge[4];
    if (state.dead && !wasDead) npcs::LayDownCorpse(entity);
    world::RequestProcessingUpdate();
    return true;
}
bool Activate(eCEntity* entity, Entry& entry, uint8_t const identity[16])
{
    const auto resume = entry.lease.phase;
    const bool active = entry.active;
    entry.active = false;
    entry.nativeActivated = false;
    entry.lease.phase = net::IsHost() ? Phase::Dormant : Phase::Frozen;
    PublishGuard(Key(identity), entry);
    if (!NativeReady(entity) || !npcplayback::GuardForLease(entity)) return false;
    npcs::LeaseReleaseMirror(identity);
    if (!Import(entity, entry.lease.state)) return false;
    entry.lease.phase = resume;
    entry.active = active;
    entry.nativeActivated = true;
    PublishGuard(Key(identity), entry);
    if (!npcplayback::ReleaseNativePresentation(entity)) {
        entry.nativeActivated = false;
        PublishGuard(Key(identity), entry);
        return false;
    }
    Entity npc(entity);
    if (entry.lease.state.dead) {
        npc.Routine.SetTask(bCString("ZS_Dead"));
        return true;
    }
    npc.Routine.AIMode = gEAIMode_Abandoned;
    if (entry.lease.state.targetPlayer) {
        auto* target = entry.lease.state.targetPlayer == net::LocalId() ? remote::ControlledBody() :
            remote::PuppetOf(entry.lease.state.targetPlayer);
        if (target) {
            npc.NPC.SetCurrentTarget(Entity(target));
            if (remote::IsPuppet(target)) remote::NoteContact(entity, target);
        }
    }
    npc.Routine.ContinueRoutine();
    return true;
}
Interest InterestOf(Checkpoint const& state, uint8_t client, eCEntity* hostNpc)
{
    Interest value;
    value.hostPresent = NativeReady(hostNpc);
    value.eligible = GetConfig().npcAuthorityFallback && installed && !installFailed;
    value.sameWorld = client > 1 && client < net::MaxPlayers && net::InWorld(client) &&
        net::Remotes()[client].latest.world == remote::CurrentWorld();
    value.ready = value.sameWorld && (net::Remotes()[client].latest.flags & 1) &&
        net::RemoteStateAgeMs(client) < 1000 && !persist::CheckpointBusy();
    value.hostRoi = GetConfig().hostRoi;
    const auto& camera = eCApplication::GetInstance().GetCurrentCamera().GetPosition();
    const auto& hero = remote::ControlledBody()->GetWorldPosition();
    const float atHero[3] = {hero.GetX(), hero.GetY(), hero.GetZ()};
    const float atCamera[3] = {camera.GetX(), camera.GetY(), camera.GetZ()};
    value.hostDistance = std::min(body::Distance(state.position, atHero), body::Distance(state.position, atCamera));
    value.clientDistance = value.sameWorld ? body::Distance(state.position, net::Remotes()[client].latest.position) : 1e30f;
    // One owner per NPC: refuse only when a LOWER-numbered client is also within reach of this NPC (it owns it).
    unsigned others = 0;
    for (uint8_t i = 2; i < client; ++i) {
        if (!net::InWorld(i) || !net::Remotes()[i].connected) continue;
        auto const& other = net::Remotes()[i].latest;
        if (other.world == remote::CurrentWorld() && body::Distance(state.position, other.position) <= ClientRadiusCm) ++others;
    }
    value.conflict = others != 0;
    return value;
}
Message Control(GuidKey const& key, Entry const& entry, Kind kind)
{
    Message note;
    note.kind = kind; note.world = entry.world; note.epoch = entry.lease.epoch;
    note.owner = entry.lease.owner; note.generation = entry.lease.generation;
    note.incarnation = entry.lease.incarnation; note.revision = entry.lease.revision;
    note.echo = entry.echo; note.state = entry.lease.state;
    if (kind == Kind::Commit && entry.lease.owner == 1) note.echo = net::Now();
    std::memcpy(note.guid, key.data(), 16);
    return note;
}
bool CurrentPeer(uint8_t player, uint32_t generation)
{
    return PeerCurrent(net::PeerJoined(player), net::PeerGeneration(player), generation);
}
bool Send(GuidKey const& key, Entry& entry, Kind kind)
{
    if (net::IsHost() && !CurrentPeer(entry.lease.owner, entry.lease.generation)) return false;
    if (kind == Kind::State && std::strcmp(entry.sentRoutine, entry.lease.state.routine))
        kind = Kind::StateFull;
    const auto note = Control(key, entry, kind);
    if (!net::SendNpcLease(net::IsHost() ? entry.lease.owner : 1, note)) return false;
    if (kind == Kind::Offer || kind == Kind::StateFull || kind == Kind::Cut)
        std::memcpy(entry.sentRoutine, entry.lease.state.routine, sizeof(entry.sentRoutine));
    entry.lastControl = net::Now();
    return true;
}
void Diagnostic(char const* action, GuidKey const& key, Entry const& entry, char const* why)
{
    char text[40];
    guid::Format(reinterpret_cast<uint8_t const*>(key.data()), text, sizeof(text));
    const uint32_t began = entry.recallAt ? entry.recallAt : entry.lease.started;
    Logf("npclease: %s guid=%s name=%s class=%s owner=%u epoch=%u life=%u revision=%u reason=%s duration_ms=%u",
         action, text, entry.lease.state.name,
         ClassOf(entry.lease.state) == NativeClass::Ordinary ? "ordinary_discovered" : "generic_creature",
         entry.lease.owner, entry.lease.epoch, entry.lease.incarnation, entry.lease.revision,
         why, began ? net::Now() - began : 0);
}
void Refusal(GuidKey const& key, char const* name, char const* reason)
{
    // Counted and logged once per GUID and reason; a re-assertion by a periodic scan is silent.
    if (!refusals.Note(key, reason)) return;
    ++refused;
    char text[40];
    guid::Format(key.data(), text, sizeof(text));
    Logf("npclease: admission-refused guid=%s name=%s reason=%s", text, name, reason);
}
void OwnEvent(GuidKey const& key, char const* name, npcown::Event event, char const* detail)
{
    if (!transitions.Changed(key, event)) return;
    char text[40], line[420];
    guid::Format(key.data(), text, sizeof(text));
    npcown::FormatEvent(line, sizeof(line), text, name, event, detail);
    Log(line);
}
void OwnEventAlways(GuidKey const& key, char const* name, npcown::Event event, char const* detail)
{
    char text[40], line[420];
    guid::Format(key.data(), text, sizeof(text));
    npcown::FormatEvent(line, sizeof(line), text, name, event, detail);
    Log(line);
}
void ReadinessDiagnostic(char const* action, GuidKey const& key, Entry const& entry, eCEntity* entity,
                         ClientReadiness const& readiness, char const* why)
{
    Diagnostic(action, key, entry, why);
    char text[40];
    guid::Format(key.data(), text, sizeof(text));
    Entity npc(entity);
    Logf("npclease: readiness guid=%s epoch=%u revision=%u active=%u local=%u confirmed=%u processed=%u actor=%u eligible=%u "
         "running=%u enabled=%u checkpoint=%u last_hit_age_ms=%u hp=%d ai=%d task=%s state=%s "
         "disabled=%u eligibility_reason=%s suspension=%s reason=%s ptr=%p e0=%u e8=%u entity_enabled=%u native=%u "
         "e0_changed_age_ms=%u e8_changed_age_ms=%u hero_cm=%.0f camera_cm=%.0f",
         text, entry.lease.epoch, entry.lease.revision, entry.active ? 1 : 0, entry.local ? 1 : 0, entry.confirmed ? 1 : 0,
         readiness.processed ? 1 : 0, readiness.actor ? 1 : 0, readiness.eligible ? 1 : 0,
         readiness.running ? 1 : 0, readiness.enabled ? 1 : 0, readiness.checkpointBusy ? 1 : 0,
         entry.lastHit ? net::Now() - entry.lastHit : UINT32_MAX,
         entity ? static_cast<int>(npc.DamageReceiver.HitPoints) : entry.lease.state.hp,
         entity ? static_cast<int>(npc.Routine.AIMode) : -1,
         entity ? npc.Routine.GetCurrentTask().GetText() : "-",
         entity ? npc.Routine.GetCurrentState().GetText() : "-",
         entity && entity->IsProcessingDisabled() ? 1 : 0,
         readiness.eligible ? "eligible" : UnavailabilityReason(entity),
         readiness.Suspension() ? readiness.Suspension() : "-",
         why, entity, world::ProcessingOf(entity).range ? 1 : 0,
         readiness.processable ? 1 : 0, entity && entity->IsEnabled() ? 1 : 0,
         entry.nativeActivated ? 1 : 0,
         net::Now() - entry.rangeChangedAt, net::Now() - entry.processableChangedAt,
         entity && remote::ControlledBody() ?
             (entity->GetWorldPosition() - remote::ControlledBody()->GetWorldPosition()).GetMagnitude() : -1.0f,
         entity ? (entity->GetWorldPosition() - eCApplication::GetInstance().GetCurrentCamera().GetPosition()).GetMagnitude() : -1.0f);
}

void ObserveProcessing(Entry& entry, eCEntity* entity, uint32_t now)
{
    const auto membership = world::ProcessingOf(entity);
    if (entry.observedEntity != entity || entry.observedRange != membership.range)
        entry.rangeChangedAt = now;
    if (entry.observedEntity != entity || entry.observedProcessable != membership.processable)
        entry.processableChangedAt = now;
    entry.observedEntity = entity;
    entry.observedRange = membership.range;
    entry.observedProcessable = membership.processable;
}
void CommitDeath(GuidKey const& key, Entry& entry)
{
    if (!entry.lease.state.dead || !(entry.lease.state.effects & NativeDeath) ||
        (entry.lease.state.effects & QuestDeath)) return;
    eCEntity* entity = npcs::NpcByGuid(reinterpret_cast<uint8_t const*>(key.data()));
    if (!NativeReady(entity) || !Import(entity, entry.lease.state)) return;
    // The host's canonical journal owns the quest notification and save watermark.
    if (journal::LeaseDeath(entity, entry.lease.owner)) CommitEffect(entry.lease.state, QuestDeath);
}
bool Exact(Message const& note, Entry const& entry)
{
    return note.world == entry.world && note.epoch == entry.lease.epoch &&
        note.incarnation == entry.lease.incarnation && note.generation == entry.lease.generation &&
        note.owner == entry.lease.owner &&
        !std::memcmp(note.state.templateId, entry.lease.state.templateId, 16) &&
        !std::strcmp(note.state.name, entry.lease.state.name);
}
void ReceiveHost(uint8_t from, Message const& note)
{
    const auto key = Key(note.guid);
    auto found = entries.find(key);
    const uint32_t now = net::Now();
    if (note.kind == Kind::Request || note.kind == Kind::Adopt) {
        if (!Running() || persist::CheckpointBusy() || !GetConfig().npcAuthorityFallback ||
            from < 2 || from >= net::MaxPlayers || !note.echo) return;
        if (found == entries.end() && !CapacityAvailable()) return;
        Entry& entry = entries[key];
        if (entry.lease.ActiveLeases()) {
            // The client reclaimed an NPC it had cut but the host never took over: re-offer from its latest checkpoint.
            if (entry.lease.phase != Phase::Frozen || entry.lease.owner != from) return;
            entry.lease.phase = Phase::Dormant;
        }
        eCEntity* native = npcs::NpcByGuid(note.guid);
        // Client-first: the client already runs this NPC; its latest checkpoint is the baseline for every class.
        const Checkpoint baseline = note.state;
        if (native) {
            if (auto const* why = EligibilityReason(native)) { Refusal(key, note.state.name, why); return; }
            if (std::memcmp(&native->GetTemplateID().GetCore(), baseline.templateId, 16)) {
                Refusal(key, note.state.name, "client checkpoint does not match the host instance template"); return;
            }
            if (!baseline.dead && Entity(native).IsDead()) {
                Refusal(key, note.state.name, "host instance is already dead"); return;
            }
        } else if (auto const* why = DiscoveryReason(baseline)) { Refusal(key, note.state.name, why); return; }
        if (!baseline.dead && journal::RecordedDeath(note.guid)) {
            Refusal(key, note.state.name, "host journal recorded this NPC's death"); return;
        }
        auto interest = InterestOf(baseline, from, native);
        interest.hostPresent = interest.hostPresent && npcs::HostCanPresent(note.guid);
        if (!entry.interest.Delegate(interest, now)) { Refusal(key, note.state.name, DelegateRefusal(interest)); return; }
        if (native && !npcplayback::GuardForLease(native)) {
            Refusal(key, baseline.name, "native property authority fence unavailable"); return;
        }
        if (authoritySerial == UINT32_MAX) { ++refused; Log("FAIL npclease: authority epoch exhausted"); return; }
        entry.world = note.world; entry.echo = note.echo;
        entry.recallAt = 0;
        entry.hasBaseline = true;
        if (!native && !entry.adopted) {
            entry.adopted = true;
            ++discovered;
            Diagnostic("discovered", key, entry, "verified native template; no host instance created");
        }
        entry.lease.incarnation = native ? npcs::Incarnation(note.guid) : note.incarnation;
        entry.previousOwner = from;
        entry.previousGeneration = net::PeerGeneration(from);
        if (!entry.lease.Offer(from, net::PeerGeneration(from), ++authoritySerial, now, baseline)) { ++refused; return; }
        refusals.Forget(key);
        PublishGuard(key, entry);
        Stop(native);
        Send(key, entry, Kind::Offer);
        Diagnostic("offer", key, entry, interest.hostPresent ? "outside primary reach" : "host presentation unavailable");
        return;
    }
    if (found == entries.end()) { ++refused; return; }
    Entry& entry = found->second;
    if (note.kind == Kind::Ack && entry.lease.phase == Phase::Host && entry.lease.owner == 1 &&
        note.owner == 1 && note.generation == 0 && from == entry.previousOwner &&
        net::PeerGeneration(from) == entry.previousGeneration && note.epoch == entry.lease.epoch &&
        note.incarnation == entry.lease.incarnation && note.world == entry.world) {
        entry.committed = true;
        return;
    }
    if (from != entry.lease.owner || net::PeerGeneration(from) != entry.lease.generation || !Exact(note, entry)) {
        ++refused; return;
    }
    if (note.kind == Kind::Prepared) {
        if (entry.lease.Prepared(note.epoch, now)) {
            entry.echo = note.echo;
            Send(key, entry, Kind::Commit);
            ++granted;
            Diagnostic("grant", key, entry, "client-first owner accepted; epoch attached");
        } else if (entry.lease.phase == Phase::Client && !entry.committed) Send(key, entry, Kind::Commit);
    } else if (note.kind == Kind::Ack) {
        entry.committed = true;
    } else if (note.kind == Kind::State || note.kind == Kind::StateFull) {
        Checkpoint value = note.state;
        value.effects |= entry.lease.state.effects; // the client's copy need not have seen the host's quest watermark yet
        if (entry.lease.AcceptState(from, note.generation, note.epoch, note.incarnation, note.revision, value)) {
            ++stateReceived;
            entry.echo = note.echo;
            CommitDeath(key, entry);
            const bool renewRecall = entry.lease.phase == Phase::Recalling && entry.recallAt &&
                now - entry.recallAt < RecallWaitMs;
            if (entry.lease.phase == Phase::Client || renewRecall) {
                // A live client keeps its epoch through host pauses and checkpoints; only a silent client is recovered.
                entry.lease.deadline = now + LeaseMs + LeaseSafetyMs;
                if (Running() && !persist::CheckpointBusy()) Send(key, entry, Kind::Renew);
            }
        }
    } else if (note.kind == Kind::Cut) {
        Checkpoint value = note.state;
        value.effects |= entry.lease.state.effects;
        if (entry.lease.Cut(from, note.generation, note.epoch, note.incarnation, note.revision, value)) {
            entry.echo = note.echo;
            CommitDeath(key, entry);
            Diagnostic("cut", key, entry, "donor frozen");
        }
    }
}
// A client-owned NPC stops natively running: the host presents it again, took it over, or recalled it. Local state is discarded
// by the host's stream; nothing is frozen.
void ReleaseLocal(GuidKey const& key, Entry& entry, eCEntity* entity, bool stop)
{
    const bool wasLocal = entry.local;
    entry.local = false; entry.confirmed = false; entry.active = false; entry.prepared = false;
    entry.nativeActivated = false; entry.cut = false; entry.recallAt = 0;
    entry.lease.phase = Phase::Host;
    entry.window.Reset(entry.lease.epoch);
    entry.ownedEntity = nullptr;
    PublishGuard(key, entry);
    if (wasLocal && stop) Stop(entity);
}
void ReceiveClient(Message const& note)
{
    const auto key = Key(note.guid);
    const uint32_t now = net::Now();
    if (note.kind == Kind::Offer) {
        // Client-first: the NPC already runs natively here; an Offer only attaches the host epoch. Never Import or Stop.
        if (!GetConfig().npcAuthorityFallback || note.owner != net::LocalId() ||
            Reached(now + LeaseSafetyMs, note.echo + note.duration)) { ++refused; return; }
        auto it = entries.find(key);
        if (it == entries.end() || !it->second.local || it->second.cut ||
            (it->second.lease.epoch && !Newer(note.epoch, it->second.lease.epoch))) {
            ++refused; return;
        }
        Entry& entry = it->second;
        eCEntity* entity = npcs::NpcByGuid(note.guid);
        if (!entity || !Eligible(entity)) { ++refused; return; }
        if (!entry.window.Prepare(note.epoch, note.echo, note.duration, now)) { ++refused; return; }
        entry.prepared = true; entry.committed = false; entry.confirmed = false;
        entry.lease.owner = note.owner; entry.lease.generation = note.generation;
        entry.lease.incarnation = note.incarnation; entry.lease.epoch = note.epoch;
        entry.lease.revision = note.revision; entry.lease.phase = Phase::Client;
        entry.hasBaseline = true;
        entry.localDeadline = note.echo + note.duration - LeaseSafetyMs;
        entry.echo = now; entry.hostSeenAt = now;
        PublishGuard(key, entry);
        Send(key, entry, Kind::Prepared);
        return;
    }
    const auto found = entries.find(key);
    if (found == entries.end()) { ++refused; return; }
    Entry& entry = found->second;
    if (note.kind == Kind::Commit && note.owner == 1 && note.world == entry.world &&
        note.incarnation == entry.lease.incarnation && Newer(note.epoch, entry.lease.epoch) &&
        (entry.cut || entry.local || entry.lease.phase == Phase::Host)) {
        const bool wasCut = entry.cut;
        eCEntity* entity = npcs::NpcByGuid(note.guid);
        ReleaseLocal(key, entry, entity, !wasCut);
        entry.lease.epoch = note.epoch; entry.lease.owner = 1; entry.lease.phase = Phase::Host;
        entry.lease.generation = note.generation;
        entry.lease.state = note.state; entry.lease.revision = note.revision;
        entry.presentationFloor = note.echo;
        entry.hasPresentationFloor = true;
        entry.yieldedAt = now ? now : 1;
        entry.unavailableAt = 0;
        entry.window.Reset(note.epoch);
        entry.echo = now;
        PublishGuard(key, entry);
        Send(key, entry, Kind::Ack);
        ++ownTotals.handBacks;
        Diagnostic("hand-back", key, entry, wasCut ? "host committed donor cut" : "host took over a client-owned NPC");
        OwnEventAlways(key, note.state.name, npcown::Event::HandBack,
            wasCut ? "reason=host committed donor cut state=latest" : "reason=host took over");
        return;
    }
    if (!Exact(note, entry)) { ++refused; return; }
    if (note.kind == Kind::Commit) {
        if (entry.lease.phase == Phase::Host && note.owner == 1) { Send(key, entry, Kind::Ack); return; }
        if (!entry.local || entry.cut) { ++refused; return; }
        if (!entry.confirmed) {
            entry.window.Commit(note.epoch, now); // telemetry only; ownership never depends on the window
            entry.confirmed = true; entry.prepared = false;
            entry.hostSeenAt = now;
            char detail[80];
            std::snprintf(detail, sizeof(detail), "epoch=%u life=%u", entry.lease.epoch, entry.lease.incarnation);
            OwnEvent(key, entry.lease.state.name, npcown::Event::Confirmed, detail);
        }
        entry.echo = now;
        Send(key, entry, Kind::Ack);
    } else if (note.kind == Kind::Renew) {
        if (!entry.local || entry.cut || !entry.confirmed) { ++refused; return; }
        entry.window.Renew(note.epoch, note.echo, note.duration, now);
        entry.hostSeenAt = now;
        entry.silentLogged = false;
        entry.lease.state.effects |= note.state.effects;
    } else if (note.kind == Kind::Recall) {
        if (!entry.recallAt) entry.recallAt = now ? now : 1;
    } else if (note.kind == Kind::Abort) {
        // The host could not keep its side of the offer: keep running natively and retry the handshake later.
        Send(key, entry, Kind::Ack);
        entry.prepared = false; entry.confirmed = false; entry.recallAt = 0;
        entry.window.Reset(0);
        entry.lease.epoch = 0;
        entry.lastRequest = now;
        PublishGuard(key, entry);
    }
}
void Receive(uint8_t from, Message const& note)
{
    if (!installed || npcremoval::Suppressed(note.guid) || note.world != net::WorldEpoch() ||
        persist::NativeLoading()) { ++refused; return; }
    Message hydrated = note;
    if (note.kind == Kind::State || note.kind == Kind::Renew) {
        const auto found = entries.find(Key(note.guid));
        if (found == entries.end() || !found->second.hasBaseline) { ++refused; return; }
        auto const& canonical = found->second.lease.state;
        if (note.kind == Kind::Renew) {
            hydrated.state = canonical;
            hydrated.state.effects |= note.state.effects;
        } else {
            std::memcpy(hydrated.state.name, canonical.name, sizeof(canonical.name));
            std::memcpy(hydrated.state.routine, canonical.routine, sizeof(canonical.routine));
        }
    }
    if (!Valid(hydrated)) { ++refused; return; }
    if (net::IsHost()) ReceiveHost(from, hydrated);
    else if (from == 1 && note.owner == net::LocalId()) ReceiveClient(hydrated);
    else if (from == 1 && note.kind == Kind::Commit && note.owner == 1) ReceiveClient(hydrated);
    else ++refused;
}
GEBool __fastcall Quest(gCQuestManager_PS* self, void* edx, eCEntity const& victim)
{
    if (!ai::ShuttingDown() && ownerDeath == &victim) return GEFalse;
    return originalQuest(self, edx, victim);
}
GEInt GE_STDCALL Kill(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    eCEntity* victim = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    Entry* entry = Find(victim);
    if (!ai::ShuttingDown() && !net::IsHost() && victim && !Player(victim) && HostWins(Key(victim))) {
        ++repeatedDeaths;
        OwnEvent(Key(victim), victim->GetName().GetText(), npcown::Event::Refused,
            "path=Kill reason=unreleased host authority; native death refused");
        return 1;
    }
    if (!ai::ShuttingDown() && entry && entry->lease.epoch &&
        (Fenced(victim) || (entry->lease.state.effects & NativeDeath))) {
        ++repeatedDeaths;
        return 1;
    }
    if (ai::ShuttingDown() || !entry || net::IsHost() || !entry->lease.ActiveLeases())
        return originalKill(spu, self, other, args);
    if (!LocalAuthority(victim) || (entry->lease.state.effects & NativeDeath)) {
        ++repeatedDeaths;
        return 1;
    }
    CommitEffect(entry->lease.state, NativeDeath); // guard before native callbacks can recursively invoke Kill
    eCEntity* before = ownerDeath;
    ownerDeath = victim;
    const GEInt result = originalKill(spu, self, other, args);
    ownerDeath = before;
    Checkpoint final = entry->lease.state;
    if (!CaptureNative(victim, final))
        Log("FAIL npclease: native death recapture unavailable; terminal state retains last canonical pose");
    final.dead = 1; final.hp = 0; final.busy = 0; final.effects |= NativeDeath;
    entry->lease.state = final;
    ++entry->lease.revision; entry->echo = net::Now();
    // Unconfirmed owners carry the death in their next Adopt/Offer baseline; the host journals it exactly once (QuestDeath bit).
    if (entry->confirmed && Send(Key(victim), *entry, Kind::State)) ++stateSent;
    ++nativeDeaths;
    ++ownTotals.deaths;
    OwnEventAlways(Key(victim), final.name, npcown::Event::Death,
        entry->confirmed ? "native death xp/loot=native quest=host-journal confirmed=1" :
                           "native death xp/loot=native quest=host-journal confirmed=0 (reported on confirmation)");
    return result;
}
bool Install()
{
    if (installed || installFailed) return installed;
    gSScript* script = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("Kill"), script) || !script) return false;
    auto* code = reinterpret_cast<unsigned char*>(script->m_funcScript);
    HMODULE owner = nullptr;
    const unsigned char prefix[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    auto* quest = reinterpret_cast<unsigned char*>(GetProcAddress(GetModuleHandleA("Game.dll"),
        "?OnNPCKilled@gCQuestManager_PS@@QAE_NABVeCEntity@@@Z"));
    const unsigned char questPrefix[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56};
    if (!code || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(code), &owner) || owner != GetModuleHandleA("Script_Game.dll") ||
        std::memcmp(code, prefix, sizeof(prefix)) || !quest || std::memcmp(quest, questPrefix, sizeof(questPrefix))) {
        installFailed = true;
        Log("FAIL npclease: native Kill/quest entry unsupported; wildlife remains host-owned");
        return false;
    }
    originalQuest = reinterpret_cast<QuestFn>(ai::JumpTo(quest, reinterpret_cast<void*>(&Quest), sizeof(questPrefix)));
    if (!originalQuest) { installFailed = true; Log("FAIL npclease: quest effect guard installation failed"); return false; }
    originalKill = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Kill), sizeof(prefix)));
    if (!originalKill) { installFailed = true; Log("FAIL npclease: death guard installation failed"); return false; }
    ai::PatchPointer(reinterpret_cast<void**>(&script->m_funcScript), reinterpret_cast<void*>(&Kill), "npclease Kill script");
    net::SetNpcLeaseHandler(&Receive);
    installed = true;
    Log("npclease: native authority ready; client-first ownership for all NPC classes absence_dwell_ms=0 lease_ms=3000 state_hz=5");
    return true;
}
void TickHost(uint32_t now)
{
    for (auto& item : entries) {
        const auto& key = item.first;
        Entry& entry = item.second;
        eCEntity* entity = npcs::NpcByGuid(reinterpret_cast<uint8_t const*>(key.data()));
        if (entry.lease.epoch) CommitDeath(key, entry);
        if (entry.lease.phase == Phase::Host && entry.hasBaseline) {
            Checkpoint current = entry.lease.state;
            if (CaptureNative(entity, current)) {
                current.effects |= entry.lease.state.effects;
                if (current.dead && !(entry.lease.state.effects & NativeDeath))
                    current.effects |= NativeDeath | QuestDeath;
                entry.lease.state = current;
            }
            if (!entry.committed && entry.previousOwner > 1 &&
                net::PeerGeneration(entry.previousOwner) == entry.previousGeneration &&
                net::InWorld(entry.previousOwner) && now - entry.lastControl >= 500) {
                net::SendNpcLease(entry.previousOwner, Control(key, entry, Kind::Commit));
                entry.lastControl = now;
            }
        }
        if (entry.lease.phase == Phase::Client || entry.lease.phase == Phase::Recalling) {
            auto value = InterestOf(entry.lease.state, entry.lease.owner, entity);
            value.hostPresent = value.hostPresent && value.hostDistance <= value.hostRoi &&
                CanResumePresentation(entity);
            const bool ownerGone = !CurrentPeer(entry.lease.owner, entry.lease.generation);
            if (ownerGone || entry.interest.Recall(value) || !Running() || entry.lease.state.dead) {
                if (entry.lease.Recall(now)) {
                    ++recalled; entry.recallAt = now ? now : 1;
                    Send(key, entry, Kind::Recall);
                    Diagnostic("recall", key, entry, "primary reach/readiness/barrier");
                }
            }
            if (NativeReady(entity) && (entry.applied != entity || entry.appliedRevision != entry.lease.revision)) {
                if (npcplayback::GuardForLease(entity) && Import(entity, entry.lease.state)) {
                    entry.applied = entity;
                    entry.appliedRevision = entry.lease.revision;
                }
            }
            CommitDeath(key, entry);
            if (entry.lease.phase == Phase::Client && !entry.committed && now - entry.lastControl >= 500)
                Send(key, entry, Kind::Commit);
        }
        if (entry.lease.phase == Phase::Frozen) {
            CommitDeath(key, entry);
            if (entity && NativeReady(entity) && TransferableStatus(entry.lease.state) && Import(entity, entry.lease.state) &&
                authoritySerial != UINT32_MAX) {
                const uint8_t client = entry.lease.owner;
                const uint32_t generation = entry.lease.generation;
                if (entry.lease.HandBack(++authoritySerial, now)) {
                    entry.committed = false;
                    if (Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data()))) {
                        if (CurrentPeer(client, generation))
                            net::SendNpcLease(client, Control(key, entry, Kind::Commit));
                        Diagnostic("host-active", key, entry, "imported frozen client cut");
                    } else Diagnostic("host-held", key, entry, "native restart unavailable");
                }
            }
        }
        const auto prior = entry.lease.phase;
        const uint8_t previousOwner = entry.lease.owner;
        const uint32_t previousGeneration = entry.lease.generation;
        if (entry.lease.Recover(now)) {
            ++recovered;
            if (prior == Phase::Offering) {
                auto abort = Control(key, entry, Kind::Abort);
                abort.owner = previousOwner;
                abort.generation = previousGeneration;
                if (CurrentPeer(previousOwner, previousGeneration))
                    net::SendNpcLease(previousOwner, abort);
                if (entity) Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data()));
            } else {
                if (authoritySerial != UINT32_MAX) entry.lease.epoch = ++authoritySerial;
                Diagnostic("adopt", key, entry, "old owner's conservative lease expired");
            }
        }
        if (entry.lease.phase == Phase::Dormant && entity && NativeReady(entity) && TransferableStatus(entry.lease.state) &&
            Running() && !persist::CheckpointBusy() && npcplayback::GuardForLease(entity) &&
            Import(entity, entry.lease.state)) {
            const uint8_t client = entry.previousOwner;
            entry.lease.phase = Phase::Host; entry.lease.owner = 1;
            entry.lease.generation = 0; entry.committed = false;
            if (Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data())) &&
                CurrentPeer(client, entry.previousGeneration))
                net::SendNpcLease(client, Control(key, entry, Kind::Commit));
        }
        PublishGuard(key, entry);
    }
}
// Which native path put the NPC into the engine's processing lists. Informational only: ownership never depends on it,
// so a failed registration can never freeze, fence or cut a client-owned NPC.
void RegisterProcessing(GuidKey const& key, Entry& entry, eCEntity* entity)
{
    const bool processedBefore = Processed(entity), processableBefore = world::ProcessingOf(entity).processable;
    bool queued = false, enabled = false;
    char const* queueWhy = "not needed";
    if (!(processedBefore && processableBefore)) {
        if (!entity->IsEnabled()) { Entity(entity).EnableProcessing(GETrue); enabled = true; }
        if (entity->IsProcessingDisabled() && !npcs::RedundantClientEntity(entity)) {
            entity->DisableProcessing(GEFalse);
            enabled = true;
        }
        queued = world::QueueEntityProcessing(entity, &queueWhy);
    }
    const auto path = npcown::Classify(processedBefore, processableBefore, queued, enabled);
    switch (path) {
    case npcown::Registration::AlreadyProcessed: ++ownTotals.registrationAlready; break;
    case npcown::Registration::Queued: ++ownTotals.registrationQueued; break;
    case npcown::Registration::Enabled: ++ownTotals.registrationEnabled; break;
    default: ++ownTotals.registrationFailed; break;
    }
    static bool seen[4] = {};
    const auto index = static_cast<size_t>(path);
    if (path == npcown::Registration::AlreadyProcessed && seen[index]) return;
    if (seen[index] && path != npcown::Registration::Unavailable) return;
    seen[index] = true;
    char detail[300];
    std::snprintf(detail, sizeof(detail),
        "path=%s processed=%u processable=%u enabled=%u processing_disabled=%u authority=%u queue=%s ownership=kept",
        npcown::Name(path), processedBefore ? 1 : 0, processableBefore ? 1 : 0, entity->IsEnabled() ? 1 : 0,
        entity->IsProcessingDisabled() ? 1 : 0, ProcessingAuthority(entity) ? 1 : 0, queueWhy);
    OwnEventAlways(key, entry.lease.state.name, npcown::Event::Registration, detail);
}

// Immediate client-first ownership of an NPC the host does not present. Returns nullptr on success, else the reason.
char const* TakeOver(eCEntity* entity, GuidKey const& key, uint32_t now)
{
    if (npcs::HostStreamClaimed(key.data())) return "unreleased host authority";
    if (npcs::HostReleasePending(key.data())) return "final host state awaiting presentation";
    if (!npcown::AcquisitionTimely(npcs::HostStreamDelay(), net::RoundTrip(1)))
        return "host stream stale; authority unknown";
    Entry& entry = entries[key];
    Checkpoint state;
    if (!CaptureNative(entity, state)) return "native checkpoint invalid";
    entry.world = net::WorldEpoch(); entry.welcome = net::WelcomeSerial();
    entry.lease.state = state; entry.lease.owner = net::LocalId();
    entry.lease.incarnation = npcs::Incarnation(key.data());
    if (!entry.lease.incarnation) entry.lease.incarnation = 1;
    entry.lease.epoch = 0; entry.lease.revision = 0; entry.lease.generation = 0;
    entry.lease.phase = Phase::Client;
    entry.lease.started = now;
    entry.window.Reset(0);
    entry.hasBaseline = true;
    entry.local = true; entry.active = true; entry.confirmed = false; entry.prepared = false;
    entry.cut = false; entry.recallAt = 0; entry.captureFailed = false;
    entry.ownedAt = now ? now : 1;
    entry.ownedEntity = entity;
    entry.lastRequest = 0; entry.requestEvery = 500; entry.hostSeenAt = 0; entry.silentLogged = false;
    entry.cutReason = nullptr;
    entry.retryAt = 0;
    std::memcpy(entry.sentRoutine, state.routine, sizeof(entry.sentRoutine));
    if (!Activate(entity, entry, key.data())) {
        entry.local = false; entry.active = false; entry.nativeActivated = false; entry.ownedEntity = nullptr;
        entry.lease.phase = Phase::Host;
        PublishGuard(key, entry);
        return "native fence/import/restart unavailable";
    }
    refusals.Forget(key);
    waiting.Forget(key);
    ++ownTotals.takeovers;
    RegisterProcessing(key, entry, entity);
    Entity npc(entity);
    std::snprintf(entry.lastTask, sizeof(entry.lastTask), "%s", npc.Routine.GetCurrentTask().GetText());
    char detail[300];
    std::snprintf(detail, sizeof(detail), "reason=host not presenting class=%s hp=%d/%d routine=%s task=%s mode=%d dist_hero_cm=%.0f",
        ClassOf(npc) == NativeClass::Creature ? "creature" : "ordinary", state.hp, state.maxHp, state.routine,
        entry.lastTask, static_cast<int>(npc.Routine.AIMode),
        (entity->GetWorldPosition() - remote::ControlledBody()->GetWorldPosition()).GetMagnitude());
    OwnEventAlways(key, state.name, npcown::Event::Owned, detail);
    transitions.Changed(key, npcown::Event::Owned);
    return nullptr;
}

// Hand the NPC to the host from its latest state (host recall, role change). The host imports it and presents it; if it does
// not commit in time the client reclaims the NPC instead of leaving it frozen.
void CutToHost(GuidKey const& key, Entry& entry, eCEntity* entity, Checkpoint const* state, char const* why, uint32_t now)
{
    entry.local = false; entry.active = false; entry.cut = true; entry.cutAt = now ? now : 1;
    entry.cutReason = why;
    entry.lease.phase = Phase::Frozen;
    entry.window.Freeze();
    if (state) entry.lease.state = *state;
    ++entry.lease.revision; entry.echo = now;
    entry.nativeActivated = false; entry.confirmed = false; entry.ownedEntity = nullptr;
    PublishGuard(key, entry);
    Diagnostic("frozen", key, entry, why);
    char detail[200];
    std::snprintf(detail, sizeof(detail), "reason=%s awaiting host hand-back from latest state hp=%d", why, entry.lease.state.hp);
    OwnEventAlways(key, entry.lease.state.name, npcown::Event::Frozen, detail);
    Stop(entity);
    Send(key, entry, Kind::Cut);
}

bool SharedWithOtherClient(eCEntity* entity)
{
    // One owner per NPC: the lowest connected client id within reach owns it, the others stay host/visual driven.
    const auto& at = entity->GetWorldPosition();
    const float here[3] = {at.GetX(), at.GetY(), at.GetZ()};
    for (uint8_t i = 2; i < net::LocalId(); ++i) {
        auto const& other = net::Remotes()[i];
        if (other.connected && (other.latest.flags & 1) && other.latest.world == remote::CurrentWorld() &&
            body::Distance(here, other.latest.position) <= ClientRadiusCm) return true;
    }
    return false;
}

// Reaction logging: the first task change after a native hit is the hit reaction the NPC plays.
void ObserveReaction(GuidKey const& key, Entry& entry, Entity& npc, uint32_t now)
{
    const char* task = npc.Routine.GetCurrentTask().GetText();
    if (std::strcmp(task, entry.lastTask)) {
        if (entry.lastHit && entry.reactionAt != entry.lastHit && now - entry.lastHit < 2500) {
            entry.reactionAt = entry.lastHit;
            char detail[200];
            std::snprintf(detail, sizeof(detail), "task=%s->%s state=%s hit_age_ms=%u", entry.lastTask[0] ? entry.lastTask : "-",
                task[0] ? task : "-", npc.Routine.GetCurrentState().GetText(), now - entry.lastHit);
            OwnEventAlways(key, entry.lease.state.name, npcown::Event::Reaction, detail);
        }
        std::snprintf(entry.lastTask, sizeof(entry.lastTask), "%s", task);
    }
}

void MaintainOwners(uint32_t now)
{
    for (auto& item : entries) {
        Entry& entry = item.second;
        const GuidKey& key = item.first;
        auto* entity = npcs::NpcByGuid(key.data());
        if (entry.cut && !entry.local) {
            // Hand-back pending: the host commits from the cut state; if it cannot (not loaded near the host) the NPC lives on.
            if (entity && now - entry.cutAt >= npcown::CutReclaimMs && !npcs::HostStreamClaimed(key.data()) &&
                npcown::AcquisitionTimely(npcs::HostStreamDelay(), net::RoundTrip(1)) &&
                !persist::CheckpointBusy()) {
                entry.cut = false; entry.lease.epoch = 0; entry.lease.phase = Phase::Host; entry.window.Reset(0);
                if (const char* why = TakeOver(entity, key, now)) {
                    OwnEventAlways(key, entry.lease.state.name, npcown::Event::Refused, why);
                } else {
                    ++ownTotals.reclaims;
                    OwnEventAlways(key, entry.lease.state.name, npcown::Event::Reclaimed,
                        "reason=host did not commit the cut in time; client keeps the NPC natively");
                }
            }
            continue;
        }
        if (!entry.local) continue;
        if (!entity) continue; // unloaded by the engine: suspended, state kept for its return
        if (entry.ownedEntity != entity && !HostWins(key)) {
            // The engine reloaded the entity under a new instance: restore the latest local state into it once it is
            // processable (ownership and the checkpoint are retained meanwhile).
            if (!NativeReady(entity)) continue;
            if (!Activate(entity, entry, key.data())) {
                ReleaseLocal(key, entry, entity, false);
                OwnEventAlways(key, entry.lease.state.name, npcown::Event::Yielded, "reason=reloaded instance could not be restored");
                continue;
            }
            entry.ownedEntity = entity;
            OwnEventAlways(key, entry.lease.state.name, npcown::Event::Owned, "reason=reloaded instance restored from latest state");
        }
        ObserveProcessing(entry, entity, now);
        const auto readiness = Readiness(entity, entry, now);
        Entity npc(entity);
        Checkpoint state = entry.lease.state;
        const bool captured = CaptureNative(entity, state);
        if (captured) state.effects |= entry.lease.state.effects;
        if (!captured && !entry.captureFailed)
            ReadinessDiagnostic("state-unavailable", key, entry, entity, readiness,
                readiness.LossReason() ? readiness.LossReason() : "native checkpoint invalid; state sends pause, NPC keeps running");
        entry.captureFailed = !captured;

        npcown::OwnerFacts facts;
        facts.role = RoleOf(entity, false);
        // A single in-flight record does not revoke an established owner. Proven host authority wins even over a local death.
        facts.hostPresenting = HostWins(key);
        facts.hostRecallDue = entry.recallAt && entry.confirmed && captured &&
            npc.Routine.AIMode != gEAIMode_Talk &&
            (!state.busy || now - entry.recallAt >= RecallWaitMs) && TransferableStatus(state);
        facts.leaseExpired = entry.confirmed && Reached(now, entry.window.deadline);
        facts.hostSilent = entry.confirmed && entry.hostSeenAt && now - entry.hostSeenAt > LeaseMs + LeaseSafetyMs;
        facts.checkpointBusy = readiness.checkpointBusy;
        facts.paused = !readiness.running;
        facts.registrationFailed = false;
        facts.processingListsMissing = !readiness.processed || !readiness.processable;
        facts.outsideRing = !readiness.insideRange;
        facts.disconnected = !net::Connected();
        if (facts.hostSilent && !entry.silentLogged) {
            entry.silentLogged = true;
            char detail[160];
            std::snprintf(detail, sizeof(detail), "silent_ms=%u owner keeps running natively; state sends continue",
                now - entry.hostSeenAt);
            OwnEventAlways(key, entry.lease.state.name, npcown::Event::HostSilent, detail);
        }
        if (char const* release = npcown::Release(facts)) {
            if (facts.hostPresenting || !entry.confirmed || !captured) {
                const uint32_t age = npcs::HostPresentationAge(entity);
                ReleaseLocal(key, entry, entity, true);
                entry.yieldedAt = now ? now : 1;
                ++ownTotals.yields;
                char detail[200];
                std::snprintf(detail, sizeof(detail), "reason=%s stream_age_ms=%u local state discarded; host stream applies",
                    release, age == UINT32_MAX ? 0u : age);
                OwnEventAlways(key, entry.lease.state.name, npcown::Event::Yielded, detail);
            } else CutToHost(key, entry, entity, &state, release, now);
            continue;
        }
        ObserveReaction(key, entry, npc, now);
        const bool event = captured && (state.hp != entry.lease.state.hp || state.dead != entry.lease.state.dead ||
            state.attitude != entry.lease.state.attitude || state.status != entry.lease.state.status ||
            state.defeated != entry.lease.state.defeated);
        const bool settledDead = state.dead && (entry.lease.state.effects & QuestDeath);
        if (captured && (event || (!settledDead && now - entry.lastState >= StateMs))) {
            entry.lease.state = state;
            if (++entry.lease.revision == 0) {
                ReleaseLocal(key, entry, entity, false);
                Log("FAIL npclease: revision exhausted; NPC released to the host stream");
                continue;
            }
            entry.echo = now;
            if (event && entry.confirmed)
                Diagnostic("state-event", key, entry, "native HP/life/attitude/status changed; owner unchanged");
            if (entry.confirmed) { if (Send(key, entry, Kind::State)) { entry.lastState = now; ++stateSent; } }
            else entry.lastState = now;
        }
        // Handshake with the host: asynchronous, backed off, never a precondition for animating or reacting.
        if (!entry.confirmed && !entry.prepared && now - entry.lastRequest >= entry.requestEvery) {
            entry.echo = now;
            if (Send(key, entry, Kind::Adopt)) {
                entry.lastRequest = now;
                entry.requestEvery = std::min<uint32_t>(entry.requestEvery * 2, 8000);
            }
        }
        if (entry.prepared && Reached(now, entry.localDeadline) && !entry.confirmed) {
            entry.prepared = false;
            entry.lease.epoch = 0; entry.window.Reset(0);
            entry.lastRequest = 0;
            Diagnostic("prepare-expired", key, entry, "late commit; client keeps running natively and retries the handshake");
        }
    }
    if (entries.size() + 64 >= MaxLeases) {
        // Forget GUIDs that are unloaded, host-driven and carry no handshake state.
        for (auto it = entries.begin(); it != entries.end();) {
            Entry const& idle = it->second;
            if (!idle.local && !idle.cut && !idle.prepared && idle.lease.phase == Phase::Host &&
                !npcs::NpcByGuid(it->first.data())) {
                guardStates.Erase(it->first);
                it = entries.erase(it);
            } else ++it;
        }
    }
}

void ScanClient(uint32_t now)
{
    static uint32_t lastWorldEntries = 0;
    if (ai::ShuttingDown() || persist::NativeLoading() || persist::HeroPending() ||
        !gCSession::GetInstance().IsGameRunning() || !remote::ControlledBody() || installFailed) return;
    const uint32_t entriesNow = net::WorldEntryCount();
    if (entriesNow != lastWorldEntries) {
        lastWorldEntries = entriesNow;
        sessionReadyAt = 0;
    }
    if (!sessionReadyAt) sessionReadyAt = now ? now : 1;
    const bool settled = npcown::StreamSettled(now, sessionReadyAt, npcs::HostStreamSeen());
    const bool census = !ownReportAt || now - ownReportAt >= 5000;
    const uint32_t streamDelay = npcs::HostStreamDelay(), roundTrip = net::RoundTrip(1);
    const bool streamUncertain = !npcown::AcquisitionTimely(streamDelay, roundTrip);
    if (census)
        Logf("NPCOWNSTREAM state_age_ms=%u rtt_ms=%u uncertain=%u claim_policy=unreleased owner_gap_policy=keep",
            streamDelay, roundTrip, streamUncertain ? 1u : 0u);
    npcown::OwnStats snap;
    std::unordered_map<std::string, unsigned> visual, waits, refusedNow;
    const auto* all = world::ProcessedEntities();
    unsigned takeovers = 0;
    for (GEU16 i = 0; all && i < all->GetCount(); ++i) {
        eCEntity* entity = all->GetAt(i);
        if (!npcplayback::IsCharacter(entity) || Player(entity)) continue;
        const auto key = Key(entity);
        const auto found = entries.find(key);
        const char* name = entity->GetName().GetText();
        if (found != entries.end() && found->second.cut && !found->second.local) {
            ++snap.frozen;
            if (census) ++waits[found->second.cutReason ? found->second.cutReason : "hand-back pending"];
            continue;
        }
        npcown::Facts facts;
        facts.role = RoleOf(entity, false);
        facts.installed = installed;
        facts.configEnabled = GetConfig().npcAuthorityFallback;
        facts.nativeReady = NativeReady(entity);
        facts.hostPresenting = npcs::HostStreamClaimed(key.data());
        facts.hostStreamUncertain = streamUncertain;
        facts.hostReleasePending = npcs::HostReleasePending(key.data());
        facts.settled = settled;
        facts.cooldownHeld = found != entries.end() && npcown::CooldownHeld(now, found->second.yieldedAt);
        facts.alreadyOwned = found != entries.end() && found->second.local && found->second.ownedEntity == entity;
        const auto decision = npcown::Decide(facts);
        switch (decision.verdict) {
        case npcown::Verdict::Excluded:
            Refusal(key, name, decision.reason);
            ++snap.refused;
            if (census) ++refusedNow[decision.reason];
            continue;
        case npcown::Verdict::HostStreamed:
            ++snap.hostStreamed;
            refusals.Forget(key);
            waiting.Forget(key);
            if (found != entries.end() && found->second.lease.phase == Phase::Host) found->second.unavailableAt = 0;
            continue;
        case npcown::Verdict::Wait:
            ++snap.waiting;
            if (census) ++waits[decision.reason];
            if (waiting.Note(key, decision.reason) && std::strncmp(decision.reason, "host stream settling", 20) &&
                std::strncmp(decision.reason, "native authority guards", 23)) {
                char detail[160];
                std::snprintf(detail, sizeof(detail), "reason=%s", decision.reason);
                OwnEvent(key, name, npcown::Event::Waiting, detail);
            }
            continue;
        case npcown::Verdict::ClientNative:
            break;
        }
        if (facts.alreadyOwned) { ++snap.nativeOwned; continue; }
        if (found == entries.end() && !CapacityAvailable()) {
            ++snap.waiting; if (census) ++waits["canonical cache full"]; continue;
        }
        Entity npc(entity);
        char const* hold = nullptr;
        if (npc.IsDead() || npc.IsDown()) hold = "dead or downed; keeps its native state";
        else if (SharedWithOtherClient(entity)) hold = "another client shares this NPC (single owner)";
        else if (found != entries.end() && found->second.retryAt && !Reached(now, found->second.retryAt))
            hold = "takeover retry back-off";
        else if (takeovers >= 8) hold = "takeover rate limit";
        if (hold) {
            ++snap.waiting;
            if (census) ++waits[hold];
            if (waiting.Note(key, hold) && std::strcmp(hold, "takeover rate limit") &&
                std::strcmp(hold, "takeover retry back-off")) {
                char detail[160];
                std::snprintf(detail, sizeof(detail), "reason=%s", hold);
                OwnEvent(key, name, npcown::Event::Waiting, detail);
            }
            continue;
        }
        if (found == entries.end()) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "verdict=client-native class=%s", ClassOf(npc) == NativeClass::Creature ? "creature" : "ordinary");
            OwnEvent(key, name, npcown::Event::Loaded, detail);
        }
        ++takeovers;
        if (const char* why = TakeOver(entity, key, now)) {
            ++snap.refused;
            if (census) ++refusedNow[why];
            Refusal(key, name, why);
            entries[key].retryAt = (now ? now : 1) + 2000;
        } else ++snap.nativeOwned;
    }
    if (!census) return;
    for (auto const& item : entries) {
        if (item.second.local) { if (item.second.confirmed) ++snap.confirmed; }
    }
    const auto pick = [&](std::unordered_map<std::string, unsigned> const& values, char const*& label, unsigned& amount) {
        label = "-"; amount = 0;
        for (auto const& value : values) if (value.second > amount) { label = value.first.c_str(); amount = value.second; }
    };
    static std::string refusedTop, waitingTop, visualTop;
    unsigned refusedCount = 0, waitCount = 0;
    char const* label = "-";
    pick(refusedNow, label, refusedCount); refusedTop = label;
    pick(waits, label, waitCount); waitingTop = label;
    snap.refusedTop = refusedTop.c_str(); snap.refusedTopCount = refusedCount;
    snap.waitingTop = waitingTop.c_str(); snap.waitingTopCount = waitCount;
    snap.frozenTop = waitingTop.c_str(); snap.frozenTopCount = snap.frozen ? waitCount : 0;
    snap.visualOnly = snap.refused + snap.waiting;
    visualTop = refusedCount >= waitCount ? refusedTop : waitingTop;
    snap.visualTop = visualTop.c_str(); snap.visualTopCount = std::max(refusedCount, waitCount);
    snap.hitsApplied = ownTotals.hitsApplied; snap.deaths = ownTotals.deaths; snap.handBacks = ownTotals.handBacks;
    snap.yields = ownTotals.yields; snap.takeovers = ownTotals.takeovers; snap.reclaims = ownTotals.reclaims;
    snap.registrationAlready = ownTotals.registrationAlready; snap.registrationQueued = ownTotals.registrationQueued;
    snap.registrationEnabled = ownTotals.registrationEnabled; snap.registrationFailed = ownTotals.registrationFailed;
    char line[700];
    npcown::FormatSummary(line, sizeof(line), snap);
    Log(line);
    ownReportAt = now ? now : 1;
}

void TickClient(uint32_t now)
{
    MaintainOwners(now);
    ScanClient(now);
}
}

bool LocalAuthority(eCEntity const* entity)
{
    return ReadGuard(entity).Local(net::Now()) && !HostWins(Key(entity));
}
bool ProcessingAuthority(eCEntity const* entity)
{
    auto* hero = remote::ControlledBody();
    return entity && hero && !net::IsHost() && !npcs::RedundantClientEntity(entity) &&
        !npcremoval::Suppressed(entity) && !HostWins(Key(entity)) &&
        LeaseProcessingRange(ReadGuard(entity).Processing(net::Now()),
        (entity->GetWorldPosition() - hero->GetWorldPosition()).GetMagnitude());
}
void NoteHit(eCEntity* victim, eCEntity* source, char const* path, int before, int after)
{
    Entry* entry = Find(victim);
    if (!entry || net::IsHost() || !entry->active || entry->lease.owner != net::LocalId()) return;
    const uint32_t now = net::Now();
    entry->lastHit = now ? now : 1;
    if (after < before) ++ownTotals.hitsApplied;
    Entity npc(victim);
    char detail[300];
    std::snprintf(detail, sizeof(detail),
        "path=%s attacker=%s hp=%d->%d native=%u confirmed=%u ai=%d task=%s state=%s", path,
        source ? Entity(source).GetName().GetText() : "-", before, after, LocalAuthority(victim) ? 1 : 0,
        entry->confirmed ? 1 : 0, static_cast<int>(npc.Routine.AIMode), npc.Routine.GetCurrentTask().GetText(),
        npc.Routine.GetCurrentState().GetText());
    OwnEventAlways(Key(victim), entry->lease.state.name, npcown::Event::Hit, detail);
}
char const* UnavailabilityReason(eCEntity* entity)
{
    if (!GetConfig().npcAuthorityFallback) return "npc_authority_fallback off";
    if (installFailed) return "native authority guard installation failed";
    if (!installed) return "native authority guards initializing";
    if (!entity) return "native NPC instance unavailable";
    if (auto const* reason = EligibilityReason(entity)) return reason;
    const auto key = Key(entity);
    if (auto const* reason = refusals.Reason(key)) return reason;
    if (auto const* reason = waiting.Reason(key)) return reason;
    return "awaiting client-first scan";
}
bool Fenced(eCEntity const* entity)
{
    return ReadGuard(entity).Fenced(net::Now());
}
bool SuppressHostPublication(uint8_t const identity[16])
{
    const auto found = entries.find(Key(identity));
    return found != entries.end() && found->second.lease.epoch && found->second.lease.phase != Phase::Host;
}
bool IgnoreHostSnapshot(uint8_t const identity[16], uint32_t captured)
{
    if (net::IsHost()) return false;
    const auto found = entries.find(Key(identity));
    if (found == entries.end()) return false;
    const auto& entry = found->second;
    if (entry.hasPresentationFloor && !Newer(captured, entry.presentationFloor)) return true;
    // A client owner keeps receiving the stream: a fresh host stream is how it learns to yield.
    if (entry.local) return false;
    return entry.lease.epoch && entry.lease.phase != Phase::Host;
}
bool IgnoreHostSnapshot(uint8_t const identity[16])
{
    return !net::IsHost() && SuppressHostPublication(identity) && !LocalOwner(identity);
}
bool LocalOwner(uint8_t const identity[16])
{
    if (net::IsHost()) return false;
    const auto found = entries.find(Key(identity));
    return found != entries.end() && found->second.local && found->second.active;
}
uint8_t RemoteOwner(eCEntity const* entity)
{
    return ReadGuard(entity).owner;
}
uint32_t Life(uint8_t const identity[16])
{
    const auto found = entries.find(Key(identity));
    return found == entries.end() || !found->second.lease.epoch ? 0 : found->second.lease.incarnation;
}
bool ContactAllowed(eCEntity const* victim, eCEntity const* source)
{
    if (Player(victim) && Player(source)) return true;
    const auto victimLease = ReadGuard(victim), sourceLease = ReadGuard(source);
    const bool leased = victimLease.leased || sourceLease.leased;
    if (!leased) return true;
    bool allowed = !Fenced(victim) && !Fenced(source);
    if (!net::IsHost() && !Player(source) && source && source->GetPropertySet(eEPropertySetType_NPC))
        allowed = allowed && LocalAuthority(source) &&
            (Player(victim) || LocalAuthority(victim));
    if (!allowed) ++contactsRefused;
    return allowed;
}
bool ActiveLeases()
{
    for (auto const& item : entries)
        if (item.second.lease.epoch && (item.second.lease.phase != Phase::Host ||
            (item.second.lease.state.dead && (item.second.lease.state.effects & NativeDeath) &&
             !(item.second.lease.state.effects & QuestDeath)))) return true;
    return false;
}
SaveOverlay Capture()
{
    return {};
}
bool Restore(SaveOverlay const&, uint32_t)
{
    // Legacy client-owned checkpoints must never override the loaded native world.
    OnWorldDeactivate();
    return true;
}
bool WriteSnapshot(FILE* file)
{
    if (!file) {
        Log("FAIL npclease: snapshot stream unavailable");
        return false;
    }
    return true;
}
bool ReadSnapshot(FILE* file, SaveOverlay& overlay)
{
    return ReadSaveStream(file, overlay);
}
void RejectSnapshot(char const* reason)
{
    OnWorldDeactivate();
    Logf("npclease: ignored legacy save overlay reason=%s; native host state retained", reason);
}
void OnWorldDeactivate()
{
    entries.clear();
    refusals.Clear();
    waiting.Clear();
    transitions.Clear();
    ownTotals = npcown::OwnStats{};
    sessionReadyAt = 0; ownReportAt = 0;
    guardStates.Clear();
    scanAt = 0;
    crashlog::CacheObjects(crashlog::CacheKind::Leases, 0, nullptr);
    seenWorld = net::WorldEpoch(); seenWelcome = net::WelcomeSerial();
}
void CaptureCacheInventory()
{
    void const* first = nullptr;
    for (auto const& item : entries)
        if (item.second.applied) { first = item.second.applied; break; }
    crashlog::CacheObjects(crashlog::CacheKind::Leases, static_cast<uint32_t>(entries.size()), first);
}
bool NewLife(uint8_t const identity[16])
{
    if (!net::IsHost()) return true;
    const auto found = entries.find(Key(identity));
    if (found == entries.end()) return true;
    if (found->second.lease.phase != Phase::Host) {
        Log("FAIL npclease: native respawn of a fenced NPC refused");
        return false;
    }
    entries.erase(found);
    guardStates.Erase(Key(identity));
    return true;
}
void Tick()
{
    if (ai::ShuttingDown() || persist::NativeLoading() || !remote::ControlledBody()) return;
    if (!Install()) return;
    if (seenWorld != net::WorldEpoch() || (!net::IsHost() && seenWelcome != net::WelcomeSerial())) OnWorldDeactivate();
    const uint32_t now = net::Now();
    if (scanAt && now - scanAt < 100) return;
    scanAt = now ? now : 1;
    const DWORD began = GetTickCount();
    if (net::IsHost()) TickHost(now);
    else TickClient(now);
    CaptureCacheInventory();
    const auto elapsedScan = GetTickCount() - began;
    ++scanSamples; scanTotalMs += elapsedScan; scanMaxMs = std::max(scanMaxMs, static_cast<unsigned>(elapsedScan));
    if (!reportAt || now - reportAt >= 5000) {
        unsigned active = 0, dormant = 0, local = 0;
        for (auto const& item : entries) {
            active += item.second.lease.ActiveLeases();
            dormant += item.second.lease.phase == Phase::Dormant;
            local += item.second.local;
        }
        const uint32_t elapsed = reportAt ? now - reportAt : 5000;
        Logf("npclease: role=%s class=all_npc_classes leased=%u client_owned=%u dormant=%u grants=%u recalls=%u recovered=%u refused=%u adopted=%u "
             "state_tx_per_s=%.1f state_rx_per_s=%.1f native_deaths=%u repeated_deaths=%u foreign_contacts=%u "
             "scan_avg_ms=%u scan_max_ms=%u",
             net::IsHost() ? "host" : "client", active, local, dormant, granted, recalled, recovered, refused, discovered,
             stateSent * 1000.0 / elapsed, stateReceived * 1000.0 / elapsed, nativeDeaths, repeatedDeaths, contactsRefused,
             scanSamples ? scanTotalMs / scanSamples : 0, scanMaxMs);
        scanSamples = scanTotalMs = scanMaxMs = 0;
        stateSent = stateReceived = 0; reportAt = now;
    }
}
}
