// SPDX-License-Identifier: GPL-3.0-or-later
#include "contact.h"
#include "log.h"
#include "npcplayback.h"
#include "npcplayback_hit_policy.h"
#include "npcplayback_hooks.h"
#include "npcplayback_skeleton.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace g3t::npcplayback {
static_assert(npcplayback_hit_policy::SpeciesOrc == static_cast<int>(gESpecies_Orc));
static_assert(npcplayback_hit_policy::UseNone == static_cast<int>(gEUseType_None));
static_assert(npcplayback_hit_policy::Use1H == static_cast<int>(gEUseType_1H));
static_assert(npcplayback_hit_policy::UseShield == static_cast<int>(gEUseType_Shield));
static_assert(npcplayback_hit_policy::UseTorch == static_cast<int>(gEUseType_Torch));
static_assert(npcplayback_hit_policy::PoseP0 == static_cast<int>(gEPose_P0));
static_assert(npcplayback_hit_policy::PoseP1 == static_cast<int>(gEPose_P1));
static_assert(npcplayback_hit_policy::PoseP2 == static_cast<int>(gEPose_P2));
static_assert(npcplayback_hit_policy::PoseP3 == static_cast<int>(gEPose_P3));

namespace {
bool ReactionMotionResourceName(char const* name)
{
    if (!name || !*name) return false;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return (lower.find("stumble") != std::string::npos || lower.find("hit") != std::string::npos) &&
           lower.find("death") == std::string::npos && lower.find("dead") == std::string::npos;
}

bool KnockbackNonBarrierEntity(eCEntity* hit, eCEntity* self)
{
    if (!hit) return false;
    for (eCEntity* cursor = hit; cursor; cursor = cursor->GetParent())
        if (cursor == self) return true;
    return hit->GetPropertySet(eEPropertySetType_NPC) ||
           hit->GetPropertySet(eEPropertySetType_PlayerMemory) ||
           hit->GetPropertySet(eEPropertySetType_CharacterMovement) ||
           hit->GetPropertySet(eEPropertySetType_Item) ||
           hit->GetPropertySet(eEPropertySetType_Projectile);
}

float ClampKnockbackToWorld(eCEntity* entity, bCVector const& origin, float const dirXZ[2],
                            float desiredStepCm, float& outGroundDeltaY)
{
    outGroundDeltaY = 0.0f;
    if (!entity || !(desiredStepCm > 0.5f)) return 0.0f;
    bCBox const& box = entity->GetWorldNodeBoundary();
    auto* movement = static_cast<gCCharacterMovement_PS*>(entity->GetPropertySet(eEPropertySetType_CharacterMovement));
    auto& physics = eCPhysicsScene::GetInstance();
    constexpr float CapsuleMarginCm = 45.0f;
    const GEU32 wallHints = eETraceRayHints_TestStaticShapes | eETraceRayHints_Distance |
                            eETraceRayHints_IncludeCollisionGroup17;
    bCVector waist = origin;
    waist.AccessY() = contact::ContactRayY(origin.GetY(), box.GetMin().GetY(), box.GetMax().GetY(),
                                           movement ? movement->GetCharacterHeight() : 0.0f);
    const float probeLen = desiredStepCm + CapsuleMarginCm;
    const bCVector wallRay(dirXZ[0] * probeLen, 0.0f, dirXZ[1] * probeLen);
    float stepCm = desiredStepCm;
    if (physics.TraceRayAllHits(waist, wallRay, entity, wallHints)) {
        auto const& report = physics.GetQueryReport();
        const int count = std::min(report.m_arrHitEntity.GetCount(), report.m_arrDistance.GetCount());
        for (int i = 0; i < count; ++i) {
            const float dist = report.m_arrDistance[i];
            if (!contact::RayHitStrictlyBetween(dist, probeLen + 1.0f)) continue;
            eCEntity* hit = report.m_arrHitEntity[i];
            if (KnockbackNonBarrierEntity(hit, entity)) continue;
            stepCm = std::min(stepCm, std::max(0.0f, dist - CapsuleMarginCm));
        }
    }
    if (!(stepCm > 0.5f)) return 0.0f;

    const GEU32 groundHints = eETraceRayHints_TestStaticShapes | eETraceRayHints_Distance;
    auto probeGround = [&](float x, float z, float& hitDist) -> bool {
        const bCVector start(x, origin.GetY() + 80.0f, z);
        const bCVector down(0.0f, -200.0f, 0.0f);
        if (!physics.TraceRayAllHits(start, down, entity, groundHints)) return false;
        auto const& report = physics.GetQueryReport();
        float best = 1e9f;
        for (int i = 0; i < report.m_arrDistance.GetCount(); ++i) {
            eCEntity* hit = i < report.m_arrHitEntity.GetCount() ? report.m_arrHitEntity[i] : nullptr;
            if (hit == entity) continue;
            const float dist = report.m_arrDistance[i];
            if (std::isfinite(dist) && dist >= 0.0f && dist <= 200.0f && dist < best) best = dist;
        }
        if (best > 200.0f) return false;
        hitDist = best;
        return true;
    };
    float originGroundDist = 0.0f;
    if (probeGround(origin.GetX(), origin.GetZ(), originGroundDist)) {
        float destGroundDist = 0.0f;
        if (!probeGround(origin.GetX() + dirXZ[0] * stepCm, origin.GetZ() + dirXZ[1] * stepCm, destGroundDist))
            return 0.0f;
        outGroundDeltaY = std::clamp(originGroundDist - destGroundDist, -60.0f, 60.0f);
    }
    return stepCm;
}

bool ResolveRecoverMotion(eCVisualAnimation_PS& animation, eCWrapper_emfx2Actor& actor, char const* hitMotion,
                          net::NpcMotion const& localOptions, float effectiveSpeed,
                          char outRecoverMotion[96], float& outRecoverDuration)
{
    outRecoverMotion[0] = '\0';
    outRecoverDuration = 0.0f;
    auto* admin = dynamic_cast<eCAnimationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCAnimationAdmin")));
    if (!admin || !hitMotion || !*hitMotion) return false;
    bCString extension;
    eCResourceAnimationMotion_PS::GetNativeFileExt(extension);
    for (bool useExitPose : {true, false}) {
        char stemBuf[96] = {};
        if (!npcplayback_hit_policy::BuildRecoverStemFromHitMotion(hitMotion, stemBuf, sizeof(stemBuf), useExitPose))
            continue;
        const bCString stem(stemBuf);
        bCString varied(stem);
        admin->GetAniVariation(varied);
        for (auto const& candidate : {varied, stem}) {
            bCString name(candidate);
            name += extension;
            if (!npcplayback_hit_policy::HitRecoverMotion(name.GetText())) continue;
            auto* resource = admin->QueryMotionDataEntity(name, eEResourceCacheBehavior_Immediate);
            if (!resource) continue;
            const bool valid = resource->SearchResourcePropertySet() != nullptr;
            const bCString fileName = resource->GetFileName();
            resource->ReleaseReference();
            if (!valid || !fileName.GetLength() || static_cast<std::size_t>(fileName.GetLength()) >= 96) continue;
            if (!detail::PresentMotion(animation, actor, 0, fileName.GetText(), localOptions, true)) continue;
            const float clipSec = static_cast<float>(actor.GetMaxTime(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0)));
            if (!(clipSec > 0.01f) || !std::isfinite(clipSec)) continue;
            strcpy_s(outRecoverMotion, 96, fileName.GetText());
            outRecoverDuration = clipSec / effectiveSpeed;
            return true;
        }
    }
    return false;
}

struct ScopedAniProbeState {
    Entity& npc;
    gCNavigation_PS* navPs = nullptr;
    gCNPC_PS* npcPs = nullptr;
    gEDirection savedDir = gEDirection_Fwd;
    gEAniState savedStance = gEAniState_Stand;
    gEPose savedPose = gEPose_None;

    ScopedAniProbeState(Entity& n, eCEntity* e)
        : npc(n),
          navPs(static_cast<gCNavigation_PS*>(e->GetPropertySet(eEPropertySetType_Navigation))),
          npcPs(static_cast<gCNPC_PS*>(e->GetPropertySet(eEPropertySetType_NPC))),
          savedDir(navPs ? navPs->GetCurrentAniDirection() : n.GetCurrentAniDirection()),
          savedStance(n.Routine.AniState),
          savedPose(npcPs ? npcPs->GetPrimaryPose() : gEPose_None)
    {
    }

    void SetDirection(gEDirection dir)
    {
        if (navPs) navPs->SetCurrentAniDirection(dir);
        else npc.SetCurrentAniDirection(dir);
    }

    ~ScopedAniProbeState()
    {
        SetDirection(savedDir);
        npc.Routine.AniState = savedStance;
        if (npcPs) npcPs->SetPrimaryPose(savedPose);
    }
};
} // namespace

namespace detail {
void CaptureEntityPoseBlend(eCEntity* entity, State& state, uint32_t now, float durationSec)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!skeleton || skeleton->mNodes.mLength <= 0 || skeleton->mNodes.mLength > 4096 || !(durationSec > 0.001f)) {
        state.poseBlend.active = false;
        return;
    }
    auto const& nodes = skeleton->mNodes;
    state.poseBlend.fromTransforms.resize(static_cast<std::size_t>(nodes.mLength));
    for (int i = 0; i < nodes.mLength; ++i) {
        bCMatrix matrix;
        std::memcpy(&matrix, reinterpret_cast<unsigned char const*>(nodes.mData + i) + 0x44, sizeof(matrix));
        state.poseBlend.fromTransforms[static_cast<std::size_t>(i)] = TransformOf(matrix, 0);
    }
    state.poseBlend.skeleton = skeleton;
    state.poseBlend.nodes = nodes.mData;
    state.poseBlend.startedAtMs = now;
    state.poseBlend.durationSec = durationSec;
    state.poseBlend.active = true;
}

bool EvaluateLocalActorPose(eCEntity* entity, NativePose& outPose)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto const originalActorProcess = OriginalActorProcess();
    if (!entity || !actor || !actor->GetInstance() || !originalActorProcess) return false;
    originalActorProcess(actor, nullptr, entity->GetWorldMatrix(), GETrue, GEFalse, 0.0f);
    auto* skeleton = actor->GetInstance();
    if (!skeleton || skeleton->mNodes.mLength <= 0 || skeleton->mNodes.mLength > 4096) return false;
    outPose.instance = skeleton;
    outPose.nodes = skeleton->mNodes.mData;
    outPose.count = skeleton->mNodes.mLength;
    outPose.evaluated = true;
    outPose.valid = true;
    outPose.bones.resize(static_cast<std::size_t>(outPose.count));
    for (int i = 0; i < outPose.count; ++i) {
        outPose.bones[static_cast<std::size_t>(i)].index = i;
        std::memcpy(&outPose.bones[static_cast<std::size_t>(i)].matrix,
                    reinterpret_cast<unsigned char const*>(skeleton->mNodes.mData + i) + 0x44,
                    sizeof(bCMatrix));
    }
    return true;
}

bool ApplyPoseBlend(State& state, NativePose& targetPose, uint32_t now)
{
    if (!state.poseBlend.active) return false;
    const float alpha = npcplayback_hit_policy::PoseBlendAlpha(
        state.poseBlend.active, state.poseBlend.startedAtMs, now, state.poseBlend.durationSec);
    if (alpha >= 0.999f || targetPose.instance != state.poseBlend.skeleton ||
        targetPose.nodes != state.poseBlend.nodes ||
        static_cast<std::size_t>(targetPose.count) != state.poseBlend.fromTransforms.size()) {
        state.poseBlend.active = false;
        return false;
    }
    for (auto& bone : targetPose.bones) {
        if (bone.index < 0 || static_cast<std::size_t>(bone.index) >= state.poseBlend.fromTransforms.size()) continue;
        pose::Transform blended = state.poseBlend.fromTransforms[static_cast<std::size_t>(bone.index)];
        const pose::Transform dst = TransformOf(bone.matrix, 0);
        if (pose::detail::RigidBasis(blended.values) && pose::detail::RigidBasis(dst.values)) {
            pose::detail::SlerpRigidBasis(blended.values, dst.values, alpha);
        } else {
            for (int k = 0; k < 12; ++k) blended.values[k] += (dst.values[k] - blended.values[k]) * alpha;
        }
        bone.matrix = MatrixOf(blended);
    }
    return true;
}

float StartLocalHitKnockback(eCEntity* entity, eCEntity const* attacker, State& state, char const* motion, uint32_t hit,
                             uint32_t now)
{
    float dirXZ[2] = {};
    float stepCm = 0.0f;
    float groundDeltaY = 0.0f;
    float anchorWorld[3] = {};
    eCEntity const* sourceAttacker = attacker ? attacker : remote::ControlledBody();
    const float rawStepCm = npcplayback_hit_policy::ParseMotionDisplacementCm(motion);
    if (entity) {
        const bCVector worldPos = entity->GetWorldPosition();
        if (state.knockback.active && state.knockback.hasPresentedPos) {
            anchorWorld[0] = state.knockback.lastPresentedPos[0];
            anchorWorld[1] = state.knockback.lastPresentedPos[1];
            anchorWorld[2] = state.knockback.lastPresentedPos[2];
        } else {
            anchorWorld[0] = worldPos.GetX();
            anchorWorld[1] = worldPos.GetY();
            anchorWorld[2] = worldPos.GetZ();
        }
        if (rawStepCm > 0.5f) {
            const bCVector origin(anchorWorld[0], anchorWorld[1], anchorWorld[2]);
            const bCVector attackerPos = sourceAttacker ? sourceAttacker->GetWorldPosition() : origin;
            const bCVector forward = entity->GetWorldMatrix().GetZAxis();
            if (npcplayback_hit_policy::ComputeKnockbackStepDirection(
                    origin.GetX(), origin.GetZ(), attackerPos.GetX(), attackerPos.GetZ(),
                    forward.GetX(), forward.GetZ(), dirXZ)) {
                stepCm = ClampKnockbackToWorld(entity, origin, dirXZ, rawStepCm, groundDeltaY);
            }
        }
    }
    npcplayback_hit_policy::StartHitKnockback(
        state.knockback, static_cast<uint16_t>(hit), now, state.localDuration, state.localRecoverDuration,
        anchorWorld, dirXZ, stepCm, groundDeltaY);
    return stepCm;
}

bool StartLocalHitPresentation(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options,
                               uint32_t hit, uint32_t now, float playSpeed, eCEntity const* attacker, uint8_t reaction)
{
    if (!entity || !motion || !*motion || !hit || !options.active || !IsPassive(entity)) return false;
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) { Log("npcplayback: local hit has no presentation actor"); return false; }
    if (state.initialized) CaptureEntityPoseBlend(entity, state, now, npcplayback_hit_policy::HitEntryBlendSeconds);
    PresentationScope scope;
    net::NpcMotion local = options;
    local.active = 3;
    local.phase = 0.0f;
    local.weight = 1.0f;
    const float effectiveSpeed = (playSpeed > 0.05f && std::isfinite(playSpeed)) ? playSpeed : 1.0f;
    char recoverMotion[96] = {};
    float recoverDuration = 0.0f;
    ResolveRecoverMotion(*animation, *actor, motion, local, effectiveSpeed, recoverMotion, recoverDuration);
    if (!PresentMotion(*animation, *actor, 0, motion, local, true)) {
        state.poseBlend.active = false;
        return false;
    }
    const float clipDuration = static_cast<float>(actor->GetMaxTime(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0)));
    if (!(clipDuration > 0.0f) || !std::isfinite(clipDuration)) {
        state.poseBlend.active = false;
        return false;
    }
    state.pendingHit = static_cast<uint16_t>(hit);
    state.acknowledgedAt = 0;
    state.localStart = now;
    state.localPlaySpeed = effectiveSpeed;
    state.localReaction = reaction;
    state.localInRecover = false;
    state.localDuration = clipDuration / effectiveSpeed;
    state.localRecoverDuration = recoverDuration;
    state.localOptions = options;
    strcpy_s(state.localMotion, motion);
    strcpy_s(state.localRecoverMotion, recoverMotion);
    const float stepCm = StartLocalHitKnockback(entity, attacker, state, motion, hit, now);
    Logf("combat: NPCHITSYNC stage=predict-start npc=%s hit=%u reaction=%u motion=%s duration_ms=%.0f "
         "recover_motion=%s recover_ms=%.0f speed=%.2f step_cm=%.0f",
         entity->GetName().GetText(), hit, reaction, motion, state.localDuration * 1000.0f,
         *state.localRecoverMotion ? state.localRecoverMotion : "(none)",
         state.localRecoverDuration * 1000.0f, state.localPlaySpeed, stepCm);
    return true;
}

bool VerifyLocalHitPresentation(eCEntity* entity, State const& state, net::NpcRecord const& record, FindName names,
                                char const* activeLocalClip, bool local, bool echo, float localPhase,
                                float& maxReplayPhaseError)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    for (int i = 0; i < Slots; ++i) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        const auto& motion = record.motions[i];
        char const* expected = nullptr;
        if (i == 0 && (local || echo)) expected = activeLocalClip;
        else if (!local && motion.active && state.suppressed[i] != motion.instance) expected = names(motion.name);
        const bool visible = actor->HasMotionInstance(slot) && actor->GetTargetWeight(slot) > 0.0001f;
        bool phaseMatches = true;
        if (expected && visible) {
            const float duration = static_cast<float>(actor->GetMaxTime(slot));
            const float phase = i == 0 && local ? localPhase : (i == 0 && echo ? 1.0f : motion.phase);
            const float expectedTime = std::min(duration * phase, std::nextafter(duration, 0.0f));
            const float error = std::fabs(static_cast<float>(actor->GetPlayTime(slot)) - expectedTime);
            maxReplayPhaseError = std::max(maxReplayPhaseError, error);
            phaseMatches = std::isfinite(error) && error <= 0.001f;
        }
        const bool expectedVisible = expected && (local || echo || motion.weight > 0.0001f);
        if ((expectedVisible && (!visible || std::strcmp(expected, animation->GetMotionDesc(slot).GetMotionFilename().GetText()))) ||
            (!expected && visible) || !phaseMatches) {
            static unsigned failedPresentations = 0;
            if (failedPresentations++ < 20)
                Logf("npcplayback: %s slot %d changed during visual update; contacts disabled", entity->GetName().GetText(), i);
            return false;
        }
    }
    return true;
}
} // namespace detail

bool ResolveHitMotion(eCEntity* entity, eCEntity const* attacker, uint8_t reaction, char const* currentMotion,
                      char outMotion[96], float& outPlaySpeed)
{
    if (!entity || !outMotion) return false;
    outMotion[0] = '\0';
    auto* admin = dynamic_cast<eCAnimationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCAnimationAdmin")));
    if (!admin) return false;

    Entity npc(entity);
    const bCVector victimPos = entity->GetWorldPosition();
    const bCVector attackerPos = attacker ? attacker->GetWorldPosition() : victimPos;
    const bCVector forward = entity->GetWorldMatrix().GetZAxis();
    const bool frontHit = npcplayback_hit_policy::FrontHitDirection(
        forward.GetX(), forward.GetZ(),
        attackerPos.GetX() - victimPos.GetX(), attackerPos.GetZ() - victimPos.GetZ());
    const gEDirection primaryDir = frontHit ? gEDirection_Back : gEDirection_Fwd;
    const bool leftFoot = npcplayback_hit_policy::NativeStumbleLeftFoot(currentMotion);
    const gEAction firstStumble = leftFoot ? gEAction_StumbleL : gEAction_StumbleR;
    const gEAction secondStumble = leftFoot ? gEAction_StumbleR : gEAction_StumbleL;

    int rightUse = npcplayback_hit_policy::UseNone;
    int leftUse = npcplayback_hit_policy::UseNone;
    if (entity->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity rightItem = npc.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (rightItem.GetInstance() && rightItem.GetInstance()->GetPropertySet(eEPropertySetType_Interaction))
            rightUse = static_cast<int>(rightItem.Interaction.GetUseType());
        Entity leftItem = npc.Inventory.GetItemFromSlot(gESlot_LeftHand);
        if (leftItem.GetInstance() && leftItem.GetInstance()->GetPropertySet(eEPropertySetType_Interaction))
            leftUse = static_cast<int>(leftItem.Interaction.GetUseType());
    }
    int species = -1;
    if (entity->GetPropertySet(eEPropertySetType_NPC)) {
        const gESpecies npcSpecies = npc.NPC.Species;
        species = static_cast<int>(npcSpecies);
    }
    const bool allowQuickClip = npcplayback_hit_policy::NativeQuickStumbleAllowed(species, rightUse, leftUse);

    gEAction candidates[4] = {firstStumble, secondStumble, gEAction_Stumble, gEAction_QuickStumble};
    constexpr int candidateCount = 4;
    if (reaction == net::HitQuickStumble && allowQuickClip) {
        candidates[0] = gEAction_QuickStumble;
        candidates[1] = firstStumble;
        candidates[2] = secondStumble;
        candidates[3] = gEAction_Stumble;
    } else if (reaction == net::HitPierceStumble) {
        candidates[0] = gEAction_PierceStumble;
        candidates[1] = firstStumble;
        candidates[2] = secondStumble;
        candidates[3] = gEAction_Stumble;
    } else if (reaction == net::HitSitKnockDown) {
        candidates[0] = gEAction_SitKnockDown;
        candidates[1] = firstStumble;
        candidates[2] = secondStumble;
        candidates[3] = gEAction_Stumble;
    }

    bCString extension;
    eCResourceAnimationMotion_PS::GetNativeFileExt(extension);
    gEAction matchedAction = gEAction_None;
    ScopedAniProbeState probeGuard(npc, entity);
    const int parsedPose = npcplayback_hit_policy::ParseMotionPrimaryPose(currentMotion);
    if (probeGuard.npcPs && parsedPose != npcplayback_hit_policy::PoseNone)
        probeGuard.npcPs->SetPrimaryPose(static_cast<gEPose>(parsedPose));

    auto probeDirection = [&](gEDirection dir) -> eCResourceDataEntity* {
        probeGuard.SetDirection(dir);
        for (int idx = 0; idx < candidateCount; ++idx) {
            const gEAction action = candidates[idx];
            const bCString stems[2] = {
                npc.GetAni(action, gEPhase_Hit),
                npc.GetAni(action, gEUseType_None, gEUseType_None, gEPhase_Hit),
            };
            for (auto const& stem : stems) {
                if (!stem.GetLength()) continue;
                bCString varied(stem);
                admin->GetAniVariation(varied);
                for (auto const& candidate : {varied, stem}) {
                    bCString name(candidate);
                    name += extension;
                    if (!ReactionMotionResourceName(name.GetText())) continue;
                    if (auto* resource = admin->QueryMotionDataEntity(name, eEResourceCacheBehavior_Immediate)) {
                        if (resource->SearchResourcePropertySet()) {
                            matchedAction = action;
                            return resource;
                        }
                        resource->ReleaseReference();
                    }
                }
            }
        }
        return nullptr;
    };

    auto probeStance = [&](gEAniState stance) -> eCResourceDataEntity* {
        npc.Routine.AniState = stance;
        if (auto* found = probeDirection(primaryDir)) return found;
        // Match Game.dll sAICombatMoveStart (0x1016b11e..0x1016b1b6): fall back from Back to Fwd only when Back is absent.
        if (frontHit) return probeDirection(gEDirection_Fwd);
        return nullptr;
    };

    eCResourceDataEntity* resource = nullptr;
    if (reaction != net::HitSitKnockDown) {
        resource = probeStance(gEAniState_Stand);
        if (!resource && probeGuard.savedStance != gEAniState_Stand)
            resource = probeStance(probeGuard.savedStance);
    } else {
        resource = probeStance(probeGuard.savedStance);
        if (!resource && probeGuard.savedStance != gEAniState_Stand)
            resource = probeStance(gEAniState_Stand);
    }
    if (!resource) return false;
    const bCString fileName = resource->GetFileName();
    resource->ReleaseReference();
    if (!fileName.GetLength() || static_cast<std::size_t>(fileName.GetLength()) >= 96) return false;
    strcpy_s(outMotion, 96, fileName.GetText());

    const bool diseased = entity->GetPropertySet(eEPropertySetType_NPC) &&
                          npc.NPC.HasStatusEffects(gEStatusEffect_Diseased) != GEFalse;
    outPlaySpeed = npcplayback_hit_policy::NativeStumblePlaySpeed(
        reaction, matchedAction == gEAction_QuickStumble, diseased);
    return true;
}

void CancelLocalHit(State& state, uint32_t now, eCEntity* entity)
{
    if (entity && state.initialized && *state.localMotion)
        detail::CaptureEntityPoseBlend(entity, state, now, npcplayback_hit_policy::HitExitBlendSeconds);
    state.pendingHit = 0;
    state.localInRecover = false;
    state.localMotion[0] = '\0';
    state.localRecoverMotion[0] = '\0';
    npcplayback_hit_policy::CancelHitKnockback(state.knockback, now);
}
}
