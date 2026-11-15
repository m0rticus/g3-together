// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_send.h"
#include "npc_companions.h"
#include "npc_corpse.h"

#include "combat.h"
#include "combat_timing.h"
#include "journal.h"
#include "log.h"
#include "net_metrics.h"
#include "npc_contact.h"
#include "npclease_presentation.h"
#include "npclease_runtime.h"
#include "npcplayback_hit_policy.h"
#include "npcsend_policy.h"
#include "npcremoval.h"
#include "talk.h"
#include "world.h"

#include <algorithm>

namespace g3t::npcs::detail {

int OnRelease(uint8_t owner, uint32_t generation, uint8_t* guids, int count)
{
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        const auto it = mirrors.find(Key(guids + i * 16));
        if (it != mirrors.end() && it->second.owner == owner && it->second.ownerGeneration == generation) {
            it->second.released = true;
            std::memmove(guids + accepted++ * 16, guids + i * 16, 16);
        }
    }
    return accepted;
}

void OnMotionName(uint32_t hash, char const* name)
{
    if (motionNames.size() < 50000 || motionNames.count(hash)) motionNames[hash] = name;
}

uint8_t FightFlags(Entity& npc)
{
    const bool fighting = npc.Routine.AIMode == gEAIMode_Combat;
    if (fighting) ++fightSent;
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    uint8_t player = PlayerOf(target);
    if (!player && target && target->GetPropertySet(eEPropertySetType_Party))
        player = PlayerOf(Entity(target).Party.GetPartyLeader().GetInstance());
    return static_cast<uint8_t>((fighting ? 4 : 0) | ((player & 7) << 3));
}

void PrepareHostSendFrame(std::vector<npcsend_candidates::GuidCandidate> const& candidates,
                          size_t firstProcessed, unsigned candidateKeyCount, unsigned processedCount,
                          HostSendFrame& out)
{
    streamDiag.BeginTick(net::Now());
    unsigned interactionNpcs = 0, processedNpcs = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!npcsend_candidates::Eligible(candidates[i])) continue;
        if (i < firstProcessed) ++interactionNpcs;
        else { ++processedNpcs; out.processedKeys.insert(candidates[i].key); }
    }
    streamDiag.Candidates(interactionNpcs, processedNpcs, candidateKeyCount, processedCount);
    net::Remote const* remotes = net::Remotes();
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        if (!remotes[id].connected) continue;
        out.recipientHere[id] = net::InWorld(id) && (remotes[id].latest.flags & 1) &&
                                remotes[id].latest.world == remote::CurrentWorld();
        streamDiag.Player(id, net::InWorld(id), out.recipientHere[id], net::RemoteStateAgeMs(id),
                          remotes[id].latest.position);
    }
    for (uint8_t id = 0; id < net::MaxPlayers; ++id)
        out.stretchOf[id] = id >= 2 ? net::NpcStretchPermille(id) : npcsend::StretchNormal;
}

eCEntity* AdmitHostSendCandidate(GuidKey const& candidateKey, eCEntity* hero, eCEntity* controlled,
                                 HostSendFrame const& frame, float const players[][3], int playerCount,
                                 uint8_t outGuid[16], GuidKey& outKey, float outAt[3])
{
    using npcstream_diag::Drop;
    net::Remote const* remotes = net::Remotes();
    const auto countSkipped = [&](GuidKey const& key, net::NpcSendDecision decision) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id)
            if (net::InWorld(id) && remotes[id].connected && !recipientNpcs[id].count(key))
                net::RecordNpcSendDecision(decision);
    };
    Entity npc = guid::Resolve(candidateKey.data());
    eCEntity* instance = npc.GetInstance();
    if (!instance) { streamDiag.Candidate(Drop::Unresolved); return nullptr; }
    if (instance == hero || remote::IsPuppet(instance) || instance == controlled) {
        streamDiag.Candidate(Drop::Hero);
        return nullptr;
    }
    if (npcremoval::Suppressed(instance)) { streamDiag.Candidate(Drop::Removed); return nullptr; }
    guid::CopyEntity(instance, outGuid);
    outKey = Key(outGuid);
    if (npclease::SuppressHostPublication(outGuid)) { streamDiag.Candidate(Drop::Lease); return nullptr; }
    const bCVector& w = instance->GetWorldPosition();
    const float where[3] = {w.GetX(), w.GetY(), w.GetZ()};
    const bool fromProcessing = frame.processedKeys.count(candidateKey) != 0;
    for (uint8_t id = 2; id < net::MaxPlayers; ++id)
        if (frame.recipientHere[id])
            streamDiag.Reach(id, body::Distance(where, remotes[id].latest.position), fromProcessing,
                             GetConfig().hostRoi, npcsend::InterestEnterCm);
    SendNote& note = sendNotes[outKey];
    note.at = net::Now();
    if (journal::Pending(instance)) {
        note.reason = "pending"; streamDiag.Candidate(Drop::Pending);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
    }
    if (mirrors.count(outKey)) {
        note.reason = "mirror"; streamDiag.Candidate(Drop::Mirror);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
    }
    if (deadStays && !net::IsHost() && deadOnHost.count(outKey)) {
        Entity corpse(instance);
        KeepDead(corpse);
        note.reason = "dead on host";
        return nullptr;
    }
    const auto back = givenBack.find(outKey);
    if (back != givenBack.end() && static_cast<int32_t>(net::Now() - back->second) < 0) {
        note.reason = "givenback"; streamDiag.Candidate(Drop::GivenBack);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
    }
    outAt[0] = w.GetX(); outAt[1] = w.GetY(); outAt[2] = w.GetZ();
    if (!CheckHostSendRange(instance, outKey, outAt, players, playerCount, note)) {
        streamDiag.Candidate(Drop::Range);
        countSkipped(outKey, net::NpcSendDecision::SkippedFar);
        return nullptr;
    }
    return instance;
}

void SendNpcToRecipients(Entity& self, eCEntity* instance, eCEntity* hostFocus, eCEntity* hostTarget,
                         GuidKey const& key, uint8_t const guid[16], float const at[3], net::NpcRecord const& r,
                         std::vector<pose::Bone> const& bones, uint8_t poseFlags, bool posed, bool moving,
                         uint32_t const stretchOf[net::MaxPlayers])
{
    using npcstream_diag::Drop;
    net::Remote const* remotes = net::Remotes();
    const uint32_t npcHash = net::HashGuid(guid);
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        if (!net::InWorld(id) || !remotes[id].connected || !(remotes[id].latest.flags & 1) ||
            remotes[id].latest.world != remote::CurrentWorld()) {
            if (net::InWorld(id) && net::PeerJoined(id))
                net::RecordNpcSendDecision(net::NpcSendDecision::SkippedIrrelevant);
            if (remotes[id].connected) streamDiag.Result(id, Drop::Away);
            continue;
        }
        const float distance = body::Distance(at, remotes[id].latest.position);
        auto& states = recipientNpcs[id];
        const auto existing = states.find(key);
        npcsend::State fresh;
        npcsend::State& state = existing == states.end() ? fresh : existing->second;
        npcsend::Input input;
        input.record = &r;
        input.bones = posed ? bones.data() : nullptr;
        input.boneCount = posed ? static_cast<int>(bones.size()) : 0;
        input.poseFlags = posed ? poseFlags : 0;
        input.now = r.sent;
        input.distanceCm = distance;
        input.interest = (((r.flags >> 3) & 7) == id || r.partyPlayer == id || r.guidePlayer == id)
            ? npcsend::Interest::Include : npcsend::Interest::Distance;
        input.focused = distance <= npcsend::ContactSafetyCm ||
            ((hostFocus == instance || hostTarget == instance || talk::IsHeld(instance) ||
              talk::TalkingWithHero(instance)) && distance <= npcsend::InterestExitCm) ||
            (npcHash && net::RemoteStateAgeMs(id) <= 1000 &&
             (net::RemoteFocusNpc(id) == npcHash || net::RemoteTargetNpc(id) == npcHash));
        input.combat = (r.flags & 4) != 0;
        input.contact = r.contact != 0;
        input.moving = moving;
        input.nearby = false;
        input.stretchPermille = stretchOf[id];
        const npcsend::Result result = npcsend::Evaluate(state, input);
        if (result.decision == npcsend::Decision::Outside) {
            net::RecordNpcSendDecision(net::NpcSendDecision::SkippedFar);
            streamDiag.Result(id, Drop::Outside);
        } else if (result.decision == npcsend::Decision::Wait) {
            net::RecordNpcSendDecision(net::NpcSendDecision::SkippedUnchanged);
            streamDiag.Result(id, Drop::Unchanged);
        } else if (result.decision == npcsend::Decision::Invalid || result.decision == npcsend::Decision::Ineligible) {
            static unsigned invalid = 0;
            if (invalid++ < 20) Logf("npcs: invalid send policy input for recipient %u", id);
            net::RecordNpcSendDecision(net::NpcSendDecision::SkippedIrrelevant);
            streamDiag.Result(id, Drop::Invalid);
        } else if (result.decision == npcsend::Decision::FinalAndRelease) {
            const uint8_t intervalWire = npclease::EncodePoseIntervalWire(npcsend::QuietCadenceMs);
            const bool finalQueued = posed
                ? net::SendPosedNpcTo(id, r, bones.data(), static_cast<int>(bones.size()), poseFlags, true, intervalWire)
                : net::SendNpcsTo(id, &r, 1, true);
            const bool releaseQueued = finalQueued && net::SendNpcReleaseTo(id, guid, 1);
            if (releaseQueued && npcsend::Commit(state, input, result.decision)) {
                states.erase(key);
                net::RecordNpcSendDecision(net::NpcSendDecision::Sent);
                streamDiag.Released(id);
            } else {
                net::RecordNpcSendDecision(net::NpcSendDecision::SendFailed);
                streamDiag.Result(id, Drop::Failed);
            }
        } else {
            if (result.decision == npcsend::Decision::Initial && !npcsend::ImmediateInitial(input) &&
                !initialPacers[id].Admit(r.sent, stretchOf[id])) {
                ++sendStats.paced;
                net::RecordNpcSendDecision(net::NpcSendDecision::SkippedPaced);
                streamDiag.Result(id, Drop::Paced);
                continue;
            }
            const uint8_t intervalWire = npclease::EncodePoseIntervalWire(
                npcsend::EffectivePresentationInterval(result, state, input));
            const bool queued = posed
                ? net::SendPosedNpcTo(id, r, bones.data(), static_cast<int>(bones.size()), poseFlags, result.essential,
                                      intervalWire)
                : net::SendNpcsTo(id, &r, 1, result.essential);
            const uint32_t previousSentAt = state.sentAt;
            const bool attackStart = combat_timing::RemoteCombat(r.flags, r.combatTarget) &&
                r.motions[0].active && state.record.motions[0].instance != r.motions[0].instance;
            const bool onSchedule = r.sent - state.idealAt >= npcsend::Stretched(npcsend::Interval(result.tier), stretchOf[id]);
            if (queued && npcsend::Commit(state, input, result.decision)) {
                if (attackStart)
                    Logf("combat: NPCATTACK stage=send npc=%s player=%u instance=%u host_captured=%u host_sent=%u "
                         "phase=%.4f reliable=%d",
                         self.GetName().GetText(), id, r.motions[0].instance, r.sent, net::Now(), r.motions[0].phase,
                         result.essential ? 1 : 0);
                sendStats.Record(result.decision, result.tier, r.sent - previousSentAt, stretchOf[id], onSchedule);
                if (existing == states.end()) states.emplace(key, std::move(fresh));
                net::RecordNpcSendDecision(net::NpcSendDecision::Sent);
                streamDiag.Sent(id);
            } else {
                net::RecordNpcSendDecision(net::NpcSendDecision::SendFailed);
                streamDiag.Result(id, Drop::Failed);
            }
        }
    }
}

void FinishOwnedTick(eCEntity* hero, float const players[][3], int playerCount)
{
    using npcstream_diag::Drop;
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) initialPacers[id].Poll(net::Now());
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        auto& states = recipientNpcs[id];
        for (auto it = states.begin(); it != states.end();) {
            if (owned.count(it->first)) { ++it; continue; }
            const auto note = sendNotes.find(it->first);
            const auto final = lastSent.find(it->first);
            const bool range = note != sendNotes.end() && !std::strcmp(note->second.reason, "range");
            bool finalQueued = !range;
            if (range && final != lastSent.end()) {
                const auto& last = final->second;
                const uint8_t intervalWire = npclease::EncodePoseIntervalWire(npcsend::QuietCadenceMs);
                finalQueued = last.posed
                    ? net::SendPosedNpcTo(id, last.record, last.bones.data(), static_cast<int>(last.bones.size()),
                                          last.poseFlags, true, intervalWire)
                    : net::SendNpcsTo(id, &last.record, 1, true);
            }
            const bool released = finalQueued && net::SendNpcReleaseTo(id, it->first.data(), 1);
            if (released) {
                net::RecordNpcSendDecision(net::NpcSendDecision::Sent);
                streamDiag.Released(id);
                it = states.erase(it);
            } else {
                net::RecordNpcSendDecision(net::NpcSendDecision::SendFailed);
                streamDiag.Result(id, Drop::Failed);
                ++it;
            }
        }
    }
    if (streamDiag.Due(net::Now())) {
        npcstream_diag::Context context;
        context.hostValid = hero != nullptr && playerCount > 0;
        if (context.hostValid) std::memcpy(context.hostAt, players[0], sizeof(context.hostAt));
        context.mode = GetConfig().hostSimSpheres ? "spheres" : GetConfig().hostSimElastic ? "elastic" : "off";
        context.roiCm = GetConfig().hostRoi;
        context.sim = world::HostSimulationStatus();
        streamDiag.Flush(net::Now(), context, [](char const* line) { Log(line); });
    }
}

void HostTick(void (*sendOwned)(eCEntity*, float const[][3], int))
{
    const uint32_t now = net::Now();
    if (!sendCadence.Due(now)) return;
    lastSend = now;
    ClaimTick(now);
    if (net::JoinCount() != knownJoins) {
        knownJoins = net::JoinCount();
        namesSent.clear();
        for (auto const& entry : forceHistory) for (auto const& force : entry.second) net::SendRagdoll(force);
    }
    float players[net::MaxPlayers][3];
    int playerCount = 0;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (hero) {
        const bCVector& p = remote::ControlledBody()->GetWorldPosition();
        players[playerCount][0] = p.GetX();
        players[playerCount][1] = p.GetY();
        players[playerCount][2] = p.GetZ();
        ++playerCount;
    }
    net::Remote const* remotes = net::Remotes();
    bool anyClient = false;
    for (int id = 2; id < net::MaxPlayers; ++id) {
        const uint32_t generation = net::PeerGeneration(static_cast<uint8_t>(id));
        if (!net::InWorld(static_cast<uint8_t>(id)) || !remotes[id].connected || recipientGenerations[id] != generation) {
            recipientNpcs[id].clear();
            recipientGenerations[id] = generation;
            initialPacers[id].Reset();
        }
        if (!remotes[id].connected) { joinedAt[id] = 0; continue; }
        anyClient = true;
        if (!(remotes[id].latest.flags & 1) || remotes[id].latest.world != remote::CurrentWorld()) continue;
        std::memcpy(players[playerCount++], remotes[id].latest.position, sizeof(float) * 3);
    }
    if (!anyClient || !playerCount) return;
    if (!captureStartedAt) captureStartedAt = now ? now : 1;
    ++captureTicks;
    sendOwned(hero, players, playerCount);
}

} // namespace g3t::npcs::detail

namespace g3t::npcs {
using namespace detail;

void HostHit(eCEntity* entity, uint8_t player, uint32_t sequence)
{
    if (!net::IsHost() || !entity || !entity->GetPropertySet(eEPropertySetType_NPC) || remote::IsPuppet(entity)) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const uint32_t at = net::Now();
    hostHits[Key(guid)] = {player, sequence, at ? at : 1};
}

void ForgetRemoved(uint8_t const guid[16])
{
    const GuidKey key = Key(guid);
    awaitedHits.erase(key);
    mirrors.erase(key);
    corpses.erase(key);
    absences.erase(key);
    pendingForces.erase(key);
    forceHistory.erase(key);
    hostHits.erase(key);
    lastSent.erase(key);
    sending.erase(key);
    owned.erase(key);
    sendNotes.erase(key);
    deadOnHost.erase(key);
    ForgetCorpseGroundAnchor(key);
    for (auto& recipient : recipientNpcs) recipient.erase(key);
    for (auto it = forceSequences.begin(); it != forceSequences.end();)
        if (it->first.guid == key) it = forceSequences.erase(it);
        else ++it;
}

void Despawn(eCEntity* entity)
{
    if (!net::IsHost() || !IsNpc(entity) || npcremoval::Suppressed(entity)) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    LastSent& last = lastSent[key];
    auto& record = last.record;
    std::memcpy(record.guid, guid, sizeof(record.guid));
    record.incarnation = Incarnation(guid);
    record.sent = net::Now();
    record.flags |= 1;
    record.hitPoints = 0;
    record.contact = 0;
    const auto& at = entity->GetWorldPosition();
    const auto turn = entity->GetWorldRotation();
    record.position[0] = at.GetX(); record.position[1] = at.GetY(); record.position[2] = at.GetZ();
    record.rotation[0] = turn.GetX(); record.rotation[1] = turn.GetY();
    record.rotation[2] = turn.GetZ(); record.rotation[3] = turn.GetW();
    last.at = record.sent;
    last.posed = false;
    last.poseFlags = 0;
    last.bones.clear();
    forceHistory.erase(key);
    net::SendNpcs(&record, 1, true);
}

uint32_t Incarnation(uint8_t const guid[16])
{
    if (const uint32_t life = npclease::Life(guid)) return life;
    const GuidKey key = Key(guid);
    if (net::IsHost()) {
        const auto found = incarnations.find(key);
        return found == incarnations.end() || !found->second ? 1 : found->second;
    }
    const auto found = mirrors.find(key);
    if (found != mirrors.end()) return found->second.latest.incarnation;
    const auto absent = absences.find(key);
    return absent == absences.end() ? 0 : absent->second.incarnation;
}

void NewLife(uint8_t const guid[16])
{
    if (npcremoval::Suppressed(guid)) return;
    if (!npclease::NewLife(guid)) return;
    const GuidKey key = Key(guid);
    if (net::IsHost()) {
        uint32_t& incarnation = incarnations[key];
        if (!incarnation) incarnation = 1;
        if (++incarnation == 0) ++incarnation;
        hostHits.erase(key);
        forceHistory.erase(key);
    }
    if (newLife && deadOnHost.erase(key)) Logf("npcs: %s no longer held dead here (it stood up again)", Resolve(guid).GetName().GetText());
    if (newLife) ForgetCorpseGroundAnchor(key);
}

bool HostKnows(uint8_t const identity[16])
{
    const GuidKey key = Key(identity);
    return lastSent.count(key) || incarnations.count(key) || owned.count(key) ||
        claimedBy.count(key) || directedBy.count(key) || mirrors.count(key) || corpses.count(key) ||
        absences.count(key) || deadOnHost.count(key) || journal::RecordedDeath(identity);
}

bool HostStreamFresh(uint8_t const identity[16])
{
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end() || found->second.hostSession != net::WelcomeSerial() || !found->second.latestTicks) return false;
    auto const& mirror = found->second;
    const uint32_t poseInt = npcplayback::PoseInterval(mirror.latest);
    const uint32_t effectiveInt = poseInt ? poseInt : npcsend::QuietCadenceMs;
    return npclease::PresentationFresh(net::Connected(), mirror.released, net::Now() - mirror.latestTicks, effectiveInt);
}

bool HostReleasePending(uint8_t const identity[16])
{
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end() || found->second.hostSession != net::WelcomeSerial()) return false;
    auto const& mirror = found->second;
    auto const& track = mirror.body.track;
    return npclease::ReleasePending(mirror.released, track.count, track.haveRenderTime,
        track.renderSent, mirror.latest.sent, body::PendingEvents(track));
}

bool HostStreamSeen()
{
    if (!hostStreamSeen || hostStreamSession != net::WelcomeSerial()) return false;
    return net::Now() - hostStreamFirstSeenAt >= 1500;
}

uint32_t HostStreamRecords(uint8_t const identity[16])
{
    const auto found = mirrors.find(Key(identity));
    return found == mirrors.end() || found->second.hostSession != net::WelcomeSerial() ? 0 : found->second.records;
}

bool HostCanPresent(uint8_t const identity[16])
{
    const auto found = lastSent.find(Key(identity));
    return found != lastSent.end() && net::Now() - found->second.at <= npclease::PresentationGraceMs;
}

void Mirrored(std::vector<eCEntity*>& out)
{
    for (auto& [key, m] : mirrors)
        if (m.resolved && !m.missing && !m.killed && m.entity.GetInstance()) out.push_back(m.entity.GetInstance());
}

bool DeadStays() { return deadStays; }

uint8_t RemoteOwner(uint8_t const guid[16])
{
    if (net::IsHost()) return 0;
    return npcplayback::IsPassive(NpcByGuid(guid)) ? uint8_t(1) : uint8_t(0);
}

uint8_t OwnerOf(eCEntity const* entity)
{
    if (npclease::LocalAuthority(entity)) return 0;
    if (net::IsHost()) return npclease::RemoteOwner(entity);
    if (npcplayback::IsPassive(entity)) return 1;
    if (!entity) return 0;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return RemoteOwner(guid);
}

bool PaysDeathWithoutKiller(eCEntity const* victim)
{
    if (npclease::LocalAuthority(victim)) return true;
    if (npcplayback::IsPassive(victim)) return false;
    if (net::IsHost() || !farXpGuard || !victim || IsMirrored(victim) || Claimed(victim)) return true;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(victim), guid);
    if (owned.count(Key(guid))) return true;
    ++farXpRefused;
    return false;
}

void FightStats(unsigned& sent, unsigned& seenAny, unsigned& seenUs, unsigned& seenNear)
{
    sent = fightSent, seenAny = fightSeenAny, seenUs = fightSeenUs, seenNear = fightSeenNear;
}

void SwingSamples(unsigned& armed, unsigned& samples)
{
    armed = swingArmed;
    samples = swingSamples;
}

namespace detail {

bool CheckHostSendRange(eCEntity* instance, GuidKey const& key, float const at[3],
                        float const players[][3], int playerCount, SendNote& note)
{
    float nearest = 1e30f;
    for (int k = 0; k < playerCount; ++k) nearest = std::min(nearest, body::Distance(at, players[k]));
    bool inRange = nearest <= GetConfig().hostRoi;
    if (!inRange && companionNear && !net::IsHost() && myClaims.count(key) && nearest <= KeepRadius) inRange = true;
    if (!inRange) {
        const auto held = contested.find(key);
        if (held != contested.end() && nearest <= KeepRadius) inRange = true;
        else if (held != contested.end()) contested.erase(held);
    }
    if (!inRange && hysteresis) {
        const auto held = sending.find(key);
        const uint32_t t = net::Now();
        if (held != sending.end() && static_cast<int32_t>(t - held->second.last) <= 1500) {
            const bool young = static_cast<int32_t>(t - held->second.since) < static_cast<int32_t>(MinHold);
            const bool fighting = Entity(instance).Routine.AIMode == gEAIMode_Combat;
            const float retain = std::max(KeepRadius, GetConfig().hostRoi + 2000.0f);
            if (nearest <= retain || ((young || fighting) && nearest <= retain + 2000.0f)) { inRange = true; ++heldBand; }
        }
    }
    if (!inRange) {
        note.reason = "range";
        return false;
    }
    note.reason = "sent";
    Held& held = sending[key];
    const uint32_t t = net::Now();
    if (!held.last || static_cast<int32_t>(t - held.last) > 1500) held.since = t;
    held.last = t;
    return true;
}

void CaptureAndRecordOwnedNpc(Entity& self, eCEntity* instance, GuidKey const& key, net::NpcRecord& r,
                              std::vector<pose::Bone>& bones, uint8_t& poseFlags, bool& posed, bool& moving)
{
    r.movementMode = static_cast<uint8_t>(self.CharacterMovement.GetMovementMode());
    r.flags = static_cast<uint8_t>((self.IsDead() ? 1 : 0) | (self.IsDown() ? 2 : 0) | FightFlags(self));
    if (instance->GetPropertySet(eEPropertySetType_Party)) {
        r.partyPlayer = PlayerOf(self.Party.GetPartyLeader().GetInstance());
        r.partyType = static_cast<uint8_t>(static_cast<gEPartyMemberType>(self.Party.PartyMemberType));
        r.waiting = self.Party.Waiting ? 1 : 0;
    }
    for (uint8_t player = 1; player < net::MaxPlayers; ++player) {
        eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
        if (actor && actor->GetPropertySet(eEPropertySetType_Party) && Entity(actor).Party.GetPartyLeader().GetInstance() == instance)
            r.guidePlayer = player;
    }
    if (self.IsDead() && (r.movementMode == gECharMovementMode_RagDollDead ||
                          !std::strncmp(self.Routine.GetCurrentTask().GetText(), "ZS_RagDollDead", 14)))
        r.flags |= 128;
    r.hitPoints = self.DamageReceiver.HitPoints;
    r.hitPointsMax = self.DamageReceiver.HitPointsMax;
    r.aniState = static_cast<uint8_t>(static_cast<int>(self.Routine.AniState));
    r.aiMode = static_cast<uint8_t>(body::AIModeOf(instance));
    r.action = static_cast<uint8_t>(static_cast<gEAction>(self.Routine.Action));
    r.attackReason = static_cast<uint8_t>(static_cast<gEAttackReason>(self.NPC.AttackReason));
    Entity damageSource = self.Inventory.GetItemFromSlot(gESlot_RightHand);
    if (!damageSource.GetInstance() || !damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) damageSource = self;
    if (damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) {
        r.damageType = static_cast<uint8_t>(static_cast<gEDamageType>(damageSource.Damage.DamageType));
        r.damageAmount = damageSource.Damage.DamageAmount;
        r.damageMultiplier = damageSource.Damage.DamageHitMultiplier;
    }
    r.sent = net::Now();
    uint32_t& incarnation = incarnations[key];
    if (!incarnation) incarnation = 1;
    r.incarnation = incarnation;
    if (auto hit = hostHits.find(key); hit != hostHits.end()) {
        r.hitPlayer = hit->second.player;
        r.hitSequence = static_cast<uint16_t>(hit->second.sequence);
    }
    const bool parrying = r.aniState == gEAniState_Parade && combat::InParadeMode(instance);
    if (parrying) r.flags |= 64;
    r.heldRight = HeldHash(self, gESlot_RightHand);
    r.heldLeft = HeldHash(self, gESlot_LeftHand);
    for (gESlot slot : {gESlot_RightHand, gESlot_LeftHand}) {
        const Entity held = self.Inventory.GetItemFromSlot(slot);
        if (!held.GetInstance()) continue;
        const uint32_t hash = net::HashName(held.GetName().GetText());
        if (namesSent.insert(hash).second) net::SendMotionName(hash, held.GetName().GetText());
    }
    char motion[96] = {};
    if (body::CurrentMotion(instance, motion, sizeof(motion)) && *motion) {
        r.motion = net::HashName(motion);
        if (namesSent.insert(r.motion).second) net::SendMotionName(r.motion, motion);
    }
    if (auto hit = hostHits.find(key); hit != hostHits.end()) {
        if (npcplayback_hit_policy::HostHitStampExpired(
                hit->second.at, r.sent, StaggerState(self.Routine.GetCurrentTask().GetText()), motion)) {
            hostHits.erase(hit);
            r.hitPlayer = 0;
            r.hitSequence = 0;
        }
    }
    const bool motionReady = npcplayback::Capture(instance, r, [](uint32_t hash, char const* name) {
        if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
    });
    poseFlags = 0;
    posed = !(r.flags & 128) && npcplayback::CapturePose(instance, bones, poseFlags,
        [](uint32_t hash, char const* name) {
            if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
        });
    r.contact = motionReady && (r.flags & 128 ? motionReady : posed) &&
                r.motions[0].weight > 0.0001f && body::LocallyAttacking(instance) ? 1 : 0;
    npcsend::SanitizeCapturedRecord(r);
    LastSent& last = lastSent[key];
    const uint32_t t = net::Now();
    if (!(posed || motionReady) && !(r.flags & 1) && !last.at)
        Logf("npcs: %s is waiting for its native skeletal pose; contacts remain disabled", self.GetName().GetText());
    const bool revived = last.at && (last.record.flags & 3) && !(r.flags & 3);
    if (revived && (last.record.flags & 1) && r.incarnation == last.record.incarnation) {
        uint32_t& revivedIncarnation = incarnations[key];
        if (++revivedIncarnation == 0) ++revivedIncarnation;
        r.incarnation = revivedIncarnation;
        hostHits.erase(key);
        forceHistory.erase(key);
        r.hitPlayer = 0;
        r.hitSequence = 0;
    }
    if (last.at) {
        float same = 0.0f, opposite = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float a = r.rotation[k] - last.record.rotation[k];
            const float b = r.rotation[k] + last.record.rotation[k];
            same += a * a;
            opposite += b * b;
        }
        if (body::Distance(r.position, last.record.position) >= npcsend::MovementPositionCm ||
            std::min(same, opposite) >= npcsend::MovementQuaternionComponent * npcsend::MovementQuaternionComponent)
            last.movingUntil = t + npcsend::MovementHoldMs;
    }
    moving = last.movingUntil && static_cast<int32_t>(last.movingUntil - t) > 0;
    last.record = r;
    last.at = t;
    last.posed = posed;
    last.poseFlags = posed ? poseFlags : 0;
    if (posed) last.bones = bones;
    else last.bones.clear();
    SentStates& states = sentStates[key];
    states.at[states.next] = r.sent;
    states.parrying[states.next] = parrying;
    states.next = (states.next + 1) % StateHistory;
}

} // namespace detail

} // namespace g3t::npcs
