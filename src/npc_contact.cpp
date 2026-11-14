// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_contact.h"
#include "npc_corpse.h"

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

std::string StumbleMotion(char const* skeleton, char const* stance)
{
    const std::string key = std::string(skeleton) + "_" + stance;
    if (auto it = stumbleFor.find(key); it != stumbleFor.end()) return it->second;
    std::string any, same;
    const std::string prefix = std::string(skeleton) + "_Stand_";
    for (auto const& [hash, name] : motionNames) {
        if (name.compare(0, prefix.size(), prefix) || name.find("Stumble") == std::string::npos || name.find("_Hit_") == std::string::npos) continue;
        if (name.find("Parade") != std::string::npos) continue;
        if (any.empty()) any = name;
        if (*stance && name.find(std::string("_") + stance + "_") != std::string::npos) { same = name; break; }
    }
    std::string chosen = same.empty() ? any : same;
    if (chosen.empty() && fallbackStumbles) {
        if (!std::strcmp(skeleton, "Hero") && (!*stance || !std::strcmp(stance, "None")))
            chosen = "Hero_Stand_None_None_P0_StumbleR_Hit_N_Back_00_%_00_P0_90";
        else if (!std::strcmp(skeleton, "Hero"))
            chosen = std::string("Hero_Stand_None_") + stance + "_P0_QuickStumble_Hit_N_Fwd_00_%_00_P0_90";
        else
            chosen = std::string(skeleton) + "_Stand_None_Fist_P0_StumbleR_Hit_N_Back_00_%_00_P0_200";
        return chosen;
    }
    if (const size_t dot = chosen.find(".xmot"); dot != std::string::npos) chosen.resize(dot);
    if (!chosen.empty()) stumbleFor[key] = chosen;
    return chosen;
}

void LocalHitWithSwing(eCEntity* entity, uint32_t sequence, uint32_t swing)
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
        std::string motion = StumbleMotion(m.skeleton, stance);
        if (!motion.empty() && motion.find(".xmot") == std::string::npos) motion += ".xmot";
        if (motion.empty() || !npcplayback::LocalHit(entity, m.replay, motion.c_str(), m.latest.motions[0], sequence, net::Now())) {
            Logf("npcs: immediate hit presentation could not start for %s", m.entity.GetName().GetText());
            return;
        }
        ++localHitsShown;
        return;
    }
}
} // namespace

bool ClearContact(eCEntity* attacker, eCEntity* hero)
{
    bCVector start = attacker->GetWorldPosition(), end = hero->GetWorldPosition();
    auto* a = static_cast<gCCharacterMovement_PS*>(attacker->GetPropertySet(eEPropertySetType_CharacterMovement));
    auto* h = static_cast<gCCharacterMovement_PS*>(hero->GetPropertySet(eEPropertySetType_CharacterMovement));
    if (!a || !h) return false;
    start.AccessY() += a->GetCharacterHeight() * 0.5f;
    end.AccessY() += h->GetCharacterHeight() * 0.5f;
    auto& physics = eCPhysicsScene::GetInstance();
    const GEU32 hints = eETraceRayHints_TestStaticShapes | eETraceRayHints_TestDynamicShapes | eETraceRayHints_Distance |
                        eETraceRayHints_IncludeCollisionGroup17;
    if (!physics.TraceRayAllHits(start, end - start, attacker, hints)) return true;
    auto const& report = physics.GetQueryReport();
    const float length = (end - start).GetMagnitude();
    for (int i = 0; i < report.m_arrHitEntity.GetCount(); ++i) {
        eCEntity* hit = report.m_arrHitEntity[i];
        if (i < report.m_arrDistance.GetCount() && report.m_arrDistance[i] > length + 1.0f) continue;
        if (!hit || (!AttachedTo(hit, attacker) && !AttachedTo(hit, hero))) {
            static unsigned noted = 0;
            if (noted++ < 12)
                Logf("npcs: displayed contact %s -> %s obstructed by %s (ray %.0f cm)",
                     attacker->GetName().GetText(), hero->GetName().GetText(), hit ? hit->GetName().GetText() : "(static geometry)", length);
            return false;
        }
    }
    return true;
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

bool SweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                float thickness, bool weapon)
{
    bCBox const& box = target->GetWorldNodeBoundary();
    const bCVector& low = box.GetMin();
    const bCVector& high = box.GetMax();
    const float radius = std::min(high.GetX() - low.GetX(), high.GetZ() - low.GetZ()) * 0.5f;
    if (!(radius > 0.0f) || !std::isfinite(radius)) return false;
    float first[3] = {(low.GetX() + high.GetX()) * 0.5f, low.GetY() + radius, (low.GetZ() + high.GetZ()) * 0.5f};
    float last[3] = {first[0], std::max(first[1], high.GetY() - radius), first[2]};
    const float limit = (radius + thickness) * (radius + thickness);
    return contact::SegmentDistanceSquared(oldA, a, first, last) <= limit ||
           contact::SegmentDistanceSquared(oldB, b, first, last) <= limit ||
           (weapon && contact::SegmentDistanceSquared(a, b, first, last) <= limit);
}

void SamplePlayerContact(uint32_t now)
{
    eCEntity* hero = remote::ControlledBody();
    if (!hero || !net::Connected() || !body::LocallyAttacking(hero)) { playerContact = PlayerContact{}; return; }
    ++playerSwingFrames;
    const uint32_t instance = npcplayback::MotionInstance(hero);
    Entity player(hero);
    float a[3], b[3], thickness = 0;
    int bone = -1;
    if (!instance || !PresentedContactPoints(player, HeldHash(player, gESlot_RightHand), a, b, thickness, bone)) {
        playerContact = PlayerContact{};
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
    const bool previous = playerContact.at && now - playerContact.at <= 100 &&
                          playerContact.instance == instance && playerContact.bone == bone;
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
    if (result.incarnation != m.latest.incarnation ||
        static_cast<uint16_t>(result.sequence) != m.replay.pendingHit) return;
    if (!result.accepted) {
        m.replay.pendingHit = 0;
        m.replay.localMotion[0] = '\0';
        m.replay.initialized = false;
        Logf("npcs: host refused hit %u on %s; authoritative state retained", result.sequence, m.entity.GetName().GetText());
    } else {
        m.replay.acknowledgedHit = static_cast<uint16_t>(result.sequence);
        m.replay.acknowledgedAt = result.processedAt;
    }
}

} // namespace g3t::npcs::detail

namespace g3t::npcs {
using namespace detail;

void LocalHit(eCEntity* entity) { LocalHit(entity, 0); }

void LocalHit(eCEntity* entity, uint32_t sequence) { LocalHitWithSwing(entity, sequence, 0); }

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
    const bool keepCorpse = m.killed && m.ragdollInitialized && !m.corpsePose.empty();
    if (m.resolved)
        Logf("npcs: mirror %s unloaded or changed property sets; waiting for its live instance",
             live.GetInstance() ? live.GetName().GetText() : "(unloaded)");
    if (previous) npcplayback::ReleaseUnavailable(previous);
    if (previous && live.GetInstance() != previous)
        ClearLocalReferencesToDeadMirror(previous, nullptr, false, true);
    m.entity = live;
    if (live.GetInstance() != previous) m.deathReferencesCleared = nullptr;
    m.resolved = m.replayReady = m.corpsePlaced = m.corpseSimulating = false;
    m.resolvedAt = 0;
    m.replay = npcplayback::State{};
    m.ragdollStarted = m.ragdollInitialized = keepCorpse;
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
    m.ragdollStarted = m.laid = m.ragdollInitialized = m.corpsePlaced = m.corpseSimulating = false;
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
