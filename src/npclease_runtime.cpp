// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_runtime.h"
#include "npclease_client.h"
#include "npclease_save.h"
#include "npclease_guards.h"
#include "npclease_presentation.h"
#include "npclease_admission.h"

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
    bool nativeActivated = false, processingQueued = false;
    bool hasPresentationFloor = false;
    uint8_t previousOwner = 0;
    eCEntity* applied = nullptr;
    bool active = false, prepared = false, cut = false, committed = false, hasBaseline = false;
    bool captureFailed = false;
    bool adopted = false;
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
std::unordered_set<std::string> storyActors;
bool storyCatalogReady = false;
std::unordered_map<GuidKey, std::string, GuidHash> refusalReasons;

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
    if (!entry.lease.epoch || entry.lease.phase == Phase::Host) {
        guardStates.Erase(key);
        return;
    }
    AuthorityGuard guard;
    guard.leased = true;
    guard.processingAllowed = !net::IsHost() && entry.active && entry.world == net::WorldEpoch() &&
        entry.welcome == net::WelcomeSerial() && Running() && !persist::CheckpointBusy() &&
        entry.window.Active(entry.lease.epoch, net::Now());
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
    if (SpeciesName(npc)) return NativeClass::Creature;
    const auto species = static_cast<gESpecies>(npc.NPC.Species);
    return species == gESpecies_Human || species == gESpecies_Orc || species == gESpecies_Goblin
        ? NativeClass::Ordinary : NativeClass::Unsupported;
}

std::string StoryKey(char const* name)
{
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return key;
}
void AddStoryActor(bCString const& name)
{
    if (name.GetLength()) storyActors.insert(StoryKey(name.GetText()));
}
bool StoryCatalog()
{
    if (storyCatalogReady) return true;
    auto* quests = gCQuestManager_PS::GetInstance();
    auto* infos = gCInfoManager_PS::GetInstance();
    if (!quests || !infos || !quests->GetQuests().GetCount() || !infos->GetInfos().GetCount()) return false;
    storyActors.clear();
    for (int i = 0; i < quests->GetQuests().GetCount(); ++i) {
        auto* quest = quests->GetQuests().GetAt(i).operator->();
        if (!quest) return false;
        AddStoryActor(quest->GetDestinationEntity());
        for (int j = 0; j < quest->GetDeliveryEntities().GetCount(); ++j)
            AddStoryActor(quest->GetDeliveryEntities().GetAt(j));
    }
    for (int i = 0; i < infos->GetInfos().GetCount(); ++i) {
        auto* info = infos->GetInfos().GetAt(i);
        if (!info) return false;
        AddStoryActor(info->GetOwner());
        AddStoryActor(info->GetConditionOwnerNearEntity());
        AddStoryActor(info->GetConditionItemContainer());
        for (auto const* names : {&info->GetCondSecondaryNPC(), &info->GetCondPlayerPartyMember()})
            for (int j = 0; j < names->GetCount(); ++j) AddStoryActor(names->GetAt(j));
        auto* script = info->AccessInfoScript().GetObject();
        if (!script) return false;
        for (int j = 0; j < script->GetCommandCount(); ++j) {
            auto* line = script->GetCommand(j);
            if (!line) return false;
            AddStoryActor(line->GetEntity1());
            AddStoryActor(line->GetEntity2());
        }
    }
    storyCatalogReady = true;
    Logf("npclease: story admission catalog ready actors=%u quests=%d infos=%d",
         static_cast<unsigned>(storyActors.size()), quests->GetQuests().GetCount(), infos->GetInfos().GetCount());
    return true;
}
bool GenericName(char const* name, char const* species)
{
    size_t length = std::strlen(species);
    if (std::strncmp(name, species, length)) return false;
    if (!name[length]) return true;
    if (name[length++] != '_') return false;
    for (size_t i = length; name[i]; ++i) if (name[i] < '0' || name[i] > '9') return false;
    return name[length] != '\0';
}
bool ExcludedRole(eCEntity* entity)
{
    if (entity->GetPropertySet(eEPropertySetType_Magic) ||
        entity->GetPropertySet(eEPropertySetType_Item)) return true;
    if (entity->GetPropertySet(eEPropertySetType_Interaction) &&
        Entity(entity).Interaction.GetOwner().GetInstance()) return true;
    auto* dialog = static_cast<gCDialog_PS*>(entity->GetPropertySet(eEPropertySetType_Dialog));
    if (dialog && (dialog->GetTradeEnabled() || dialog->GetTeachEnabled() || dialog->GetPartyEnabled()))
        return true;
    auto* party = static_cast<gCParty_PS*>(entity->GetPropertySet(eEPropertySetType_Party));
    if (!party) return false;
    Entity npc(entity);
    return npc.Party.GetPartyLeader().GetInstance() || npc.Party.Waiting ||
        static_cast<gEPartyMemberType>(npc.Party.PartyMemberType) != gEPartyMemberType_None ||
        party->GetMembers().GetCount() != 0;
}
bool PortableEquipment(Entity& npc, gESlot slot)
{
    const Entity held = npc.Inventory.GetItemFromSlot(slot);
    if (!held.GetInstance()) return true;
    const bool hand = slot == gESlot_RightHand || slot == gESlot_LeftHand;
    const bool natural = held.GetInstance() == npc.GetInstance() ||
        (held.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) &&
         held.Interaction.GetUseType() == gEUseType_Fist);
    const auto* creator = npc.GetInstance()->GetCreator();
    if (!creator || !creator->GetPropertySet(eEPropertySetType_Inventory))
        return PortableCreatureSlot(true, hand, natural, false);
    Entity prototype(const_cast<eCTemplateEntity*>(creator));
    const Entity expected = prototype.Inventory.GetDefaultItemFromSlot(slot);
    const bool same = held.GetInstance() == npc.GetInstance() || (expected.GetInstance() &&
        !std::memcmp(&held.GetInstance()->GetTemplateID().GetCore(),
                     &expected.GetInstance()->GetTemplateID().GetCore(), 16));
    return PortableCreatureSlot(true, hand, natural, same);
}

char const* EquipmentReason(Entity& npc)
{
    if (!PortableEquipment(npc, gESlot_RightHand)) return "unportable right-hand equipment";
    if (!PortableEquipment(npc, gESlot_LeftHand)) return "unportable left-hand equipment";
    if (!PortableEquipment(npc, gESlot_Body)) return "unportable body equipment";
    if (!PortableEquipment(npc, gESlot_Helmet)) return "unportable helmet equipment";
    return nullptr;
}

char const* EligibilityReason(eCEntity* entity, char const* name = nullptr, bool prototype = false)
{
    if (!entity) return "native NPC instance unavailable";
    if (npcremoval::Suppressed(entity)) return "authored removal";
    if (!prototype && (Player(entity) || npcs::RedundantClientEntity(entity))) return "player or duplicate identity";
    if (!entity->GetPropertySet(eEPropertySetType_NPC) ||
        !entity->GetPropertySet(eEPropertySetType_DamageReceiver) ||
        !entity->GetPropertySet(eEPropertySetType_ScriptRoutine) ||
        !entity->GetPropertySet(eEPropertySetType_CharacterMovement) ||
        !entity->GetPropertySet(eEPropertySetType_Inventory) ||
        !entity->GetPropertySet(eEPropertySetType_Navigation)) return "incomplete native NPC/template";
    Entity npc(entity);
    Admission admission;
    admission.type = ClassOf(npc);
    admission.complete = prototype || entity->GetCreator() != nullptr;
    admission.mortal = !npc.NPC.Immortal;
    admission.independent = !ExcludedRole(entity);
    const char* species = SpeciesName(npc);
    const bCString nativeName = npc.GetName();
    const char* instanceName = name ? name : nativeName.GetText();
    if (species) {
        const auto* creator = prototype ? entity : entity->GetCreator();
        admission.portableCreature = GenericName(instanceName, species) && creator &&
            !std::strcmp(creator->GetName().GetText(), species) && (prototype || !EquipmentReason(npc));
    } else if (admission.type == NativeClass::Ordinary) {
        admission.storyCatalog = StoryCatalog();
        admission.storyReferenced = storyActors.count(StoryKey(instanceName)) != 0 ||
            storyActors.count(StoryKey(entity->GetName().GetText())) != 0;
    }
    return admission.Refusal();
}
bool Eligible(eCEntity* entity)
{
    return !EligibilityReason(entity);
}
char const* DiscoveryReason(Checkpoint const& state)
{
    if (!Valid(state) || state.dead || state.status) return "invalid/dead/status-bearing discovery checkpoint";
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
ClientReadiness Readiness(eCEntity* entity, Entry const& entry, uint32_t now)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    ClientReadiness result{Running(), GetConfig().npcAuthorityFallback, persist::CheckpointBusy(),
        Processed(entity), animation && animation->GetActor(), Eligible(entity),
        Reached(now, entry.localDeadline)};
    result.processingEnabled = entity && entity->IsEnabled() && !entity->IsProcessingDisabled();
    result.processable = world::ProcessingOf(entity).processable;
    auto* hero = remote::ControlledBody();
    if (!net::IsHost()) {
        result.insideRange = entity && hero &&
            LeaseProcessingRange(true, (entity->GetWorldPosition() - hero->GetWorldPosition()).GetMagnitude());
    }
    result.collecting = entry.processingQueued &&
        AwaitingInitialCollection(entry.nativeActivated, entry.activationAt, now);
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
        body::LocallyAttacking(entity) || (target && !TargetPlayer(target)) || talk::IsHeld(entity) ||
        state.status != 0);
    if (state.dead) {
        state.hp = 0;
        auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
        state.busy = animation && animation->GetRagDollEnabled() && animation->GetRagDollController();
    }
    return Valid(state);
}
void Stop(eCEntity* entity)
{
    if (!entity || !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;
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
    unsigned clients = 0;
    for (uint8_t i = 2; i < net::MaxPlayers; ++i) clients += net::InWorld(i) && net::Remotes()[i].connected;
    value.conflict = clients != 1;
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
bool Send(GuidKey const& key, Entry& entry, Kind kind)
{
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
    ++refused;
    auto found = refusalReasons.find(key);
    if (found != refusalReasons.end() && found->second == reason) return;
    if (found != refusalReasons.end()) found->second = reason;
    else if (refusalReasons.size() < MaxLeases) refusalReasons.emplace(key, reason);
    char text[40];
    guid::Format(key.data(), text, sizeof(text));
    Logf("npclease: admission-refused guid=%s name=%s reason=%s", text, name, reason);
}
void ReadinessDiagnostic(char const* action, GuidKey const& key, Entry const& entry, eCEntity* entity,
                         ClientReadiness const& readiness, char const* why)
{
    Diagnostic(action, key, entry, why);
    char text[40];
    guid::Format(key.data(), text, sizeof(text));
    Entity npc(entity);
    Logf("npclease: readiness guid=%s epoch=%u revision=%u active=%u processed=%u actor=%u eligible=%u "
         "running=%u enabled=%u checkpoint=%u remaining_ms=%d last_hit_age_ms=%u hp=%d ai=%d task=%s state=%s "
         "disabled=%u right=%s left=%s body=%s helmet=%s eligibility_reason=%s "
         "reason=%s ptr=%p e0=%u e8=%u entity_enabled=%u native=%u collecting=%u "
         "e0_changed_age_ms=%u e8_changed_age_ms=%u hero_cm=%.0f camera_cm=%.0f",
         text, entry.lease.epoch, entry.lease.revision, entry.active ? 1 : 0,
         readiness.processed ? 1 : 0, readiness.actor ? 1 : 0, readiness.eligible ? 1 : 0,
         readiness.running ? 1 : 0, readiness.enabled ? 1 : 0, readiness.checkpointBusy ? 1 : 0,
         static_cast<int32_t>(entry.localDeadline - net::Now()),
         entry.lastHit ? net::Now() - entry.lastHit : UINT32_MAX,
         entity ? static_cast<int>(npc.DamageReceiver.HitPoints) : entry.lease.state.hp,
         entity ? static_cast<int>(npc.Routine.AIMode) : -1,
         entity ? npc.Routine.GetCurrentTask().GetText() : "-",
         entity ? npc.Routine.GetCurrentState().GetText() : "-",
         entity && entity->IsProcessingDisabled() ? 1 : 0,
         entity ? npc.Inventory.GetItemFromSlot(gESlot_RightHand).GetName().GetText() : "-",
         entity ? npc.Inventory.GetItemFromSlot(gESlot_LeftHand).GetName().GetText() : "-",
         entity ? npc.Inventory.GetItemFromSlot(gESlot_Body).GetName().GetText() : "-",
         entity ? npc.Inventory.GetItemFromSlot(gESlot_Helmet).GetName().GetText() : "-",
         readiness.eligible ? "eligible" : entity && EquipmentReason(npc) ? EquipmentReason(npc) :
             UnavailabilityReason(entity),
         why, entity, world::ProcessingOf(entity).range ? 1 : 0,
         readiness.processable ? 1 : 0, entity && entity->IsEnabled() ? 1 : 0,
         entry.nativeActivated ? 1 : 0, readiness.collecting ? 1 : 0,
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
        if (entry.lease.ActiveLeases()) return;
        eCEntity* native = npcs::NpcByGuid(note.guid);
        Checkpoint baseline;
        if (native && CaptureNative(native, baseline)) {
            if (entry.lease.state.dead) baseline = entry.lease.state;
            entry.hasBaseline = true;
        } else if (native) {
            Refusal(key, note.state.name, EligibilityReason(native) ? EligibilityReason(native) :
                "host instance exists but canonical capture unavailable");
            return;
        } else if (entry.hasBaseline && Valid(entry.lease.state)) baseline = entry.lease.state;
        else if (HostUnknown(native != nullptr, npcs::HostKnows(note.guid), entry.hasBaseline) &&
                 note.incarnation == 1 && ValidDiscovery(note.state)) {
            baseline = note.state;
            entry.lease.state = baseline;
            entry.hasBaseline = true;
            if (!entry.adopted) {
                entry.adopted = true;
                ++discovered;
                Diagnostic("discovered", key, entry, "verified native template; no host instance created");
            }
        } else {
            Refusal(key, note.state.name, npcs::HostKnows(note.guid) ?
                "host-known GUID without canonical baseline; discovery refused" :
                DiscoveryReason(note.state) ? DiscoveryReason(note.state) : "host canonical baseline unavailable");
            return;
        }
        if (std::memcmp(note.state.templateId, baseline.templateId, 16) ||
            std::strcmp(note.state.name, baseline.name)) {
            Refusal(key, note.state.name, "discovery identity does not match canonical template/name"); return;
        }
        entry.lease.state = baseline;
        if (!MayDelegate(ClassOf(baseline),
                HostUnknown(native != nullptr, npcs::HostKnows(note.guid), false),
                entry.adopted && !native)) {
            Refusal(key, baseline.name, "ordinary NPC has a host instance/record; host-owned");
            return;
        }
        auto interest = InterestOf(baseline, from, native);
        interest.hostPresent = interest.hostPresent && npcs::HostCanPresent(note.guid);
        if (!entry.interest.Delegate(interest, now)) return;
        if (baseline.status || baseline.dead) {
            Refusal(key, baseline.name, "canonical NPC life/status not transferable"); return;
        }
        if (baseline.busy && interest.hostPresent) {
            if (!entry.recallAt) entry.recallAt = now ? now : 1;
            if (now - entry.recallAt < RecallWaitMs) return;
        }
        if (native && !npcplayback::GuardForLease(native)) {
            Refusal(key, baseline.name, "native property authority fence unavailable"); return;
        }
        if (authoritySerial == UINT32_MAX) { ++refused; Log("FAIL npclease: authority epoch exhausted"); return; }
        entry.world = note.world; entry.echo = note.echo;
        entry.recallAt = 0;
        entry.lease.incarnation = entry.adopted ? note.incarnation : npcs::Incarnation(note.guid);
        entry.previousOwner = from;
        entry.previousGeneration = net::PeerGeneration(from);
        if (!entry.lease.Offer(from, net::PeerGeneration(from), ++authoritySerial, now, baseline)) { ++refused; return; }
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
            Diagnostic("grant", key, entry, "client imported inert baseline");
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
            if ((entry.lease.phase == Phase::Client || renewRecall) && Running() && !persist::CheckpointBusy()) {
                entry.lease.deadline = now + LeaseMs + LeaseSafetyMs;
                Send(key, entry, Kind::Renew);
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
void ReceiveClient(Message const& note)
{
    const auto key = Key(note.guid);
    const uint32_t now = net::Now();
    if (note.kind == Kind::Offer) {
        if (!GetConfig().npcAuthorityFallback || !Running() || note.owner != net::LocalId() ||
            Reached(now + LeaseSafetyMs, note.echo + note.duration)) { ++refused; return; }
        auto it = entries.find(key);
        if (it == entries.end() || (it->second.lease.epoch && !Newer(note.epoch, it->second.lease.epoch))) {
            ++refused; return;
        }
        Entry& entry = it->second;
        eCEntity* entity = npcs::NpcByGuid(note.guid);
        if (!NativeReady(entity) || !Eligible(entity) || !npcplayback::GuardForLease(entity)) { ++refused; return; }
        if (!entry.window.Prepare(note.epoch, note.echo, note.duration, now)) { ++refused; return; }
        entry.active = false; entry.prepared = true; entry.cut = false; entry.committed = false;
        entry.world = note.world; entry.welcome = net::WelcomeSerial();
        entry.lease.owner = note.owner; entry.lease.generation = note.generation;
        entry.lease.incarnation = note.incarnation; entry.lease.epoch = note.epoch;
        entry.lease.started = entry.unavailableAt ? entry.unavailableAt : now;
        entry.lease.revision = note.revision; entry.lease.state = note.state; entry.lease.phase = Phase::Offering;
        entry.hasBaseline = true;
        std::memcpy(entry.sentRoutine, note.state.routine, sizeof(entry.sentRoutine));
        entry.localDeadline = note.echo + note.duration - LeaseSafetyMs;
        entry.echo = now;
        PublishGuard(key, entry);
        if (!Import(entity, note.state)) { ++refused; entry.prepared = false; return; }
        Send(key, entry, Kind::Prepared);
        return;
    }
    const auto found = entries.find(key);
    if (found == entries.end()) { ++refused; return; }
    Entry& entry = found->second;
    if (note.kind == Kind::Commit && note.owner == 1 && note.world == entry.world &&
        note.incarnation == entry.lease.incarnation && Newer(note.epoch, entry.lease.epoch) && entry.cut) {
        entry.active = false;
        entry.lease.epoch = note.epoch; entry.lease.owner = 1; entry.lease.phase = Phase::Host;
        entry.lease.generation = note.generation;
        entry.lease.state = note.state; entry.lease.revision = note.revision;
        entry.presentationFloor = note.echo;
        entry.hasPresentationFloor = true;
        entry.prepared = false; entry.cut = false; entry.recallAt = 0;
        entry.unavailableAt = 0;
        entry.window.Reset(note.epoch);
        entry.echo = now;
        PublishGuard(key, entry);
        Send(key, entry, Kind::Ack);
        Diagnostic("hand-back", key, entry, "host committed donor cut");
        return;
    }
    if (!Exact(note, entry)) { ++refused; return; }
    if (note.kind == Kind::Commit) {
        if (entry.lease.phase == Phase::Host && note.owner == 1) { Send(key, entry, Kind::Ack); return; }
        if (entry.active) {
            if (entry.window.Active(note.epoch, now)) Send(key, entry, Kind::Ack);
            else ++refused;
            return;
        }
        if (!entry.prepared || entry.cut || !Running() || !entry.window.Commit(note.epoch, now)) { ++refused; return; }
        entry.lease.phase = Phase::Client;
        entry.active = true; entry.prepared = false; entry.echo = now;
        entry.nativeActivated = false; entry.activationAt = now; entry.ownerReportAt = 0;
        auto* entity = npcs::NpcByGuid(note.guid);
        PublishGuard(key, entry);
        entry.processingQueued = false;
        if (entity && !entity->IsProcessingDisabled() && Eligible(entity) &&
            ProcessingAuthority(entity) && npcplayback::GuardForLease(entity)) {
            Entity(entity).EnableProcessing(GETrue);
            entry.processingQueued = world::QueueEntityProcessing(entity);
        }
        Send(key, entry, Kind::Ack);
        ObserveProcessing(entry, entity, now);
        ReadinessDiagnostic("activation-queued", key, entry, entity, Readiness(entity, entry, now),
            entry.processingQueued ? "committed permit; awaiting native e0/e8 collection" :
                                     "native processing registration unavailable; donor fenced");
    } else if (note.kind == Kind::Renew) {
        if (!entry.active || entry.cut || !entry.window.Renew(note.epoch, note.echo, note.duration, now)) { ++refused; return; }
        entry.localDeadline = entry.window.deadline;
        entry.lease.state.effects |= note.state.effects;
        PublishGuard(key, entry);
    } else if (note.kind == Kind::Recall) {
        if (!entry.recallAt) entry.recallAt = now ? now : 1;
    } else if (note.kind == Kind::Abort) {
        entry.active = false; entry.prepared = false; entry.lease.phase = Phase::Host;
        entry.cut = false; entry.recallAt = 0;
        entry.window.Reset(note.epoch);
        PublishGuard(key, entry);
        Stop(npcs::NpcByGuid(note.guid));
        Send(key, entry, Kind::Ack);
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
    if (Send(Key(victim), *entry, Kind::State)) ++stateSent;
    ++nativeDeaths;
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
    Log("npclease: native authority ready; creature/discovered-ordinary admission=on absence_dwell_ms=0 lease_ms=3000 state_hz=5");
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
            if (entry.interest.Recall(value) || !Running() || entry.lease.state.dead) {
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
                if (entry.lease.HandBack(++authoritySerial, now)) {
                    entry.committed = false;
                    if (Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data()))) {
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
            if (Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data())) && client > 1)
                net::SendNpcLease(client, Control(key, entry, Kind::Commit));
        }
        PublishGuard(key, entry);
    }
}
void TickClient(uint32_t now)
{
    for (auto& item : entries) {
        Entry& entry = item.second;
        if (entry.prepared && Reached(now, entry.localDeadline)) {
            entry.prepared = false; entry.lease.phase = Phase::Host; entry.window.Freeze();
            PublishGuard(item.first, entry);
            Stop(npcs::NpcByGuid(reinterpret_cast<uint8_t const*>(item.first.data())));
            Diagnostic("prepare-expired", item.first, entry, "late commit cannot activate donor");
        }
        if (!entry.active) continue;
        auto* entity = npcs::NpcByGuid(reinterpret_cast<uint8_t const*>(item.first.data()));
        ObserveProcessing(entry, entity, now);
        auto readiness = Readiness(entity, entry, now);
        const char* lost = readiness.LossReason();
        if (!entry.nativeActivated && !lost) {
            if (!entry.processingQueued) lost = "native processing registration unavailable";
            else if (readiness.processed && readiness.processable) {
                if (Activate(entity, entry, item.first.data())) {
                    readiness = Readiness(entity, entry, now);
                    lost = readiness.LossReason();
                    ReadinessDiagnostic("active", item.first, entry, entity, readiness,
                        lost ? lost : "native client AI; collected baseline imported and playback retired");
                    entry.ownerReportAt = now;
                } else {
                    entry.active = true;
                    lost = "native guard/import/restart unavailable";
                }
            } else continue;
        }
        Checkpoint state = entry.lease.state;
        const bool captured = entry.nativeActivated && CaptureNative(entity, state);
        if (!captured && !entry.captureFailed)
            ReadinessDiagnostic("state-unavailable", item.first, entry, entity, readiness,
                lost ? lost : "native checkpoint invalid; renewal cannot advance");
        entry.captureFailed = !captured;
        if (entry.nativeActivated && (!entry.ownerReportAt || now - entry.ownerReportAt >= 1000)) {
            ReadinessDiagnostic("owner", item.first, entry, entity, readiness, lost ? lost : "native owner retained");
            entry.ownerReportAt = now;
        }
        if (lost || (entry.recallAt && captured && (!state.busy || now - entry.recallAt >= RecallWaitMs))) {
            // Status tasks cannot be restarted from a scalar duration; defer their cut while the lease remains healthy.
            if (!lost && !TransferableStatus(state)) continue;
            entry.active = false; entry.cut = true; entry.lease.phase = Phase::Frozen;
            entry.window.Freeze();
            if (captured) entry.lease.state = state;
            ++entry.lease.revision; entry.echo = now;
            PublishGuard(item.first, entry);
            ReadinessDiagnostic("frozen", item.first, entry, entity, readiness, lost ? lost : "safe recall cut");
            entry.nativeActivated = false;
            Stop(entity);
            Send(item.first, entry, Kind::Cut);
            continue;
        }
        const bool event = captured && (state.hp != entry.lease.state.hp || state.dead != entry.lease.state.dead ||
            state.attitude != entry.lease.state.attitude || state.status != entry.lease.state.status ||
            state.defeated != entry.lease.state.defeated);
        if (captured && (event || now - entry.lastState >= StateMs)) {
            entry.lease.state = state;
            if (++entry.lease.revision == 0) {
                entry.active = false; PublishGuard(item.first, entry); Stop(entity);
                Log("FAIL npclease: revision exhausted"); continue;
            }
            entry.echo = now;
            if (event) Diagnostic("state-event", item.first, entry, "native HP/life/attitude/status changed; owner unchanged");
            if (Send(item.first, entry, Kind::State)) { entry.lastState = now; ++stateSent; }
        }
    }
    if (!Running() || persist::CheckpointBusy() || !GetConfig().npcAuthorityFallback) return;
    const auto* all = world::ProcessedEntities();
    eCEntity* hero = remote::ControlledBody();
    const auto& p = hero->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    for (GEU16 i = 0; all && i < all->GetCount(); ++i) {
        eCEntity* entity = all->GetAt(i);
        if (!npcplayback::IsCharacter(entity) || Player(entity) || npcs::RedundantClientEntity(entity) ||
            npcremoval::Suppressed(entity)) continue;
        if (auto const* why = EligibilityReason(entity)) {
            if (!npcs::HostPresentationAvailable(entity)) Refusal(Key(entity), entity->GetName().GetText(), why);
            continue;
        }
        if (!NativeReady(entity)) continue;
        const auto key = Key(entity);
        const auto found = entries.find(key);
        if (npcs::HostPresentationAvailable(entity)) {
            if (found != entries.end() && found->second.lease.phase == Phase::Host)
                found->second.unavailableAt = 0;
            continue;
        }
        if (found == entries.end() && !CapacityAvailable()) continue;
        Entry& entry = entries[key];
        if (!entry.unavailableAt) entry.unavailableAt = now ? now : 1;
        if (entry.active || entry.prepared || entry.cut || now - entry.lastRequest < 500) continue;
        Checkpoint state;
        if (!CaptureNative(entity, state) || state.dead || state.status ||
            body::Distance(here, state.position) > ClientRadiusCm) continue;
        entry.world = net::WorldEpoch(); entry.lease.state = state; entry.lease.owner = net::LocalId();
        entry.hasBaseline = true;
        entry.lease.incarnation = npcs::Incarnation(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entry.lease.incarnation) entry.lease.incarnation = 1;
        entry.echo = now;
        const auto kind = npcs::HostPresentationAge(entity) == UINT32_MAX ? Kind::Adopt : Kind::Request;
        if (Send(key, entry, kind)) entry.lastRequest = now;
    }
}
}

bool LocalAuthority(eCEntity const* entity)
{
    return ReadGuard(entity).Local(net::Now());
}
bool ProcessingAuthority(eCEntity const* entity)
{
    auto* hero = remote::ControlledBody();
    return entity && hero && !net::IsHost() && !npcs::RedundantClientEntity(entity) &&
        !npcremoval::Suppressed(entity) && LeaseProcessingRange(ReadGuard(entity).Processing(net::Now()),
        (entity->GetWorldPosition() - hero->GetWorldPosition()).GetMagnitude());
}
void NoteHit(eCEntity* victim, eCEntity* source, char const* path, int before, int after)
{
    Entry* entry = Find(victim);
    if (!entry || net::IsHost() || !entry->active || entry->lease.owner != net::LocalId()) return;
    const uint32_t now = net::Now();
    entry->lastHit = now ? now : 1;
    const auto readiness = Readiness(victim, *entry, now);
    char text[40];
    guid::Format(Key(victim).data(), text, sizeof(text));
    Entity npc(victim);
    Logf("npclease: hit guid=%s epoch=%u revision=%u path=%s attacker=%s hp=%d->%d local=%u "
         "processed=%u actor=%u remaining_ms=%d ai=%d task=%s state=%s reason=%s",
         text, entry->lease.epoch, entry->lease.revision, path, source ? Entity(source).GetName().GetText() : "-",
         before, after, LocalAuthority(victim) ? 1 : 0, readiness.processed ? 1 : 0, readiness.actor ? 1 : 0,
         static_cast<int32_t>(entry->localDeadline - now), static_cast<int>(npc.Routine.AIMode),
         npc.Routine.GetCurrentTask().GetText(), npc.Routine.GetCurrentState().GetText(),
         readiness.LossReason() ? readiness.LossReason() : "native owner retained");
}
char const* UnavailabilityReason(eCEntity* entity)
{
    if (!GetConfig().npcAuthorityFallback) return "fallback disabled; host-only presentation unavailable";
    if (installFailed) return "native authority guard installation failed; host-owned";
    if (!installed) return "native authority guard initialization pending";
    if (auto const* reason = EligibilityReason(entity)) return reason;
    const auto refusedGuid = refusalReasons.find(Key(entity));
    if (refusedGuid != refusalReasons.end()) return refusedGuid->second.c_str();
    return "awaiting reliable host-authorized native cut";
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
    return (entry.lease.epoch && entry.lease.phase != Phase::Host) ||
        (entry.hasPresentationFloor && !Newer(captured, entry.presentationFloor));
}
bool IgnoreHostSnapshot(uint8_t const identity[16])
{
    return !net::IsHost() && SuppressHostPublication(identity);
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
    storyActors.clear();
    storyCatalogReady = false;
    refusalReasons.clear();
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
        unsigned active = 0, dormant = 0;
        for (auto const& item : entries) {
            active += item.second.lease.ActiveLeases();
            dormant += item.second.lease.phase == Phase::Dormant;
        }
        const uint32_t elapsed = reportAt ? now - reportAt : 5000;
        Logf("npclease: role=%s class=creature_or_discovered_ordinary leased=%u dormant=%u grants=%u recalls=%u recovered=%u refused=%u adopted=%u "
             "state_tx_per_s=%.1f state_rx_per_s=%.1f native_deaths=%u repeated_deaths=%u foreign_contacts=%u "
             "scan_avg_ms=%u scan_max_ms=%u",
             net::IsHost() ? "host" : "client", active, dormant, granted, recalled, recovered, refused, discovered,
             stateSent * 1000.0 / elapsed, stateReceived * 1000.0 / elapsed, nativeDeaths, repeatedDeaths, contactsRefused,
             scanSamples ? scanTotalMs / scanSamples : 0, scanMaxMs);
        scanSamples = scanTotalMs = scanMaxMs = 0;
        stateSent = stateReceived = 0; reportAt = now;
    }
}
}
