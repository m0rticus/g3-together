// SPDX-License-Identifier: GPL-3.0-or-later
#include "contact.h"
#include "npcplayback.h"
#include "npcplayback_hit_policy.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace g3t::npcplayback {
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

float ClampKnockbackToWorld(eCEntity* entity, float const dirXZ[2], float desiredStepCm, float& outGroundDeltaY)
{
    outGroundDeltaY = 0.0f;
    if (!entity || !(desiredStepCm > 0.5f)) return 0.0f;
    const bCVector origin = entity->GetWorldPosition();
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
}

namespace detail {
float StartLocalHitKnockback(eCEntity* entity, eCEntity const* attacker, State& state, char const* motion, uint32_t hit,
                             uint32_t now)
{
    float dirXZ[2] = {};
    float stepCm = 0.0f;
    float groundDeltaY = 0.0f;
    eCEntity const* sourceAttacker = attacker ? attacker : remote::ControlledBody();
    const float rawStepCm = npcplayback_hit_policy::ParseMotionDisplacementCm(motion);
    if (rawStepCm > 0.5f && entity) {
        const bCVector victimPos = entity->GetWorldPosition();
        const bCVector attackerPos = sourceAttacker ? sourceAttacker->GetWorldPosition() : victimPos;
        const bCVector forward = entity->GetWorldMatrix().GetZAxis();
        if (npcplayback_hit_policy::ComputeKnockbackStepDirection(
                victimPos.GetX(), victimPos.GetZ(), attackerPos.GetX(), attackerPos.GetZ(),
                forward.GetX(), forward.GetZ(), dirXZ)) {
            stepCm = ClampKnockbackToWorld(entity, dirXZ, rawStepCm, groundDeltaY);
        }
    }
    npcplayback_hit_policy::StartHitKnockback(
        state.knockback, static_cast<uint16_t>(hit), now, state.localDuration, dirXZ, stepCm, groundDeltaY);
    return stepCm;
}
}

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
    const bCString dirToken(frontHit ? "Back" : "Fwd");
    const bool leftFoot = currentMotion && std::strstr(currentMotion, "_L.") != nullptr;
    const gEAction firstStumble = leftFoot ? gEAction_StumbleR : gEAction_StumbleL;
    const gEAction secondStumble = leftFoot ? gEAction_StumbleL : gEAction_StumbleR;

    bool allowQuickClip = true;
    if (reaction == net::HitQuickStumble && npc.NPC.Species != gESpecies_Orc &&
        entity->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity rightItem = npc.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (rightItem.GetInstance() && rightItem.GetInstance()->GetPropertySet(eEPropertySetType_Interaction)) {
            const gEUseType use = rightItem.Interaction.GetUseType();
            if (use == gEUseType_2H || use == gEUseType_CrossBow || use == gEUseType_Halberd || use == gEUseType_Axe)
                allowQuickClip = false;
        }
    }

    gEAction candidates[4] = {firstStumble, secondStumble, gEAction_Stumble, gEAction_QuickStumble};
    int candidateCount = 4;
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

    auto probeActions = [&]() -> eCResourceDataEntity* {
        for (int idx = 0; idx < candidateCount; ++idx) {
            const gEAction action = candidates[idx];
            const bCString stems[4] = {
                npc.GetAniEx(action, gEPhase_Hit, dirToken, GEFalse),
                npc.GetAni(action, gEPhase_Hit),
                npc.GetAniEx(gEUseType_None, gEUseType_None, action, gEPhase_Hit, dirToken, GEFalse),
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

    eCResourceDataEntity* resource = probeActions();
    if (!resource && npc.Routine.AniState == gEAniState_Parade) {
        const gEAniState savedStance = npc.Routine.AniState;
        npc.Routine.AniState = gEAniState_Stand;
        resource = probeActions();
        npc.Routine.AniState = savedStance;
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

void CancelLocalHit(State& state, uint32_t now)
{
    npcplayback_hit_policy::CancelHitKnockback(state.knockback, now);
}
}
