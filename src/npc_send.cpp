// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_send.h"
#include "npc_companions.h"
#include "npc_corpse.h"

#include "combat.h"
#include "combat_timing.h"
#include "guide_policy.h"
#include "infocmd.h"
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

void OnNpcEquipment(net::NpcEquipmentNote const& note)
{
    if (net::IsHost() || npcremoval::Suppressed(note.guid) || !note.incarnation) return;
    const GuidKey key = Key(note.guid);
    npc_equipment::BackEquipment incoming{};
    incoming.incarnation = note.incarnation;
    std::memcpy(incoming.slots, note.backSlots, sizeof(incoming.slots));
    auto& pending = pendingBackEquipment[key];
    if (npc_equipment::ShouldAcceptBackEquipment(pending.incarnation, incoming.incarnation))
        pending = incoming;
    if (const auto found = mirrors.find(key);
        found != mirrors.end() &&
        npc_equipment::ShouldAcceptBackEquipment(found->second.latest.incarnation, incoming.incarnation) &&
        npc_equipment::ShouldAcceptBackEquipment(found->second.backEquipment.incarnation, incoming.incarnation)) {
        found->second.backEquipment = incoming;
    }
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
        else ++processedNpcs;
    }
    streamDiag.Candidates(interactionNpcs, processedNpcs, candidateKeyCount, processedCount);
    for (uint8_t player = 1; player < net::MaxPlayers; ++player) {
        eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
        if (actor && actor->GetPropertySet(eEPropertySetType_Party))
            out.partyLeaderOf[player] = Entity(actor).Party.GetPartyLeader().GetInstance();
    }
    net::Remote const* remotes = net::Remotes();
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        if (!remotes[id].connected) continue;
        const bool here = net::InWorld(id) && (remotes[id].latest.flags & 1) &&
                          remotes[id].latest.world == remote::CurrentWorld();
        out.recipientHere[id] = here;
        if (here) out.anyRecipientHere = true;
        streamDiag.Player(id, net::InWorld(id), here, net::RemoteStateAgeMs(id),
                          remotes[id].latest.position);
    }
    for (uint8_t id = 0; id < net::MaxPlayers; ++id)
        out.stretchOf[id] = id >= 2 ? net::NpcStretchPermille(id) : npcsend::StretchNormal;
}

eCEntity* AdmitHostSendCandidate(GuidKey const& candidateKey, eCEntity* hero, eCEntity* controlled,
                                 HostSendFrame const& frame, float const players[][3], int playerCount,
                                 uint8_t outGuid[16], GuidKey& outKey, float outAt[3],
                                 LastSent*& outExistingLast)
{
    using npcstream_diag::Drop;
    outExistingLast = nullptr;
    net::Remote const* remotes = net::Remotes();
    const auto countSkipped = [&](GuidKey const& key, net::NpcSendDecision decision) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id)
            if (net::InWorld(id) && remotes[id].connected && !recipientNpcs[id].count(key))
                net::RecordNpcSendDecision(decision);
    };
    eCEntity* instance = nullptr;
    bool fromProcessing = false;
    bool haveCachedGuid = false;
    if (auto const* cached = frame.FindEntity(candidateKey)) {
        fromProcessing = cached->fromProcessed;
        if (!cached->ambiguous && cached->instance) {
            instance = cached->instance;
            std::memcpy(outGuid, candidateKey.data(), 16);
            outKey = candidateKey;
            haveCachedGuid = true;
        }
    }
    if (!haveCachedGuid) {
        Entity npc = guid::Resolve(candidateKey.data());
        instance = npc.GetInstance();
    }
    if (!instance) { streamDiag.Candidate(Drop::Unresolved); return nullptr; }
    if (instance == hero || remote::IsPuppet(instance) || instance == controlled) {
        streamDiag.Candidate(Drop::Hero);
        return nullptr;
    }
    if (npcremoval::Suppressed(instance)) { streamDiag.Candidate(Drop::Removed); return nullptr; }
    if (!haveCachedGuid) {
        guid::CopyEntity(instance, outGuid);
        outKey = Key(outGuid);
    }
    if (EnsureHostDespawn(instance, outGuid)) {
        sendNotes[outKey] = {"despawned", net::Now()};
        streamDiag.Candidate(Drop::Removed);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant);
        return nullptr;
    }
    if (npclease::SuppressHostPublication(outGuid)) { streamDiag.Candidate(Drop::Lease); return nullptr; }
    const bCVector& w = instance->GetWorldPosition();
    const float where[3] = {w.GetX(), w.GetY(), w.GetZ()};
    if (frame.anyRecipientHere) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id)
            if (frame.recipientHere[id])
                streamDiag.Reach(id, body::Distance(where, remotes[id].latest.position), fromProcessing,
                                 GetConfig().hostRoi, npcsend::InterestEnterCm);
    }
    SendNote& note = sendNotes[outKey];
    note.at = net::Now();
    if (journal::Pending(instance)) {
        note.reason = "pending"; streamDiag.Candidate(Drop::Pending);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
    }
    if (!mirrors.empty() && mirrors.count(outKey)) {
        note.reason = "mirror"; streamDiag.Candidate(Drop::Mirror);
        countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
    }
    if (deadStays && !net::IsHost() && !deadOnHost.empty() && deadOnHost.count(outKey)) {
        Entity corpse(instance);
        KeepDead(corpse);
        note.reason = "dead on host";
        return nullptr;
    }
    if (!givenBack.empty()) {
        const auto back = givenBack.find(outKey);
        if (back != givenBack.end() && static_cast<int32_t>(net::Now() - back->second) < 0) {
            note.reason = "givenback"; streamDiag.Candidate(Drop::GivenBack);
            countSkipped(outKey, net::NpcSendDecision::SkippedIrrelevant); return nullptr;
        }
    }
    outAt[0] = w.GetX(); outAt[1] = w.GetY(); outAt[2] = w.GetZ();
    if (const auto lastIt = lastSent.find(outKey); lastIt != lastSent.end()) outExistingLast = &lastIt->second;
    if (!CheckHostSendRange(instance, outKey, outAt, players, playerCount, outExistingLast, note)) {
        streamDiag.Candidate(Drop::Range);
        countSkipped(outKey, net::NpcSendDecision::SkippedFar);
        return nullptr;
    }
    return instance;
}

bool QueueNpcSnapshotTo(uint8_t id, net::NpcRecord const& record, std::vector<pose::Bone> const& bones,
                        bool posed, uint8_t poseFlags, bool essential, uint8_t intervalWire)
{
    return posed
        ? net::SendPosedNpcTo(id, record, bones.data(), static_cast<int>(bones.size()), poseFlags, essential, intervalWire)
        : net::SendNpcsTo(id, &record, 1, essential);
}

bool CaptureSkeletonPose(eCEntity* instance, uint8_t flags, LastSent& last,
                         std::vector<pose::Bone>& bones, uint8_t& poseFlags)
{
    if (flags & 128) {
        poseFlags = 0;
        bones.clear();
        last.posed = false;
        last.poseFlags = 0;
        last.bones.clear();
        return false;
    }
    poseFlags = 0;
    const bool posed = npcplayback::CapturePose(instance, bones, poseFlags,
        [](uint32_t hash, char const* name) {
            if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
        });
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    void const* nodesPtr = skeleton ? skeleton->mNodes.mData : nullptr;
    const int nodeCount = skeleton ? skeleton->mNodes.mLength : 0;
    if (last.cachedActor != actor || last.cachedSkeleton != skeleton ||
        last.cachedNodes != nodesPtr || last.cachedNodeCount != nodeCount || !last.skeletonEpoch) {
        if (++last.skeletonEpoch == 0) ++last.skeletonEpoch;
        last.cachedActor = actor;
        last.cachedSkeleton = skeleton;
        last.cachedNodes = nodesPtr;
        last.cachedNodeCount = nodeCount;
    }
    last.cachedSkeletonValid = posed;
    last.posed = posed;
    last.poseFlags = posed ? poseFlags : 0;
    if (posed) last.bones = bones;
    else last.bones.clear();
    return posed;
}

void SendNpcToRecipients(Entity& self, eCEntity* instance, eCEntity* hostFocus, eCEntity* hostTarget,
                         GuidKey const& key, uint8_t const guid[16], float const at[3], net::NpcRecord const& r,
                         LastSent& last, HostSendFrame const& frame, std::vector<pose::Bone>& bones,
                         npcsend::PoseMeta const& poseMeta, bool poseCaptured, uint8_t poseFlags, bool posed,
                         bool moving, bool outsideAll)
{
    using npcstream_diag::Drop;
    net::Remote const* remotes = net::Remotes();
    const auto recordAway = [&](uint8_t id) {
        if (net::InWorld(id) && net::PeerJoined(id))
            net::RecordNpcSendDecision(net::NpcSendDecision::SkippedIrrelevant);
        if (remotes[id].connected) streamDiag.Result(id, Drop::Away);
    };
    if (outsideAll) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            if (!frame.recipientHere[id]) recordAway(id);
            else { net::RecordNpcSendDecision(net::NpcSendDecision::SkippedFar); streamDiag.Result(id, Drop::Outside); }
        }
        return;
    }
    const uint32_t npcHash = net::HashGuid(guid);
    const bool hostFocusedOrTalk = hostFocus == instance || hostTarget == instance ||
        talk::IsHeld(instance) || talk::TalkingWithHero(instance);
    npc_equipment::BackEquipment backEquip{};
    bool backCaptured = false;
    const auto ensureBackEquip = [&]() {
        if (backCaptured) return;
        backCaptured = true;
        backEquip = {};
        backEquip.incarnation = r.incarnation;
        constexpr gESlot BackSlots[npc_equipment::BackSlotCount] = {
            gESlot_BackLeft, gESlot_BackRight, gESlot_Bow, gESlot_Crossbow
        };
        auto* inv = static_cast<gCInventory_PS*>(instance->GetPropertySet(eEPropertySetType_Inventory));
        for (size_t i = 0; i < npc_equipment::BackSlotCount; ++i) {
            const gESlot slot = BackSlots[i];
            const Entity backItem = self.Inventory.GetItemFromSlot(slot);
            const GEInt stackIdx = backItem.GetInstance() ? self.Inventory.FindStackIndex(slot) : -1;
            auto const* stack = inv && stackIdx >= 0 ? inv->GetStack(stackIdx) : nullptr;
            const bool merchandise = stackIdx >= 0 &&
                (self.Inventory.IsStackMerchandise(stackIdx) != GEFalse ||
                 (stack && stack->GetType() == gEStackType_Merchandise));
            if (!npc_equipment::ShouldCaptureHostBackSlot(backItem.GetInstance() != nullptr, merchandise)) continue;
            char const* backName = backItem.GetName().GetText();
            const uint32_t hash = net::HashName(backName);
            backEquip.slots[i] = hash;
            if (hash && namesSent.insert(hash).second) net::SendMotionName(hash, backName);
        }
    };
    static RecipientNpcState freshEntry;
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        if (!frame.recipientHere[id]) { recordAway(id); continue; }
        const float distance = body::Distance(at, remotes[id].latest.position);
        auto& states = recipientNpcs[id];
        const auto existing = states.find(key);
        if (existing == states.end()) freshEntry = RecipientNpcState{};
        RecipientNpcState& entry = existing == states.end() ? freshEntry : existing->second;
        npcsend::State& state = entry.policy;
        npcsend::Input input;
        input.record = &r;
        input.now = r.sent;
        input.distanceCm = distance;
        input.interest = (((r.flags >> 3) & 7) == id || r.partyPlayer == id || r.guidePlayer == id)
            ? npcsend::Interest::Include : npcsend::Interest::Distance;
        input.focused = distance <= npcsend::ContactSafetyCm ||
            (hostFocusedOrTalk && distance <= npcsend::InterestExitCm) ||
            (npcHash && net::RemoteStateAgeMs(id) <= 1000 &&
             (net::RemoteFocusNpc(id) == npcHash || net::RemoteTargetNpc(id) == npcHash));
        input.combat = (r.flags & 4) != 0;
        input.contact = r.contact != 0;
        input.moving = moving;
        input.stretchPermille = frame.stretchOf[id];

        npcsend::Result result{};
        bool evaluated = false;
        if (!poseCaptured) {
            npcsend::PoseMeta recipientMeta = poseMeta;
            recipientMeta.matchesBaseline = poseMeta.matchesBaseline &&
                entry.skeletonEpoch != 0 && entry.skeletonEpoch == last.skeletonEpoch;
            if (npcsend::Preflight(state, input, recipientMeta, result) == npcsend::PoseNeed::NotNeeded) {
                evaluated = true;
            } else if (result.decision == npcsend::Decision::Initial && !npcsend::ImmediateInitial(input) &&
                       !initialPacers[id].CanAdmit(r.sent, frame.stretchOf[id])) {
                initialPacers[id].Admit(r.sent, frame.stretchOf[id]);
                ++sendStats.paced;
                net::RecordNpcSendDecision(net::NpcSendDecision::SkippedPaced);
                streamDiag.Result(id, Drop::Paced);
                continue;
            } else {
                poseCaptured = true;
                posed = CaptureSkeletonPose(instance, r.flags, last, bones, poseFlags);
            }
        }
        if (!evaluated) {
            input.bones = posed ? bones.data() : nullptr;
            input.boneCount = posed ? static_cast<int>(bones.size()) : 0;
            input.poseFlags = posed ? poseFlags : 0;
            input.bonesVerified = posed;
            result = npcsend::Evaluate(state, input);
        }

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
            const bool finalQueued = QueueNpcSnapshotTo(id, r, bones, posed, poseFlags, true, intervalWire);
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
                !initialPacers[id].Admit(r.sent, frame.stretchOf[id])) {
                ++sendStats.paced;
                net::RecordNpcSendDecision(net::NpcSendDecision::SkippedPaced);
                streamDiag.Result(id, Drop::Paced);
                continue;
            }
            const uint8_t intervalWire = npclease::EncodePoseIntervalWire(
                npcsend::EffectivePresentationInterval(result, state, input));
            const bool queued = QueueNpcSnapshotTo(id, r, bones, posed, poseFlags, result.essential, intervalWire);
            const uint32_t previousSentAt = state.sentAt;
            const bool attackStart = combat_timing::RemoteCombat(r.flags, r.combatTarget) &&
                r.motions[0].active && state.record.motions[0].instance != r.motions[0].instance;
            const bool onSchedule = r.sent - state.idealAt >= npcsend::Stretched(npcsend::Interval(result.tier), frame.stretchOf[id]);
            if (queued && npcsend::Commit(state, input, result.decision)) {
                if (attackStart)
                    Logf("combat: NPCATTACK stage=send npc=%s player=%u instance=%u host_captured=%u host_sent=%u "
                         "phase=%.4f reliable=%d",
                         self.GetName().GetText(), id, r.motions[0].instance, r.sent, net::Now(), r.motions[0].phase,
                         result.essential ? 1 : 0);
                sendStats.Record(result.decision, result.tier, r.sent - previousSentAt, frame.stretchOf[id], onSchedule);
                RecipientNpcState& committed = existing == states.end()
                    ? states.emplace(key, std::move(freshEntry)).first->second
                    : entry;
                committed.skeletonEpoch = posed ? last.skeletonEpoch : 0;
                ensureBackEquip();
                const bool haveSentBack = committed.haveBackEquip && committed.backEquipJoin == knownJoins;
                if (npc_equipment::ShouldSendBackEquipment(
                        haveSentBack,
                        haveSentBack ? committed.backEquip : npc_equipment::BackEquipment{},
                        backEquip)) {
                    net::NpcEquipmentNote equipNote{};
                    std::memcpy(equipNote.guid, guid, sizeof(equipNote.guid));
                    equipNote.incarnation = backEquip.incarnation;
                    std::memcpy(equipNote.backSlots, backEquip.slots, sizeof(equipNote.backSlots));
                    if (net::SendNpcEquipmentTo(id, equipNote)) {
                        committed.backEquip = backEquip;
                        committed.backEquipJoin = knownJoins;
                        committed.haveBackEquip = true;
                    }
                }
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
        if (states.empty()) continue;
        for (auto it = states.begin(); it != states.end();) {
            if (owned.count(it->first)) { ++it; continue; }
            const auto note = sendNotes.find(it->first);
            const auto final = lastSent.find(it->first);
            const bool range = note != sendNotes.end() && !std::strcmp(note->second.reason, "range");
            bool finalQueued = !range;
            if (range && final != lastSent.end()) {
                const auto& last = final->second;
                const uint8_t intervalWire = npclease::EncodePoseIntervalWire(npcsend::QuietCadenceMs);
                finalQueued = QueueNpcSnapshotTo(id, last.record, last.bones, last.posed, last.poseFlags, true, intervalWire);
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
        const uint32_t now = net::Now();
        npcstream_diag::Context context;
        context.hostValid = hero != nullptr && playerCount > 0;
        if (context.hostValid) std::memcpy(context.hostAt, players[0], sizeof(context.hostAt));
        context.mode = GetConfig().hostSimSpheres ? "spheres" : GetConfig().hostSimElastic ? "elastic" : "off";
        context.roiCm = GetConfig().hostRoi;
        context.sim = world::HostSimulationStatus();
        context.perf = host_frame_diag::Global().TakeStreamSummary(now);
        streamDiag.Flush(now, context, [](char const* line) { Log(line); });
        for (auto& [key, last] : lastSent) {
            if (last.bones.capacity() == 0 || owned.find(key) != owned.end()) continue;
            bool activeRecipient = false;
            for (int id = 2; id < net::MaxPlayers; ++id) {
                if (recipientNpcs[id].find(key) != recipientNpcs[id].end()) {
                    activeRecipient = true;
                    break;
                }
            }
            if (!activeRecipient) {
                last.posed = false;
                last.poseFlags = 0;
                std::vector<pose::Bone>().swap(last.bones);
            }
        }
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
        npcplayback::ResetPublishedNames();
        for (auto const& entry : forceHistory) for (auto const& force : entry.second) net::SendRagdoll(force);
        for (auto const& entry : absences) net::SendNpcs(&entry.second, 1, true);
        journal::SyncToClients();
        infocmd::SyncToClients();
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
    bool needWorldSync = false;
    for (int id = 2; id < net::MaxPlayers; ++id) {
        const uint32_t generation = net::PeerGeneration(static_cast<uint8_t>(id));
        if (!net::InWorld(static_cast<uint8_t>(id)) || !remotes[id].connected || recipientGenerations[id] != generation) {
            recipientNpcs[id].clear();
            recipientGenerations[id] = generation;
            initialPacers[id].Reset();
            syncedWorldGen[id] = 0;
            syncedWorldId[id] = 0;
        }
        if (!remotes[id].connected) { joinedAt[id] = 0; continue; }
        anyClient = true;
        if (!(remotes[id].latest.flags & 1) || remotes[id].latest.world != remote::CurrentWorld()) continue;
        if (syncedWorldGen[id] != generation || syncedWorldId[id] != remotes[id].latest.world) {
            syncedWorldGen[id] = generation;
            syncedWorldId[id] = remotes[id].latest.world;
            needWorldSync = true;
        }
        std::memcpy(players[playerCount++], remotes[id].latest.position, sizeof(float) * 3);
    }
    if (needWorldSync) {
        journal::SyncToClients();
        infocmd::SyncToClients();
    }
    if (!anyClient || !playerCount) {
        npcplayback::SyncHostCaptureVisibility(nullptr, 0);
        return;
    }
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
    for (auto& sentBack : sentBackEquipment) sentBack.erase(key);
    pendingBackEquipment.erase(key);
    for (auto it = forceSequences.begin(); it != forceSequences.end();)
        if (it->first.guid == key) it = forceSequences.erase(it);
        else ++it;
}

bool IsDespawned(uint8_t const guid[16])
{
    if (!guid || absences.empty()) return false;
    const GuidKey key = Key(guid);
    const auto absent = absences.find(key);
    if (absent == absences.end()) return false;
    return !net::IsHost() || static_cast<int32_t>(Incarnation(guid) - absent->second.incarnation) <= 0;
}

bool EnsureHostDespawn(eCEntity* entity, uint8_t const guid[16])
{
    if (!net::IsHost() || !guid || !entity || npcremoval::Suppressed(guid)) return false;
    if (IsDespawned(guid)) return true;
    if (!entity->GetPropertySet(eEPropertySetType_NPC)) return false;
    Entity npc(entity);
    const auto& at = entity->GetWorldPosition();
    const float pos[3] = {at.GetX(), at.GetY(), at.GetZ()};
    if (static_cast<int>(npc.NPC.Species) != npcpresence::DespawnedSpecies &&
        !npcpresence::IsVoidDespawnPosition(pos)) {
        return false;
    }
    if (!IsNpc(entity)) return false;
    Despawn(entity);
    return true;
}

void Despawn(eCEntity* entity)
{
    if (!net::IsHost() || !IsNpc(entity) || npcremoval::Suppressed(entity)) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    const uint32_t inc = Incarnation(guid);
    const auto existing = absences.find(key);
    const bool alreadySent = existing != absences.end() && existing->second.incarnation == inc;
    net::NpcRecord record{};
    std::memcpy(record.guid, guid, sizeof(record.guid));
    record.incarnation = inc;
    record.sent = net::Now();
    record.flags = 1;
    if (auto* receiver = static_cast<gCDamageReceiver_PS*>(entity->GetPropertySet(eEPropertySetType_DamageReceiver))) {
        const int32_t maxHp = receiver->GetHitPointsMax();
        if (maxHp >= 1) record.hitPointsMax = maxHp;
    }
    record.aiMode = static_cast<uint8_t>(gEAIMode_Dead);
    const auto& at = entity->GetWorldPosition();
    const auto turn = entity->GetWorldRotation();
    record.position[0] = at.GetX(); record.position[1] = at.GetY(); record.position[2] = at.GetZ();
    if (!npcpresence::IsVoidDespawnPosition(record.position)) {
        record.position[0] = npcpresence::VoidDespawnX;
        record.position[1] = npcpresence::VoidDespawnY;
        record.position[2] = npcpresence::VoidDespawnZ;
    }
    record.rotation[0] = turn.GetX(); record.rotation[1] = turn.GetY();
    record.rotation[2] = turn.GetZ(); record.rotation[3] = turn.GetW();
    npcsend::SanitizeCapturedRecord(record);
    LastSent& last = lastSent[key];
    last.record = record;
    last.at = record.sent;
    last.heldSince = 0;
    last.heldLast = 0;
    last.cachedSkeletonValid = false;
    last.posed = false;
    last.poseFlags = 0;
    last.bones.clear();
    absences[key] = record;
    corpses.erase(key);
    sending.erase(key);
    owned.erase(key);
    forceHistory.erase(key);
    for (auto& states : recipientNpcs) states.erase(key);
    for (auto& sentBack : sentBackEquipment) sentBack.erase(key);
    ForgetCorpseGroundAnchor(key);
    if (!alreadySent) net::SendNpcs(&record, 1, true);
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
        absences.erase(key);
        hostHits.erase(key);
        forceHistory.erase(key);
        lastSent.erase(key);
        if (net::Connected()) {
            if (eCEntity* live = Resolve(guid).GetInstance(); IsNpc(live)) {
                net::NpcRecord r{};
                std::memcpy(r.guid, guid, sizeof(r.guid));
                r.incarnation = incarnation;
                r.sent = net::Now();
                const bCVector& at = live->GetWorldPosition();
                const bCQuaternion q = live->GetWorldRotation();
                r.position[0] = at.GetX(); r.position[1] = at.GetY(); r.position[2] = at.GetZ();
                r.rotation[0] = q.GetX(); r.rotation[1] = q.GetY(); r.rotation[2] = q.GetZ(); r.rotation[3] = q.GetW();
                if (auto* receiver = static_cast<gCDamageReceiver_PS*>(
                        live->GetPropertySet(eEPropertySetType_DamageReceiver))) {
                    r.hitPoints = receiver->GetHitPoints();
                    r.hitPointsMax = receiver->GetHitPointsMax();
                }
                r.aiMode = static_cast<uint8_t>(body::AIModeOf(live));
                npcsend::SanitizeCapturedRecord(r);
                net::SendNpcs(&r, 1, true);
                net::Remote const* remotes = net::Remotes();
                for (uint8_t id = 2; id < net::MaxPlayers; ++id)
                    if (net::InWorld(id) && remotes[id].connected && !recipientNpcs[id].count(key))
                        net::SendNpcReleaseTo(id, guid, 1);
            }
        }
    }
    if (newLife && deadOnHost.erase(key)) Logf("npcs: %s no longer held dead here (it stood up again)", Resolve(guid).GetName().GetText());
    if (newLife) ForgetCorpseGroundAnchor(key);
}

bool HostKnows(uint8_t const identity[16]); // implemented in npc_qa.cpp


namespace detail {

bool CheckHostSendRange(eCEntity* instance, GuidKey const& key, float const at[3],
                        float const players[][3], int playerCount, LastSent const* existingLast,
                        SendNote& note)
{
    float nearest = 1e30f;
    for (int k = 0; k < playerCount; ++k) nearest = std::min(nearest, body::Distance(at, players[k]));
    bool inRange = nearest <= GetConfig().hostRoi;
    if (!inRange && companionNear && !net::IsHost() && !myClaims.empty() && myClaims.count(key) && nearest <= KeepRadius)
        inRange = true;
    if (!inRange && !contested.empty()) {
        const auto held = contested.find(key);
        if (held != contested.end() && nearest <= KeepRadius) inRange = true;
        else if (held != contested.end()) contested.erase(held);
    }
    if (!inRange && hysteresis && existingLast && existingLast->heldLast) {
        const uint32_t t = net::Now();
        if (static_cast<int32_t>(t - existingLast->heldLast) <= 1500) {
            const bool young = static_cast<int32_t>(t - existingLast->heldSince) < static_cast<int32_t>(MinHold);
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
    return true;
}

LastSent& CaptureAndRecordOwnedNpc(Entity& self, eCEntity* instance, GuidKey const& key,
                                   float const at[3], HostSendFrame const& frame,
                                   LastSent* existingLast, net::NpcRecord& r,
                                   std::vector<pose::Bone>& bones, npcsend::PoseMeta& poseMeta,
                                   bool& poseCaptured, uint8_t& poseFlags, bool& posed,
                                   bool& moving, bool& outsideAll)
{
    LastSent& last = existingLast ? *existingLast : lastSent[key];
    const uint32_t t = net::Now();
    if (!last.heldLast || static_cast<int32_t>(t - last.heldLast) > 1500) last.heldSince = t;
    last.heldLast = t;
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

    r.movementMode = static_cast<uint8_t>(self.CharacterMovement.GetMovementMode());
    r.flags = static_cast<uint8_t>((self.IsDead() ? 1 : 0) | (self.IsDown() ? 2 : 0) | FightFlags(self));
    bool partyWaiting = false;
    if (instance->GetPropertySet(eEPropertySetType_Party)) {
        r.partyPlayer = PlayerOf(self.Party.GetPartyLeader().GetInstance());
        r.partyType = static_cast<uint8_t>(static_cast<gEPartyMemberType>(self.Party.PartyMemberType));
        partyWaiting = self.Party.Waiting != GEFalse;
    }
    for (uint8_t player = 1; player < net::MaxPlayers; ++player) {
        if (frame.partyLeaderOf[player] == instance) r.guidePlayer = player;
    }
    const bool navOnDestination = instance->GetPropertySet(eEPropertySetType_Navigation) &&
                                  self.Navigation.IsOnDestination != GEFalse;
    r.waiting = guide_policy::EncodeWaitingField(r.partyPlayer, r.guidePlayer, partyWaiting, navOnDestination);
    if (self.IsDead() && (r.movementMode == gECharMovementMode_RagDollDead ||
                          !std::strncmp(self.Routine.GetCurrentTask().GetText(), "ZS_RagDollDead", 14)))
        r.flags |= 128;
    r.hitPoints = self.DamageReceiver.HitPoints;
    r.hitPointsMax = self.DamageReceiver.HitPointsMax;
    r.aniState = static_cast<uint8_t>(static_cast<int>(self.Routine.AniState));
    r.aiMode = static_cast<uint8_t>(body::AIModeOf(instance));
    r.action = static_cast<uint8_t>(static_cast<gEAction>(self.Routine.Action));
    r.attackReason = static_cast<uint8_t>(static_cast<gEAttackReason>(self.NPC.AttackReason));
    r.sent = t;
    uint32_t& incarnation = incarnations[key];
    if (!incarnation) incarnation = 1;
    r.incarnation = incarnation;

    net::Remote const* remotes = net::Remotes();
    bool anyInterestedOrLive = false;
    bool needRemotePose = false;
    if (frame.anyRecipientHere) {
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            if (!frame.recipientHere[id]) continue;
            const bool activeMatch = ((r.flags >> 3) & 7) == id || r.partyPlayer == id || r.guidePlayer == id ||
                (!recipientNpcs[id].empty() && recipientNpcs[id].find(key) != recipientNpcs[id].end());
            const float dist = activeMatch ? 0.0f : body::Distance(at, remotes[id].latest.position);
            if (activeMatch || dist <= npcsend::InterestEnterCm) {
                anyInterestedOrLive = true;
                needRemotePose = true;
                break;
            }
            if (dist <= npcsend::InterestExitCm) anyInterestedOrLive = true;
        }
    }
    const bool parrying = r.aniState == gEAniState_Parade && combat::InParadeMode(instance);
    if (parrying) r.flags |= 64;

    outsideAll = !anyInterestedOrLive;
    poseMeta = {};
    poseCaptured = false;
    poseFlags = 0;
    posed = false;
    auto finalizeHostRecord = [&](bool capturedPose, bool keepNamesEpoch) {
        npcsend::SanitizeCapturedRecord(r);
        const bool revived = last.at && (last.record.flags & 3) && !(r.flags & 3);
        if (revived && (last.record.flags & 1) && r.incarnation == last.record.incarnation) {
            uint32_t& revivedIncarnation = incarnations[key];
            if (++revivedIncarnation == 0) ++revivedIncarnation;
            r.incarnation = revivedIncarnation;
            if (!hostHits.empty()) hostHits.erase(key);
            if (!forceHistory.empty()) forceHistory.erase(key);
            r.hitPlayer = 0;
            r.hitSequence = 0;
        }
        last.record = r;
        last.at = t;
        if (keepNamesEpoch) last.namesJoin = knownJoins;
        if (!capturedPose && (r.flags & 128)) {
            last.posed = false;
            last.poseFlags = 0;
            last.bones.clear();
        }
        SentStates& states = sentStates[key];
        states.at[states.next] = r.sent;
        states.parrying[states.next] = parrying;
        states.next = (states.next + 1) % StateHistory;
    };
    if (!anyInterestedOrLive) {
        finalizeHostRecord(false, false);
        return last;
    }

    const Entity heldRight = self.Inventory.GetItemFromSlot(gESlot_RightHand);
    const Entity heldLeft = self.Inventory.GetItemFromSlot(gESlot_LeftHand);
    Entity damageSource = heldRight;
    if (!damageSource.GetInstance() || !damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) damageSource = self;
    if (damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) {
        r.damageType = static_cast<uint8_t>(static_cast<gEDamageType>(damageSource.Damage.DamageType));
        r.damageAmount = damageSource.Damage.DamageAmount;
        r.damageMultiplier = damageSource.Damage.DamageHitMultiplier;
    }
    const bool needJoinPublish = !last.at || last.namesJoin != knownJoins;
    if (heldRight.GetInstance()) {
        const char* rightName = heldRight.GetName().GetText();
        r.heldRight = net::HashName(rightName);
        if ((needJoinPublish || r.heldRight != last.record.heldRight) && namesSent.insert(r.heldRight).second)
            net::SendMotionName(r.heldRight, rightName);
    }
    if (heldLeft.GetInstance()) {
        const char* leftName = heldLeft.GetName().GetText();
        r.heldLeft = net::HashName(leftName);
        if ((needJoinPublish || r.heldLeft != last.record.heldLeft) && namesSent.insert(r.heldLeft).second)
            net::SendMotionName(r.heldLeft, leftName);
    }
    char motion[128] = {};
    if (body::CurrentMotion(instance, motion, sizeof(motion)) && *motion) {
        r.motion = net::HashName(motion);
        if ((needJoinPublish || r.motion != last.record.motion) && namesSent.insert(r.motion).second)
            net::SendMotionName(r.motion, motion);
    }
    if (!hostHits.empty()) {
        const auto hit = hostHits.find(key);
        if (hit != hostHits.end()) {
            if (npcplayback_hit_policy::HostHitStampExpired(
                    hit->second.at, r.sent, StaggerState(self.Routine.GetCurrentTask().GetText()), motion)) {
                hostHits.erase(hit);
            } else {
                r.hitPlayer = hit->second.player;
                r.hitSequence = npcplayback_hit_policy::WrapNonZeroHitSeq(hit->second.sequence);
            }
        }
    }
    const bool allowOffscreenCapture = needRemotePose && !(r.flags & 1);
    const bool motionReady = npcplayback::Capture(instance, r, [](uint32_t hash, char const* name) {
        if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
    }, allowOffscreenCapture);
    if (!(r.flags & 128)) {
        auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
        auto* actor = animation ? animation->GetActor() : nullptr;
        auto* skeleton = actor ? actor->GetInstance() : nullptr;
        if (skeleton && skeleton->mNodes.mData &&
            skeleton->mNodes.mLength > 0 && skeleton->mNodes.mLength <= pose::MaxBones) {
            poseMeta.boneCount = skeleton->mNodes.mLength;
            poseMeta.poseFlags = animation->GetAnimationFactory().IsFacialAnimationStarted() ? pose::Facial : 0;
            poseMeta.matchesBaseline = last.cachedSkeletonValid &&
                last.cachedActor == actor &&
                last.cachedSkeleton == skeleton &&
                last.cachedNodes == skeleton->mNodes.mData &&
                last.cachedNodeCount == poseMeta.boneCount &&
                last.bones.size() == static_cast<size_t>(poseMeta.boneCount);
        }
    }
    const bool mayAttack = motionReady && !(r.flags & 128) && r.motions[0].weight > 0.0001f &&
        std::strstr(motion, "_Hit") != nullptr;
    if (mayAttack && body::LocallyAttacking(instance)) {
        poseCaptured = true;
        posed = CaptureSkeletonPose(instance, r.flags, last, bones, poseFlags);
        r.contact = posed ? 1 : 0;
    } else {
        r.contact = 0;
    }
    if (!poseMeta.boneCount && !motionReady && !(r.flags & 1) && !last.at)
        Logf("npcs: %s is waiting for its native skeletal pose; contacts remain disabled", self.GetName().GetText());
    finalizeHostRecord(poseCaptured, true);
    return last;
}

} // namespace detail

} // namespace g3t::npcs
