// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcs.h"

#include "ai.h"
#include "combat.h"
#include "combat_hostility.h"
#include "combat_timing.h"
#include "downed.h"
#include "journal.h"
#include "log.h"
#include "net_metrics.h"
#include "npc_companions.h"
#include "npc_contact.h"
#include "npc_corpse.h"
#include "npc_identity.h"
#include "npc_qa.h"
#include "npc_send.h"
#include "npc_state.h"
#include "npclease_presentation.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcsend_candidates.h"
#include "relation.h"
#include "relation_policy.h"
#include "talk.h"
#include "world.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace g3t::npcs {
namespace {
using namespace detail;

int OnNpcs(net::NpcRecord* records, int count, uint32_t now, uint32_t sent, uint8_t owner, uint32_t generation)
{
    if (net::IsHost() || owner != 1) return 0;
    if (!receiving) return 0;
    if (count > 0) {
        const uint32_t serial = net::WelcomeSerial();
        if (!hostStreamSeen || hostStreamSession != serial)
            hostStreamFirstSeenAt = now ? now : 1;
        hostStreamSeen = true;
        hostStreamSession = serial;
    }
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        const GuidKey key = Key(records[i].guid);
        const uint32_t captured = records[i].sent ? records[i].sent : sent;
        if (npclease::IgnoreHostSnapshot(records[i].guid, captured)) continue;
        if (!AdmitIncomingNpc(records[i], key, captured, owner, now)) continue;
        Mirror& m = mirrors[key];
        const bool sameOwner = m.owner == owner && m.ownerGeneration == generation &&
                               m.hostSession == net::WelcomeSerial() && m.latest.incarnation == records[i].incarnation;
        if (m.latestTicks && sameOwner && static_cast<int32_t>(captured - m.latestSent) < 0) continue;
        const bool reentered = m.released;
        m.released = false;
        if (m.latestTicks && (!sameOwner || reentered)) {
            ResetMirrorForNewStream(m, key, records[i], sameOwner);
            m.replay = npcplayback::State{};
            npclease::ForgetReceivedNpcPoseBefore(records[i].guid, records[i].incarnation, captured);
        }
        if (const auto pending = pendingBackEquipment.find(key);
            pending != pendingBackEquipment.end() && pending->second.incarnation == records[i].incarnation &&
            npc_equipment::ShouldAcceptBackEquipment(m.backEquipment.incarnation, pending->second.incarnation)) {
            m.backEquipment = pending->second;
        }
        if (!m.firstSeen) m.firstSeen = now ? now : 1;
        m.owner = owner;
        m.ownerGeneration = generation;
        m.hostSession = net::WelcomeSerial();
        if (records[i].motions[0].active &&
            combat_timing::IncomingCombat(records[i].flags, records[i].combatTarget, net::LocalId()) &&
            (!m.latestTicks || m.latest.motions[0].instance != records[i].motions[0].instance))
            Logf("combat: NPCATTACK stage=receive npc_hash=%08X player=%u instance=%u host_sample=%u client_received=%u phase=%.4f",
                 net::HashGuid(records[i].guid), net::LocalId(), records[i].motions[0].instance, captured, now,
                 records[i].motions[0].phase);
        const bool sameTick = m.latestTicks && sameOwner && captured == m.latestSent;
        if (!sameTick) {
            m.previous = m.latestTicks ? m.latest : records[i];
            m.previousTicks = m.latestTicks ? m.latestTicks : now;
            m.previousSent = m.latestTicks ? m.latestSent : captured;
        }
        m.latest = records[i];
        m.latestTicks = now;
        m.latestSent = captured;
        if (ragdoll::IsGroundOriginSample(records[i].flags, records[i].movementMode,
                                          {records[i].position[0], records[i].position[1], records[i].position[2]})) {
            std::memcpy(m.lastGroundPos, records[i].position, sizeof(m.lastGroundPos));
            m.lastGroundIncarnation = records[i].incarnation;
            m.haveLastGroundPos = true;
        }
        ++m.records;
        m.staleHeld = false;
        body::MotionState motion;
        PackMotionFromRecord(motion, records[i]);
        motion.combatTarget = records[i].combatTarget;
        m.body.track.predictive = false;
        m.body.track.combatPlayer = net::IsHost() ? 0 : net::LocalId();
        body::SetPoseInterval(m.body.track, npcplayback::PoseInterval(records[i]));
        body::Feed(m.body.track, records[i].position, records[i].rotation, captured, now, records[i].movementMode, motion);
        records[accepted++] = records[i];
    }
    return accepted;
}

void SendOwned(eCEntity* hero, float const players[][3], int playerCount)
{
    if (!net::IsHost()) return;
    owned.clear();
    lastPlayerCount = playerCount;
    for (int k = 0; k < playerCount; ++k) std::memcpy(lastPlayers[k], players[k], sizeof(float) * 3);
    eCEntity* controlled = remote::ControlledBody();
    static std::vector<npcsend_candidates::GuidCandidate> candidates;
    static HostSendFrame frame;
    static std::vector<pose::Bone> bones;
    candidates.clear();
    frame.Clear();
    const auto addCandidate = [&](eCEntity* instance, bool fromProcessed) {
        if (!instance || instance == hero || instance == controlled) return;
        if (!npcplayback::IsCharacter(instance) ||
            instance->GetPropertySet(eEPropertySetType_PlayerMemory) != nullptr ||
            remote::IsPuppet(instance)) return;
        uint8_t guid[16];
        guid::CopyEntity(instance, guid);
        const GuidKey key = ToGuidKey(guid);
        candidates.push_back(npcsend_candidates::GuidCandidate{
            key, true, false, false, false, false
        });
        frame.NoteCandidate(key, instance, fromProcessed);
    };
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) addCandidate(all.GetAt(i).GetInstance(), false);
    const size_t firstProcessed = candidates.size();
    auto const* processed = world::ProcessedEntities();
    for (GEU16 i = 0; processed && i < processed->GetCount(); ++i) addCandidate(processed->GetAt(i), true);
    const std::vector<GuidKey> candidateKeys = npcsend_candidates::Union(candidates);
    PrepareHostSendFrame(candidates, firstProcessed, static_cast<unsigned>(candidateKeys.size()),
                         processed ? processed->GetCount() : 0u, frame);
    eCEntity* hostFocus = controlled && controlled->GetPropertySet(eEPropertySetType_Focus)
        ? Entity(controlled).Focus.GetFocusEntity().GetInstance() : nullptr;
    eCEntity* hostTarget = controlled && controlled->GetPropertySet(eEPropertySetType_NPC)
        ? Entity(controlled).NPC.GetCurrentTarget().GetInstance() : nullptr;
    for (GuidKey const& candidateKey : candidateKeys) {
        uint8_t guid[16];
        GuidKey key{};
        float at[3] = {};
        eCEntity* instance = AdmitHostSendCandidate(candidateKey, hero, controlled, frame, players, playerCount,
                                                    guid, key, at);
        if (!instance) continue;
        net::NpcRecord r{};
        std::memcpy(r.guid, guid, 16);
        owned.insert(key);
        std::memcpy(r.position, at, sizeof(at));
        const bCQuaternion q = instance->GetWorldRotation();
        r.rotation[0] = q.GetX(); r.rotation[1] = q.GetY(); r.rotation[2] = q.GetZ(); r.rotation[3] = q.GetW();
        Entity self(instance);
        combat_hostility::SyncHostNpc(instance);
        r.attitude = relation::Outgoing(instance, !self.IsDead());
        eCEntity* combatTarget = self.NPC.GetCurrentTarget().GetInstance();
        if (combatTarget && !remote::IsPuppet(combatTarget) && combatTarget != controlled &&
            combatTarget != hero) {
            uint8_t targetGuid[16];
            guid::CopyEntity(combatTarget, targetGuid);
            r.combatTarget = net::HashGuid(targetGuid);
        } else {
            r.combatTarget = 0;
        }
        uint8_t poseFlags = 0;
        bool posed = false, moving = false;
        npc_equipment::BackEquipment backEquip{};
        CaptureAndRecordOwnedNpc(self, instance, key, frame.partyLeaderOf, r, backEquip, bones, poseFlags, posed, moving);
        SendNpcToRecipients(self, instance, hostFocus, hostTarget, key, guid, at, r, backEquip, bones, poseFlags, posed, moving, frame.stretchOf);
    }
    FinishOwnedTick(hero, players, playerCount);
}

void TryPlayerBlow(eCEntity* victim)
{
    eCEntity* hero = remote::ControlledBody();
    if (!playerContact.ready || !hero || !victim || Entity(victim).IsDead() || npcremoval::Suppressed(victim) ||
        !ClientPresent(victim)) return;
    ++playerSweepTests;
    if (!SweepsBody(victim, playerContact.previousA, playerContact.previousB,
                    playerContact.a, playerContact.b, playerContact.thickness, playerContact.bone == -2)) return;
    ++playerOverlaps;
    if (!ClearContact(hero, victim)) { ++playerObstructed; return; }
    if (combat::PlayerBlow(victim)) ++playerContactHits;
}

void LogClientDeath(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    LogClientDeathImpl(m, shown, now);
}

void TryLocalBlow(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    NoteIncomingClockTransition(m, shown, now);
    eCEntity* hero = m.body.contactTarget;
    if (!hero || !m.replayReady || !shown.contact || (shown.flags & 3) || *m.replay.localMotion ||
        now - m.latestTicks > 250 || !shown.motions[0].active) {
        if (*m.replay.localMotion) UpdateIncomingMissReason(m, "interrupted_by_local_stumble");
        FinishIncomingSwingDiag(m, shown, now);
        m.contactAt = 0;
        return;
    }
    if (combat::IgnoreFriendlyHit(hero, m.entity.GetInstance())) return;
    ++localContactWindows;
    const uint32_t instance = shown.motions[0].instance;
    if (m.contactInstance != instance) {
        FinishIncomingSwingDiag(m, shown, now);
        m.contactInstance = instance; m.contactConsumed = false; m.contactAt = 0;
        ResetIncomingSwing(m, instance, hero);
    }
    m.swingMaxPhase = std::max(m.swingMaxPhase, shown.motions[0].phase);
    float a[3], b[3], thickness = 0;
    int bone = -1;
    if (!PresentedContactPoints(m.entity, shown.heldRight, a, b, thickness, bone)) {
        ++localContactMissing;
        UpdateIncomingMissReason(m, "missing_weapon_geometry");
        m.contactAt = 0;
        return;
    }
    if (!combat_timing::HitPhaseReached(shown.motions[0].phase)) {
        RecordPreHitContact(m, hero, a, b, bone, shown.motions[0].phase, now);
        return;
    }
    const bool previous = m.contactAt && now - m.contactAt <= 100 && m.contactBone == bone;
    float oldA[3], oldB[3];
    std::memcpy(oldA, m.contactA, sizeof(oldA)); std::memcpy(oldB, m.contactB, sizeof(oldB));
    std::memcpy(m.contactA, a, sizeof(a)); std::memcpy(m.contactB, b, sizeof(b));
    m.contactAt = now; m.contactBone = bone;
    if (!previous || m.contactConsumed) return;
    ++localContactTests;
    char const* hitSource = "live-sweep";
    if (!EvaluateIncomingSwingSweep(hero, m, oldA, oldB, a, b, thickness, bone == -2, hitSource)) {
        ++localBlowsOutOfReach;
        return;
    }
    ++localContactOverlaps;
    ContactObstruction obstruction;
    if (!ClearContact(m.entity.GetInstance(), hero, &obstruction)) {
        UpdateIncomingMissReason(m, "wall_obstructed");
        std::snprintf(m.swingBlocker, sizeof(m.swingBlocker), "%s", obstruction.blocker);
        m.swingRayCm = obstruction.rayCm; m.swingHitCm = obstruction.hitCm;
        ++localContactWalls; ++localBlowsOutOfReach;
        return;
    }
    m.swingArcCount = 0;
    ++localBlowsJudged;
    LogLocalBlowContact(m, hero, shown, instance, hitSource, now);
    if (!combat::LocalBlow(m.entity.GetInstance())) {
        UpdateIncomingMissReason(m, "local_blow_rejected");
        std::snprintf(m.swingBlocker, sizeof(m.swingBlocker), "%s", combat::LastLocalBlowRejectReason());
        FinishIncomingSwingDiag(m, shown, now);
    }
    m.contactConsumed = true;
    m.swingLogged = true;
}

void MirrorTick() {}

NpcOpponents IndexNpcOpponents()
{
    NpcOpponents result;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* entity = all.GetAt(i).GetInstance();
        if (!entity || npcremoval::Suppressed(entity)) continue;
        uint8_t bytes[16];
        guid::CopyEntity(entity, bytes);
        const auto inserted = result.emplace(net::HashGuid(bytes), entity);
        if (!inserted.second && inserted.first->second != entity) inserted.first->second = nullptr;
    }
    return result;
}

eCEntity* NpcOpponent(NpcOpponents const& opponents, uint32_t hash)
{
    const auto found = opponents.find(hash);
    return found == opponents.end() ? nullptr : found->second;
}

struct TargetingWatch : TargetingWatchState {};

// Expose read-only native predicates without casting a base object to a fictitious derived instance.
struct FocusInspection : gCFocus_PS
{
    static float Range(gCFocus_PS const& focus) { return (focus.*&FocusInspection::GetCurrentMaxRange)(); }
    static float Priority(gCFocus_PS const& focus, eCEntity* e) { return (focus.*&FocusInspection::GetCurrentPriority)(e); }
    static bool Valid(gCFocus_PS const& focus, eCEntity* e) { return (focus.*&FocusInspection::IsValidEntity)(e); }
    static bool Check(gCFocus_PS& focus, eCEntity* e, float& angle)
    {
        return (focus.*&FocusInspection::FocusCheck)(e, e, 181.0f, angle);
    }
};

void LogFocusQueue(uint32_t serial)
{
    static auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleA("Script_Game.dll"));
    static const bool supported = VerifyFocusQueueLayout(module);
    if (!supported) {
        Logf("npcs: FOCUSQUEUE seq=%u layout=unsupported", serial);
        return;
    }
    struct Queue { unsigned char* data; int count; } queue{};
    if (!ReadProcessMemory(GetCurrentProcess(), module + 0x118d54, &queue, sizeof(queue), nullptr) ||
        queue.count < 0 || queue.count > 1024 || (queue.count && !queue.data)) {
        Logf("npcs: FOCUSQUEUE seq=%u layout=invalid", serial);
        return;
    }
    Logf("npcs: FOCUSQUEUE seq=%u count=%d", serial, queue.count);
    for (int i = 0; i < queue.count; ++i) {
        struct Action { int action, argument; eCEntity* target; } entry{};
        if (!ReadProcessMemory(GetCurrentProcess(), queue.data + i * 0xb0, &entry, sizeof(entry), nullptr)) {
            Logf("npcs: FOCUSQUEUE seq=%u slot=%d read=failed", serial, i);
            break;
        }
        Logf("npcs: FOCUSQUEUE seq=%u slot=%d action=%d argument=%d target=%s",
             serial, i, entry.action, entry.argument, FocusRawIdentity(entry.target).c_str());
    }
}

void LogFocusVictim(uint32_t serial)
{
    Entity victim = Resolve(targetingWatch.victim);
    eCEntity* entity = victim.GetInstance();
    if (!entity) {
        char id[40];
        guid::Format(targetingWatch.victim, id, sizeof(id));
        Logf("npcs: FOCUSVICTIM seq=%u id=%s loaded=0", serial, id);
        return;
    }
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    auto* receiver = static_cast<gCDamageReceiver_PS*>(entity->GetPropertySet(eEPropertySetType_DamageReceiver));
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    const Mirror* mirror = FocusMirror(entity);
    const bool npc = entity->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    Logf("npcs: FOCUSVICTIM seq=%u id=%s name=%.48s ai=%d hp=%ld dead=%d down=%d combat=%lu entity_flags=%08X group=%d ragdoll=%d life=%u terminal=%d shown_dead=%d latest_flags=%u shown_sent=%u latest_sent=%u",
         serial, FocusIdentity(entity).c_str(), entity->GetName().GetText(),
         routine ? static_cast<int>(victim.Routine.AIMode) : -1, receiver ? receiver->GetHitPoints() : -1L,
         routine && victim.IsDead(), routine && victim.IsDown(), npc ? static_cast<unsigned long>(victim.NPC.CombatState) : 0,
         FocusEntityFlags(entity), static_cast<int>(entity->GetCollisionGroup()), animation && animation->GetRagDollEnabled(),
         mirror ? mirror->deathLife.incarnation : 0, mirror && mirror->deathLife.dead, mirror && mirror->killed,
         mirror ? mirror->latest.flags : 0, mirror ? mirror->shownSent : 0, mirror ? mirror->latestSent : 0);
    if (npc)
        Logf("npcs: FOCUSVICTIMBIND seq=%u target=%s alternative=%s attacker=%s last=%s spell=%s",
             serial, FocusIdentity(victim.NPC.GetCurrentTarget().GetInstance()).c_str(),
             FocusIdentity(victim.NPC.GetAlternativeTarget().GetInstance()).c_str(),
             FocusIdentity(victim.NPC.GetCurrentAttacker().GetInstance()).c_str(),
             FocusIdentity(victim.NPC.GetLastAttacker().GetInstance()).c_str(),
             FocusIdentity(victim.NPC.GetSpellTarget().GetInstance()).c_str());
}

void LogFocusCandidate(uint32_t serial, eCEntity* hero, gCFocus_PS& focus, eCEntity* candidate,
                       gCInteraction_PS* interaction, bool inRoi, bool nativeListed)
{
    eCEntity* heroTarget = Entity(hero).NPC.GetCurrentTarget().GetInstance();
    Entity entity(candidate);
    const bool item = candidate->GetPropertySet(eEPropertySetType_Item) != nullptr;
    const bool collected = inRoi && candidate->IsPickable() && static_cast<int>(candidate->GetCollisionGroup()) != 5 &&
        (!item || !entity.Item.IsEquipped());
    auto* routine = static_cast<gCScriptRoutine_PS*>(candidate->GetPropertySet(eEPropertySetType_ScriptRoutine));
    auto* receiver = static_cast<gCDamageReceiver_PS*>(candidate->GetPropertySet(eEPropertySetType_DamageReceiver));
    auto* inventory = static_cast<gCInventory_PS*>(candidate->GetPropertySet(eEPropertySetType_Inventory));
    const bool npc = candidate->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    const bool dead = routine && entity.IsDead();
    bool loot = !inventory || (!dead && !candidate->IsKilled());
    for (int s = 0; !loot && s < inventory->GetStackCount(); ++s) {
        const auto* stack = inventory->GetStack(s);
        loot = stack && static_cast<int>(stack->GetCategory()) != 0 &&
            static_cast<int>(inventory->GetStackCategory(s)) != 9 && static_cast<int>(stack->GetType().GetValue()) != 2;
    }
    float angle = 0.0f;
    const bool valid = FocusInspection::Valid(focus, candidate);
    const bool check = collected && valid && FocusInspection::Check(focus, candidate, angle);
    eCEntity* nativeTarget = npc ? entity.NPC.GetCurrentTarget().GetInstance() : nullptr;
    const Mirror* mirror = FocusMirror(candidate);
    const int scriptPriority = GetScriptAdminExt().CallScript(bCString("GetFocusEntityPriority"), candidate, nullptr, 0);
    Logf("npcs: FOCUSCAND seq=%u id=%s name=%.48s roi=%d collected=%d native_listed=%d type_valid=%d loot=%d native_check=%d angle=%.1f priority=%.1f script=%d ai=%d hp=%ld dead=%d down=%d combat=%lu target=%s same_target=%d host_target=%08X ready=%d shown_sent=%u latest_sent=%u flags=%08X group=%d interaction_priority=%d user=%s",
         serial, FocusIdentity(candidate).c_str(), candidate->GetName().GetText(), inRoi, collected, nativeListed, valid, loot, check, angle,
         FocusInspection::Priority(focus, candidate), scriptPriority, routine ? static_cast<int>(entity.Routine.AIMode) : -1,
         receiver ? receiver->GetHitPoints() : -1L, dead, routine && entity.IsDown(),
         npc ? static_cast<unsigned long>(entity.NPC.CombatState) : 0, FocusIdentity(nativeTarget).c_str(),
         npc && nativeTarget == heroTarget, mirror ? mirror->shownCombatTarget : 0, Ready(candidate),
         mirror ? mirror->shownSent : 0, mirror ? mirror->latestSent : 0, FocusEntityFlags(candidate),
         static_cast<int>(candidate->GetCollisionGroup()), interaction ? static_cast<int>(interaction->GetFocusPriority().GetValue()) : -1,
         FocusIdentity(interaction ? interaction->AccessUser().GetEntity() : nullptr).c_str());
    if (npc)
        Logf("npcs: FOCUSRULE seq=%u id=%s attitude=%d last_fight=%d party=%s host_flags=%u latest_ai=%u latest_target=%08X life=%u terminal=%d shown_dead=%d",
             serial, FocusIdentity(candidate).c_str(), static_cast<int>(entity.NPC.AttitudeToPlayer2),
             static_cast<int>(entity.NPC.LastFightAgainstPlayer),
             FocusIdentity(candidate->GetPropertySet(eEPropertySetType_Party) ? entity.Party.GetPartyLeader().GetInstance() : nullptr).c_str(),
             mirror ? mirror->latest.flags : 0, mirror ? mirror->latest.aiMode : 0,
             mirror ? mirror->latest.combatTarget : 0, mirror ? mirror->deathLife.incarnation : 0,
             mirror && mirror->deathLife.dead, mirror && mirror->killed);
}

void LogFocusCandidates(uint32_t serial, eCEntity* hero, gCFocus_PS& focus)
{
    auto const& interactions = gCInteractionAdmin::GetInstance().GetInteractionsInROI();
    const float range = FocusInspection::Range(focus);
    std::vector<eCEntity*> cached;
    const bool haveCached = ReadFocusCandidates(serial, focus, cached);
    std::unordered_set<eCEntity*> native(cached.begin(), cached.end()), roi, seen;
    Logf("npcs: FOCUSSCAN seq=%u roi_count=%d max_range=%.1f scan_mode=%d cached_available=%d cached_count=%zu source=roi_and_last_native_collection",
         serial, interactions.GetCount(), range, static_cast<int>(focus.GetCombatMode()), haveCached, cached.size());
    for (int i = 0; i < interactions.GetCount(); ++i) {
        auto* interaction = interactions.GetAt(i);
        eCEntity* candidate = interaction ? interaction->GetEntity() : nullptr;
        if (candidate) roi.insert(candidate);
        if (!candidate || candidate == hero || candidate->GetDistanceToEntity(*hero) > range || !seen.insert(candidate).second) continue;
        LogFocusCandidate(serial, hero, focus, candidate, interaction, true, native.count(candidate) != 0);
    }
    for (auto* candidate : cached) {
        if (!candidate || !seen.insert(candidate).second) continue;
        if (!FocusProcessed(candidate)) {
            Logf("npcs: FOCUSCACHED seq=%u target=%s", serial, FocusRawIdentity(candidate).c_str());
            continue;
        }
        auto* interaction = static_cast<gCInteraction_PS*>(candidate->GetPropertySet(eEPropertySetType_Interaction));
        LogFocusCandidate(serial, hero, focus, candidate, interaction, roi.count(candidate) != 0, true);
    }
    for (auto const* records : {&mirrors, &corpses})
        for (auto const& entry : *records) {
            Entity live = Resolve(entry.second.latest.guid);
            eCEntity* candidate = live.GetInstance();
            if (!candidate || candidate == hero || candidate->GetDistanceToEntity(*hero) > range || !seen.insert(candidate).second) continue;
            auto* interaction = static_cast<gCInteraction_PS*>(candidate->GetPropertySet(eEPropertySetType_Interaction));
            LogFocusCandidate(serial, hero, focus, candidate, interaction, false, false);
        }
}

void LogFocusRelations(uint32_t serial, eCEntity* hero)
{
    eCEntity* victim = Resolve(targetingWatch.victim).GetInstance();
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        if (!entity.GetInstance() || !entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) continue;
        eCEntity* target = entity.NPC.GetCurrentTarget().GetInstance();
        eCEntity* attacker = entity.NPC.GetCurrentAttacker().GetInstance();
        eCEntity* last = entity.NPC.GetLastAttacker().GetInstance();
        if (target != hero && attacker != hero && last != hero &&
            (!victim || (target != victim && attacker != victim && last != victim))) continue;
        Logf("npcs: FOCUSREL seq=%u npc=%s target=%s attacker=%s last=%s party=%s ai=%d",
             serial, FocusIdentity(entity.GetInstance()).c_str(), FocusIdentity(target).c_str(),
             FocusIdentity(attacker).c_str(), FocusIdentity(last).c_str(),
             FocusIdentity(entity.GetInstance()->GetPropertySet(eEPropertySetType_Party) ? entity.Party.GetPartyLeader().GetInstance() : nullptr).c_str(),
             entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine) ? static_cast<int>(entity.Routine.AIMode) : -1);
    }
}

void TickTargetingWatch(uint32_t now)
{
    if (net::IsHost()) return;
    eCEntity* hero = remote::ControlledBody();
    if (!hero) hero = gCSession::GetInstance().GetHero();
    if (!hero || !hero->GetPropertySet(eEPropertySetType_NPC) || !hero->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;
    Entity local(hero);
    auto* routine = static_cast<gCScriptRoutine_PS*>(hero->GetPropertySet(eEPropertySetType_ScriptRoutine));
    auto* focus = static_cast<gCFocus_PS*>(hero->GetPropertySet(eEPropertySetType_Focus));
    auto* animation = static_cast<eCVisualAnimation_PS*>(hero->GetPropertySet(eEPropertySetType_Animation));
    const int weaponMode = static_cast<int>(gCEntity::GetCombatMode(hero));
    const int aiMode = static_cast<int>(local.Routine.AIMode);
    const uint32_t combat = local.NPC.CombatState;
    const bool changed = targetingWatch.body != hero || targetingWatch.weaponMode != weaponMode ||
        targetingWatch.aiMode != aiMode || targetingWatch.combatState != combat;
    const bool recent = targetingWatch.haveKill && now - targetingWatch.killedAt <= 30000;
    const bool drawn = weaponMode >= 2 && weaponMode <= 4;
    targetingWatch.body = hero;
    targetingWatch.weaponMode = weaponMode;
    targetingWatch.aiMode = aiMode;
    targetingWatch.combatState = combat;
    if (!changed && !(drawn && recent && now - targetingWatch.loggedAt >= 1000)) return;
    targetingWatch.loggedAt = now;
    const uint32_t serial = ++targetingWatch.serial;
    Logf("npcs: FOCUSSTATE seq=%u reason=%s hero=%s weapon=%d drawn=%d ai=%d combat=%u task=%.48s state=%.48s action=%d ani=%d focus_mode=%d lock=%d autocollect=%d autoaim=%d kill_age=%ld range_rating=%d overrides=%.1f/%.1f/%.1f/%.1f draw_name=%d highlight=%d/%d session_hero=%p",
         serial, changed ? "transition" : "postkill", FocusIdentity(hero).c_str(), weaponMode, drawn, aiMode, combat,
         local.Routine.GetCurrentTask().GetText(), local.Routine.GetCurrentState().GetText(),
         static_cast<int>(local.Routine.Action), static_cast<int>(local.Routine.AniState),
         focus ? static_cast<int>(focus->GetCurrentMode().GetValue()) : -1, focus && focus->GetFocusLocked(),
         focus && focus->GetAutoCollectFocus(), animation && animation->IsAutoAimingEnabled(),
         targetingWatch.haveKill ? static_cast<long>(now - targetingWatch.killedAt) : -1L,
         focus && focus->GetEnableRangeRating(), focus ? focus->GetOverrideItemPriority() : 0.0f,
         focus ? focus->GetOverrideCharacterPriority() : 0.0f, focus ? focus->GetOverrideInteractPriority() : 0.0f,
         focus ? focus->GetOverrideRange() : 0.0f, focus && focus->GetDrawFocusName(),
         focus && focus->GetHighlightFocus(), focus && focus->GetHighlightFocusInFight(), gCSession::GetInstance().GetHero());
    Logf("npcs: FOCUSBIND seq=%u focus=%s current=%s alternative=%s attacker=%s last=%s spell=%s",
         serial, FocusIdentity(focus ? local.Focus.GetFocusEntity().GetInstance() : nullptr).c_str(),
         FocusIdentity(local.NPC.GetCurrentTarget().GetInstance()).c_str(),
         FocusIdentity(local.NPC.GetAlternativeTarget().GetInstance()).c_str(),
         FocusIdentity(local.NPC.GetCurrentAttacker().GetInstance()).c_str(),
         FocusIdentity(local.NPC.GetLastAttacker().GetInstance()).c_str(),
         FocusIdentity(local.NPC.GetSpellTarget().GetInstance()).c_str());
    Logf("npcs: FOCUSLOOP seq=%u spu_self=%s spu_target=%s spu_other=%s alignment=%s autoaim=%s right=%p left=%p",
         serial, FocusRawIdentity(routine->GetSPU().GetSelfEntity()).c_str(),
         FocusRawIdentity(routine->GetSPU().GetTargetEntity()).c_str(),
         FocusRawIdentity(routine->GetSPU().GetOtherEntity()).c_str(),
         FocusIdentity(hero->GetPropertySet(eEPropertySetType_CharacterMovement) ? local.GetAlignmentTarget().GetInstance() : nullptr).c_str(),
         FocusRawIdentity(AutoAimTarget(animation)).c_str(), gCEntity::GetWeapon(hero, GETrue), gCEntity::GetWeapon(hero, GEFalse));
    LogFocusQueue(serial);
    if (recent) {
        LogFocusVictim(serial);
        if (focus) LogFocusCandidates(serial, hero, *focus);
        LogFocusRelations(serial, hero);
    }
}

unsigned attitudeRepairs = 0;

void KeepHostAttitude(Mirror& m, uint8_t wanted, char const* via)
{
    eCEntity* instance = m.entity.GetInstance();
    auto* properties = instance ? static_cast<gCNPC_PS*>(instance->GetPropertySet(eEPropertySetType_NPC)) : nullptr;
    if (!properties) return;
    wanted = combat_hostility::MirrorAttitude(instance, wanted);
    const int stored = static_cast<int>(static_cast<gEAttitude>(m.entity.NPC.AttitudeToPlayer2));
    const relation::Apply apply = relation::ApplyHost(stored, wanted, m.attitudeWritten);
    if (apply == relation::Apply::Ignore) return;
    if (apply == relation::Apply::Keep) {
        m.attitudeWritten = wanted;
        return;
    }
    if (apply == relation::Apply::Repair) {
        ++m.attitudeDrifts;
        if (++attitudeRepairs <= 40 || m.attitudeDrifts == 1)
            Logf("relation: mismatch kind=native-drift npc=%s stored=%d written=%u host=%u via=%s; repaired",
                 m.entity.GetName().GetText(), stored, static_cast<unsigned>(m.attitudeWritten), static_cast<unsigned>(wanted), via);
    }
    properties->AccessAttitudeToPlayer2().AccessValue() = static_cast<gEAttitude>(wanted);
    m.attitudeWritten = wanted;
}

void RecordMirrorFirstDeath(Mirror& m, net::NpcRecord const& record, uint32_t now)
{
    if (m.deathLogged) return;
    m.fallbackGraceUntil = now ? now + 500 : 501;
    m.focusWatchUntil = now ? now + 5000 : 5001;
    m.focusWatchLogged = false;
    strcpy_s(m.presentationSource, RagdollSource(m));
    if (record.hitPlayer)
        std::snprintf(m.killerAttribution, sizeof(m.killerAttribution), "reported_player_%u", record.hitPlayer);
    LogClientDeath(m, record, now);
}

void PassiveMirrorTick()
{
    if (net::IsHost()) return;
    const uint32_t now = net::Now();
    const auto opponents = mirrors.empty() ? NpcOpponents{} : IndexNpcOpponents();
    BeginPassiveMirrorFrame(now);
    for (auto it = mirrors.begin(); it != mirrors.end();) {
        Mirror& m = it->second;
        if (npclease::IgnoreHostSnapshot(m.latest.guid) || npclease::LocalOwner(m.latest.guid)) { ++it; continue; }
        eCEntity* previous = m.entity.GetInstance();
        Entity live = Resolve(m.latest.guid);
        if (live.GetInstance() != m.entity.GetInstance() || (m.resolved && !IsNpc(live.GetInstance()))) {
            ResetUnloadedMirror(m, live, previous);
            m.attitudeWritten = relation::Unset;
        }
        if (!m.resolved || !m.entity.GetInstance()) {
            if (m.resolvedAt && now - m.resolvedAt < RetryLookup && !IsNpc(live.GetInstance())) { ++it; continue; }
            m.entity = Resolve(m.latest.guid);
            m.resolvedAt = now ? now : 1;
            m.resolved = IsNpc(m.entity.GetInstance());
            m.missing = !m.resolved;
            if (!m.resolved) { ++it; continue; }
            npcplayback::GuardForLease(m.entity.GetInstance());
            npcplayback::Silence(m.entity.GetInstance());
            if (m.entity.GetInstance()->GetPropertySet(eEPropertySetType_CharacterMovement))
                m.entity.CharacterMovement.StopMovement();
            if (auto* routine = static_cast<gCScriptRoutine_PS*>(
                m.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine))) {
                strncpy_s(m.body.routine, static_cast<bCString const&>(routine->GetRoutine()).GetText(), _TRUNCATE);
                m.body.aiMode = static_cast<int>(static_cast<gEAIMode>(m.entity.Routine.AIMode));
            }
            m.body.pacified = true;
            m.defeatedBefore = m.entity.NPC.DefeatedByPlayer != GEFalse;
            m.guarded = true;
        }
        if (m.killed && (m.latest.flags & 1)) {
            SyncMirrorBackEquipment(m, m.latest, true);
            m.deathClearCount += ClearLocalReferencesToDeadMirror(m.entity.GetInstance(), &m.deathCombatStopCount,
                m.deathReferencesCleared != m.entity.GetInstance());
            m.deathReferencesCleared = m.entity.GetInstance();
        }
        TickDeathFocusWatch(m, now);
        m.replayReady = false;
        if (!HostPresentationAvailable(m.entity.GetInstance())) {
            KeepHostAttitude(m, m.latest.attitude, "held");
            if (!m.staleHeld) {
                m.staleHeld = true;
                ResetMirrorIncomingContact(m);
                npcplayback::ResetLocalHitState(m.replay);
                ++staleMirrorReleases;
                Logf("npclease: degraded name=%s reason=%s age_ms=%u contacts=off",
                     m.entity.GetName().GetText(), m.released ? "host release" : "presentation unavailable",
                     now - m.latestTicks);
            }
            if (m.latest.flags & 3) npcplayback::ResetLocalHitState(m.replay);
            const bool bufferedAliveAhead = !m.killed && !m.released && net::Connected() &&
                now - m.latestTicks <= StaleAfter && m.body.track.haveRenderTime &&
                m.body.track.count > 0 && !(m.body.track.snaps[0].motion.flags & 1);
            if ((m.latest.flags & 1) && !bufferedAliveAhead) {
                m.deathLife.Observe(m.latest.incarnation, true);
                m.killed = true;
                if (!m.killedAt) m.killedAt = now ? now : 1;
                if (auto* receiver = static_cast<gCDamageReceiver_PS*>(
                        m.entity.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver)))
                    receiver->AccessHitPoints() = 0;
                KeepDead(m.entity);
                SyncMirrorBackEquipment(m, m.latest, true);
                m.deathClearCount += ClearLocalReferencesToDeadMirror(m.entity.GetInstance(), &m.deathCombatStopCount,
                    m.deathReferencesCleared != m.entity.GetInstance());
                m.deathReferencesCleared = m.entity.GetInstance();
                RecordMirrorFirstDeath(m, m.latest, now);
                PresentDeadMirror(it->first, m, m.latest, now);
            } else if (m.latest.flags & 2) {
                if (auto* routine = static_cast<gCScriptRoutine_PS*>(
                        m.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine))) {
                    routine->AccessAIMode().AccessValue() = static_cast<gEAIMode>(m.latest.aiMode);
                    routine->AccessAniState().AccessValue() = static_cast<gEAniState>(m.latest.aniState);
                }
                if (!m.replay.initialized)
                    m.replayReady = npcplayback::Replay(m.entity.GetInstance(), m.replay, m.latest, &MotionNameOf, now);
            } else if (!YieldMirrorForClientTalk(m, m.latest.flags, now) && ClientPresent(m.entity.GetInstance())) {
                npcplayback::HoldUnavailable(m.entity.GetInstance(), m.replay);
            }
            ++it;
            continue;
        }
        if (m.staleHeld || m.replay.holding) {
            npcplayback::ReleaseUnavailable(m.entity.GetInstance());
            m.staleHeld = false;
        }
        m.replay.holding = false;
        body::Presentation presentation;
        net::NpcRecord shown = m.latest;
        m.body.track.predictive = false;
        if (!body::Sample(m.body.track, now, shown.position, shown.rotation, -body::PlaybackDelay(m.body.track), true, &presentation)) {
            KeepHostAttitude(m, m.latest.attitude, "unsampled");
            ++it;
            continue;
        }
        CopyMotionToRecord(shown, presentation);
        shown.combatTarget = presentation.motion.combatTarget;
        if (!m.deathLife.Observe(shown.incarnation, (shown.flags & 1) != 0)) { ++it; continue; }
        m.shownSent = presentation.sent;
        shown.sent = presentation.sent;
        m.havePresentation = true;
        if (!(shown.flags & 3)) {
            auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
            if (animation && animation->GetRagDollEnabled()) animation->StopRagDoll();
        }
        const bool localDeath = m.entity.IsDead();
        if (npclease::ReconcileLife(localDeath, m.killed, (shown.flags & 1) != 0)) {
            ReconcileMirrorAlive(it->first, m);
            if (localDeath)
                Logf("NPCRECONCILE name=%s incarnation=%u local_dead=1 host_alive=1 host_hp=%d receive_age_ms=%u",
                     m.entity.GetName().GetText(), shown.incarnation, shown.hitPoints, now - m.latestTicks);
        }
        if (shown.flags & 3) {
            ResetMirrorIncomingContact(m);
            npcplayback::ResetLocalHitState(m.replay);
        }
        const bool firstDeath = !m.killed;
        m.killed = (shown.flags & 1) != 0;
        if (m.killed && (firstDeath || m.deathReferencesCleared != m.entity.GetInstance())) {
            SyncMirrorBackEquipment(m, shown, true);
            if (m.deathReferencesCleared != m.entity.GetInstance()) {
                m.deathClearCount = ClearLocalReferencesToDeadMirror(m.entity.GetInstance(), &m.deathCombatStopCount);
                m.deathReferencesCleared = m.entity.GetInstance();
            }
            RecordMirrorFirstDeath(m, shown, now);
        } else if (!m.killed) {
            m.deathReferencesCleared = nullptr;
        }
        auto* routine = m.entity.GetInstance()
            ? static_cast<gCScriptRoutine_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine))
            : nullptr;
        auto* receiver = m.entity.GetInstance()
            ? static_cast<gCDamageReceiver_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver))
            : nullptr;
        auto* npcProperties = m.entity.GetInstance()
            ? static_cast<gCNPC_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC))
            : nullptr;
        if (!routine || !receiver || !npcProperties) {
            if (m.entity.GetInstance())
                Logf("npcs: %s lost its script property during presentation; rebinding next frame", m.entity.GetName().GetText());
            m.resolved = false;
            m.resolvedAt = 0;
            ++it;
            continue;
        }
        m.attitudeShown = shown.attitude;
        KeepHostAttitude(m, shown.attitude, "shown");
        receiver->AccessHitPointsMax() = shown.hitPointsMax;
        receiver->AccessHitPoints() = m.killed ? 0 : shown.hitPoints;
        SyncMirrorPartyAndGuide(it->first, m, shown);
        if (!m.killed && YieldMirrorForClientTalk(m, shown.flags, now)) { ++it; continue; }
        routine->AccessAIMode().AccessValue() = m.killed ? gEAIMode_Dead : static_cast<gEAIMode>(shown.aiMode);
        routine->AccessAction().AccessValue() = static_cast<gEAction>(shown.action);
        npcProperties->AccessAttackReason().AccessValue() = static_cast<gEAttackReason>(shown.attackReason);
        m.shownCombatTarget = shown.combatTarget;
        const uint8_t target = (shown.flags >> 3) & 7;
        eCEntity* opponent = nullptr;
        if (!m.killed) {
            opponent = shown.combatTarget ? NpcOpponent(opponents, shown.combatTarget)
                : target ? (target == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(target)) : nullptr;
            if (shown.combatTarget && !opponent && m.unresolvedCombatTarget != shown.combatTarget)
                Logf("npcs: FOCUSTARGET mirror=%p name=%s hash=%08X result=missing_or_ambiguous",
                     m.entity.GetInstance(), m.entity.GetName().GetText(), shown.combatTarget);
        }
        m.unresolvedCombatTarget = !m.killed && shown.combatTarget && !opponent ? shown.combatTarget : 0;
        npcProperties->AccessCurrentTargetEntity().SetEntity(opponent);
        routine->AccessAniState().AccessValue() = static_cast<gEAniState>(shown.aniState);
        if (m.killed) {
            PresentDeadMirror(it->first, m, shown, now);
            ++it;
            continue;
        }
        SyncMirrorEquipmentAndDamage(m, shown);
        m.replayReady = npcplayback::Replay(m.entity.GetInstance(), m.replay, shown, &MotionNameOf, now);
        if (m.replayReady) {
            RecordMirrorWalkTelemetry(m, shown, now);
            m.body.contactTarget = combat_timing::IncomingCombat(shown.flags, shown.combatTarget, net::LocalId())
                                      ? remote::ControlledBody() : nullptr;
            TryLocalBlow(m, shown, now);
        } else {
            npcplayback::SetEntityPose(m.entity.GetInstance(), body::Pose(*m.entity.GetInstance(), shown.position, shown.rotation));
        }
        ++it;
    }
    if (playerContact.ready) {
        const auto all = Entity::GetNPCs();
        for (int i = 0; i < all.GetCount(); ++i) {
            eCEntity* victim = all.GetAt(i).GetInstance();
            if (npcplayback::IsPassive(victim)) TryPlayerBlow(victim);
        }
    }
}
} // namespace

bool MirrorFightsHero() { return detail::MirrorFightsHeroImpl(); }

void LeaseReleaseMirror(uint8_t const identity[16])
{
    const auto key = Key(identity);
    if (const auto found = mirrors.find(key); found != mirrors.end()) {
        auto& mirror = found->second;
        if (mirror.entity.GetInstance()) {
            npcplayback::ReleaseUnavailable(mirror.entity.GetInstance());
            if (auto* routine = static_cast<gCScriptRoutine_PS*>(
                mirror.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine)))
                static_cast<bCString&>(routine->AccessRoutine()) = bCString(mirror.body.routine);
            if (mirror.guarded) mirror.entity.NPC.DefeatedByPlayer = mirror.defeatedBefore;
        }
        mirrors.erase(found);
    }
    npclease::ForgetReceivedNpcPose(identity);
    pendingBackEquipment.erase(key);
    deadOnHost.erase(key); corpses.erase(key); absences.erase(key); ForgetCorpseGroundAnchor(key);
}

uint32_t HostPresentationAge(eCEntity const* entity)
{
    if (!entity) return UINT32_MAX;
    uint8_t identity[16];
    Guid(const_cast<eCEntity*>(entity), identity);
    const auto found = mirrors.find(Key(identity));
    return found == mirrors.end() || !found->second.latestTicks ? UINT32_MAX :
        net::Now() - found->second.latestTicks;
}

bool HostPresentationAvailable(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t identity[16];
    Guid(const_cast<eCEntity*>(entity), identity);
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end() || found->second.hostSession != net::WelcomeSerial()) return false;
    const auto& mirror = found->second;
    if (mirror.entity.GetInstance() != entity) return false;
    if (mirror.released)
        return net::Connected() && net::Now() - mirror.latestTicks <= StaleAfter && HostReleasePending(identity);
    const uint32_t poseInt = npcplayback::PoseInterval(mirror.latest);
    const uint32_t effectiveInt = poseInt ? poseInt : npcsend::QuietCadenceMs;
    return npclease::PresentationFresh(net::Connected(), mirror.released, net::Now() - mirror.latestTicks,
        effectiveInt);
}

bool HostAttitude(eCEntity const* entity, int& attitude)
{
    if (!entity) return false;
    uint8_t identity[16];
    Guid(const_cast<eCEntity*>(entity), identity);
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end()) return false;
    const Mirror& m = found->second;
    const uint8_t value = m.attitudeWritten != relation::Unset ? m.attitudeWritten : m.latest.attitude;
    if (!relation::MirrorAnswersFromHost(m.entity.GetInstance() == entity, m.hostSession == net::WelcomeSerial(),
                                         m.latestTicks != 0, value)) return false;
    attitude = value;
    return true;
}

bool RelationOf(eCEntity const* entity, RelationInfo& info)
{
    return detail::RelationOfImpl(entity, info);
}

bool HostStreamClaimed(uint8_t const identity[16])
{
    const auto found = mirrors.find(Key(identity));
    return found != mirrors.end() && npclease::HostClaim(net::Connected(),
        found->second.hostSession == net::WelcomeSerial(), found->second.released, found->second.records);
}

uint32_t HostStreamDelay()
{
    uint32_t youngest = UINT32_MAX;
    const uint32_t now = net::Now();
    for (auto const& item : mirrors) {
        auto const& mirror = item.second;
        if (!npclease::HostClaim(net::Connected(), mirror.hostSession == net::WelcomeSerial(),
                mirror.released, mirror.records) || (mirror.latest.flags & 1)) continue;
        auto const& track = mirror.body.track;
        const uint32_t age = track.haveOffset
            ? static_cast<uint32_t>(std::max(static_cast<int32_t>(
                now - static_cast<uint32_t>(track.offset) - mirror.latest.sent), 0))
            : now - mirror.latestTicks;
        youngest = std::min(youngest, age);
    }
    return youngest == UINT32_MAX ? 0 : youngest;
}

net::NpcRecord const* CombatRecord(eCEntity const* entity)
{
    if (!entity || npcremoval::Suppressed(entity) || npclease::Fenced(entity) || !net::Connected()) return nullptr;
    uint8_t identity[16];
    Guid(const_cast<eCEntity*>(entity), identity);
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end() || found->second.released || found->second.hostSession != net::WelcomeSerial() ||
        found->second.entity.GetInstance() != entity ||
        found->second.latest.incarnation != Incarnation(identity) ||
        net::Now() - found->second.latestTicks > StaleAfter) return nullptr;
    return &found->second.latest;
}

bool CombatTarget(eCEntity* entity, eCEntity*& target)
{
    target = nullptr;
    if (!entity || !entity->GetPropertySet(eEPropertySetType_NPC)) return false;
    if (IsMirrored(entity)) {
        auto const* record = CombatRecord(entity);
        if (!record) return false;
        if (record->combatTarget) {
            target = NpcOpponent(IndexNpcOpponents(), record->combatTarget);
            return target != nullptr;
        }
        const uint8_t player = (record->flags >> 3) & 7;
        target = player ? (player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player)) : nullptr;
        return !player || target;
    }
    target = Entity(entity).NPC.GetCurrentTarget().GetInstance();
    return true;
}

void AwaitHit(eCEntity* entity, uint32_t sequence)
{
    if (!entity || !sequence) return;
    uint8_t identity[16];
    Guid(entity, identity);
    awaitedHits[Key(identity)] = {sequence, Incarnation(identity), npcplayback::MotionInstance(remote::ControlledBody())};
}

void OnWorldDeactivate()
{
    quarantinedEntities.clear();
    identityCensusAt = identityLogAt = 0;
    targetingWatch = TargetingWatch{};
    mirrors.clear();
    detail::ResetWorldNpcTables();
}

void OnEntityDestroyed(eCEntity const* entity)
{
    if (!entity) return;
    for (auto it = quarantinedEntities.begin(); it != quarantinedEntities.end();) {
        if (it->first == entity || it->second.root == entity) it = quarantinedEntities.erase(it);
        else ++it;
    }
    hiddenClientStates.erase(entity);
    if (targetingWatch.body == entity) targetingWatch.body = nullptr;
    auto clearDestroyedPointers = [entity](Mirror& m) {
        if (m.deathReferencesCleared == entity) m.deathReferencesCleared = nullptr;
        if (m.watchedFocus == entity) m.watchedFocus = nullptr;
        if (m.body.contactTarget == entity) m.body.contactTarget = nullptr;
        if (m.entity.GetInstance() == entity) {
            m.corpseActor = (m.killed && m.corpseSettled && !m.corpsePose.empty())
                ? reinterpret_cast<void const*>(1)
                : nullptr;
        }
    };
    for (auto& entry : mirrors) clearDestroyedPointers(entry.second);
    for (auto& entry : corpses) clearDestroyedPointers(entry.second);
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

void Start()
{
    (void)&MirrorTick;
    net::SetNpcHandlers(&OnNpcs, &OnMotionName);
    net::SetNpcReleaseHandler(&OnRelease);
    net::SetNpcEquipmentHandler(&OnNpcEquipment);
    net::SetRagdollHandler(&OnRagdollNote);
    net::SetHitResultHandler(&OnHitResult);
}

void Tick()
{
    gCSession& session = gCSession::GetInstance();
    if (!session.IsGameRunning() || !session.GetHero()) return;
    Entity hero(session.GetHero());
    const int xp = static_cast<int>(hero.PlayerMemory.XP);
    if (lastXp >= 0 && xp != lastXp) Logf("xp: %+d total=%d", xp - lastXp, xp);
    lastXp = xp;
    npclease::Tick();
    if (net::IsHost()) HostTick(&SendOwned);
    InstallRagdollHook(net::Now());
    PassiveMirrorTick();
    CorpseTick(net::Now());
    detail::ReconcileClientPresence();
    TickTargetingWatch(net::Now());
}

} // namespace g3t::npcs
