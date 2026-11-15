// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcplayback.h"
#include "npcplayback_hit_policy.h"
#include "npcplayback_hooks.h"
#include "npcplayback_skeleton.h"
#include "npcsend_policy.h"
#include "combat_timing.h"
#include "guid_key.h"
#include "npclease_runtime.h"
#include "npclease_presentation.h"

#include "body.h"
#include "downed.h"
#include "log.h"
#include "net.h"
#include "remote.h"
#include "npcs.h"
#include "npcremoval.h"
#include "guards.h"
#include "guid.h"
#include "talk.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace g3t::npcplayback {
namespace {
using detail::ActorProcess;
using detail::Animation;
using detail::ApplyPose;
using detail::FullPose;
using detail::NativePose;
using detail::NativePoseScope;
using detail::PrepareTransforms;
using detail::PresentMotion;
using detail::PresentationScope;
using detail::Rig;
using detail::Slots;
using detail::VisualScope;

struct ProbeSentinel {
    eCEntity const* target = nullptr;
};
constexpr ProbeSentinel probe{};

struct ReceivedPose {
    pose::History history;
    uint32_t arrived = 0, incarnation = 0, intervalMs = 16;
};
std::unordered_map<GuidKey, ReceivedPose, GuidHash> receivedPoses;

struct Unavailable {
    eCEntity const* entity = nullptr;
    State state;
    uint32_t seen = 0, attempted = 0;
    uint32_t hitSwing = 0;
    uint32_t evaluatedTick = 0;
    std::string candidate;
    std::string leaseReason;
    char const* reason = "no evaluated idle or held pose";
    bool noted = false, failed = false, retry = false, evaluatedOk = false;
};
std::unordered_map<GuidKey, Unavailable, GuidHash> unavailable;

struct ProvenPose {
    eCEntity const* entity = nullptr;
    npclease::PresentationProof proof;
    std::vector<pose::Bone> bones;
    uint8_t flags = 0;
    uint32_t seen = 0;
    uint32_t idleStarted = 0;
    std::string idleMotion;
    float reactionDuration = 0.0f;
};
std::unordered_map<GuidKey, ProvenPose, GuidHash> provenPoses;

uint32_t poseCleanupAt = 0;
uint32_t playbackTick = 0;
bool enabled = false, clientMode = false;
std::atomic<uint32_t> predictedHits{0}, skippedEchoes{0}, presentedFrames{0};
std::atomic<uint32_t> posedFrames{0}, poseWaits{0}, stalePoses{0};
float maximumReplayPhaseError = 0.0f;

bool Passive(eCEntity const* entity)
{
    if (!entity || guards::IsLocalPlayer(entity)) return false;
    if (entity == probe.target) return true;
    if (!enabled || !IsCharacter(entity)) return false;
    if (entity->GetPropertySet(eEPropertySetType_PlayerMemory) || entity == gCSession::GetInstance().GetHero() ||
        entity == remote::ControlledBody() || remote::IsPuppet(entity)) return false;
    return npclease::Fenced(entity) || (clientMode && !npclease::LocalAuthority(entity));
}
}

void OnWorldDeactivate()
{
    receivedPoses.clear();
    unavailable.clear();
    provenPoses.clear();
    detail::ClearSkeletonCache();
    detail::ResetHooksState();
    poseCleanupAt = 0;
    playbackTick = 0;
}

bool Start(bool client)
{
    clientMode = client;
    enabled = true;
    net::SetNpcPoseHandler([](net::NpcRecord const& record, pose::Bone const* bones, int count, uint8_t flags, bool, uint8_t interval) {
        if (net::IsHost() || npcremoval::Suppressed(record.guid) ||
            npclease::IgnoreHostSnapshot(record.guid, record.sent)) return;
        auto& received = receivedPoses[ToGuidKey(record.guid)];
        if (received.history.Push(record.incarnation, record.sent, bones, count, flags)) {
            received.arrived = net::Now();
            received.incarnation = record.incarnation;
            received.intervalMs = npclease::DecodePoseIntervalWire(interval);
            npclease::SyncMirrorPoseInterval(record.guid, record.incarnation, received.intervalMs);
        }
        else ++stalePoses;
    });
    if (!detail::InstallHooks()) {
        enabled = false;
        Log("FAIL NPC replication cannot start: required native playback hooks are unavailable");
        return false;
    }
    return true;
}

bool Available() { return enabled; }

bool IsCharacter(eCEntity const* entity)
{
    return entity && npclease::CharacterPresentation(entity->GetName().GetText(),
        entity->GetPropertySet(eEPropertySetType_NPC) != nullptr,
        entity->GetPropertySet(eEPropertySetType_ScriptRoutine) != nullptr,
        entity->GetPropertySet(eEPropertySetType_CharacterMovement) != nullptr,
        entity->GetPropertySet(eEPropertySetType_DamageReceiver) != nullptr);
}

void ForgetRemoved(uint8_t const guid[16])
{
    const GuidKey key = ToGuidKey(guid);
    receivedPoses.erase(key);
    unavailable.erase(key);
    provenPoses.erase(key);
}

uint32_t PoseInterval(net::NpcRecord const& record)
{
    const auto found = receivedPoses.find(ToGuidKey(record.guid));
    return found != receivedPoses.end() && found->second.incarnation == record.incarnation
        ? found->second.intervalMs : 0;
}

bool IsPassive(eCEntity const* entity) { return Passive(entity); }
bool GuardForLease(eCEntity* entity) { return enabled && entity && detail::RegisterEntity(entity); }

void Silence(eCEntity* entity)
{
    if (!Passive(entity)) return;
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    if (!routine || (!routine->AccessCurrentTask().GetLength() && !routine->AccessCurrentState().GetLength())) return;
    routine->AIFullStop();
    routine->AccessCurrentTask() = bCString();
    routine->AccessCurrentState() = bCString();
}

void Stats(LogFn log)
{
    auto const& counters = detail::Counters();
    char line[768];
    Format(line, "PASSIVE enabled=%d client=%d routines_blocked=%u scripts_blocked=%u movement_blocked=%u "
                 "tasks_blocked=%u animations_blocked=%u entities_blocked=%u properties_blocked=%u "
                 "local_hits=%u echo_skipped=%u presented_frames=%u events_blocked=%u ticks_blocked=%u phase_error_ms=%.3f "
                 "posed_frames=%u pose_waits=%u stale_poses=%u",
           enabled, clientMode, counters.blockedRoutines.load(), counters.blockedScripts.load(),
           counters.blockedMovement.load(), counters.blockedTasks.load(), counters.blockedAnimations.load(),
           counters.blockedEntities.load(), counters.blockedProperties.load(),
           predictedHits.load(), skippedEchoes.load(), presentedFrames.load(),
           counters.blockedEvents.load(), counters.blockedNpcTicks.load(),
           maximumReplayPhaseError * 1000.0f, posedFrames.load(), poseWaits.load(), stalePoses.load());
    log(line);
}

namespace {
bool PreparePose(eCEntity* entity, eCVisualAnimation_PS& animation, State& state,
                 net::NpcRecord const& record, FindName names, NativePose& result)
{
    const auto received = receivedPoses.find(ToGuidKey(record.guid));
    std::vector<pose::Transform> transforms;
    uint8_t flags = 0;
    constexpr uint32_t BoundarySlackMs = npcsend::QuietCadenceMs + 50;
    if (received == receivedPoses.end() ||
        !received->second.history.SampleClamped(record.incarnation, record.sent, BoundarySlackMs, BoundarySlackMs,
                                                transforms, &flags)) {
        ++poseWaits;
        return false;
    }
    return PrepareTransforms(entity, animation, state, transforms, flags, names, result);
}

bool ReactionMotion(char const* name)
{
    return npcplayback_hit_policy::HitImpactMotion(name);
}

GuidKey EntityKey(eCEntity const* entity)
{
    const eCEntityProxy proxy(const_cast<eCEntity*>(entity));
    return ToGuidKey(reinterpret_cast<uint8_t const*>(&proxy.GetEntityID().GetCore()));
}

bool RememberPose(eCEntity* entity, npclease::PoseProvenance provenance)
{
    auto* animation = Animation(entity);
    if (!animation) return false;
    const auto rig = Rig(*animation);
    std::vector<pose::Bone> bones;
    uint8_t flags = 0;
    if (!CapturePose(entity, bones, flags, nullptr) || !(rig == Rig(*animation))) return false;
    auto& saved = provenPoses[EntityKey(entity)];
    saved.entity = entity;
    saved.proof.Record(rig, provenance, true, true);
    saved.bones = std::move(bones);
    saved.flags = flags;
    saved.seen = net::Now();
    saved.idleStarted = 0;
    saved.idleMotion.clear();
    return true;
}

bool IdleResourceName(char const* name)
{
    if (!name || !*name) return false;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return (lower.find("ambient") != std::string::npos || lower.find("idle") != std::string::npos) &&
           lower.find("combat") == std::string::npos && lower.find("hit") == std::string::npos &&
           lower.find("death") == std::string::npos && lower.find("dead") == std::string::npos &&
           lower.find("stumble") == std::string::npos;
}

bool ReactionResourceName(char const* name)
{
    if (!name || !*name) return false;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return (lower.find("stumble") != std::string::npos || lower.find("hit") != std::string::npos) &&
           lower.find("death") == std::string::npos && lower.find("dead") == std::string::npos;
}

bool EvaluateIdle(eCEntity* entity, eCVisualAnimation_PS& animation, uint32_t elapsed, bool loop = true)
{
    auto* actor = animation.GetActor();
    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    if (!actor || !actor->HasMotionInstance(slot)) return false;
    const float duration = static_cast<float>(actor->GetMaxTime(slot));
    if (!(duration > 0.0f) || !std::isfinite(duration)) return false;
    actor->SetIntroExtroBlending(slot, 0.0f, 0.0f);
    actor->SetTargetWeight(slot, 1.0f);
    actor->SetPlaySpeed(slot, 0.0f);
    actor->PauseMotion(GEFalse, slot);
    actor->SetPlayTime(slot, loop ? npclease::IdleTime(elapsed, duration) : npclease::ReactionTime(elapsed, duration));
    NativePose evaluation;
    evaluation.evaluateOnly = true;
    const auto rig = Rig(animation);
    {
        PresentationScope presentation(false);
        VisualScope visual(entity);
        NativePoseScope scope(&evaluation);
        ActorProcess(actor, nullptr, entity->GetWorldMatrix(), GETrue, GEFalse, 0.0f);
    }
    if (!evaluation.evaluated || !(rig == Rig(animation))) return false;
    actor->ProcessGeometry();
    return true;
}

bool IdleFailure(eCEntity* entity, char const* reason)
{
    unavailable[EntityKey(entity)].reason = reason;
    return false;
}

eCResourceDataEntity* NativeMotion(eCEntity* entity, gEAction action, gEPhase phase, bool reaction = false)
{
    auto* admin = dynamic_cast<eCAnimationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCAnimationAdmin")));
    if (!admin) { IdleFailure(entity, "native animation administration unavailable"); return nullptr; }
    Entity npc(entity);
    bCString extension;
    eCResourceAnimationMotion_PS::GetNativeFileExt(extension);
    // GetAni fixes native direction/primary pose; GetAniName would insert the supplied direction verbatim.
    auto tryCurrentStance = [&]() -> eCResourceDataEntity* {
        for (auto const& stem : {npc.GetAni(action, phase),
                                npc.GetAni(action, gEUseType_None, gEUseType_None, phase)}) {
            if (!stem.GetLength()) continue;
            bCString varied(stem);
            admin->GetAniVariation(varied);
            for (auto const& candidate : {varied, stem}) {
                bCString name(candidate);
                name += extension;
                const bool accepted = reaction ? ReactionResourceName(name.GetText()) : IdleResourceName(name.GetText());
                if (!accepted) continue;
                unavailable[EntityKey(entity)].candidate = name.GetText();
                if (auto* resource = admin->QueryMotionDataEntity(name, eEResourceCacheBehavior_Immediate))
                    return resource;
            }
        }
        return nullptr;
    };
    if (auto* resource = tryCurrentStance()) return resource;
    // NPCs lingering in gEAniState_Parade (e.g. Hamlar with None_Staff) can resolve to an invalid .fxm parade descriptor;
    // fall back to gEAniState_Stand for the probe so idle/reaction bootstrap stays valid and keeps contacts active.
    if (npc.Routine.AniState == gEAniState_Parade) {
        const gEAniState savedStance = npc.Routine.AniState;
        npc.Routine.AniState = gEAniState_Stand;
        auto* fallback = tryCurrentStance();
        npc.Routine.AniState = savedStance;
        if (fallback) return fallback;
    }
    return nullptr;
}

bool BootstrapIdle(eCEntity* entity, eCVisualAnimation_PS& animation)
{
    auto* actor = animation.GetActor();
    if (!actor || !actor->GetInstance()) return IdleFailure(entity, "native animation actor unavailable");
    auto* resource = NativeMotion(entity, gEAction_Ambient, gEPhase_Loop);
    if (!resource) return IdleFailure(entity, "native ambient/loop resource unavailable");
    const bool idle = IdleResourceName(resource->GetFileName().GetText());
    if (!idle || !resource->SearchResourcePropertySet()) {
        resource->ReleaseReference();
        return IdleFailure(entity, "idle resource is not an evaluable ambient motion");
    }
    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    {
        PresentationScope scope;
        for (int i = 0; i < Slots; ++i) {
            const auto old = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
            if (actor->HasMotionInstance(old)) animation.StopMotion(old, 0.0f);
        }
        animation.SetMotion(slot, resource);
        resource->ReleaseReference();
        animation.PlayMotion(slot, nullptr);
    }
    if (!actor->HasMotionInstance(slot)) return IdleFailure(entity, "idle motion did not start");
    const float duration = static_cast<float>(actor->GetMaxTime(slot));
    if (!(duration > 0.0f) || !std::isfinite(duration)) return IdleFailure(entity, "idle motion duration invalid");
    if (!EvaluateIdle(entity, animation, 0) ||
        !actor->HasMotionInstance(slot) || actor->GetTargetWeight(slot) <= 0.0001f ||
        !IdleResourceName(animation.GetMotionDesc(slot).GetMotionFilename().GetText()))
        return IdleFailure(entity, "idle skeletal evaluation failed");
    if (!RememberPose(entity, npclease::PoseProvenance::Idle))
        return IdleFailure(entity, "evaluated idle skeleton cannot be captured");
    auto& saved = provenPoses[EntityKey(entity)];
    saved.idleStarted = net::Now();
    saved.idleMotion = animation.GetMotionDesc(slot).GetMotionFilename().GetText();
    return true;
}
}

bool RememberNativePresentation(eCEntity* entity)
{
    if (!IsCharacter(entity) || npcremoval::Suppressed(entity)) return false;
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto const originalActorProcess = detail::OriginalActorProcess();
    if (!actor || !actor->GetInstance() || !originalActorProcess) return false;
    bool playing = false;
    for (int i = 0; i < Slots; ++i) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        if (actor->HasMotionInstance(slot) && actor->GetTargetWeight(slot) > 0.0001f &&
            static_cast<float>(actor->GetPlayTime(slot)) > 0.0f) playing = true;
    }
    if (!playing) return false;
    const auto rig = Rig(*animation);
    originalActorProcess(actor, nullptr, entity->GetWorldMatrix(), GETrue, GEFalse, 0.0f);
    if (!(rig == Rig(*animation))) return false;
    return RememberPose(entity, npclease::PoseProvenance::Native);
}

bool CosmeticHit(eCEntity* entity, uint32_t swing)
{
    if (!entity || !swing || !clientMode || !Passive(entity) || npclease::Fenced(entity) ||
        npcremoval::Suppressed(entity) || npcs::RedundantClientEntity(entity) ||
        npcs::HostPresentationAvailable(entity)) return false;
    Entity npc(entity);
    if (npc.IsDead() || npc.IsDown() || talk::IsHeld(entity) || talk::TalkingWithHero(entity)) return false;
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance()) return IdleFailure(entity, "cosmetic hit actor unavailable");
    const GuidKey key = EntityKey(entity);
    auto& waiting = unavailable[key];
    if (waiting.entity == entity && waiting.hitSwing == swing) return true;
    waiting.entity = entity;
    waiting.seen = net::Now();
    waiting.hitSwing = swing;
    auto* resource = NativeMotion(entity, gEAction_QuickStumble, gEPhase_Hit, true);
    if (!resource) resource = NativeMotion(entity, gEAction_StumbleR, gEPhase_Hit, true);
    if (!resource) {
        char id[40];
        guid::Format(key.data(), id, sizeof(id));
        Logf("npcplayback: cosmetic-hit unavailable guid=%s name=%s candidate=%s damage=none",
             id, npc.GetName().GetText(), waiting.candidate.c_str());
        return false;
    }
    if (!ReactionResourceName(resource->GetFileName().GetText()) || !resource->SearchResourcePropertySet()) {
        resource->ReleaseReference();
        return IdleFailure(entity, "cosmetic hit resource is not an evaluable reaction motion");
    }
    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    {
        PresentationScope scope;
        for (int i = 0; i < Slots; ++i) {
            const auto old = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
            if (actor->HasMotionInstance(old)) animation->StopMotion(old, 0.0f);
        }
        animation->SetMotion(slot, resource);
        resource->ReleaseReference();
        animation->PlayMotion(slot, nullptr);
    }
    if (!EvaluateIdle(entity, *animation, 1, false) ||
        !RememberPose(entity, npclease::PoseProvenance::Reaction))
        return IdleFailure(entity, "cosmetic hit skeletal evaluation failed");
    auto& saved = provenPoses[key];
    saved.idleStarted = net::Now();
    saved.idleMotion = animation->GetMotionDesc(slot).GetMotionFilename().GetText();
    saved.reactionDuration = static_cast<float>(actor->GetMaxTime(slot));
    waiting.retry = false;
    waiting.evaluatedTick = 0;
    waiting.state.holding = false;
    ++predictedHits;
    return true;
}

bool LocalHit(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options, uint32_t hit, uint32_t now,
              float playSpeed, eCEntity const* attacker, uint8_t reaction)
{
    if (!detail::StartLocalHitPresentation(entity, state, motion, options, hit, now, playSpeed, attacker, reaction))
        return false;
    ++predictedHits;
    return true;
}

bool Replay(eCEntity* entity, State& state, net::NpcRecord const& record, FindName names, uint32_t now)
{
    if (!entity || !names) return false;
    Silence(entity);
    PresentationScope scope;
    auto* animation = Animation(entity);
    if (animation) animation->SetForceAlwaysVisible(GETrue);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    if (state.incarnation != record.incarnation) {
        provenPoses.erase(EntityKey(entity));
        state = State{};
        state.incarnation = record.incarnation;
    }
    if (record.flags & 3) ResetLocalHitState(state);
    const bool acknowledged = state.pendingHit && record.hitPlayer == net::LocalId() &&
                              static_cast<int16_t>(record.hitSequence - state.pendingHit) >= 0;
    if (acknowledged) {
        if (!state.acknowledgedAt || state.acknowledgedHit != record.hitSequence)
            state.acknowledgedAt = record.sent ? record.sent : 1;
        state.acknowledgedHit = record.hitSequence;
        if (record.motions[0].instance != state.localOptions.instance) state.pendingHit = 0;
    }
    if (state.pendingHit && state.acknowledgedAt &&
        static_cast<int32_t>(record.sent - state.acknowledgedAt) >= 0 &&
        record.motions[0].instance != state.localOptions.instance) state.pendingHit = 0;
    if (npcplayback_hit_policy::PendingHitExpired(state.pendingHit, state.localStart, now))
        state.pendingHit = 0;
    const float localAge = static_cast<float>(now - state.localStart) / 1000.0f;
    char const* firstName = record.motions[0].active ? names(record.motions[0].name) : nullptr;
    const bool ownSequenceSeen = record.hitPlayer == net::LocalId() &&
        npcplayback_hit_policy::OwnImpactSequenceSeen(state.acknowledgedHit, record.hitSequence);
    const bool slot0OwnImpactEcho = record.motions[0].active && ownSequenceSeen &&
        npcplayback_hit_policy::HitImpactMotion(firstName);
    const bool slot0OwnRecoverEcho = record.motions[0].active && ownSequenceSeen &&
        npcplayback_hit_policy::HitRecoverMotion(firstName);
    const bool slot0OwnStumbleEcho = slot0OwnImpactEcho || slot0OwnRecoverEcho;
    const bool hostInterruptingNonStumble = ownSequenceSeen && state.knockback.hostImpactDone &&
        firstName && !npcplayback_hit_policy::HitStumbleSequenceMotion(firstName);
    const auto localStep = npcplayback_hit_policy::EvaluateLocalStumbleTimeline(
        *state.localMotion != '\0', *state.localRecoverMotion != '\0', localAge,
        state.localDuration, state.localRecoverDuration, hostInterruptingNonStumble);
    const bool local = npcplayback_hit_policy::LocalStaggerActive(
        *state.localMotion != '\0', localAge, localStep.totalDurationSec) &&
        localStep.stage != npcplayback_hit_policy::LocalStumbleStage::None;
    const bool firstSuppressed = record.motions[0].active &&
        (state.suppressed[0] == record.motions[0].instance || slot0OwnStumbleEcho);
    NativePose hostPose;
    const bool havePose = PreparePose(entity, *animation, state, record, names, hostPose);
    const float hostFirstPhase = record.motions[0].active ? record.motions[0].phase : 0.0f;
    const uint32_t knockbackAckAt = state.acknowledgedHit == state.knockback.hitSequence ? state.acknowledgedAt : 0;
    const bool matchKnockback =
        npcplayback_hit_policy::KnockbackMatchesHostRecord(state.knockback, state.acknowledgedHit, record.hitSequence);
    const float hostClipStepCm = (slot0OwnImpactEcho && matchKnockback)
        ? npcplayback_hit_policy::EffectiveNativeKnockbackCm(
              npcplayback_hit_policy::ParseMotionDisplacementCm(firstName), state.knockback.hitDurationSec)
        : -1.0f;
    float presentedPosition[3] = {record.position[0], record.position[1], record.position[2]};
    npcplayback_hit_policy::AdvanceHitKnockback(
        state.knockback, now, record.sent, knockbackAckAt,
        slot0OwnImpactEcho && matchKnockback, slot0OwnRecoverEcho && matchKnockback, hostFirstPhase,
        (record.flags & 3) != 0, record.position, presentedPosition, hostClipStepCm);
    const bool holdEcho = npcplayback_hit_policy::HoldSuppressedImpact(
        local, *state.localMotion != '\0', firstSuppressed, localAge, localStep.totalDurationSec,
        havePose, hostFirstPhase, slot0OwnRecoverEcho);
    if (local && localStep.stage == npcplayback_hit_policy::LocalStumbleStage::Recover && !state.localInRecover) {
        if (state.initialized)
            detail::CaptureEntityPoseBlend(entity, state, now, npcplayback_hit_policy::HitRecoverBlendSeconds);
        state.localInRecover = true;
        Logf("combat: NPCHITSYNC stage=recover-start npc=%s hit=%u motion=%s recover_ms=%.0f",
             entity->GetName().GetText(), state.acknowledgedHit ? state.acknowledgedHit : state.pendingHit,
             state.localRecoverMotion, state.localRecoverDuration * 1000.0f);
    }
    char const* activeLocalClip =
        (localStep.stage == npcplayback_hit_policy::LocalStumbleStage::Recover ||
         (!local && *state.localRecoverMotion != '\0'))
            ? state.localRecoverMotion
            : state.localMotion;
    if (!local && !holdEcho && *state.localMotion) {
        if (state.initialized)
            detail::CaptureEntityPoseBlend(entity, state, now, npcplayback_hit_policy::HitExitBlendSeconds);
        if (firstSuppressed && state.suppressed[0] == record.motions[0].instance)
            state.suppressed[0] = 0;
        char const* mode = firstSuppressed ? "host-pose-tail"
            : (firstName && std::strstr(firstName, "_Recover_") ? "host-recover" : "host-motion");
        const float leadCm = std::hypot(state.knockback.currentOffset[0], state.knockback.currentOffset[2]);
        Logf("combat: NPCHITSYNC stage=handoff npc=%s hit=%u mode=%s local_age_ms=%.0f local_duration_ms=%.0f "
             "host_motion=%s host_phase=%.3f lead_cm=%.1f",
             entity->GetName().GetText(), state.acknowledgedHit ? state.acknowledgedHit : state.pendingHit, mode,
             localAge * 1000.0f, localStep.totalDurationSec * 1000.0f, firstName ? firstName : "(none)",
             hostFirstPhase, leadCm);
        *state.localMotion = '\0';
        *state.localRecoverMotion = '\0';
        state.localInRecover = false;
    }
    bool echo = false;
    if (!local && !holdEcho && !havePose) return false;
    const bCMatrix pose = body::Pose(*entity, presentedPosition, record.rotation);
    entity->SetToWorldMatrix(pose);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(pose);
    actor = animation->GetActor();
    if (!actor) return false;
    animation->ClearLookAtConstraints();
    if (animation->IsAutoAimingEnabled()) {
        bCString bone;
        animation->EnableAutoAiming(GEFalse, nullptr, bone);
    }
    for (int i = 0; i < Slots; ++i) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        auto const& motion = record.motions[i];
        char const* name = motion.active ? names(motion.name) : nullptr;
        const bool ownImpactEcho = motion.active && ownSequenceSeen &&
            npcplayback_hit_policy::HitStumbleSequenceMotion(name);
        if (ownImpactEcho) state.effects[i] = motion.instance;
        if ((local || holdEcho) && ownImpactEcho && state.suppressed[i] != motion.instance) {
            state.suppressed[i] = motion.instance;
            ++skippedEchoes;
        }
        if (local) {
            if (i == 0) {
                net::NpcMotion overlay = state.localOptions;
                overlay.phase = localStep.phase;
                overlay.weight = 1.0f;
                const bool changedLocal =
                    std::strcmp(activeLocalClip, animation->GetMotionDesc(slot).GetMotionFilename().GetText()) != 0;
                if (!PresentMotion(*animation, *actor, i, activeLocalClip, overlay, changedLocal)) return false;
            } else if (actor->HasMotionInstance(slot)) {
                animation->StopMotion(slot, 0.0f);
            }
            state.instances[i] = 0;
            continue;
        }
        if (holdEcho && motion.active && state.suppressed[i] == motion.instance && i == 0 && *activeLocalClip) {
            net::NpcMotion overlay = state.localOptions;
            overlay.phase = 1.0f;
            overlay.weight = 1.0f;
            const bool changedLocal =
                std::strcmp(activeLocalClip, animation->GetMotionDesc(slot).GetMotionFilename().GetText()) != 0;
            if (!PresentMotion(*animation, *actor, i, activeLocalClip, overlay, changedLocal)) return false;
            echo = true;
            continue;
        }
        if (!motion.active || state.suppressed[i] == motion.instance) {
            if (actor->HasMotionInstance(slot)) animation->StopMotion(slot, 0.0f);
            state.instances[i] = 0;
            continue;
        }
        if (!name) {
            if (!echo && havePose) { state.instances[i] = 0; continue; }
            Logf("npcplayback: waiting for motion dictionary entry %08x", motion.name);
            return false;
        }
        const bool changedMotion = std::strcmp(name, animation->GetMotionDesc(slot).GetMotionFilename().GetText()) != 0;
        if (!PresentMotion(*animation, *actor, i, name, motion,
                           !state.initialized || state.instances[i] != motion.instance || changedMotion)) {
            if (echo || !havePose) return false;
            state.instances[i] = 0;
            continue;
        }
        if (ReactionMotion(name) && state.effects[i] != motion.instance) {
            state.effects[i] = motion.instance;
            const bCString effect(std::strstr(name, "Parade") ? "eff_col_wh_01_me_me" : "eff_col_wh_02_me_fl");
            if (EffectSystem::ExistsEffect(effect)) EffectSystem::StartEffect(effect, Entity(entity));
        }
        state.instances[i] = motion.instance;
    }
    if (!local && !echo) {
        *state.localMotion = '\0';
        *state.localRecoverMotion = '\0';
        state.localInRecover = false;
    }
    state.initialized = true;
    ++presentedFrames;
    if (!local && !echo && !havePose) return false;
    if (!local && !echo) {
        const bool crossfadingExit = detail::ApplyPoseBlend(state, hostPose, now);
        NativePoseScope poseScope(&hostPose);
        UpdatePresentation(entity);
        if (!hostPose.evaluated || !hostPose.applied || !hostPose.valid) {
            Log("npcplayback: canonical skeletal evaluation failed; contacts disabled");
            return false;
        }
        if (!crossfadingExit && (!FullPose(hostPose) || !RememberPose(entity, npclease::PoseProvenance::Canonical))) {
            provenPoses.erase(EntityKey(entity));
        }
        state.holding = false;
        unavailable.erase(EntityKey(entity));
        ++posedFrames;
        const auto& first = record.motions[0];
        if (first.active && state.timedInstance != first.instance &&
            combat_timing::IncomingCombat(record.flags, record.combatTarget, net::LocalId())) {
            state.timedInstance = first.instance;
            const float duration = static_cast<float>(actor->GetMaxTime(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0)));
            const float seekMs = duration * first.phase * 1000.0f;
            const float ageMs = first.speed > 0.0f ? seekMs / first.speed : 0.0f;
            const uint32_t started = record.sent - static_cast<uint32_t>(ageMs);
            Logf("combat: NPCATTACK stage=display npc=%s player=%u instance=%u host_start_est=%u host_sample=%u "
                 "client_displayed=%u phase=%.4f seek_ms=%.1f duration_ms=%.1f native_speed=%.3f motion=%s",
                 entity->GetName().GetText(), net::LocalId(), first.instance, started, record.sent, now,
                 first.phase, seekMs, duration * 1000.0f, first.speed, names(first.name));
        }
        // The authoritative bones are the visible pose. Secondary native clip bookkeeping cannot invalidate them.
        return true;
    }
    NativePose localPose;
    if (!detail::EvaluateLocalActorPose(entity, localPose)) return false;
    detail::ApplyPoseBlend(state, localPose, now);
    { NativePoseScope poseScope(&localPose); UpdatePresentation(entity); }
    if (!localPose.evaluated || !localPose.applied || !localPose.valid) return false;
    if (!detail::VerifyLocalHitPresentation(entity, state, record, names, activeLocalClip, local, echo,
                                            localStep.phase, maximumReplayPhaseError))
        return false;
    state.holding = false;
    unavailable.erase(EntityKey(entity));
    return true;
}

bool ReplayFrozenPose(eCEntity* entity, State& state, std::vector<pose::Bone> const& bones, uint8_t flags)
{
    auto* animation = Animation(entity);
    if (!animation || bones.empty()) return false;
    PresentationScope scope;
    std::vector<pose::Transform> transforms;
    transforms.reserve(bones.size());
    for (auto const& bone : bones) transforms.push_back(pose::Decode(bone));
    NativePose frozen;
    if (!PrepareTransforms(entity, *animation, state, transforms, flags, nullptr, frozen)) return false;
    NativePoseScope poseScope(&frozen);
    UpdatePresentation(entity);
    return frozen.evaluated && frozen.applied && frozen.valid;
}

bool HoldUnavailable(eCEntity* entity, State& callerState)
{
    if (!entity || !Passive(entity) || npcremoval::Suppressed(entity)) return false;
    if (npcs::HostPresentationAvailable(entity)) {
        callerState.holding = false;
        ReleaseUnavailable(entity);
        return false;
    }
    Entity npc(entity);
    if (npc.IsDead() || npc.IsDown()) return false;
    auto* animation = Animation(entity);
    if (!animation || !animation->GetActor()) return false;
    const GuidKey key = EntityKey(entity);
    auto& waiting = unavailable[key];
    if (waiting.entity != entity) {
        waiting = Unavailable{};
        waiting.entity = entity;
    }
    waiting.seen = net::Now();
    State& state = waiting.state;
    auto found = provenPoses.find(key);
    if (found != provenPoses.end() &&
        (found->second.entity != entity || !found->second.proof.Usable(Rig(*animation)))) {
        provenPoses.erase(found);
        found = provenPoses.end();
        state.holding = false;
    }
    if (found == provenPoses.end()) state.holding = false;
    if (found != provenPoses.end() && found->second.proof.provenance == npclease::PoseProvenance::Reaction &&
        (net::Now() - found->second.idleStarted) * 0.001f >= found->second.reactionDuration) {
        provenPoses.erase(found);
        found = provenPoses.end();
        state.holding = false;
    }
    if (!state.holding) {
        const auto& p = entity->GetWorldPosition();
        const auto q = entity->GetWorldRotation();
        state.heldPosition[0] = p.GetX(); state.heldPosition[1] = p.GetY(); state.heldPosition[2] = p.GetZ();
        state.heldRotation[0] = q.GetX(); state.heldRotation[1] = q.GetY();
        state.heldRotation[2] = q.GetZ(); state.heldRotation[3] = q.GetW();
    }
    if (animation->GetRagDollEnabled()) animation->StopRagDoll();
    npc.CharacterMovement.StopMovement();
    const auto root = body::Pose(*entity, state.heldPosition, state.heldRotation);
    entity->SetToWorldMatrix(root);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(root);
    if (playbackTick && waiting.evaluatedTick == playbackTick && state.holding) {
        callerState.holding = state.holding;
        return waiting.evaluatedOk;
    }
    auto finish = [&](bool ok) {
        waiting.evaluatedTick = playbackTick;
        waiting.evaluatedOk = ok;
        callerState.holding = state.holding;
        return ok;
    };
    if (found == provenPoses.end()) {
        const uint32_t now = net::Now();
        if (waiting.retry && now - waiting.attempted < 1000) return finish(false);
        waiting.attempted = now;
        const bool bootstrapped = BootstrapIdle(entity, *animation);
        waiting.retry = !bootstrapped;
        entity->SetToWorldMatrix(root);
        if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
            rigid->SetPose(root);
        if (!bootstrapped) return finish(false);
        found = provenPoses.find(key);
    }
    if (npcs::HostPresentationAvailable(entity) || found == provenPoses.end() ||
        !found->second.proof.Usable(Rig(*animation))) return finish(false);
    if (found->second.proof.provenance == npclease::PoseProvenance::Idle ||
        found->second.proof.provenance == npclease::PoseProvenance::Reaction) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
        const bool loop = found->second.proof.provenance == npclease::PoseProvenance::Idle;
        if (found->second.idleMotion != animation->GetMotionDesc(slot).GetMotionFilename().GetText() ||
            !EvaluateIdle(entity, *animation, net::Now() - found->second.idleStarted, loop)) {
            provenPoses.erase(found);
            state.holding = false;
            return finish(false);
        }
        const uint32_t began = found->second.idleStarted;
        const std::string motion = found->second.idleMotion;
        const auto provenance = found->second.proof.provenance;
        const float duration = found->second.reactionDuration;
        if (!RememberPose(entity, provenance)) return finish(false);
        auto& saved = provenPoses[key];
        saved.idleStarted = began;
        saved.idleMotion = motion;
        saved.reactionDuration = duration;
        state.holding = true;
        return finish(true);
    }
    if (!state.holding) {
        state.heldPose = found->second.bones;
        state.heldFlags = found->second.flags;
        state.holding = true;
    }
    found->second.seen = net::Now();
    entity->SetToWorldMatrix(root);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(root);
    return finish(ReplayFrozenPose(entity, state, state.heldPose, state.heldFlags));
}

void ReleaseUnavailable(eCEntity* entity)
{
    if (!entity) return;
    const GuidKey key = EntityKey(entity);
    const auto found = unavailable.find(key);
    if (found == unavailable.end()) return;
    // A lease restart must not reuse an old idle slot or root.
    provenPoses.erase(key);
    unavailable.erase(found);
}

bool ReleaseNativePresentation(eCEntity* entity)
{
    if (!entity || Passive(entity)) return false;
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    ReleaseUnavailable(entity);
    provenPoses.erase(EntityKey(entity));
    if (!animation->GetRagDollEnabled()) {
        for (int i = 0; i < Slots; ++i) {
            const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
            if (actor->HasMotionInstance(slot)) actor->StopMotion(slot, 0.0f);
        }
    }
    return true;
}

void UpdatePresentation(eCEntity* entity)
{
    auto* animation = Animation(entity);
    if (!animation) return;
    PresentationScope scope(false);
    VisualScope visual(entity);
    animation->SetForceAlwaysVisible(GETrue);
    animation->OnPreProcess();
    animation->OnProcess();
    animation->OnPostProcess();
    NativePose* presentingPose = detail::CurrentPresentingPose();
    if (presentingPose && !presentingPose->evaluateOnly) {
        auto* actor = animation->GetActor();
        if (actor && ApplyPose(*actor, *presentingPose)) actor->ProcessGeometry();
    }
}

void PresentationCensus(uint32_t now)
{
    static uint32_t last = 0;
    if (last && now - last < 5000) return;
    last = now ? now : 1;
    unsigned host = 0, held = 0, idle = 0, native = 0, visual = 0, unproven = 0, hidden = 0, removed = 0, objects = 0;
    std::unordered_map<std::string, unsigned> reasons;
    std::unordered_map<std::string, unsigned> refusals;
    GuidKey sample{};
    bool haveSample = false;
    char const* sampleName = "-";
    auto const* all = world::ProcessedEntities();
    for (GEU16 i = 0; all && i < all->GetCount(); ++i) {
        auto* entity = all->GetAt(i);
        if (!entity || !entity->GetPropertySet(eEPropertySetType_NPC)) continue;
        if (!IsCharacter(entity)) { ++objects; continue; }
        if (!entity->IsRenderingEnabled()) ++hidden;
        if (npcremoval::Suppressed(entity)) { ++removed; continue; }
        if (npclease::LocalAuthority(entity)) { ++native; continue; }
        if (!Passive(entity)) continue;
        if (npcs::HostPresentationAvailable(entity)) { ++host; continue; }
        ++refusals[npclease::UnavailabilityReason(entity)];
        const auto key = EntityKey(entity);
        const auto proof = provenPoses.find(key);
        auto* animation = Animation(entity);
        if (proof != provenPoses.end() && proof->second.entity == entity && animation &&
            proof->second.proof.Usable(Rig(*animation))) {
            if (proof->second.proof.provenance == npclease::PoseProvenance::Idle) { ++idle; ++visual; }
            else if (proof->second.proof.provenance == npclease::PoseProvenance::Reaction) ++visual;
            else ++held;
        } else {
            ++unproven;
            const auto waiting = unavailable.find(key);
            ++reasons[waiting == unavailable.end() ? "awaiting presentation" : waiting->second.reason];
            if (!haveSample) { sample = key; haveSample = true; sampleName = entity->GetName().GetText(); }
        }
    }
    char const* top = "-";
    unsigned topCount = 0;
    for (auto const& reason : reasons)
        if (reason.second > topCount) { top = reason.first.c_str(); topCount = reason.second; }
    char const* topRefusal = "-";
    unsigned refusalCount = 0;
    for (auto const& reason : refusals)
        if (reason.second > refusalCount) { topRefusal = reason.first.c_str(); refusalCount = reason.second; }
    char id[40] = "-";
    if (haveSample) guid::Format(sample.data(), id, sizeof(id));
    Logf("npcplayback: census host=%u held=%u idle=%u native=%u visual=%u unproven=%u hidden=%u removed=%u static=%u "
         "top_reason=%s count=%u sample=%s guid=%s top_refusal=%s refusal_count=%u",
         host, held, idle, native, visual, unproven, hidden, removed, objects, top, topCount, sampleName, id,
         topRefusal, refusalCount);
}

void Tick()
{
    if (++playbackTick == 0) ++playbackTick;
    const uint32_t cleanupNow = net::Now();
    if (!poseCleanupAt || cleanupNow - poseCleanupAt >= 1000) {
        poseCleanupAt = cleanupNow;
        for (auto it = receivedPoses.begin(); it != receivedPoses.end();)
            if (cleanupNow - it->second.arrived > npcsend::PoseRetentionMs) it = receivedPoses.erase(it);
            else ++it;
        detail::PruneSkeletonCache(cleanupNow);
        for (auto it = provenPoses.begin(); it != provenPoses.end();)
            if (cleanupNow - it->second.seen > 5000) it = provenPoses.erase(it);
            else ++it;
        for (auto it = unavailable.begin(); it != unavailable.end();)
            if (cleanupNow - it->second.seen > 5000) it = unavailable.erase(it);
            else ++it;
    }
    if (enabled && clientMode && gCSession::GetInstance().IsGameRunning()) {
        const auto* all = world::ProcessedEntities();
        for (GEU16 i = 0; all && i < all->GetCount(); ++i) {
            auto* entity = (*all)[i];
            if (!entity || !entity->GetPropertySet(eEPropertySetType_NPC)) continue;
            if (npcs::RedundantClientEntity(entity)) continue;
            if (!IsCharacter(entity)) continue;
            Entity npc(entity);
            if (!Passive(npc.GetInstance())) continue;
            if (!detail::RegisterEntity(npc.GetInstance())) {
                Log("FAIL npcplayback: an NPC property class could not be guarded; NPC processing disabled");
                npc.EnableProcessing(GEFalse);
            }
            Silence(npc.GetInstance());
            const GuidKey key = EntityKey(npc.GetInstance());
            if (!npcs::HostPresentationAvailable(npc.GetInstance()) && !npc.IsDead() && !npc.IsDown() &&
                !npcremoval::Suppressed(npc.GetInstance())) {
                auto& held = unavailable[key];
                if (held.entity != npc.GetInstance()) held = Unavailable{};
                held.entity = npc.GetInstance();
                held.seen = cleanupNow;
                const char* leaseReason = npclease::UnavailabilityReason(npc.GetInstance());
                if (!held.noted || held.leaseReason != leaseReason) {
                    char id[40];
                    guid::Format(key.data(), id, sizeof(id));
                    Logf("npclease: degraded guid=%s name=%s reason=%s contacts=off",
                         id, npc.GetName().GetText(), leaseReason);
                    held.leaseReason = leaseReason;
                    held.noted = true;
                }
                if (!HoldUnavailable(npc.GetInstance(), held.state)) {
                    if (!held.failed) {
                        char id[40];
                        guid::Format(key.data(), id, sizeof(id));
                        Logf("npcplayback: fallback guid=%s name=%s source=native-unproven host_age_ms=%u "
                             "rendering=%u reason=%s candidate=%s; retaining visibility contacts=off",
                             id, npc.GetName().GetText(), npcs::HostPresentationAge(entity),
                             entity->IsRenderingEnabled() ? 1 : 0, held.reason, held.candidate.c_str());
                        held.failed = true;
                    }
                } else {
                    held.failed = false;
                }
            } else ReleaseUnavailable(npc.GetInstance());
        }
        PresentationCensus(cleanupNow);
    }
}

namespace detail {
bool TaskBlocked(gCScriptRoutine_PS* self, bCString const& task)
{
    eCEntity* entity = self->GetEntity();
    if (downed::RejectReaction(entity, task.GetText(), "routine-task")) return true;
    if (!Passive(entity) || task.GetLength() == 0) return false;
    RecordBlockedTaskVerdict(entity, task.GetText());
    return true;
}
} // namespace detail

}

namespace g3t::npclease {
void ForgetReceivedNpcPose(uint8_t const guid[16])
{
    if (!guid) return;
    npcplayback::receivedPoses.erase(ToGuidKey(guid));
}

void ForgetReceivedNpcPoseBefore(uint8_t const guid[16], uint32_t incarnation, uint32_t sent)
{
    if (!guid) return;
    const auto found = npcplayback::receivedPoses.find(ToGuidKey(guid));
    if (found == npcplayback::receivedPoses.end()) return;
    found->second.history.DiscardBefore(incarnation, sent);
    if (!found->second.history.Size()) npcplayback::receivedPoses.erase(found);
}
} // namespace g3t::npclease
