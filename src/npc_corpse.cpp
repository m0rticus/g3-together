// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_corpse.h"
#include "npcplayback_skeleton.h"

#include "ai.h"
#include "containers.h"
#include "downed.h"
#include "log.h"
#include "npcremoval.h"
#include "world.h"

#include <Windows.h>
#include <cstdio>

namespace g3t::npcs::detail {
namespace {
using Vec3 = ragdoll::Vector;
static_assert(static_cast<uint8_t>(gECharMovementMode_RagDollDead) == ragdoll::RagdollDeadMovementMode);

struct CorpseGroundAnchor {
    uint32_t incarnation = 0;
    Vec3 origin{};
    uint32_t updatedAt = 0;
};

std::unordered_map<GuidKey, CorpseGroundAnchor, GuidHash> corpseGroundAnchors;

bool FiniteMatrix(bCMatrix const& matrix, float maxTranslationCm = ragdoll::MaximumTransformCoordCm)
{
    const bCVector axes[] = {matrix.GetXAxis(), matrix.GetYAxis(), matrix.GetZAxis(), matrix.GetTranslation()};
    float values[12];
    for (int axis = 0; axis < 4; ++axis) {
        values[axis * 3] = axes[axis].GetX();
        values[axis * 3 + 1] = axes[axis].GetY();
        values[axis * 3 + 2] = axes[axis].GetZ();
    }
    return ragdoll::FiniteMatrix12(values, maxTranslationCm);
}

Vec3 SafeVec3(bCVector const& v, Vec3 fallback = {0.0f, 0.0f, 0.0f})
{
    const Vec3 out{v.GetX(), v.GetY(), v.GetZ()};
    return ragdoll::Finite(out) ? out : fallback;
}

int SafeActorBoneCount(eCVisualAnimation_PS* animation)
{
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!skeleton || !skeleton->mNodes.mData) return 0;
    return ragdoll::ClampedBoneCount(skeleton->mNodes.mLength);
}

bool ActorBonesFinite(eCVisualAnimation_PS* animation)
{
    const int count = SafeActorBoneCount(animation);
    if (count <= 0) return false;
    auto const* nodes = animation->GetActor()->GetInstance()->mNodes.mData;
    for (int i = 0; i < count; ++i) {
        const bCMatrix local = npcplayback::detail::ReadNodeMatrix(nodes, i);
        if (!FiniteMatrix(local, 5000.0f)) return false;
    }
    return true;
}

bool EnsureFiniteRagdollPose(Mirror& m, eCEntity* entity, eCVisualAnimation_PS* animation)
{
    if (!entity || !animation || !animation->GetActor() || !animation->GetActor()->GetInstance()) return false;
    if (!FiniteMatrix(entity->GetWorldMatrix())) {
        if (m.corpsePose.empty() || !FiniteMatrix(m.corpseRoot)) return false;
        npcplayback::SetEntityPose(entity, m.corpseRoot);
    }
    if (ActorBonesFinite(animation)) return true;
    if (!m.corpsePose.empty())
        npcplayback::ReplayFrozenPose(entity, m.replay, m.corpsePose, m.corpsePoseFlags);
    else if (!animation->GetRagDollEnabled())
        npcplayback::UpdatePresentation(entity);
    if (ActorBonesFinite(animation)) return true;
    if (m.ragdollFails < 3)
        Logf("npcs: CLIENTRAGDOLL name=%s action=refuse_start reason=invalid_bones", entity->GetName().GetText());
    return false;
}

void RecordCorpseGroundAnchor(GuidKey const& key, uint32_t incarnation, Vec3 const& origin, uint32_t now, bool overwrite)
{
    if (!ragdoll::Finite(origin)) return;
    if (corpseGroundAnchors.size() > 512) {
        for (auto it = corpseGroundAnchors.begin(); it != corpseGroundAnchors.end();) {
            if (!mirrors.count(it->first) && !corpses.count(it->first)) it = corpseGroundAnchors.erase(it);
            else ++it;
        }
    }
    auto found = corpseGroundAnchors.find(key);
    if (found != corpseGroundAnchors.end() && !overwrite && found->second.incarnation == incarnation &&
        ragdoll::Finite(found->second.origin)) {
        found->second.updatedAt = now;
        return;
    }
    corpseGroundAnchors[key] = {incarnation, origin, now};
}

Vec3 ResolveCorpseGroundAnchor(Mirror const& m)
{
    const auto found = corpseGroundAnchors.find(Key(m.latest.guid));
    if (found != corpseGroundAnchors.end() && found->second.incarnation == m.latest.incarnation &&
        ragdoll::Finite(found->second.origin))
        return found->second.origin;
    if (!m.corpsePose.empty() && FiniteMatrix(m.corpseRoot))
        return SafeVec3(m.corpseRoot.GetTranslation());
    return SafeVec3(m.entity.GetInstance()->GetWorldPosition());
}

void SetEntityAndRigidPose(eCEntity* entity, bCMatrix const& pose)
{
    npcplayback::SetEntityPose(entity, pose);
}

void RequestCorpseRoiUpdate(uint32_t now)
{
    if (!corpseRoiUpdateAt || now - corpseRoiUpdateAt >= 1000) {
        world::RequestProcessingUpdate();
        corpseRoiUpdateAt = now ? now : 1;
    }
}

void TraceCorpse(Mirror& m, uint32_t now)
{
    if (!m.ragdollInitialized || m.ragdollTraceStage >= 3) return;
    if (m.ragdollTraceStage &&
        now - m.ragdollTraceAt < (m.ragdollTraceStage == 1 ? 250u : 1000u)) return;
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    char identity[40];
    guid::Format(m.latest.guid, identity, sizeof(identity));
    const Vec3 root = SafeVec3(entity->GetWorldPosition());
    const Vec3 velocity = SafeVec3(entity->GetLinearVelocity());
    Vec3 center{};
    unsigned bones = 0;
    bCMatrix bone;
    const int boneCount = SafeActorBoneCount(animation);
    for (int i = 0; i < boneCount && animation->GetBoneMatrix(i, bone); ++i) {
        if (!FiniteMatrix(bone)) continue;
        const bCVector& at = bone.GetTranslation();
        center.x += at.GetX();
        center.y += at.GetY();
        center.z += at.GetZ();
        ++bones;
    }
    if (bones) { center.x /= bones; center.y /= bones; center.z /= bones; }
    else center = root;
    if (!ragdoll::Finite(center)) center = root;
    const uint32_t dt = m.ragdollTraceStage ? now - m.ragdollTraceSampleAt : 0;
    const Vec3 previousCenter{m.ragdollTracePosition[0], m.ragdollTracePosition[1], m.ragdollTracePosition[2]};
    const Vec3 boneVelocity = ragdoll::SafeTraceVelocity(center, previousCenter, dt, m.ragdollTraceStage != 0);
    if (!m.ragdollTraceStage) m.ragdollTraceAt = now;
    Logf("npcs: CLIENTRAGDOLL name=%s guid=%s action=sample incarnation=%u stage=%u elapsed_ms=%u source=%s applied=%u physics=%d root=%.1f/%.1f/%.1f velocity=%.1f/%.1f/%.1f bone_center=%.1f/%.1f/%.1f bone_velocity=%.1f/%.1f/%.1f sample_ms=%u",
         m.entity.GetName().GetText(), identity, m.latest.incarnation, m.ragdollTraceStage, now - m.ragdollTraceAt,
         m.deathImpulse.Pending() ? "pending" : RagdollSource(m), m.deathImpulse.applied,
         animation && animation->GetRagDollController() ? 1 : 0, root.x, root.y, root.z,
         velocity.x, velocity.y, velocity.z, center.x, center.y, center.z,
         boneVelocity.x, boneVelocity.y, boneVelocity.z, dt);
    m.ragdollTraceSampleAt = now;
    m.ragdollTracePosition[0] = center.x;
    m.ragdollTracePosition[1] = center.y;
    m.ragdollTracePosition[2] = center.z;
    ++m.ragdollTraceStage;
}
} // namespace

eCEntity* AutoAimTarget(eCVisualAnimation_PS* animation)
{
    if (!animation) return nullptr;
    static const bool supported = [] {
        auto* engine = GetModuleHandleA("Engine.dll");
        auto* code = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
            "?EnableAutoAiming@eCVisualAnimation_PS@@QAEX_NPAVeCEntity@@AAVbCString@@@Z")) : nullptr;
        static unsigned char const expected[] = {
            0x8b, 0x44, 0x24, 0x08, 0x8a, 0x54, 0x24, 0x04, 0x89, 0x81, 0x7c, 0x01, 0x00, 0x00,
            0x8b, 0x44, 0x24, 0x0c, 0x88, 0x91, 0x7b, 0x01, 0x00, 0x00
        };
        if (code && !std::memcmp(code, expected, sizeof(expected))) return true;
        Log("npcs: unsupported native auto-aim target layout; exact-target release unavailable");
        return false;
    }();
    eCEntity* target = nullptr;
    if (supported) std::memcpy(&target, reinterpret_cast<unsigned char const*>(animation) + 0x17c, sizeof(target));
    return target;
}

unsigned ClearLocalReferencesToDeadMirror(eCEntity* dead, unsigned* combatStops, bool first, bool removed)
{
    if (!dead || net::IsHost()) return 0;

    unsigned cleared = 0;
    if (combatStops) *combatStops = 0;
    const bool wasDead = !removed && Entity(dead).IsDead();
    if (!removed) {
        if (auto* routine = static_cast<gCScriptRoutine_PS*>(dead->GetPropertySet(eEPropertySetType_ScriptRoutine)))
            routine->AccessAIMode().AccessValue() = gEAIMode_Dead;
        auto* receiver = static_cast<gCDamageReceiver_PS*>(dead->GetPropertySet(eEPropertySetType_DamageReceiver));
        if (receiver) receiver->AccessHitPoints() = 0;
        if (first) Logf("npcs: CLIENTDEATHSTATE name=%.64s instance=%p native_dead_before=%d native_dead_after=%d hp=%ld",
                        dead->GetName().GetText(), dead, wasDead ? 1 : 0, Entity(dead).IsDead() ? 1 : 0,
                        receiver ? receiver->GetHitPoints() : -1L);
        if (first) containers::PrepareCorpseLoot(dead);
    }
    eCEntity* bodies[2] = {gCSession::GetInstance().GetHero(), remote::ControlledBody()};
    for (int i = 0; i < 2; ++i) {
        eCEntity* body = bodies[i];
        if (!body || (i > 0 && bodies[0] == body)) continue;

        Entity local(body);
        auto* focus = static_cast<gCFocus_PS*>(body->GetPropertySet(eEPropertySetType_Focus));
        auto* animation = static_cast<eCVisualAnimation_PS*>(body->GetPropertySet(eEPropertySetType_Animation));
        const bool focused = focus && local.Focus.GetFocusEntity().GetInstance() == dead;
        const bool npc = body->GetPropertySet(eEPropertySetType_NPC) != nullptr;
        const bool aligned = body->GetPropertySet(eEPropertySetType_CharacterMovement) &&
                             local.GetAlignmentTarget().GetInstance() == dead;
        const bool aimed = AutoAimTarget(animation) == dead;
        if (!first && !focused && !aligned && !aimed &&
            (!npc || (local.NPC.GetCurrentTarget().GetInstance() != dead &&
                      local.NPC.GetAlternativeTarget().GetInstance() != dead &&
                      local.NPC.GetCurrentAttacker().GetInstance() != dead &&
                      local.NPC.GetLastAttacker().GetInstance() != dead &&
                      local.NPC.GetSpellTarget().GetInstance() != dead))) continue;
        const int priority = !removed
            ? GetScriptAdminExt().CallScript(bCString("GetFocusEntityPriority"), dead, nullptr, 0) : -1;
        const bool release = removed || (first && !wasDead) || priority < 0;
        if (first) LogDeathFocus(dead, body, "before", 0, removed);
        if (npc) {
            if (release && local.NPC.GetCurrentTarget().GetInstance() == dead) { local.NPC.SetCurrentTarget(Entity()); ++cleared; }
            if (release && local.NPC.GetAlternativeTarget().GetInstance() == dead) { local.NPC.SetAlternativeTarget(Entity()); ++cleared; }
            if (local.NPC.GetCurrentAttacker().GetInstance() == dead) { local.NPC.SetCurrentAttacker(Entity()); ++cleared; }
            if (local.NPC.GetLastAttacker().GetInstance() == dead) { local.NPC.SetLastAttacker(Entity()); ++cleared; }
            if (release && local.NPC.GetSpellTarget().GetInstance() == dead) { local.NPC.SetSpellTarget(Entity()); ++cleared; }
        }
        if (release && aligned) { local.SetAlignmentTarget(Entity()); ++cleared; }
        if (release && aimed) { local.EnableAutoAiming(GEFalse, Entity(), bCString()); ++cleared; }
        if (focused && (first || release || focus->GetFocusLocked())) {
            focus->SetFocusLocked(GEFalse);
            focus->SetFocusEntity(nullptr);
            focus->FindFocusEntity(GEFalse);
            ++cleared;
        }
        if (first) LogDeathFocus(dead, body, "selected", 0, removed);
    }
    return cleared;
}

void KeepDead(Entity& npc)
{
    if (npc.IsDead()) return;
    npc.NPC.DefeatedByPlayer = GETrue;
    npc.Routine.AIMode = gEAIMode_Dead;
    if (++deadKept < 20) Logf("npcs: %s is dead on the host; dead here again", npc.GetName().GetText());
}

bool Lies(eCEntity* entity, char* shows, size_t size, char* slots, size_t slotsSize)
{
    body::CurrentMotion(entity, shows, size);
    bool still = true;
    *slots = '\0';
    if (auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)))
        for (int slot = 1; slot <= 3; ++slot) {
            char name[128] = {};
            body::MotionName(animation, slot, name, sizeof(name));
            const bool runs = animation->GetActor() && animation->GetActor()->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot));
            still = still && !runs;
            Format(slots + strlen(slots), slotsSize - strlen(slots), "%s%d:%s:%s", slot > 1 ? "|" : "", slot, runs ? "run" : "off", *name ? name : "-");
        }
    return std::strstr(shows, "_Lie") && body::MotionRunning(entity) && still;
}

void LateCheck(Mirror& m, uint32_t now, char const* kind)
{
    if (m.lateNoted || !m.killedAt || static_cast<int32_t>(now - m.killedAt) < 45000) return;
    m.lateNoted = true;
    char shows[128], slots[400];
    const bool lies = Lies(m.entity.GetInstance(), shows, sizeof(shows), slots, sizeof(slots));
    auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    const bool localPhysics = animation && animation->GetRagDollEnabled();
    Logf("npcs: corpse %s (%s) after 45 s: %s (shows %s, slots %s)", m.entity.GetName().GetText(), kind,
         localPhysics ? "local ragdoll active" : lies ? "lies" : "STANDS", shows, slots);
}

bool HostForceQueued(Mirror const& m)
{
    const auto pending = pendingForces.find(Key(m.latest.guid));
    if (pending == pendingForces.end()) return false;
    for (auto const& force : pending->second)
        if (force.incarnation == m.latest.incarnation) return true;
    return false;
}

bool HostForceAvailable(Mirror const& m)
{
    const auto pending = pendingForces.find(Key(m.latest.guid));
    if (pending == pendingForces.end()) return false;
    for (auto const& force : pending->second)
        if (force.incarnation == m.latest.incarnation &&
            (m.killed || static_cast<int32_t>(m.shownSent - force.sent) >= 0))
            return true;
    return false;
}

bool TryApplyForce(Mirror& m, uint32_t now)
{
    if (!net::IsHost() && gCSession::GetInstance().IsPaused()) {
        if (m.deathImpulse.Pending() && m.fallbackGraceUntil)
            m.fallbackGraceUntil = ragdoll::ExtendPausedFallbackGrace(m.fallbackGraceUntil, now);
        return false;
    }
    auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    if (!m.killed || !m.ragdollInitialized || !animation) return false;
    const bool active = animation->GetRagDollEnabled() && animation->GetRagDollController();
    char identity[40];
    guid::Format(m.latest.guid, identity, sizeof(identity));
    if (!active && m.deathImpulse.Pending()) {
        m.deathImpulse.Choose(ragdoll::Source::Gravity);
        ++ragdollGravityDeaths;
        strcpy_s(m.presentationSource, "gravity");
        Logf("npcs: CLIENTRAGDOLL name=%s guid=%s action=force incarnation=%u source=gravity applied=0 magnitude=0 reason=physics_stopped",
             m.entity.GetName().GetText(), identity, m.latest.incarnation);
    }
    if (!active) return false;

    const Vec3 groundOrigin = ResolveCorpseGroundAnchor(m);
    bool applied = false;
    const auto pending = pendingForces.find(Key(m.latest.guid));
    if (pending != pendingForces.end()) {
        for (auto it = pending->second.begin(); it != pending->second.end();) {
            if (it->incarnation < m.latest.incarnation) { it = pending->second.erase(it); ++ragdollForcesLate; continue; }
            if (it->incarnation != m.latest.incarnation) { ++it; continue; }
            if (!m.deathImpulse.Pending()) { it = pending->second.erase(it); ++ragdollForcesLate; continue; }
            const Vec3 raw{it->force[0], it->force[1], it->force[2]};
            const Vec3 rawOffset{it->at[0], it->at[1], it->at[2]};
            Vec3 force{}, offset{}, at{};
            if (!ragdoll::ShapeCorpseImpulse(raw, rawOffset, groundOrigin, force, offset, at)) {
                Logf("npcs: CLIENTRAGDOLL name=%s guid=%s action=refuse_force incarnation=%u sequence=%u reason=nonfinite",
                     m.entity.GetName().GetText(), identity, it->incarnation, it->sequence);
                it = pending->second.erase(it);
                ++ragdollForcesLate;
                continue;
            }
            if (!m.deathImpulse.Choose(ragdoll::Source::Host)) { ++it; continue; }
            if (ragdoll::Magnitude(force) > 0.0)
                animation->AddForceAtPos(bCVector(force.x, force.y, force.z), bCVector(at.x, at.y, at.z));
            Logf("npcs: CLIENTRAGDOLL name=%s guid=%s action=force incarnation=%u sequence=%u source=host_force applied=%u raw_magnitude=%.1f magnitude=%.1f force=%.1f/%.1f/%.1f at_relative=%.1f/%.1f/%.1f raw_force=%.1f/%.1f/%.1f raw_at_relative=%.1f/%.1f/%.1f anchor=%.1f/%.1f/%.1f",
                 m.entity.GetName().GetText(), identity, it->incarnation, it->sequence, m.deathImpulse.applied,
                 ragdoll::Magnitude(raw), ragdoll::Magnitude(force), force.x, force.y, force.z, offset.x, offset.y, offset.z,
                 raw.x, raw.y, raw.z, rawOffset.x, rawOffset.y, rawOffset.z, groundOrigin.x, groundOrigin.y, groundOrigin.z);
            it = pending->second.erase(it);
            ++ragdollForcesApplied;
            strcpy_s(m.presentationSource, "host_force");
            applied = true;
        }
        if (pending->second.empty()) pendingForces.erase(pending);
    }
    if (applied) return true;

    if (!m.deathImpulse.Pending()) return false;
    if (!m.fallbackGraceUntil) m.fallbackGraceUntil = now ? now + 500 : 501;
    if (HostForceQueued(m) || static_cast<int32_t>(now - m.fallbackGraceUntil) < 0) return false;

    if (m.deathImpulse.Choose(ragdoll::Source::Gravity)) {
        ++ragdollGravityDeaths;
        strcpy_s(m.presentationSource, "gravity");
        Logf("npcs: CLIENTRAGDOLL name=%s guid=%s action=force incarnation=%u source=gravity applied=0 magnitude=0",
             m.entity.GetName().GetText(), identity, m.latest.incarnation);
    }
    return false;
}

bool HasPendingCorpseForce(Mirror const& m) { return m.deathImpulse.Pending() && HostForceAvailable(m); }

char const* RagdollSource(Mirror const& m)
{
    return m.deathImpulse.HostApplied() || (m.deathImpulse.Pending() && HostForceQueued(m)) ? "host_force" : "gravity";
}

void CaptureCorpsePose(Mirror& m, uint32_t now, bool final)
{
    if (!final && !m.corpsePose.empty() && now - m.corpsePoseAt < 100) return;
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance()) return;
    if (!FiniteMatrix(entity->GetWorldMatrix()) || !ActorBonesFinite(animation)) return;
    std::vector<pose::Bone> bones;
    uint8_t flags = 0;
    if (!npcplayback::CapturePose(entity, bones, flags, nullptr)) return;
    m.corpsePose = std::move(bones);
    m.corpsePoseFlags = flags;
    m.corpseRoot = entity->GetWorldMatrix();
    m.corpseActor = actor->GetInstance();
    m.corpsePoseAt = now;
}

void RestoreCorpseActor(Mirror& m)
{
    if (!m.ragdollInitialized) return;
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance() || !m.corpseActor || actor->GetInstance() == m.corpseActor) return;
    if (!m.corpseSettled || m.corpsePose.empty() || !FiniteMatrix(m.corpseRoot)) {
        m.corpseActor = actor->GetInstance();
        m.ragdollStarted = m.ragdollInitialized = m.corpseSimulating = false;
        m.lieCheckAt = m.ragdollTryAt = 0;
        return;
    }
    animation->SetRagDollEnabled(GEFalse, 0.0f, GETrue);
    SetEntityAndRigidPose(entity, m.corpseRoot);
    m.corpseActor = actor->GetInstance();
    m.corpseSimulating = false;
}

void UpdateCorpsePresentation(Mirror& m, uint32_t now)
{
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!animation || !actor) return;
    const bool sessionPaused = !net::IsHost() && gCSession::GetInstance().IsPaused();
    if (!net::IsHost() && m.killed && !m.ragdollInitialized) {
        if (m.corpsePose.empty()) CaptureCorpsePose(m, now);
        if (!m.corpsePose.empty())
            npcplayback::ReplayFrozenPose(entity, m.replay, m.corpsePose, m.corpsePoseFlags);
        return;
    }
    const bool controllerActive = animation->GetRagDollEnabled() && animation->GetRagDollController();
    if (ragdoll::ShouldFreezeActiveRagdollPresentation(
            net::IsHost(), sessionPaused, controllerActive, m.ragdollInitialized && !m.corpseSettled)) {
        m.ragdollPaused = true;
        m.corpseSimulating = controllerActive;
        return;
    }
    if (m.ragdollPaused && !sessionPaused) {
        const bool restart = ragdoll::ShouldRestartUnsettledCorpseAfterPause(
            true, false, m.ragdollInitialized, m.corpseSettled, controllerActive);
        m.ragdollPaused = false;
        if (m.ragdollTraceSampleAt) m.ragdollTraceSampleAt = now;
        if (restart && actor->GetInstance() && ActorBonesFinite(animation)) {
            animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);
            ++m.ragdollWakes;
        }
    }
    if (animation->GetRagDollEnabled() && animation->GetRagDollController()) {
        npcplayback::UpdatePresentation(entity);
        const bool active = animation->GetRagDollEnabled() && animation->GetRagDollController();
        CaptureCorpsePose(m, now, !active);
        m.corpseSimulating = active;
        if (!active) m.corpseSettled = true;
    } else if (m.ragdollInitialized) {
        m.corpseSimulating = false;
        m.corpseSettled = true;
        const bool restored = !m.corpsePose.empty() &&
            npcplayback::ReplayFrozenPose(entity, m.replay, m.corpsePose, m.corpsePoseFlags);
        if (!restored && !(m.lieNoted & 16)) {
            m.lieNoted |= 16;
            Logf("npcs: settled corpse %s awaits its cached render pose; death is not restarted", entity->GetName().GetText());
        }
    } else {
        npcplayback::UpdatePresentation(entity);
    }
    TraceCorpse(m, now);
}

void PresentDeadMirror(GuidKey const& key, Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    deadOnHost.insert(key);
    auto* entity = m.entity.GetInstance();
    if (!m.corpsePlaced) {
        if (m.ragdollInitialized && m.corpseSettled && !m.corpsePose.empty() && FiniteMatrix(m.corpseRoot)) {
            SetEntityAndRigidPose(entity, m.corpseRoot);
            RecordCorpseGroundAnchor(key, shown.incarnation, SafeVec3(m.corpseRoot.GetTranslation()), now, false);
            m.corpsePlaced = true;
        } else {
            const bCVector& cur = entity->GetWorldPosition();
            ragdoll::CorpseOriginInput originInput{};
            originInput.entityPos = {cur.GetX(), cur.GetY(), cur.GetZ()};
            const bool haveDurableGround = m.haveLastGroundPos && m.lastGroundIncarnation == shown.incarnation;
            if (haveDurableGround) {
                originInput.previousPos = {m.lastGroundPos[0], m.lastGroundPos[1], m.lastGroundPos[2]};
                originInput.havePreviousAlive = true;
            } else {
                originInput.previousPos = {m.previous.position[0], m.previous.position[1], m.previous.position[2]};
                originInput.havePreviousAlive = m.previousTicks != 0 &&
                    ragdoll::IsGroundOriginSample(m.previous.flags, m.previous.movementMode, originInput.previousPos);
            }
            originInput.shownPos = {shown.position[0], shown.position[1], shown.position[2]};
            originInput.livePresented = (m.replay.initialized || m.replayReady) && FiniteMatrix(entity->GetWorldMatrix());
            originInput.hostRagdoll = (shown.flags & 128) != 0 ||
                shown.movementMode == static_cast<uint8_t>(gECharMovementMode_RagDollDead) ||
                (m.latest.flags & 128) != 0 ||
                m.latest.movementMode == static_cast<uint8_t>(gECharMovementMode_RagDollDead);
            ragdoll::CorpseOriginResult chosen{};
            if (ragdoll::SelectCorpseOrigin(originInput, chosen)) {
                if (!chosen.keepLiveMatrix) {
                    const float pos[3] = {chosen.origin.x, chosen.origin.y, chosen.origin.z};
                    bCMatrix pose = body::Pose(*entity, pos, shown.rotation);
                    if (!FiniteMatrix(pose)) {
                        pose = entity->GetWorldMatrix();
                        pose.AccessTranslation() = bCVector(chosen.origin.x, chosen.origin.y, chosen.origin.z);
                    }
                    if (FiniteMatrix(pose)) SetEntityAndRigidPose(entity, pose);
                }
                if (FiniteMatrix(entity->GetWorldMatrix())) {
                    RecordCorpseGroundAnchor(key, shown.incarnation, chosen.origin, now, true);
                    m.corpsePlaced = true;
                }
            }
        }
    }
    LayDown(m, now);
    TryApplyForce(m, now);
    UpdateCorpsePresentation(m, now);
}

void OnRagdollNote(net::RagdollNote const& note)
{
    if (npcremoval::Suppressed(note.guid)) return;
    const GuidKey key = Key(note.guid);
    const auto absent = absences.find(key);
    if (absent != absences.end() && static_cast<int32_t>(note.incarnation - absent->second.incarnation) <= 0) {
        ++ragdollForcesLate;
        return;
    }
    const auto current = mirrors.find(key);
    if (current != mirrors.end() && static_cast<int32_t>(note.incarnation - current->second.latest.incarnation) < 0) {
        ++ragdollForcesLate;
        return;
    }
    if (current != mirrors.end() && note.incarnation == current->second.latest.incarnation &&
        !current->second.deathImpulse.Pending()) {
        ++ragdollForcesLate;
        return;
    }
    auto& queue = pendingForces[key];
    if (queue.size() >= 64) { Log("npcs: ragdoll force queue full; publication refused"); return; }
    if (!forceSequences[ForceSeqKey{key, note.incarnation}].Accept(note.sequence)) return;
    queue.push_back(note);
    ++ragdollForcesReceived;
    const auto it = mirrors.find(key);
    if (it == mirrors.end()) return;
    Mirror& m = it->second;
    m.fallbackGraceUntil = 0;
    m.lieCheckAt = 0;
    if (!m.entity.GetInstance()) return;
    TryApplyForce(m, net::Now());
}

void LayDown(Mirror& m, uint32_t now)
{
    RestoreCorpseActor(m);
    if (!deadPose || static_cast<int32_t>(now - m.lieCheckAt) < 0) return;
    if (m.ragdollStarted || (!net::IsHost() && m.killed) ||
        (m.latest.flags & 128) || m.latest.movementMode == gECharMovementMode_RagDollDead) {
        auto* entity = m.entity.GetInstance();
        auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
        if (m.ragdollInitialized) {
            TryApplyForce(m, now);
            m.lieCheckAt = now + (HasPendingCorpseForce(m) ? 100 : 5000);
            return;
        }
        const bool sessionPaused = !net::IsHost() && gCSession::GetInstance().IsPaused();
        if (ragdoll::ShouldDeferCorpseRagdoll(net::IsHost(), sessionPaused, m.ragdollInitialized)) {
            if (!npcplayback::IsPassive(entity)) body::Pacify(m.entity, m.body);
            if (m.corpsePose.empty()) CaptureCorpsePose(m, now);
            if (FiniteMatrix(entity->GetWorldMatrix()))
                RecordCorpseGroundAnchor(Key(m.latest.guid), m.latest.incarnation,
                                         SafeVec3(entity->GetWorldPosition()), now, false);
            if (auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)))
                routine->AccessAIMode().AccessValue() = gEAIMode_Dead;
            m.entity.CharacterMovement.StopMovement();
            if (m.deathImpulse.Pending() && m.fallbackGraceUntil)
                m.fallbackGraceUntil = ragdoll::ExtendPausedFallbackGrace(m.fallbackGraceUntil, now);
            m.ragdollPaused = true;
            m.ragdollTryAt = 0;
            m.lieCheckAt = 0;
            strcpy_s(m.presentationSource, "paused_defer");
            return;
        }
        auto completeRagdollStart = [&]() {
            if (!m.ragdollInitialized) {
                m.ragdollInitialized = true;
                m.ragdollPaused = false;
                m.corpseSettled = false;
                m.corpsePoseAt = 0;
                ++m.ragdollStarts;
                ++ragdollsStarted;
                strcpy_s(m.presentationSource, RagdollSource(m));
                Logf("npcs: CLIENTRAGDOLL name=%s action=start source=%s host_force_queued=%d host_force_available=%d fallback=%s",
                     m.entity.GetName().GetText(), m.presentationSource, HostForceQueued(m), HostForceAvailable(m), "gravity");
            }
            TryApplyForce(m, now);
            m.lieCheckAt = now + (HasPendingCorpseForce(m) ? 100 : 5000);
        };
        if (!m.ragdollStarted) {
            if (!npcplayback::IsPassive(entity)) body::Pacify(m.entity, m.body);
            CaptureCorpsePose(m, now);
            if (FiniteMatrix(entity->GetWorldMatrix()))
                RecordCorpseGroundAnchor(Key(m.latest.guid), m.latest.incarnation, SafeVec3(entity->GetWorldPosition()), now, false);
            auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
            if (!routine) {
                m.ragdollTryAt = now + 1500;
                m.lieCheckAt = now + 1500;
                strcpy_s(m.presentationSource, "not_ready");
                world::RequestProcessingUpdate();
                return;
            }
            routine->AccessAIMode().AccessValue() = gEAIMode_Dead;
            m.entity.CharacterMovement.StopMovement();
            const bool haveActor = animation && animation->GetActor() && animation->GetActor()->GetInstance();
            if (haveActor && !EnsureFiniteRagdollPose(m, entity, animation)) {
                ++m.ragdollFails;
                ++ragdollsNotReady;
                m.ragdollTryAt = now + (m.ragdollFails < 4 ? 250 : 1500);
                m.lieCheckAt = m.ragdollTryAt;
                strcpy_s(m.presentationSource, m.ragdollFails <= 1 ? "not_ready" : "retry");
                return;
            }
            if (haveActor) m.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead);
            m.ragdollStarted = true;
            RequestCorpseRoiUpdate(now);
        }
        if (animation && animation->GetRagDollEnabled() && animation->GetRagDollController()) {
            completeRagdollStart();
            return;
        }
        if (animation && animation->GetActor() && animation->GetActor()->GetInstance() &&
            static_cast<int32_t>(now - m.ragdollTryAt) >= 0) {
            if (!EnsureFiniteRagdollPose(m, entity, animation)) {
                ++m.ragdollFails;
                ++ragdollsNotReady;
                m.ragdollTryAt = now + (m.ragdollFails < 4 ? 250 : 1500);
                m.lieCheckAt = m.ragdollTryAt;
                strcpy_s(m.presentationSource, m.ragdollFails <= 1 ? "not_ready" : "retry");
                return;
            }
            using Owner = eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner;
            m.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead);
            for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
                const auto type = static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot);
                animation->GetActor()->StopMotion(type, 0.0f);
                animation->GetActor()->SetTargetWeight(type, 0.0f);
                animation->SetMotionOwner(type, static_cast<Owner>(2));
            }
            if (!animation->GetRagDollEnabled() || !animation->GetRagDollController())
                animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);
            if (animation->GetRagDollEnabled() && animation->GetRagDollController()) {
                completeRagdollStart();
                return;
            }
            if (!m.corpsePose.empty())
                npcplayback::ReplayFrozenPose(entity, m.replay, m.corpsePose, m.corpsePoseFlags);
            ++m.ragdollFails;
            ++ragdollsNotReady;
            m.ragdollTryAt = now + (m.ragdollFails < 4 ? 1500 : 5000);
            m.lieCheckAt = m.ragdollTryAt;
            strcpy_s(m.presentationSource, m.ragdollFails <= 1 ? "not_ready" : "retry");
            if (m.ragdollFails <= 3 || m.ragdollFails % 10 == 0) RequestCorpseRoiUpdate(now);
            if (m.ragdollFails <= 2 || m.ragdollFails == 10)
                Logf("npcs: corpse %s ragdoll physics not ready (try %u); preserving death pose and retrying", m.entity.GetName().GetText(), m.ragdollFails);
        }
        if (!m.ragdollInitialized) {
            if (!m.corpsePose.empty())
                npcplayback::ReplayFrozenPose(m.entity.GetInstance(), m.replay, m.corpsePose, m.corpsePoseFlags);
            RequestCorpseRoiUpdate(now);
            m.ragdollTryAt = now + 1500;
            m.lieCheckAt = m.ragdollTryAt;
            strcpy_s(m.presentationSource, "retry");
            return;
        }
    }
    if (m.killed && !net::IsHost()) return;
    if (m.lieTries >= 40 && !(m.lieNoted & 4)) {
        m.lieNoted |= 4;
        char shows[128];
        body::CurrentMotion(m.entity.GetInstance(), shows, sizeof(shows));
        Logf("npcs: corpse %s: lying motion not running after 40 tries; retrying slowly (shows %s)", m.entity.GetName().GetText(), shows);
    }
    m.lieCheckAt = now + (m.lieTries >= 40 ? 5000 : 400);
    char shows[128];
    body::CurrentMotion(m.entity.GetInstance(), shows, sizeof(shows));
    LearnSkeleton(m, shows);
    char const* knownLoop = std::strstr(shows, "_LieDead_") && std::strstr(shows, "_Ambient_Loop_") ? shows : nullptr;
    auto ownerMotion = motionNames.find(m.latest.motion);
    if (ownerMotion != motionNames.end() && std::strstr(ownerMotion->second.c_str(), "_LieDead_") &&
        std::strstr(ownerMotion->second.c_str(), "_Ambient_Loop_"))
        knownLoop = ownerMotion->second.c_str();
    if (!*m.skeleton) {
        auto name = motionNames.find(m.latest.motion);
        if (name != motionNames.end()) LearnSkeleton(m, name->second.c_str());
    }
    if (!*m.skeleton) {
        if (!(m.lieNoted & 1)) {
            m.lieNoted |= 1;
            Logf("npcs: corpse %s: no skeleton known from its motions (shows %s); it cannot be laid down yet", m.entity.GetName().GetText(), shows);
        }
        return;
    }
    m.entity.CharacterMovement.StopMovement();
    if (auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation)))
        for (int slot = 1; slot <= 3; ++slot)
            if (animation->GetActor() && animation->GetActor()->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot)))
                animation->StopMotion(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot), 0.2f), ++slotsStopped;
    if (std::strstr(shows, "LieDead") && body::MotionRunning(m.entity.GetInstance())) {
        m.lieTries = 0;
        if (!(m.lieNoted & 2)) {
            m.lieNoted |= 2;
            Logf("npcs: corpse %s lies (%s)", m.entity.GetName().GetText(), shows);
        }
        return;
    }
    if (knownLoop) {
        if (!m.laid) { m.laid = true; ++corpsesLaid; }
        m.entity.Routine.AniState = gEAniState_LieDead;
        ++corpseLoops;
        ++m.lieTries;
        if (!(m.lieNoted & 8)) {
            m.lieNoted |= 8;
            Logf("npcs: corpse %s uses observed death loop %s", m.entity.GetName().GetText(), knownLoop);
        }
        if (!body::PlayAniInstruction(m.entity.GetInstance(), m.body, knownLoop))
            m.entity.StartPlayAni(bCString(knownLoop), 0, GEFalse, 1.0f, GEFalse);
        return;
    }
    char motion[128];
    if (!m.laid) {
        Format(motion, "%s_Stand_None_None_P0_LieDead_Begin_N_Back_00_%%_00_P0_200", m.skeleton);
        m.laid = true;
        m.lieCheckAt = now + 2000;
        ++corpsesLaid;
    } else {
        Format(motion, "%s_LieDead_None_None_P0_Ambient_Loop_N_Back_00_%%_00_P0_0", m.skeleton);
        m.entity.Routine.AniState = gEAniState_LieDead;
        ++corpseLoops;
        ++m.lieTries;
        if (!body::PlayAniInstruction(m.entity.GetInstance(), m.body, motion))
            m.entity.StartPlayAni(bCString(motion), 0, GEFalse, 1.0f, GEFalse);
        return;
    }
    ++m.lieTries;
    m.entity.StartPlayAni(bCString(motion), 0, GEFalse, 1.0f, GEFalse);
}

void LogClientDeathImpl(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    if (m.deathLogged || net::IsHost()) return;
    m.deathLogged = true;
    targetingWatch.haveKill = true;
    targetingWatch.killedAt = now;
    std::memcpy(targetingWatch.victim, shown.guid, sizeof(targetingWatch.victim));

    if (m.localHitValid && now - m.localHitAt <= 5000)
        std::strcpy(m.killerAttribution, "local_player");
    else if (shown.hitPlayer)
        std::snprintf(m.killerAttribution, sizeof(m.killerAttribution), "reported_player_%u", shown.hitPlayer);

    if (m.ragdollFails) std::strcpy(m.presentationSource, "not_ready");
    else std::strcpy(m.presentationSource, RagdollSource(m));

    eCEntity* hero = remote::ControlledBody();
    if (!hero) hero = gCSession::GetInstance().GetHero();
    bCString task("-");
    char const* focus = "-", *target = "-", *alignment = "-";
    if (hero) {
        Entity h(hero);
        if (hero->GetPropertySet(eEPropertySetType_ScriptRoutine)) task = h.Routine.GetCurrentTask();
        if (hero->GetPropertySet(eEPropertySetType_Focus)) {
            if (eCEntity* f = h.Focus.GetFocusEntity().GetInstance()) focus = f->GetName().GetText();
        }
        if (hero->GetPropertySet(eEPropertySetType_NPC)) {
            if (eCEntity* t = h.NPC.GetCurrentTarget().GetInstance()) target = t->GetName().GetText();
        }
        if (hero->GetPropertySet(eEPropertySetType_CharacterMovement)) {
            if (eCEntity* a = h.GetAlignmentTarget().GetInstance()) alignment = a->GetName().GetText();
        }
    }
    Logf("npcs: CLIENTDEATH name=%s killer=%s dead=%d ragdoll_host=%d host_force_queued=%d host_force_available=%d source=%s refs_cleared=%u combat_stops=%u hero_task=%s focus=%s target=%s alignment=%s",
         m.entity.GetName().GetText(), m.killerAttribution, m.entity.IsDead() ? 1 : 0,
         (shown.flags & 128) != 0, HostForceQueued(m), HostForceAvailable(m), m.presentationSource,
         m.deathClearCount, m.deathCombatStopCount, task.GetText(), focus, target, alignment);
}

void CorpseTick(uint32_t now)
{
    for (auto it = corpses.begin(); it != corpses.end();) {
        Mirror& c = it->second;
        eCEntity* previous = c.entity.GetInstance();
        c.entity = Resolve(c.latest.guid);
        if (previous && c.entity.GetInstance() != previous)
            ClearLocalReferencesToDeadMirror(previous, nullptr, false, true);
        if (!c.entity.GetInstance()) { ++it; continue; }
        if (mirrors.count(it->first)) { it = corpses.erase(it); continue; }
        if (!net::IsHost() && net::Connected()) c.hostSession = net::WelcomeSerial();
        const bool heldDead = deadStays && !net::IsHost() && deadOnHost.count(it->first);
        if (heldDead && !c.entity.IsDead()) c.entity.NPC.DefeatedByPlayer = GETrue;
        if (!net::IsHost() && (heldDead || c.entity.IsDead())) {
            const bool first = c.deathReferencesCleared != c.entity.GetInstance();
            c.deathClearCount += ClearLocalReferencesToDeadMirror(c.entity.GetInstance(), &c.deathCombatStopCount, first);
            c.deathReferencesCleared = c.entity.GetInstance();
            if (first) {
                c.focusWatchUntil = now ? now + 5000 : 5001;
                c.focusWatchLogged = false;
            }
            TickDeathFocusWatch(c, now);
        }
        if (!c.entity.IsDead()) { it = corpses.erase(it); continue; }
        if (!net::IsHost() && c.wasMirror) {
            c.killed = true;
            PresentDeadMirror(it->first, c, c.latest, now);
            LateCheck(c, now, "mirror");
            ++it;
            continue;
        }
        LayDown(c, now);
        if (npcplayback::IsPassive(c.entity.GetInstance())) UpdateCorpsePresentation(c, now);
        LateCheck(c, now, c.wasMirror ? "mirror" : "journal");
        ++it;
    }
}

void ForgetCorpseGroundAnchor(GuidKey const& key)
{
    corpseGroundAnchors.erase(key);
}

void ClearCorpseGroundAnchors()
{
    corpseGroundAnchors.clear();
}

} // namespace g3t::npcs::detail

namespace g3t::npcs {
using namespace detail;

void ReleaseRemovedAim(eCEntity* entity)
{
    if (!entity) return;
    eCEntity* bodies[] = {gCSession::GetInstance().GetHero(), remote::ControlledBody()};
    for (auto* body : bodies) {
        if (!body) continue;
        auto* animation = static_cast<eCVisualAnimation_PS*>(body->GetPropertySet(eEPropertySetType_Animation));
        if (AutoAimTarget(animation) == entity)
            Entity(body).EnableAutoAiming(GEFalse, Entity(), bCString());
    }
}

void LayDownCorpse(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    if (auto existing = corpses.find(key); existing != corpses.end()) {
        existing->second.hostSession = net::WelcomeSerial();
        return;
    }
    Mirror& c = corpses[key];
    c.hostSession = net::WelcomeSerial();
    c.entity = Entity(entity);
    std::memcpy(c.latest.guid, guid, 16);
    c.latest.flags = 1;
    c.killed = true;
    c.killedAt = net::Now() ? net::Now() : 1;
    if (!net::IsHost()) {
        containers::PrepareCorpseLoot(entity);
        c.deathClearCount = ClearLocalReferencesToDeadMirror(entity, &c.deathCombatStopCount);
        c.deathReferencesCleared = entity;
        c.focusWatchUntil = c.killedAt + 5000;
        c.focusWatchLogged = false;
        LogClientDeathImpl(c, c.latest, c.killedAt);
    }
}

} // namespace g3t::npcs
