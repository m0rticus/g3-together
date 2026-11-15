// SPDX-License-Identifier: GPL-3.0-or-later
#include "npclease_internal.h"
#include "npclease_presentation.h"

#include "body.h"
#include "config.h"
#include "journal.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "npcremoval.h"
#include "persist.h"
#include "remote.h"

#include <Game.h>
#include <algorithm>
#include <cstring>

namespace g3t::npclease::detail {
namespace {
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

Checkpoint HostInterestState(Checkpoint const& baseline, Entry const& entry, eCEntity* native, bool frozen)
{
    Checkpoint probe = baseline;
    if (frozen) return probe;
    if (native) {
        const auto& p = native->GetWorldPosition();
        probe.position[0] = p.GetX(); probe.position[1] = p.GetY(); probe.position[2] = p.GetZ();
    } else if (entry.hasBaseline) {
        std::memcpy(probe.position, entry.lease.state.position, sizeof(probe.position));
    }
    return probe;
}

void SyncOfferedHostState(Entry& entry, eCEntity* native, Checkpoint const& priorState, bool hadBaseline, bool frozen)
{
    if (frozen) return;
    const uint8_t effects = entry.lease.state.effects;
    if (native) {
        Checkpoint hostState = entry.lease.state;
        if (CaptureNative(native, hostState)) {
            hostState.effects |= effects;
            entry.lease.state = hostState;
        }
    } else if (hadBaseline) {
        Checkpoint saved = priorState;
        saved.effects |= effects;
        entry.lease.state = saved;
    }
}

void RejectToClient(uint8_t client, Message const& note, eCEntity* native)
{
    if (client < 2 || client >= net::MaxPlayers || !net::InWorld(client)) return;
    Message reply;
    reply.kind = Kind::Reject;
    reply.world = note.world;
    reply.owner = client;
    reply.incarnation = native ? npcs::Incarnation(note.guid) : (note.incarnation ? note.incarnation : 1);
    if (!reply.incarnation) reply.incarnation = 1;
    reply.echo = note.echo;
    std::memcpy(reply.guid, note.guid, 16);
    net::SendNpcLease(client, reply);
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
} // namespace

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
        const Checkpoint baseline = note.state, priorState = entry.lease.state;
        const bool hadBaseline = entry.hasBaseline;
        const uint8_t priorEffects = entry.lease.state.effects;
        auto reject = [&](char const* name, char const* why) { Refusal(key, name, why); RejectToClient(from, note, native); };
        if (npcs::EnsureHostDespawn(native, note.guid)) { reject(note.state.name, "host despawned this NPC"); return; }
        if ((entry.lease.state.dead || (priorEffects & NativeDeath)) && !baseline.dead) {
            reject(note.state.name, "host lease recorded this NPC's death"); return;
        }
        if (npcremoval::SuppressedUnloaded(note.guid, baseline.name, baseline.position)) {
            reject(baseline.name, "authored removal"); return;
        }
        if (native) {
            if (auto const* why = EligibilityReason(native)) { reject(note.state.name, why); return; }
            if (std::memcmp(&native->GetTemplateID().GetCore(), baseline.templateId, 16)) {
                reject(note.state.name, "client checkpoint does not match the host instance template"); return;
            }
            if (!baseline.dead && Entity(native).IsDead()) {
                reject(note.state.name, "host instance is already dead"); return;
            }
        } else if (auto const* why = DiscoveryReason(baseline)) { reject(note.state.name, why); return; }
        if (!baseline.dead && journal::RecordedDeath(note.guid)) {
            reject(note.state.name, "host journal recorded this NPC's death"); return;
        }
        auto interest = InterestOf(HostInterestState(baseline, entry, native, frozen), from, native);
        interest.hostPresent = interest.hostPresent && npcs::HostCanPresent(note.guid);
        if (!entry.interest.Delegate(interest, now)) { reject(note.state.name, DelegateRefusal(interest)); return; }
        if (native && !npcplayback::GuardForLease(native)) {
            reject(baseline.name, "native property authority fence unavailable"); return;
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
        SyncOfferedHostState(entry, native, priorState, hadBaseline, frozen);
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
