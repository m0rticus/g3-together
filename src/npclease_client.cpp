// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_internal.h"
#include "npclease_save.h"
#include "npclease_presentation.h"
#define G3T_NPCLEASE_NATIVE_RUNTIME
#include "npclease_admission.h"
#undef G3T_NPCLEASE_NATIVE_RUNTIME
#include "npcpresence_policy.h"

#include "ai.h"
#include "body.h"
#include "combat_hostility.h"
#include "config.h"
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
bool Exact(Message const& note, Entry const& entry)
{
    return note.world == entry.world && note.epoch == entry.lease.epoch &&
        note.incarnation == entry.lease.incarnation && note.generation == entry.lease.generation &&
        note.owner == entry.lease.owner &&
        !std::memcmp(note.state.templateId, entry.lease.state.templateId, 16) &&
        !std::strcmp(note.state.name, entry.lease.state.name);
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

NativeClass ClassOf(Entity& npc)
{
    return SpeciesClass(static_cast<gESpecies>(npc.NPC.Species));
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
    return combat_hostility::CoopTargetPlayer(target);
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

// A client-owned NPC stops natively running: the host presents it again, took it over, or recalled it. Local state is discarded
// by the host's stream; nothing is frozen.
void ReleaseLocal(GuidKey const& key, Entry& entry, eCEntity* entity, bool stop)
{
    const bool wasLocal = entry.local;
    if (entry.vetted && entry.world == net::WorldEpoch() && entry.welcome == net::WelcomeSerial()) {
        const uint32_t now = net::Now();
        if (entry.lease.owner == net::LocalId() || entry.lease.owner == 0) entry.lease.owner = 1;
        entry.handbackUntil = (now ? now : 1) + 750u;
        if (!entry.handbackUntil) entry.handbackUntil = 1;
    }
    entry.local = false; entry.confirmed = false; entry.active = false; entry.prepared = false;
    entry.vetted = false; entry.hasOfferedState = false;
    entry.nativeActivated = false; entry.cut = false; entry.recallAt = 0;
    entry.lease.phase = Phase::Host;
    entry.window.Reset(entry.lease.epoch);
    entry.ownedEntity = nullptr;
    if (wasLocal) combat_hostility::ClearClientLeasedNpc(key.data());
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
        entry.offeredState = note.state;
        entry.hasOfferedState = !npcpresence::OfferMatchesLocalBaseline(
            entry.lease.state.position, entry.lease.state.hp, entry.lease.state.dead,
            note.state.position, note.state.hp, note.state.dead);
        if (!entry.hasOfferedState) { entry.vetted = true; entry.vettedInSession = true; }
        entry.localDeadline = note.echo + note.duration - LeaseSafetyMs;
        entry.echo = now; entry.hostSeenAt = now;
        PublishGuard(key, entry);
        Send(key, entry, Kind::Prepared);
        return;
    }
    const auto found = entries.find(key);
    if (found == entries.end()) { ++refused; return; }
    Entry& entry = found->second;
    if (note.kind == Kind::Reject && note.world == entry.world && note.owner == net::LocalId() &&
        entry.local && !entry.confirmed) {
        eCEntity* entity = npcs::NpcByGuid(note.guid);
        ReleaseLocal(key, entry, entity, true);
        entry.vettedInSession = false;
        entry.handbackUntil = 0;
        entry.yieldedAt = now ? now : 1;
        entry.retryAt = (now ? now : 1) + 5000;
        OwnEvent(key, entry.lease.state.name, npcown::Event::Refused, "reason=host rejected lease request");
        return;
    }
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
            bool ready = true;
            if (entry.hasOfferedState) {
                entry.lease.state = entry.offeredState;
                entry.hasOfferedState = false;
                eCEntity* entity = npcs::NpcByGuid(note.guid);
                ready = entity && NativeReady(entity) && Activate(entity, entry, note.guid);
                if (!ready) entry.ownedEntity = nullptr;
            }
            entry.window.Commit(note.epoch, now); // telemetry only; ownership never depends on the window
            entry.confirmed = true; entry.prepared = false;
            if (ready) { entry.vetted = true; entry.vettedInSession = true; }
            entry.hostSeenAt = now;
            combat_hostility::OnClientLeaseConfirmed(note.guid);
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
    const uint8_t priorEffects = entry.lease.state.effects;
    Checkpoint state;
    if (!CaptureNative(entity, state)) return "native checkpoint invalid";
    state.effects |= priorEffects;
    if (entry.world != net::WorldEpoch() || entry.welcome != net::WelcomeSerial()) {
        entry.vettedInSession = false;
        entry.handbackUntil = 0;
    }
    entry.world = net::WorldEpoch(); entry.welcome = net::WelcomeSerial();
    entry.handbackUntil = 0;
    entry.lease.state = state; entry.lease.owner = net::LocalId();
    entry.lease.incarnation = npcs::Incarnation(key.data());
    if (!entry.lease.incarnation) entry.lease.incarnation = 1;
    entry.lease.epoch = 0; entry.lease.revision = 0; entry.lease.generation = 0;
    entry.lease.phase = Phase::Client;
    entry.lease.started = now;
    entry.window.Reset(0);
    entry.hasBaseline = true;
    entry.local = true; entry.active = true; entry.confirmed = false; entry.prepared = false;
    entry.vetted = false; entry.hasOfferedState = false;
    entry.cut = false; entry.recallAt = 0; entry.captureFailed = false;
    entry.hasPresentationFloor = false; entry.presentationFloor = 0;
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
    combat_hostility::ClearClientLeasedNpc(key.data());
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
            if (entity && (!entry.cutReason || std::strcmp(entry.cutReason, "tame/control spell")) &&
                now - entry.cutAt >= npcown::CutReclaimMs && !npcs::HostStreamClaimed(key.data()) &&
                npcown::AcquisitionTimely(npcs::HostStreamDelay(), net::RoundTrip(1)) &&
                !persist::CheckpointBusy() && !SharedWithOtherClient(entity)) {
                const bool wasVetted = entry.vetted && entry.world == net::WorldEpoch();
                entry.cut = false; entry.lease.epoch = 0; entry.lease.phase = Phase::Host; entry.window.Reset(0);
                if (const char* why = TakeOver(entity, key, now)) {
                    OwnEventAlways(key, entry.lease.state.name, npcown::Event::Refused, why);
                } else {
                    if (wasVetted) { entry.vetted = true; entry.vettedInSession = true; }
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
            if (entry.confirmed) { entry.vetted = true; entry.vettedInSession = true; }
            OwnEventAlways(key, entry.lease.state.name, npcown::Event::Owned, "reason=reloaded instance restored from latest state");
        }
        ObserveProcessing(entry, entity, now);
        const auto readiness = Readiness(entity, entry, now);
        combat_hostility::SyncClientLeasedNpc(entity);
        Entity npc(entity);
        Checkpoint state = entry.lease.state;
        const bool captured = CaptureNative(entity, state, true);
        if (captured) state.effects |= entry.lease.state.effects;
        if (!captured && !entry.captureFailed)
            ReadinessDiagnostic("state-unavailable", key, entry, entity, readiness,
                readiness.LossReason() ? readiness.LossReason() : "native checkpoint invalid; state sends pause, NPC keeps running");
        entry.captureFailed = !captured;

        npcown::OwnerFacts facts;
        facts.role = RoleOf(entity, false);
        // A single in-flight record does not revoke an established owner. Proven host authority wins even over a local death.
        facts.hostPresenting = HostWins(key);
        facts.sharedWithLowerClient = SharedWithOtherClient(entity);
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
            state.defeated != entry.lease.state.defeated || state.aiMode != entry.lease.state.aiMode ||
            state.attackReason != entry.lease.state.attackReason ||
            state.targetPlayer != entry.lease.state.targetPlayer);
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
    std::unordered_map<std::string, unsigned> waits, refusedNow;
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
        else if (npcs::IsDespawned(key.data()) || journal::RecordedDeath(key.data())) hold = "host recorded death or despawn";
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
    char line[700];
    FormatScanCensus(line, sizeof(line), snap, ownTotals, refusedNow, waits);
    Log(line);
    ownReportAt = now ? now : 1;
}

void TickClient(uint32_t now)
{
    SyncGuardHostClaims();
    MaintainOwners(now);
    ScanClient(now);
}

}
using namespace detail;

void RelinquishToHost(eCEntity* entity, char const* reason)
{
    if (net::IsHost() || !entity) return;
    Entry* entry = Find(entity);
    if (!entry || !entry->local) return;
    const auto key = Key(entity);
    Checkpoint state = entry->lease.state;
    const bool captured = CaptureNative(entity, state, true);
    if (captured) state.effects |= entry->lease.state.effects;
    if (!entry->confirmed || !captured) ReleaseLocal(key, *entry, entity, true);
    else CutToHost(key, *entry, entity, &state, reason, net::Now());
}
bool VettedLocalAuthority(eCEntity const* entity)
{
    if (net::IsHost() || !entity) return false;
    const auto key = Key(entity);
    const auto it = entries.find(key);
    return it != entries.end() && it->second.local && it->second.active && it->second.vetted &&
        it->second.ownedEntity == entity && it->second.world == net::WorldEpoch() &&
        it->second.welcome == net::WelcomeSerial() && !HostWins(key);
}
bool HandbackPending(uint8_t const guid[16])
{
    if (net::IsHost() || !guid) return false;
    const auto it = entries.find(Key(guid));
    if (it == entries.end() || it->second.world != net::WorldEpoch() || it->second.welcome != net::WelcomeSerial())
        return false;
    if (it->second.cut && !it->second.local && it->second.vetted) return true;
    return !it->second.local && it->second.handbackUntil != 0 && it->second.lease.owner == 1 &&
           static_cast<int32_t>(it->second.handbackUntil - net::Now()) > 0;
}
bool PreviouslyVetted(uint8_t const guid[16])
{
    if (net::IsHost() || !guid) return false;
    const auto it = entries.find(Key(guid));
    return it != entries.end() && it->second.vettedInSession &&
        it->second.world == net::WorldEpoch() && it->second.welcome == net::WelcomeSerial();
}
}
