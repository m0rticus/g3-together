// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_runtime.h"
#include "npclease_client.h"
#include "npclease_save.h"
#include "npclease_guards.h"

#include "ai.h"
#include "body.h"
#include "config.h"
#include "guid.h"
#include "journal.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "persist.h"
#include "remote.h"
#include "talk.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

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
    uint8_t previousOwner = 0;
    eCEntity* applied = nullptr;
    bool active = false, prepared = false, cut = false, committed = false, hasBaseline = false;
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
unsigned scanSamples = 0, scanTotalMs = 0, scanMaxMs = 0;
bool installed = false, installFailed = false;
gFScript originalKill = nullptr;
using QuestFn = GEBool(__fastcall*)(gCQuestManager_PS*, void*, eCEntity const&);
QuestFn originalQuest = nullptr;
thread_local eCEntity* ownerDeath = nullptr;

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
    guard.locallyDriven = !net::IsHost() && entry.active && entry.world == net::WorldEpoch() &&
        entry.welcome == net::WelcomeSerial() && Running() && !persist::CheckpointBusy() &&
        entry.window.Active(entry.lease.epoch, net::Now());
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
    default: return nullptr;
    }
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
bool Eligible(eCEntity* entity)
{
    if (!entity || Player(entity) || !entity->GetPropertySet(eEPropertySetType_NPC) ||
        !entity->GetPropertySet(eEPropertySetType_DamageReceiver) ||
        !entity->GetPropertySet(eEPropertySetType_ScriptRoutine) ||
        !entity->GetPropertySet(eEPropertySetType_CharacterMovement) ||
        !entity->GetPropertySet(eEPropertySetType_Inventory) ||
        !entity->GetPropertySet(eEPropertySetType_Navigation)) return false;
    Entity npc(entity);
    const char* species = SpeciesName(npc);
    if (!species || !GenericName(npc.GetName().GetText(), species) || npc.NPC.Immortal) return false;
    const auto* creator = entity->GetCreator();
    if (!creator || std::strcmp(creator->GetName().GetText(), species)) return false;
    if (entity->GetPropertySet(eEPropertySetType_Party) &&
        (npc.Party.GetPartyLeader().GetInstance() || npc.Party.Waiting)) return false;
    if (talk::IsHeld(entity) || talk::TalkingWithHero(entity)) return false;
    for (auto slot : {gESlot_RightHand, gESlot_LeftHand, gESlot_Body, gESlot_Helmet})
        if (npc.Inventory.GetItemFromSlot(slot).GetInstance()) return false;
    return true;
}
bool Processed(eCEntity const* entity)
{
    if (!entity || entity->IsProcessingDisabled()) return false;
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) if (all.GetAt(i).GetInstance() == entity) return true;
    return false;
}
bool NativeReady(eCEntity* entity)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    return Processed(entity) && animation && animation->GetActor();
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
    entry.lease.phase = net::IsHost() ? Phase::Dormant : Phase::Frozen;
    PublishGuard(Key(identity), entry);
    if (!NativeReady(entity) || !npcplayback::GuardForLease(entity)) return false;
    npcs::LeaseReleaseMirror(identity);
    if (!Import(entity, entry.lease.state)) return false;
    entry.lease.phase = resume;
    entry.active = active;
    PublishGuard(Key(identity), entry);
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
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* entity = all.GetAt(i).GetInstance();
        if (!entity || Player(entity) || Eligible(entity) || Entity(entity).IsDead()) continue;
        const auto& p = entity->GetWorldPosition();
        const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
        if (body::Distance(at, state.position) < 4000) { value.conflict = true; break; }
    }
    return value;
}
Message Control(GuidKey const& key, Entry const& entry, Kind kind)
{
    Message note;
    note.kind = kind; note.world = entry.world; note.epoch = entry.lease.epoch;
    note.owner = entry.lease.owner; note.generation = entry.lease.generation;
    note.incarnation = entry.lease.incarnation; note.revision = entry.lease.revision;
    note.echo = entry.echo; note.state = entry.lease.state;
    std::memcpy(note.guid, key.data(), 16);
    return note;
}
bool Send(GuidKey const& key, Entry& entry, Kind kind)
{
    const auto note = Control(key, entry, kind);
    if (!net::SendNpcLease(net::IsHost() ? entry.lease.owner : 1, note)) return false;
    entry.lastControl = net::Now();
    return true;
}
void Diagnostic(char const* action, GuidKey const& key, Entry const& entry, char const* why)
{
    char text[40];
    guid::Format(reinterpret_cast<uint8_t const*>(key.data()), text, sizeof(text));
    const uint32_t began = entry.recallAt ? entry.recallAt : entry.lease.started;
    Logf("npclease: %s guid=%s owner=%u epoch=%u life=%u revision=%u reason=%s duration_ms=%u",
         action, text, entry.lease.owner, entry.lease.epoch, entry.lease.incarnation, entry.lease.revision,
         why, began ? net::Now() - began : 0);
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
    if (note.kind == Kind::Request) {
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
        } else if (entry.hasBaseline && Valid(entry.lease.state)) baseline = entry.lease.state;
        else {
            ++refused;
            if (!entry.lastRequest || now - entry.lastRequest >= 5000) {
                Diagnostic("refused", key, entry, "no canonical host instance/baseline");
                entry.lastRequest = now ? now : 1;
            }
            return;
        }
        if (std::memcmp(note.state.templateId, baseline.templateId, 16) ||
            std::strcmp(note.state.name, baseline.name)) { ++refused; return; }
        entry.lease.state = baseline;
        auto interest = InterestOf(baseline, from, native);
        if (!entry.interest.Delegate(interest, now)) return;
        if (baseline.status || baseline.dead) { ++refused; return; }
        if (baseline.busy) {
            if (!entry.recallAt) entry.recallAt = now ? now : 1;
            if (now - entry.recallAt < RecallWaitMs) return;
        }
        if (native && !npcplayback::GuardForLease(native)) { ++refused; return; }
        if (authoritySerial == UINT32_MAX) { ++refused; Log("FAIL npclease: authority epoch exhausted"); return; }
        entry.world = note.world; entry.echo = note.echo;
        entry.recallAt = 0;
        entry.lease.incarnation = npcs::Incarnation(note.guid);
        entry.previousOwner = from;
        entry.previousGeneration = net::PeerGeneration(from);
        if (!entry.lease.Offer(from, net::PeerGeneration(from), ++authoritySerial, now, baseline)) { ++refused; return; }
        PublishGuard(key, entry);
        Stop(native);
        Send(key, entry, Kind::Offer);
        Diagnostic("offer", key, entry, "outside primary host reach");
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
    } else if (note.kind == Kind::State) {
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
        entry.lease.started = now;
        entry.lease.revision = note.revision; entry.lease.state = note.state; entry.lease.phase = Phase::Offering;
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
        entry.prepared = false; entry.cut = false; entry.recallAt = 0;
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
        if (!Activate(npcs::NpcByGuid(note.guid), entry, note.guid)) {
            entry.active = false; entry.lease.phase = Phase::Frozen;
            Log("FAIL npclease: committed client instance is unavailable; authority remains fenced");
            return;
        }
        Send(key, entry, Kind::Ack);
        Diagnostic("active", key, entry, "native client AI");
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
    if (!installed || !Valid(note) || note.world != net::WorldEpoch() || persist::NativeLoading()) { ++refused; return; }
    if (net::IsHost()) ReceiveHost(from, note);
    else if (from == 1 && note.owner == net::LocalId()) ReceiveClient(note);
    else if (from == 1 && note.kind == Kind::Commit && note.owner == 1) ReceiveClient(note);
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
    Log("npclease: restricted wildlife authority ready; lease_ms=3000 state_hz=5 primary_far_cm=max(12000,ROI+2000)");
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
                    const auto note = Control(key, entry, Kind::Commit);
                    net::SendNpcLease(client, note);
                    if (Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data())))
                        Diagnostic("host-active", key, entry, "imported frozen client cut");
                    else Diagnostic("host-held", key, entry, "native restart unavailable");
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
            if (client > 1) net::SendNpcLease(client, Control(key, entry, Kind::Commit));
            Activate(entity, entry, reinterpret_cast<uint8_t const*>(key.data()));
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
        Checkpoint state = entry.lease.state;
        const bool captured = CaptureNative(entity, state);
        const bool lost = !Running() || !GetConfig().npcAuthorityFallback || persist::CheckpointBusy() ||
            !NativeReady(entity) || Reached(now, entry.localDeadline);
        if (lost || (entry.recallAt && captured && (!state.busy || now - entry.recallAt >= RecallWaitMs))) {
            // Status tasks cannot be restarted from a scalar duration; defer their cut while the lease remains healthy.
            if (!lost && !TransferableStatus(state)) continue;
            entry.active = false; entry.cut = true; entry.lease.phase = Phase::Frozen;
            entry.window.Freeze();
            if (captured) entry.lease.state = state;
            ++entry.lease.revision; entry.echo = now;
            PublishGuard(item.first, entry);
            Stop(entity);
            Send(item.first, entry, Kind::Cut);
            Diagnostic("frozen", item.first, entry, lost ? "lease/readiness/barrier" : "safe recall cut");
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
            if (Send(item.first, entry, Kind::State)) { entry.lastState = now; ++stateSent; }
        }
    }
    if (!Running() || persist::CheckpointBusy() || !GetConfig().npcAuthorityFallback) return;
    const auto all = Entity::GetNPCs();
    eCEntity* hero = remote::ControlledBody();
    const auto& p = hero->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* entity = all.GetAt(i).GetInstance();
        if (!Eligible(entity) || !NativeReady(entity)) continue;
        const auto key = Key(entity);
        const auto found = entries.find(key);
        if (found == entries.end() && !CapacityAvailable()) continue;
        Entry& entry = entries[key];
        if (entry.active || entry.prepared || entry.cut || now - entry.lastRequest < 500) continue;
        Checkpoint state;
        if (!CaptureNative(entity, state) || state.dead || state.status || body::Distance(here, state.position) > 4000) continue;
        entry.world = net::WorldEpoch(); entry.lease.state = state; entry.lease.owner = net::LocalId();
        entry.lease.incarnation = npcs::Incarnation(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entry.lease.incarnation) entry.lease.incarnation = 1;
        entry.echo = now;
        if (Send(key, entry, Kind::Request)) entry.lastRequest = now;
    }
}
}

bool LocalAuthority(eCEntity const* entity)
{
    return ReadGuard(entity).Local(net::Now());
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
    SaveOverlay result;
    if (!net::IsHost()) return result;
    for (auto const& item : entries) {
        const auto& lease = item.second.lease;
        if (!lease.epoch || !Valid(lease.state)) continue;
        Saved saved;
        std::memcpy(saved.guid, item.first.data(), 16);
        saved.incarnation = lease.incarnation; saved.epoch = lease.epoch; saved.revision = lease.revision;
        saved.state = lease.state;
        result.push_back(saved);
    }
    return result;
}
bool Restore(SaveOverlay const& overlay)
{
    std::vector<uint8_t> bytes;
    if (!EncodeOverlay(overlay, bytes)) {
        OnWorldDeactivate();
        Log("FAIL npclease: invalid required save overlay; leases revoked, native host state retained");
        return false;
    }
    OnWorldDeactivate();
    for (auto const& saved : overlay) {
        Entry& entry = entries[Key(saved.guid)];
        entry.world = net::WorldEpoch(); entry.lease.state = saved.state;
        entry.lease.incarnation = saved.incarnation; entry.lease.epoch = saved.epoch;
        entry.lease.revision = saved.revision; entry.lease.phase = Phase::Dormant;
        entry.hasBaseline = true;
        authoritySerial = std::max(authoritySerial, saved.epoch);
        PublishGuard(Key(saved.guid), entry);
    }
    seenWorld = net::WorldEpoch(); seenWelcome = net::WelcomeSerial();
    Logf("npclease: restored %u dormant canonical NPCs; no saved lease owner resumed", static_cast<unsigned>(overlay.size()));
    return true;
}
bool WriteSnapshot(FILE* file)
{
    std::string text;
    if (!file || !EncodeSaveText(Capture(), text) || fwrite(text.data(), 1, text.size(), file) != text.size()) {
        Log("FAIL npclease: required save overlay capture/write failed");
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
    Logf("FAIL npclease: required save overlay %s; leases revoked, native host state retained", reason);
}
void OnWorldDeactivate()
{
    entries.clear();
    guardStates.Clear();
    scanAt = 0;
    seenWorld = net::WorldEpoch(); seenWelcome = net::WelcomeSerial();
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
    const auto elapsedScan = GetTickCount() - began;
    ++scanSamples; scanTotalMs += elapsedScan; scanMaxMs = std::max(scanMaxMs, static_cast<unsigned>(elapsedScan));
    if (!reportAt || now - reportAt >= 5000) {
        unsigned active = 0, dormant = 0;
        for (auto const& item : entries) {
            active += item.second.lease.ActiveLeases();
            dormant += item.second.lease.phase == Phase::Dormant;
        }
        const uint32_t elapsed = reportAt ? now - reportAt : 5000;
        Logf("npclease: role=%s leased=%u dormant=%u grants=%u recalls=%u recovered=%u refused=%u "
             "state_tx_per_s=%.1f state_rx_per_s=%.1f native_deaths=%u repeated_deaths=%u foreign_contacts=%u "
             "scan_avg_ms=%u scan_max_ms=%u",
             net::IsHost() ? "host" : "client", active, dormant, granted, recalled, recovered, refused,
             stateSent * 1000.0 / elapsed, stateReceived * 1000.0 / elapsed, nativeDeaths, repeatedDeaths, contactsRefused,
             scanSamples ? scanTotalMs / scanSamples : 0, scanMaxMs);
        scanSamples = scanTotalMs = scanMaxMs = 0;
        stateSent = stateReceived = 0; reportAt = now;
    }
}
}
