// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_runtime.h"
#include "npclease_client.h"
#include "npclease_save.h"
#include "npclease_guards.h"
#include "npclease_presentation.h"
#define G3T_NPCLEASE_NATIVE_RUNTIME
#include "npclease_admission.h"
#undef G3T_NPCLEASE_NATIVE_RUNTIME
#include "npcown_policy.h"

#include "ai.h"
#include "body.h"
#include "combat.h"
#include "config.h"
#include "guards.h"
#include "guid.h"
#include "journal.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "npcremoval.h"
#include "npcremoval_policy.h"
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
namespace detail {
size_t GuidHash::operator()(GuidKey const& key) const { return net::HashGuid(key.data()); }
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
    guard.hostClaimed = !net::IsHost() && HostWins(key);
    guardStates.Put(key, guard);
}
void SyncGuardHostClaims()
{
    for (auto const& item : entries) {
        const auto guard = guardStates.Get(item.first);
        if (guard.leased && guard.hostClaimed != HostWins(item.first)) PublishGuard(item.first, item.second);
    }
}
AuthorityGuard ReadGuard(eCEntity const* entity)
{
    if (!entity) return {};
    return guardStates.Get(Key(entity));
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

char const* EligibilityReason(eCEntity* entity, char const*, bool prototype)
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
bool CaptureNative(eCEntity* entity, Checkpoint& state, bool allowRoleTransition)
{
    if (!entity) return false;
    auto role = RoleOf(entity, false);
    if (allowRoleTransition) role.summonOrFollower = false;
    if (npcown::Exclusion(role)) return false;
    Entity npc(entity);
    state.targetPlayer = TargetPlayer(npc.NPC.GetCurrentTarget().GetInstance());
    CaptureNativeFields(entity, npc, state);
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
    ImportNativeFields(entity, state, wasDead);
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
         CheckpointClass(entry.lease.state) == NativeClass::Ordinary ? "ordinary_discovered" : "generic_creature",
         entry.lease.owner, entry.lease.epoch, entry.lease.incarnation, entry.lease.revision,
         why, began ? net::Now() - began : 0);
}
void CommitDeath(GuidKey const& key, Entry& entry)
{
    if (!entry.lease.state.dead || !(entry.lease.state.effects & NativeDeath) ||
        (entry.lease.state.effects & QuestDeath)) return;
    eCEntity* entity = npcs::NpcByGuid(reinterpret_cast<uint8_t const*>(key.data()));
    if (!entity) return;
    if (NativeReady(entity)) {
        if (!Import(entity, entry.lease.state)) return;
    } else {
        if (!entity->GetPropertySet(eEPropertySetType_NPC) ||
            !entity->GetPropertySet(eEPropertySetType_DamageReceiver) ||
            !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;
        Entity npc(entity);
        npc.DamageReceiver.HitPoints = 0;
        npc.Routine.AIMode = gEAIMode_Dead;
    }
    // The host's canonical journal owns the quest notification and save watermark.
    if (entry.lease.owner == 1 && entry.previousOwner > 1) {
        if (journal::LeaseDeath(entity, entry.previousOwner)) CommitEffect(entry.lease.state, QuestDeath);
    } else if (journal::LeaseDeath(entity, entry.lease.owner)) CommitEffect(entry.lease.state, QuestDeath);
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
        const bool frozen = entry.lease.phase == Phase::Frozen;
        if (entry.lease.ActiveLeases()) {
            if (entry.lease.phase != Phase::Frozen || entry.lease.owner != from) {
                if (!frozen || (from >= entry.lease.owner &&
                                CurrentPeer(entry.lease.owner, entry.lease.generation))) return;
            }
        }
        eCEntity* native = npcs::NpcByGuid(note.guid);
        // Client-first: the client already runs this NPC; its latest checkpoint is the baseline for every class.
        const Checkpoint baseline = note.state;
        const uint8_t priorEffects = entry.lease.state.effects;
        if ((entry.lease.state.dead || (priorEffects & NativeDeath)) && !baseline.dead) {
            Refusal(key, note.state.name, "host lease recorded this NPC's death"); return;
        }
        if (npcremoval::SuppressedUnloaded(note.guid, baseline.name, baseline.position)) {
            Refusal(key, baseline.name, "authored removal"); return;
        }
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
        const Lease previousLease = entry.lease;
        auto handBack = Control(key, entry, Kind::Commit);
        if (frozen) entry.lease.phase = Phase::Dormant;
        // The host's quest watermark is not necessarily present in the client's baseline.
        entry.lease.state.effects = 0;
        if (!entry.lease.Offer(from, net::PeerGeneration(from), ++authoritySerial, now, baseline)) {
            entry.lease = previousLease; --authoritySerial; ++refused; return;
        }
        if (frozen && previousLease.owner != from && CurrentPeer(previousLease.owner, previousLease.generation)) {
            handBack.owner = 1; handBack.generation = 0;
            handBack.epoch = entry.lease.epoch; handBack.echo = now;
            net::SendNpcLease(previousLease.owner, handBack);
        }
        entry.lease.state.effects |= priorEffects;
        CommitDeath(key, entry);
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
    eCEntity* killer = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* victim = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    Entry* entry = Find(victim);
    if (!ai::ShuttingDown() && !net::IsHost() && victim && !Player(victim) && HostWins(Key(victim))) {
        ++repeatedDeaths;
        OwnEvent(Key(victim), victim->GetName().GetText(), npcown::Event::Refused,
            "path=Kill reason=unreleased host authority; native death refused");
        return 1;
    }
    if (!ai::ShuttingDown() && entry && entry->lease.epoch &&
        (Fenced(victim) || (entry->lease.state.effects & NativeDeath))) { ++repeatedDeaths; return 1; }
    if (ai::ShuttingDown()) return originalKill(spu, self, other, args);
    const bool wasDefeated = victim && victim->GetPropertySet(eEPropertySetType_NPC) && Entity(victim).NPC.DefeatedByPlayer;
    guards::LocalPlayer localPlayer;
    if (!entry || net::IsHost() || !entry->lease.ActiveLeases()) {
        const GEInt r = originalKill(spu, self, other, args);
        combat::ResolveKillXp(victim, killer, wasDefeated);
        return r;
    }
    if (!LocalAuthority(victim) || (entry->lease.state.effects & NativeDeath)) { ++repeatedDeaths; return 1; }
    CommitEffect(entry->lease.state, NativeDeath); // guard before native callbacks can recursively invoke Kill
    eCEntity* before = ownerDeath; ownerDeath = victim;
    const GEInt result = originalKill(spu, self, other, args);
    ownerDeath = before; combat::ResolveKillXp(victim, killer, wasDefeated);
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
            const bool recallDead = entry.lease.state.dead && value.hostPresent;
            if (ownerGone && entry.lease.state.dead) {
                CommitDeath(key, entry);
                entry.lease.state.dead = 1;
            }
            if (ownerGone || entry.interest.Recall(value) || !Running() || recallDead) {
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

}
using namespace detail;

bool LocalAuthority(eCEntity const* entity)
{
    return ReadGuard(entity).Local(net::Now());
}
bool ProcessingAuthority(eCEntity const* entity)
{
    auto* hero = remote::ControlledBody();
    return entity && hero && !net::IsHost() && !npcs::RedundantClientEntity(entity) &&
        !npcremoval::Suppressed(entity) &&
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
    // A client owner keeps receiving the stream: a fresh host stream is how it learns to yield.
    if (entry.local) return false;
    if (entry.hasPresentationFloor && Newer(entry.presentationFloor, captured)) return true;
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
