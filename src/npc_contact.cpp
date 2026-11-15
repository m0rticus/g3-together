// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_contact.h"
#include "npc_corpse.h"

#include "combat.h"
#include "combat_timing.h"
#include "contact.h"
#include "log.h"
#include "npclease_runtime.h"
#include "npcremoval.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3t::npcs::detail {
namespace {
bool AttachedTo(eCEntity* entity, eCEntity* owner)
{
    if (!entity || !owner) return false;
    eCEntity* right = nullptr;
    eCEntity* left = nullptr;
    if (owner->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity actor(owner);
        right = actor.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance();
        left = actor.Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance();
    }
    eCEntity* pending[16] = {entity};
    unsigned count = 1;
    for (unsigned i = 0; i < count; ++i) {
        eCEntity* current = pending[i];
        if (current == owner || current == right || current == left) return true;
        auto add = [&](eCEntity* next) {
            if (next && count < 16 && std::find(pending, pending + count, next) == pending + count) pending[count++] = next;
        };
        add(current->GetParent());
        if (current->GetPropertySet(eEPropertySetType_Interaction)) add(Entity(current).Interaction.GetOwner().GetInstance());
    }
    return false;
}

void Point(bCVector const& vector, float out[3])
{
    out[0] = vector.GetX();
    out[1] = vector.GetY();
    out[2] = vector.GetZ();
}

std::string StumbleMotion(char const* skeleton, char const* stance, uint8_t reaction, bool frontHit, bool leftFoot)
{
    char const* dirTag = frontHit ? "_Back_" : "_Fwd_";
    char const* actionTag = reaction == net::HitQuickStumble ? "_QuickStumble_Hit_"
        : reaction == net::HitPierceStumble ? "_PierceStumble_Hit_"
        : (leftFoot ? "_StumbleL_Hit_" : "_StumbleR_Hit_");
    const std::string key = std::string(skeleton) + "_" + stance + actionTag + dirTag;
    if (auto it = stumbleFor.find(key); it != stumbleFor.end()) return it->second;
    std::string bestExact, bestStanceDir, bestDir, bestAction, bestStance, any;
    const std::string prefix = std::string(skeleton) + "_Stand_";
    for (auto const& [hash, name] : motionNames) {
        if (name.compare(0, prefix.size(), prefix) || name.find("Stumble") == std::string::npos || name.find("_Hit_") == std::string::npos) continue;
        if (name.find("Parade") != std::string::npos) continue;
        if (any.empty()) any = name;
        const bool matchStance = !*stance || name.find(std::string("_") + stance + "_") != std::string::npos;
        const bool matchAction = name.find(actionTag) != std::string::npos ||
            (reaction != net::HitQuickStumble && name.find("_Stumble") != std::string::npos && name.find("_QuickStumble_") == std::string::npos);
        const bool matchDir = name.find(dirTag) != std::string::npos;
        if (matchStance && bestStance.empty()) bestStance = name;
        if (matchDir && bestDir.empty()) bestDir = name;
        if (matchStance && matchDir && bestStanceDir.empty()) bestStanceDir = name;
        if (matchStance && matchAction && bestAction.empty()) bestAction = name;
        if (matchStance && matchAction && matchDir) { bestExact = name; break; }
    }
    std::string chosen = !bestExact.empty() ? bestExact
        : !bestStanceDir.empty() ? bestStanceDir
        : !bestDir.empty() ? bestDir
        : fallbackStumbles ? std::string()
        : !bestAction.empty() ? bestAction
        : !bestStance.empty() ? bestStance : any;
    if (chosen.empty() && fallbackStumbles) {
        char const* dirWord = frontHit ? "Back" : "Fwd";
        char const* stepWord = frontHit ? "90" : "200";
        char const* sideWord = leftFoot ? "_P0_StumbleL_Hit_N_" : "_P0_StumbleR_Hit_N_";
        if (!std::strcmp(skeleton, "Hero") && (!*stance || !std::strcmp(stance, "None")))
            chosen = std::string("Hero_Stand_None_None") + sideWord + dirWord + "_00_%_00_P0_" + stepWord;
        else if (!std::strcmp(skeleton, "Hero") && reaction == net::HitQuickStumble)
            chosen = std::string("Hero_Stand_None_") + stance + "_P0_QuickStumble_Hit_N_" + dirWord + "_00_%_00_P0_90";
        else if (!std::strcmp(skeleton, "Hero"))
            chosen = std::string("Hero_Stand_None_") + stance + sideWord + dirWord + "_00_%_00_P0_" + stepWord;
        else
            chosen = std::string(skeleton) + "_Stand_None_Fist" + sideWord + dirWord + "_00_%_00_P0_" + stepWord;
        return chosen;
    }
    if (const size_t dot = chosen.find(".xmot"); dot != std::string::npos) chosen.resize(dot);
    if (!chosen.empty()) stumbleFor[key] = chosen;
    return chosen;
}

void LocalHitWithSwing(eCEntity* entity, uint32_t sequence, uint32_t swing, uint8_t reaction = 0)
{
    if (entity && sequence && !npcs::HostPresentationAvailable(entity)) {
        const uint32_t effectiveSwing = swing ? swing : npcplayback::MotionInstance(remote::ControlledBody());
        npcplayback::CosmeticHit(entity, effectiveSwing);
        return;
    }
    if (!entity || !sequence || !npcs::Ready(entity)) return;
    for (auto& entry : mirrors) {
        Mirror& m = entry.second;
        if (m.entity.GetInstance() != entity) continue;
        if (m.killed) continue;
        m.localHitValid = true;
        const uint32_t hitAt = net::Now();
        m.localHitAt = hitAt ? hitAt : 1;
        std::strcpy(m.killerAttribution, "local_player");
        char shown[128] = {}, stance[16] = {};
        body::CurrentMotion(entity, shown, sizeof(shown));
        LearnSkeleton(m, shown);
        int field = 0;
        for (char const* c = shown; *c && field < 4; ++c) {
            if (*c == '_') { ++field; continue; }
            if (field == 3) {
                const size_t n = std::strlen(stance);
                if (n + 1 < sizeof(stance)) stance[n] = *c, stance[n + 1] = '\0';
            }
        }
        eCEntity* attacker = remote::ControlledBody();
        const uint8_t effectiveReaction = reaction ? reaction : static_cast<uint8_t>(net::HitStumble);
        char nativeMotion[96] = {};
        float playSpeed = 1.3f;
        std::string motion;
        if (npcplayback::ResolveHitMotion(entity, attacker, effectiveReaction, shown, nativeMotion, playSpeed)) {
            motion = nativeMotion;
        } else {
            const bCVector victimPos = entity->GetWorldPosition();
            const bCVector attackerPos = attacker ? attacker->GetWorldPosition() : victimPos;
            const bCVector forward = entity->GetWorldMatrix().GetZAxis();
            const bool frontHit = npcplayback_hit_policy::FrontHitDirection(
                forward.GetX(), forward.GetZ(),
                attackerPos.GetX() - victimPos.GetX(), attackerPos.GetZ() - victimPos.GetZ());
            const bool leftFoot = npcplayback_hit_policy::NativeStumbleLeftFoot(shown);
            motion = StumbleMotion(m.skeleton, stance, effectiveReaction, frontHit, leftFoot);
            if (!motion.empty() && motion.find(".xmot") == std::string::npos) motion += ".xmot";
            const bool quickUsed = motion.find("QuickStumble") != std::string::npos;
            playSpeed = npcplayback_hit_policy::NativeStumblePlaySpeed(effectiveReaction, quickUsed, false);
        }
        if (motion.empty() || !npcplayback::LocalHit(entity, m.replay, motion.c_str(), m.latest.motions[0], sequence,
                                                     net::Now(), playSpeed, attacker, effectiveReaction)) {
            Logf("npcs: immediate hit presentation could not start for %s", m.entity.GetName().GetText());
            return;
        }
        ++localHitsShown;
        return;
    }
}
bCVector ChestRayEndpoint(eCEntity* entity)
{
    bCVector point = entity->GetWorldPosition();
    bCBox const& box = entity->GetWorldNodeBoundary();
    auto* move = static_cast<gCCharacterMovement_PS*>(entity->GetPropertySet(eEPropertySetType_CharacterMovement));
    point.AccessY() = contact::ContactRayY(point.GetY(), box.GetMin().GetY(), box.GetMax().GetY(),
                                           move ? move->GetCharacterHeight() : 0.0f);
    return point;
}

bool IsNonBarrierEntity(eCEntity* hit)
{
    return hit && (hit->GetPropertySet(eEPropertySetType_NPC) ||
                   hit->GetPropertySet(eEPropertySetType_PlayerMemory) ||
                   hit->GetPropertySet(eEPropertySetType_CharacterMovement) ||
                   hit->GetPropertySet(eEPropertySetType_Item) ||
                   hit->GetPropertySet(eEPropertySetType_Projectile));
}
} // namespace

bool ClearContact(eCEntity* attacker, eCEntity* hero, ContactObstruction* obstruction)
{
    if (!attacker || !hero) return false;
    const bCVector start = ChestRayEndpoint(attacker), end = ChestRayEndpoint(hero);
    const float length = (end - start).GetMagnitude();
    if (!(length > 4.0f) || !std::isfinite(length)) return true;
    auto& physics = eCPhysicsScene::GetInstance();
    // Match native IsFreeLOSAndSurrounding (Game.dll 0xea8f0): test static world geometry plus Mover group 17
    // (doors/gates), never TestDynamicShapes (which hits nearby NPCs' +100 cm WeaponTrigger/Parade spheres, corpses, and items).
    const GEU32 hints = eETraceRayHints_TestStaticShapes | eETraceRayHints_Distance | eETraceRayHints_IncludeCollisionGroup17;
    if (!physics.TraceRayAllHits(start, end - start, attacker, hints)) return true;
    auto const& report = physics.GetQueryReport();
    float bestDist = length;
    eCEntity* bestHit = nullptr;
    bool obstructed = false;
    const int count = std::min(report.m_arrHitEntity.GetCount(), report.m_arrDistance.GetCount());
    for (int i = 0; i < count; ++i) {
        const float dist = report.m_arrDistance[i];
        if (!contact::RayHitStrictlyBetween(dist, length)) continue;
        eCEntity* hit = report.m_arrHitEntity[i];
        if (hit && (AttachedTo(hit, attacker) || AttachedTo(hit, hero) || IsNonBarrierEntity(hit))) continue;
        if (!obstructed || dist < bestDist) {
            obstructed = true;
            bestDist = dist;
            bestHit = hit;
        }
    }
    if (!obstructed) return true;
    char const* blockerName = (bestHit && bestHit->GetName().GetText() && *bestHit->GetName().GetText())
                                  ? bestHit->GetName().GetText()
                                  : "(static_geometry)";
    if (obstruction) {
        std::snprintf(obstruction->blocker, sizeof(obstruction->blocker), "%s", blockerName);
        obstruction->rayCm = length;
        obstruction->hitCm = bestDist;
    }
    static unsigned noted = 0;
    if (noted++ < 24)
        Logf("npcs: displayed contact %s -> %s obstructed by %s (hit %.0f / ray %.0f cm)",
             attacker->GetName().GetText(), hero->GetName().GetText(), blockerName, bestDist, length);
    return false;
}

bool PresentedContactPoints(Entity& entity, uint32_t heldRight, float a[3], float b[3], float& thickness, int& bone)
{
    eCEntity* weapon = nullptr;
    if (heldRight) {
        Entity held = entity.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (!held.GetInstance() || HeldHash(entity, gESlot_RightHand) != heldRight) return false;
        if (held.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) && held.Interaction.GetUseType() != gEUseType_Fist)
            weapon = held.GetInstance();
    }
    if (weapon) {
        bCBox const& box = weapon->GetLocalNodeBoundary();
        float low[3], high[3];
        Point(box.GetMin(), low);
        Point(box.GetMax(), high);
        int axis = 0;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(low[i]) || !std::isfinite(high[i]) || high[i] < low[i]) return false;
            if (high[i] - low[i] > high[axis] - low[axis]) axis = i;
        }
        if (high[axis] - low[axis] < 1.0f) return false;
        float first[3], last[3];
        thickness = 0.0f;
        for (int i = 0; i < 3; ++i) {
            first[i] = last[i] = (low[i] + high[i]) * 0.5f;
            if (i != axis) thickness = std::max(thickness, (high[i] - low[i]) * 0.5f);
        }
        first[axis] = low[axis];
        last[axis] = high[axis];
        Point(bCVector(first[0], first[1], first[2]).GetTransformed(weapon->GetWorldMatrix()), a);
        Point(bCVector(last[0], last[1], last[2]).GetTransformed(weapon->GetWorldMatrix()), b);
        bone = -2;
        return true;
    }
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance()) return false;
    bCMatrix right, left;
    if (actor->GetNodeByName(bCString("Bip01 R Hand")) && actor->GetNodeByName(bCString("Bip01 L Hand")) &&
        animation->GetBoneMatrix(bCString("Bip01 R Hand"), right) &&
        animation->GetBoneMatrix(bCString("Bip01 L Hand"), left)) {
        Point(right.GetTranslation(), a);
        Point(left.GetTranslation(), b);
        thickness = 8.0f;
        bone = -3;
        return true;
    }
    const bCMatrix& world = entity.GetInstance()->GetWorldMatrix();
    const bCVector forward = bCVector(0, 0, 1).GetTransformed(world) - world.GetTranslation();
    const bCVector& origin = entity.GetInstance()->GetWorldPosition();
    float farthest = -1e20f;
    bone = -1;
    const int bones = actor->GetInstance()->mNodes.mLength;
    if (bones <= 0 || bones > 4096) return false;
    for (int i = 0; i < std::min(bones, 128); ++i) {
        bCMatrix at;
        if (!animation->GetBoneMatrix(i, at)) break;
        const bCVector delta = at.GetTranslation() - origin;
        const float projection = delta.GetX() * forward.GetX() + delta.GetY() * forward.GetY() + delta.GetZ() * forward.GetZ();
        if (projection > farthest) { farthest = projection; bone = i; Point(at.GetTranslation(), a); }
    }
    if (bone < 0) return false;
    std::memcpy(b, a, sizeof(float) * 3);
    thickness = 10.0f;
    return true;
}

bool MeasureSweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                       float thickness, bool weapon, float& gapCm)
{
    bCBox const& box = target->GetWorldNodeBoundary();
    const bCVector& low = box.GetMin();
    const bCVector& high = box.GetMax();
    const float radius = std::min(high.GetX() - low.GetX(), high.GetZ() - low.GetZ()) * 0.5f;
    if (!(radius > 0.0f) || !std::isfinite(radius)) { gapCm = 1e9f; return false; }
    float first[3] = {(low.GetX() + high.GetX()) * 0.5f, low.GetY() + radius, (low.GetZ() + high.GetZ()) * 0.5f};
    float last[3] = {first[0], std::max(first[1], high.GetY() - radius), first[2]};
    const float reach = radius + thickness;
    const float distSq = contact::SweptSegmentDistanceSquared(oldA, oldB, a, b, first, last, weapon);
    gapCm = std::sqrt(std::max(0.0f, distSq)) - reach;
    return distSq <= reach * reach;
}

bool SweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                float thickness, bool weapon)
{
    float gapCm = 0.0f;
    return MeasureSweepsBody(target, oldA, oldB, a, b, thickness, weapon, gapCm);
}

void UpdateIncomingMissReason(Mirror& m, char const* reason)
{
    m.swingMissReason = contact::ChooseIncomingMissReason(m.swingMissReason, reason);
}

void ResetIncomingSwing(Mirror& m, uint32_t instance, eCEntity* hero)
{
    (void)hero;
    m.swingArcCount = 0;
    m.swingTests = 0;
    m.swingLogged = instance == 0;
    m.swingMaxPhase = 0.0f;
    m.swingClosestGapCm = 1e9f;
    m.swingMissReason = "phase_below_hit_threshold";
    m.swingBlocker[0] = '\0';
    m.swingRayCm = 0.0f;
    m.swingHitCm = 0.0f;
}

void ResetMirrorIncomingContact(Mirror& m)
{
    m.contactInstance = 0;
    m.contactAt = 0;
    m.contactConsumed = false;
    m.contactBone = -1;
    m.lastClockProtected = false;
    m.body.contactTarget = nullptr;
    m.body.lastAt = 0;
    ResetIncomingSwing(m, 0, nullptr);
}

void RecordPreHitContact(Mirror& m, eCEntity* hero, float const a[3], float const b[3], int bone, float phase, uint32_t now)
{
    static unsigned contactsLogged = 0;
    if (!m.contactAt && hero && contactsLogged++ < 16) {
        const auto& at = hero->GetWorldPosition();
        Logf("npcs: visible contact %s instance=%u bone=%d at %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f hero %.0f/%.0f/%.0f",
             m.entity.GetName().GetText(), m.contactInstance, bone, a[0], a[1], a[2], b[0], b[1], b[2],
             at.GetX(), at.GetY(), at.GetZ());
    }
    if (m.contactBone != bone) m.swingArcCount = 0;
    std::memcpy(m.contactA, a, sizeof(m.contactA));
    std::memcpy(m.contactB, b, sizeof(m.contactB));
    m.contactAt = now;
    m.contactBone = bone;
    if (phase < 0.30f) return;
    if (m.swingArcCount > 0 &&
        body::Distance(m.swingArcA[m.swingArcCount - 1], a) < 8.0f &&
        body::Distance(m.swingArcB[m.swingArcCount - 1], b) < 8.0f)
        return;
    if (m.swingArcCount == Mirror::MaxSwingArcSamples) {
        for (int i = 1; i < Mirror::MaxSwingArcSamples; ++i) {
            std::memcpy(m.swingArcA[i - 1], m.swingArcA[i], sizeof(m.swingArcA[i]));
            std::memcpy(m.swingArcB[i - 1], m.swingArcB[i], sizeof(m.swingArcB[i]));
        }
        --m.swingArcCount;
    }
    std::memcpy(m.swingArcA[m.swingArcCount], a, sizeof(m.swingArcA[0]));
    std::memcpy(m.swingArcB[m.swingArcCount], b, sizeof(m.swingArcB[0]));
    ++m.swingArcCount;
}

bool EvaluateIncomingSwingSweep(eCEntity* hero, Mirror& m, float const oldA[3], float const oldB[3],
                                float const a[3], float const b[3], float thickness, bool weapon, char const*& hitSource)
{
    if (m.swingTests < 255) ++m.swingTests;
    float gapCm = 1e9f;
    if (MeasureSweepsBody(hero, oldA, oldB, a, b, thickness, weapon, gapCm)) {
        m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
        hitSource = "live-sweep";
        return true;
    }
    m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
    if (m.swingArcCount > 0) {
        const uint8_t count = m.swingArcCount;
        for (uint8_t i = 1; i < count; ++i) {
            if (MeasureSweepsBody(hero, m.swingArcA[i - 1], m.swingArcB[i - 1],
                                  m.swingArcA[i], m.swingArcB[i], thickness, weapon, gapCm)) {
                m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
                hitSource = "strike-arc-at-hit-phase";
                return true;
            }
            m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
        }
        if (MeasureSweepsBody(hero, m.swingArcA[count - 1], m.swingArcB[count - 1], oldA, oldB, thickness, weapon, gapCm)) {
            m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
            hitSource = "strike-arc-at-hit-phase";
            return true;
        }
        m.swingClosestGapCm = std::min(m.swingClosestGapCm, gapCm);
        m.swingArcCount = 0;
    }
    UpdateIncomingMissReason(m, "blade_outside_hero_capsule");
    return false;
}

void FinishIncomingSwingDiag(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    if (!m.contactInstance || m.contactConsumed || m.swingLogged) return;
    m.swingLogged = true;
    const int32_t bufferMs = static_cast<int32_t>(now - (shown.sent + static_cast<uint32_t>(m.body.track.offset)));
    Logf("combat: NPCATTACK stage=local-miss npc=%s player=%u instance=%u reason=%s max_phase=%.3f tests=%u "
         "closest_gap_cm=%.1f blocker=%s hit_cm=%.0f ray_cm=%.0f presentation_ms=%d backlog_ms=%d clock_protected=%d",
         m.entity.GetInstance() ? m.entity.GetName().GetText() : "(unloaded)", net::LocalId(), m.contactInstance,
         m.swingMissReason, m.swingMaxPhase, static_cast<unsigned>(m.swingTests),
         m.swingTests ? m.swingClosestGapCm : -1.0f, m.swingBlocker[0] ? m.swingBlocker : "-",
         m.swingHitCm, m.swingRayCm, bufferMs, m.body.track.backlogMs,
         m.body.track.combatClockProtected ? 1 : 0);
}

void NoteIncomingClockTransition(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    (void)shown;
    (void)now;
    const bool nowProtected = m.body.track.combatClockProtected;
    if (m.lastClockProtected == nowProtected) return;
    m.lastClockProtected = nowProtected;
    if (!nowProtected && m.body.track.backlogMs >= 100 && m.entity.GetInstance())
        Logf("combat: NPCATTACK stage=clock-release npc=%s player=%u backlog_ms=%d delay_ms=%d reason=between-swing-catchup",
             m.entity.GetName().GetText(), net::LocalId(), m.body.track.backlogMs, body::PlaybackDelay(m.body.track));
}

void LogLocalBlowContact(Mirror& m, eCEntity* hero, net::NpcRecord const& shown, uint32_t instance,
                         char const* hitSource, uint32_t now)
{
    const int32_t bufferMs = static_cast<int32_t>(now - (shown.sent + static_cast<uint32_t>(m.body.track.offset)));
    const uint32_t oneWayMs = net::RoundTrip(m.owner) / 2;
    Logf("combat: NPCATTACK stage=local-contact npc=%s player=%u instance=%u host_sample=%u client_judged=%u "
         "phase=%.4f native_hit_phase=%.3f hit_source=%s closest_gap_cm=%.1f presentation_ms=%d backlog_ms=%d "
         "one_way_est_ms=%u compensation_est_ms=%d clock_protected=%d block_state=%d authority=defender-local",
         m.entity.GetName().GetText(), net::LocalId(), instance, shown.sent, now, shown.motions[0].phase,
         combat_timing::NativeHitPhase, hitSource, m.swingClosestGapCm, bufferMs, m.body.track.backlogMs,
         oneWayMs, bufferMs + static_cast<int32_t>(oneWayMs), m.body.track.combatClockProtected ? 1 : 0,
         combat::InParadeMode(hero) ? 1 : 0);
}

void SamplePlayerContact(uint32_t now)
{
    eCEntity* hero = remote::ControlledBody();
    if (!hero || !net::Connected()) { playerContact = PlayerContact{}; return; }
    Entity player(hero);
    float a[3], b[3], thickness = 0;
    int bone = -1;
    if (!PresentedContactPoints(player, HeldHash(player, gESlot_RightHand), a, b, thickness, bone)) {
        playerContact = PlayerContact{};
        return;
    }
    if (!body::LocallyAttacking(hero)) {
        std::memcpy(playerContact.a, a, sizeof(a));
        std::memcpy(playerContact.b, b, sizeof(b));
        playerContact.instance = 0;
        playerContact.at = now;
        playerContact.thickness = thickness;
        playerContact.bone = bone;
        playerContact.ready = false;
        return;
    }
    ++playerSwingFrames;
    const uint32_t instance = npcplayback::MotionInstance(hero);
    if (!instance) {
        playerContact.ready = false;
        return;
    }
    if (instance != playerContact.instance) {
        const auto& origin = hero->GetWorldPosition();
        Logf("npcs: player sweep %u root %.0f/%.0f/%.0f blade %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f thickness %.1f",
             instance, origin.GetX(), origin.GetY(), origin.GetZ(), a[0], a[1], a[2], b[0], b[1], b[2], thickness);
        auto* animation = static_cast<eCVisualAnimation_PS*>(hero->GetPropertySet(eEPropertySetType_Animation));
        if (animation && animation->GetActor() && animation->GetActor()->GetNodeByName(bCString("Bip01 R Hand"))) {
            bCMatrix hand;
            if (animation->GetBoneMatrix(bCString("Bip01 R Hand"), hand)) {
                const auto& at = hand.GetTranslation();
                Logf("npcs: player sweep %u rendered hand %.0f/%.0f/%.0f", instance, at.GetX(), at.GetY(), at.GetZ());
            }
        }
        if (auto* target = player.NPC.GetCurrentTarget().GetInstance()) {
            const auto& box = target->GetWorldNodeBoundary();
            Logf("npcs: player sweep %u target %s bounds %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f", instance,
                 target->GetName().GetText(), box.GetMin().GetX(), box.GetMin().GetY(), box.GetMin().GetZ(),
                 box.GetMax().GetX(), box.GetMax().GetY(), box.GetMax().GetZ());
        }
    }
    const bool previous = playerContact.at && now - playerContact.at <= 100 && playerContact.bone == bone;
    std::memcpy(playerContact.previousA, playerContact.a, sizeof(a));
    std::memcpy(playerContact.previousB, playerContact.b, sizeof(b));
    std::memcpy(playerContact.a, a, sizeof(a));
    std::memcpy(playerContact.b, b, sizeof(b));
    playerContact.instance = instance;
    playerContact.at = now;
    playerContact.thickness = thickness;
    playerContact.bone = bone;
    playerContact.ready = previous;
}

bool StaggerState(char const* name)
{
    static char const* const names[] = {"ZS_Stumble", "ZS_QuickStumble", "ZS_PierceStumble", "ZS_ParadeStumble", "ZS_HeavyParadeStumble"};
    for (char const* known : names)
        if (!std::strcmp(name, known)) return true;
    return false;
}

void OnHitResult(net::HitResult const& result)
{
    const GuidKey key = Key(result.guid);
    const auto awaited = awaitedHits.find(key);
    if (awaited != awaitedHits.end() && awaited->second.sequence == result.sequence &&
        (!awaited->second.incarnation || awaited->second.incarnation == result.incarnation)) {
        const uint32_t swing = awaited->second.swing;
        awaitedHits.erase(awaited);
        if (result.accepted && result.incarnation == npcs::Incarnation(result.guid))
            LocalHitWithSwing(npcs::NpcByGuid(result.guid), result.sequence, swing);
    }
    const auto found = mirrors.find(key);
    if (found == mirrors.end()) return;
    Mirror& m = found->second;
    if (static_cast<uint16_t>(result.sequence) != m.replay.pendingHit) return;
    if (!result.accepted) {
        npcplayback::CancelLocalHit(m.replay, net::Now(), m.entity.GetInstance());
        m.replay.initialized = false;
        Logf("npcs: host refused hit %u on %s; authoritative state retained", result.sequence, m.entity.GetName().GetText());
    } else if (result.incarnation == m.latest.incarnation) {
        m.replay.acknowledgedHit = static_cast<uint16_t>(result.sequence);
        m.replay.acknowledgedAt = result.processedAt ? result.processedAt : 1;
        Logf("combat: NPCHITSYNC stage=host-ack npc=%s hit=%u accepted=1 processed_at=%u",
             m.entity.GetInstance() ? m.entity.GetName().GetText() : "(unloaded)", result.sequence,
             m.replay.acknowledgedAt);
    }
}

} // namespace g3t::npcs::detail

namespace g3t::npcs {
using namespace detail;

void LocalHit(eCEntity* entity) { LocalHit(entity, 0, 0); }

void LocalHit(eCEntity* entity, uint32_t sequence, uint8_t reaction) { LocalHitWithSwing(entity, sequence, 0, reaction); }

bool LocalReaction(eCEntity const* entity, char const* state)
{
    if (npcplayback::IsPassive(entity)) return false;
    if (!entity || !nativeHits) return false;
    const uint32_t now = net::Now();
    for (auto const& entry : mirrors) {
        Mirror const& m = entry.second;
        if (m.entity.GetInstance() != entity) continue;
        if (!m.body.localNative || static_cast<int32_t>(m.body.localHitUntil - now) <= 0) return false;
        if (StaggerState(state)) return true;
        if (!std::strncmp(state, "ZS_", 3)) return false;
        Entity task = m.entity;
        return StaggerState(task.Routine.GetCurrentTask().GetText());
    }
    return false;
}

uint32_t SeenAt(eCEntity const* entity)
{
    if (!entity) return 0;
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return entry.second.havePresentation ? entry.second.shownSent
                   : GetConfig().movementSmoothing && body::interpolate ? 0 : entry.second.latestSent;
    return 0;
}

bool ParryingAt(eCEntity const* entity, uint32_t at, bool& parrying)
{
    if (!lagBlocks || !entity || !at || static_cast<int32_t>(net::Now() - at) < 0 ||
        net::Now() - at > SentStatesFor) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    const auto it = sentStates.find(Key(guid));
    if (it == sentStates.end()) return false;
    int best = -1;
    for (int i = 0; i < StateHistory; ++i) {
        const uint32_t t = it->second.at[i];
        if (!t || static_cast<int32_t>(t - at) > 0 || static_cast<int32_t>(at - t) > static_cast<int32_t>(SentStatesFor)) continue;
        if (best < 0 || static_cast<int32_t>(t - it->second.at[best]) > 0) best = i;
    }
    if (best < 0) return false;
    parrying = it->second.parrying[best];
    return true;
}

bool InAction(eCEntity const* entity)
{
    if (!entity) return false;
    const uint32_t now = net::Now();
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return static_cast<int32_t>(entry.second.body.actionUntil - now) > 0;
    return false;
}

bool DiedOnHost(eCEntity const* entity)
{
    if (!entity) return false;
    for (auto const& entry : mirrors)
        if (entry.second.killed && entry.second.entity.GetInstance() == entity) return true;
    return false;
}

bool IsMirrored(eCEntity const* entity)
{
    if (npclease::LocalAuthority(entity)) return false;
    if (npcplayback::IsPassive(entity)) return true;
    if (!entity) return false;
    for (auto const& entry : mirrors)
        if (entry.second.body.pacified && entry.second.entity.GetInstance() == entity) return true;
    return false;
}

bool Ready(eCEntity const* entity)
{
    if (RedundantClientEntity(entity)) return false;
    if (npcremoval::Suppressed(entity)) return false;
    if (npclease::LocalAuthority(entity)) return true;
    if (npclease::Fenced(entity)) return false;
    if (!IsMirrored(entity)) return true;
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return HostPresentationAvailable(entity) && entry.second.replayReady;
    return false;
}

void LeaseBaseline(eCEntity* entity, char* routine, size_t capacity, uint8_t& defeated)
{
    if (!entity || !routine || !capacity) return;
    uint8_t identity[16];
    Guid(entity, identity);
    const auto found = mirrors.find(Key(identity));
    if (found == mirrors.end()) return;
    auto const& mirror = found->second;
    if (mirror.body.pacified) strncpy_s(routine, capacity, mirror.body.routine, _TRUNCATE);
    if (mirror.guarded) defeated = mirror.defeatedBefore ? 1 : 0;
}

namespace detail {

void BeginPassiveMirrorFrame(uint32_t now)
{
    if (!absenceCheckAt || now - absenceCheckAt >= 250) {
        absenceCheckAt = now ? now : 1;
        for (auto const& entry : absences) {
            auto const& record = entry.second;
            Entity live = Resolve(record.guid);
            eCEntity* entity = live.GetInstance();
            if (!IsNpc(entity)) continue;
            const bCMatrix pose = body::Pose(*entity, record.position, record.rotation);
            entity->SetToWorldMatrix(pose);
            if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
                rigid->SetPose(pose);
            ClearLocalReferencesToDeadMirror(entity, nullptr, false);
        }
    }
    SamplePlayerContact(now);
    if (lastFrameAt) {
        const uint32_t gap = now - lastFrameAt;
        ++frameCount;
        frameSumMs += gap;
        slowFrames += gap > 33;
        hitchFrames += gap > 66;
        maxFrameMs = std::max(maxFrameMs, gap);
    }
    lastFrameAt = now;
}

void ResetUnloadedMirror(Mirror& m, Entity const& live, eCEntity* previous)
{
    const bool keepCorpse = m.killed && m.corpseSettled && !m.corpsePose.empty();
    if (m.resolved)
        Logf("npcs: mirror %s unloaded or changed property sets; waiting for its live instance",
             live.GetInstance() ? live.GetName().GetText() : "(unloaded)");
    if (previous) npcplayback::ReleaseUnavailable(previous);
    if (previous && live.GetInstance() != previous)
        ClearLocalReferencesToDeadMirror(previous, nullptr, false, true);
    m.entity = live;
    if (live.GetInstance() != previous) m.deathReferencesCleared = nullptr;
    m.resolved = m.replayReady = m.corpsePlaced = m.corpseSimulating = m.ragdollPaused = false;
    m.resolvedAt = 0;
    m.replay = npcplayback::State{};
    ResetMirrorIncomingContact(m);
    m.ragdollStarted = m.ragdollInitialized = m.corpseSettled = keepCorpse;
    if (!keepCorpse) m.corpsePose.clear();
    m.corpseActor = nullptr;
    m.corpsePoseAt = m.ragdollTryAt = m.lieCheckAt = 0;
    m.ragdollStarts = m.ragdollWakes = m.ragdollFails = m.lieTries = m.lieNoted = 0;
    m.laid = m.localHitValid = m.guarded = false;
    m.localHitAt = m.fallbackGraceUntil = 0;
    std::strcpy(m.presentationSource, RagdollSource(m));
}

void ReconcileMirrorAlive(GuidKey const& key, Mirror& m)
{
    npcplayback::ResetLocalHitState(m.replay);
    ResetMirrorIncomingContact(m);
    m.ragdollStarted = m.laid = m.ragdollInitialized = m.corpsePlaced = m.corpseSimulating = false;
    m.ragdollPaused = m.corpseSettled = false;
    m.deathImpulse = {};
    m.ragdollTraceAt = m.ragdollTraceSampleAt = m.ragdollTraceStage = 0;
    m.localHitValid = m.deathLogged = m.focusWatchLogged = false;
    m.localHitAt = m.fallbackGraceUntil = m.focusWatchUntil = 0;
    m.deathClearCount = m.deathCombatStopCount = 0;
    std::strcpy(m.presentationSource, "gravity");
    std::strcpy(m.killerAttribution, "host_or_npc_unknown");
    deadOnHost.erase(key);
    ForgetCorpseGroundAnchor(key);
}

void SyncMirrorEquipmentAndDamage(Mirror& m, net::NpcRecord const& shown)
{
    if (shown.heldRight != HeldHash(m.entity, gESlot_RightHand) ||
        shown.heldLeft != HeldHash(m.entity, gESlot_LeftHand)) {
        EnsureItem(m.entity, shown.heldRight);
        EnsureItem(m.entity, shown.heldLeft);
        m.entity.Inventory.HoldStacks(StackOf(m.entity, shown.heldLeft), StackOf(m.entity, shown.heldRight));
        ++holdChanges;
    }
    if (auto name = motionNames.find(shown.motions[0].name); name != motionNames.end())
        LearnSkeleton(m, name->second.c_str());
    Entity damageSource = m.entity.Inventory.GetItemFromSlot(gESlot_RightHand);
    if (!damageSource.GetInstance() || !damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage))
        damageSource = m.entity;
    if (auto* damage = static_cast<gCDamage_PS*>(damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage))) {
        damage->AccessDamageType().AccessValue() = static_cast<gEDamageType>(shown.damageType);
        damage->AccessDamageAmount() = shown.damageAmount;
        damage->AccessDamageHitMultiplier() = shown.damageMultiplier;
    }
}

void RecordMirrorWalkTelemetry(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    const uint32_t dt = m.body.lastAt ? now - m.body.lastAt : 0;
    const float traveled = body::Distance(m.body.lastPos, shown.position);
    char const* expected = MotionNameOf(shown.motions[0].name);
    if (body::IsLocomotionMotion(expected) && dt && dt < 250 && traveled < 300.0f) {
        m.body.walkExpectedMs += dt;
        if (traveled * 1000.0f / static_cast<float>(dt) > 10.0f) {
            m.body.walkTravelMs += dt;
            char visible[128] = {};
            body::CurrentMotion(m.entity.GetInstance(), visible, sizeof(visible));
            if (!body::IsLocomotionMotion(visible) || !body::MotionRunning(m.entity.GetInstance()))
                m.body.walkSlideMs += dt;
        } else {
            m.body.walkStillMs += dt;
        }
    }
    std::memcpy(m.body.lastPos, shown.position, sizeof(m.body.lastPos));
    m.body.lastAt = now;
}

bool MirrorFightsHeroImpl()
{
    eCEntity* body = remote::ControlledBody();
    if (!body || !net::LocalId()) return false;
    const bCVector& p = body->GetWorldPosition();
    const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
    bool any = false, us = false;
    for (auto const& entry : mirrors) {
        Mirror const& m = entry.second;
        if (m.killed || !(m.latest.flags & 4) || (m.latest.flags & 1)) continue;
        any = true;
        if (((m.latest.flags >> 3) & 7) != net::LocalId()) continue;
        us = true;
        if (body::Distance(m.latest.position, at) <= 3000.0f) {
            ++fightSeenAny, ++fightSeenUs, ++fightSeenNear;
            return true;
        }
    }
    fightSeenAny += any;
    fightSeenUs += us;
    return false;
}

} // namespace detail

} // namespace g3t::npcs
