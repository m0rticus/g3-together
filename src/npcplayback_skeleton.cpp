// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcplayback_skeleton.h"
#include "npcplayback_hooks.h"

#include "combat_timing.h"
#include "log.h"
#include "net.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace g3t::npcplayback::detail {
namespace {

static_assert(sizeof(bCMatrix) == 64, "bCMatrix layout must be four contiguous bCVector4 rows");

bool ContainsInsensitive(char const* haystack, char const* needle) noexcept
{
    if (!haystack || !needle || !*needle) return false;
    for (char const* p = haystack; *p; ++p) {
        char const* a = p;
        char const* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == *b) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

struct SourceSkeleton {
    void const* instance = nullptr;
    void const* nodes = nullptr;
    std::string actorName;
    std::vector<std::string> names;
    std::vector<uint32_t> hashes;
    uint64_t publishedEpoch = 0;
    PublishName publishedFn = nullptr;
    uint32_t seen = 0;
    bool valid = false;
};

std::unordered_map<eCWrapper_emfx2Actor*, SourceSkeleton> sourceSkeletons;
uint64_t publishedNameEpoch = 1;

} // namespace

eCVisualAnimation_PS* Animation(eCEntity* entity)
{
    return entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
}

pose::Transform TransformOf(bCMatrix const& matrix, uint32_t name)
{
    pose::Transform transform;
    transform.name = name;
    float raw[16];
    std::memcpy(raw, &matrix, sizeof(raw));
    transform.values[0] = raw[0]; transform.values[1] = raw[1]; transform.values[2] = raw[2];
    transform.values[3] = raw[4]; transform.values[4] = raw[5]; transform.values[5] = raw[6];
    transform.values[6] = raw[8]; transform.values[7] = raw[9]; transform.values[8] = raw[10];
    transform.values[9] = raw[12]; transform.values[10] = raw[13]; transform.values[11] = raw[14];
    return transform;
}

bCMatrix MatrixOf(pose::Transform const& transform)
{
    bCMatrix matrix;
    const float raw[16] = {
        transform.values[0], transform.values[1], transform.values[2], 0.0f,
        transform.values[3], transform.values[4], transform.values[5], 0.0f,
        transform.values[6], transform.values[7], transform.values[8], 0.0f,
        transform.values[9], transform.values[10], transform.values[11], 1.0f
    };
    std::memcpy(&matrix, raw, sizeof(raw));
    return matrix;
}

bool SetFacialRig(eCVisualAnimation_PS& animation, bool facial)
{
    auto& factory = animation.GetAnimationFactory();
    if (factory.IsFacialAnimationStarted() == facial) return true;
    if (!facial) return animation.StopFacialMotion() != GEFalse;
    if (!factory.StartFacialAnimation()) return false;
    // The native facial-motion wrapper relinks these cached node references after assembling facial bones.
    animation.RelinkAttachments();
    animation.RelinkLookAtConstraints();
    return true;
}

float CurrentWeight(eCWrapper_emfx2Actor const& actor, int slot)
{
    unsigned char const* instance = nullptr;
    std::memcpy(&instance, reinterpret_cast<unsigned char const*>(&actor) + 0x10 + slot * sizeof(void*), sizeof(instance));
    float weight = 0.0f;
    // SetTargetWeight sets both fields; +0x24 is the current blend and +0x28 is its destination.
    if (instance) std::memcpy(&weight, instance + 0x24, sizeof(weight));
    return weight;
}

bool ApplyPose(eCWrapper_emfx2Actor& actor, NativePose& pose)
{
    auto* skeleton = actor.GetInstance();
    if (!skeleton || skeleton != pose.instance || skeleton->mNodes.mData != pose.nodes ||
        skeleton->mNodes.mLength != pose.count) return pose.valid = false;
    for (auto const& bone : pose.bones) {
        if (bone.index < 0 || bone.index >= skeleton->mNodes.mLength) return pose.valid = false;
        WriteNodeMatrix(skeleton->mNodes.mData, bone.index, bone.matrix);
    }
    pose.applied = true;
    return true;
}

bool FullPose(NativePose const& pose)
{
    if (pose.count <= 0 || pose.bones.size() != static_cast<size_t>(pose.count)) return false;
    if (pose.count <= pose::MaxBones) {
        bool seen[pose::MaxBones] = {};
        for (auto const& bone : pose.bones) {
            if (bone.index < 0 || bone.index >= pose.count || seen[bone.index]) return false;
            seen[bone.index] = true;
        }
        return true;
    }
    std::vector<bool> seen(static_cast<size_t>(pose.count), false);
    for (auto const& bone : pose.bones) {
        if (bone.index < 0 || bone.index >= pose.count || seen[bone.index]) return false;
        seen[bone.index] = true;
    }
    return true;
}

npclease::PresentationRig Rig(eCVisualAnimation_PS& animation)
{
    auto* actor = animation.GetActor();
    auto* instance = actor ? actor->GetInstance() : nullptr;
    return {actor, instance, instance ? instance->mNodes.mData : nullptr,
            instance ? instance->mNodes.mLength : 0,
            animation.GetAnimationFactory().IsFacialAnimationStarted() != GEFalse};
}

bool CaptureFrame(eCEntity* entity, Frame& frame)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    frame.facial = animation->GetAnimationFactory().IsFacialAnimationStarted() != GEFalse;
    const bCVector& at = entity->GetWorldPosition();
    frame.position[0] = at.GetX(), frame.position[1] = at.GetY(), frame.position[2] = at.GetZ();
    const bCQuaternion rotation = entity->GetWorldRotation();
    frame.rotation[0] = rotation.GetX(), frame.rotation[1] = rotation.GetY();
    frame.rotation[2] = rotation.GetZ(), frame.rotation[3] = rotation.GetW();
    if (auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)))
        frame.aniState = routine->AccessAniState().GetValue();
    for (int i = 0; i < Slots; ++i) {
        Motion& motion = frame.motions[i];
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        motion.active = animation->HasMotion(slot) && actor->HasMotionInstance(slot);
        if (!motion.active) continue;
        motion.weight = CurrentWeight(*actor, i);
        if (!std::isfinite(motion.weight) || motion.weight < 0.0f || motion.weight > 1.0f) {
            Log("npcplayback: invalid native current blend weight");
            return false;
        }
        motion.active = motion.weight > 0.0001f || actor->GetTargetWeight(slot) > 0.0001f;
        if (!motion.active) continue;
        // The SDK descriptor has no declared storage. Only use the engine-owned instance.
        auto& description = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation->GetMotionDesc(slot));
        motion.resource = description.GetMotion();
        if (!motion.resource) return false;
        if (std::strlen(description.GetMotionFilename().GetText()) >= sizeof(motion.name)) return false;
        strcpy_s(motion.name, description.GetMotionFilename().GetText());
        CapturedOptions options;
        const bool haveCaptured = LookupCapturedOptions(actor, i, options);
        if (!haveCaptured || !options.available || options.motion != &animation->GetMotion(slot)) {
            static unsigned missingOptions = 0;
            if (missingOptions++ < 20)
                Logf("npcplayback: %s slot %d lacks matching actor options for %s (captured=%d)",
                     entity->GetName().GetText(), i, motion.name, options.available ? 1 : 0);
            return false;
        }
        motion.options = options.words;
        motion.defaults = options.defaults;
        motion.instance = options.instance;
        motion.started = options.started;
        const float maximum = static_cast<float>(actor->GetMaxTime(slot));
        if (!(maximum > 0.0f) || !std::isfinite(maximum)) return false;
        motion.duration = maximum;
        motion.time = std::min(static_cast<float>(actor->GetPlayTime(slot)), std::nextafter(maximum, 0.0f));
        motion.speed = actor->GetPlaySpeed(slot);
        motion.intro = actor->GetIntroBlending(slot);
        motion.extro = actor->GetExtroBlending(slot);
        motion.blend = actor->GetBlendMode(slot);
    }
    return true;
}

bool PrepareTransforms(eCEntity* entity, eCVisualAnimation_PS& animation, State& state,
                       std::vector<pose::Transform> const& transforms, uint8_t flags,
                       FindName names, NativePose& result)
{
    auto& factory = animation.GetAnimationFactory();
    const bool facial = (flags & pose::Facial) != 0;
    if (factory.IsFacialAnimationStarted() != facial) {
        if (!SetFacialRig(animation, facial)) {
            Logf("npcplayback: %s cannot apply the host facial-rig state", entity->GetName().GetText());
            return false;
        }
        state.initialized = false;
        state.actor = nullptr;
        state.actorName.clear();
        state.nodes = nullptr;
        state.nodeCount = 0;
    }
    auto* actor = animation.GetActor();
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!ValidSkeleton(skeleton)) return false;
    auto const& nodes = skeleton->mNodes;
    char const* actorName = actor->GetActorName().GetText();
    result.actor = actor;
    result.actorName = actorName ? actorName : "";
    result.instance = skeleton;
    result.nodes = nodes.mData;
    result.count = nodes.mLength;
    if (state.actor != actor || state.actorName != result.actorName ||
        state.nodes != nodes.mData || state.nodeCount != nodes.mLength) {
        state.boneIndices.clear();
        state.boneIndices.reserve(static_cast<size_t>(nodes.mLength));
        for (int i = 0; i < nodes.mLength; ++i) {
            const auto hash = net::HashName(actor->GetName(nodes.mData + i).GetText());
            const auto inserted = state.boneIndices.emplace(hash, i);
            if (!inserted.second) inserted.first->second = -1;
        }
        state.actor = actor;
        state.actorName = result.actorName;
        state.nodes = nodes.mData;
        state.nodeCount = nodes.mLength;
    }
    result.bones.reserve(transforms.size());
    for (auto const& transform : transforms) {
        const auto found = state.boneIndices.find(transform.name);
        if (found == state.boneIndices.end() || found->second < 0) {
            static unsigned missingBones = 0;
            if (missingBones++ < 20)
                Logf("npcplayback: %s canonical bone %08x (%s) is unavailable on the mirror (nodes=%d)",
                     entity->GetName().GetText(), transform.name,
                     names && names(transform.name) ? names(transform.name) : "(cached pose)", nodes.mLength);
            return false;
        }
        NativeBone bone;
        bone.index = found->second;
        bone.matrix = MatrixOf(transform);
        result.bones.push_back(bone);
    }
    return true;
}

bool PresentMotion(eCVisualAnimation_PS& animation, eCWrapper_emfx2Actor& actor, int index,
                   char const* name, net::NpcMotion const& motion, bool restart)
{
    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(index);
    if (restart || !actor.HasMotionInstance(slot)) {
        auto& description = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation.GetMotionDesc(slot));
        description.SetMotion(bCString(name));
        auto* resource = description.GetMotion();
        if (!resource) { Logf("npcplayback: motion resource unavailable: %s", name); return false; }
        animation.SetMotion(slot, resource);
        MotionOptions options;
        std::memcpy(options.data(), motion.options, sizeof(motion.options));
        animation.PlayMotion(slot, (motion.active & 2u) ? nullptr :
                             reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(options.data()));
    }
    if (!actor.HasMotionInstance(slot)) { Logf("npcplayback: motion did not start: %s", name); return false; }
    const float duration = static_cast<float>(actor.GetMaxTime(slot));
    if (!(duration > 0.0f) || !std::isfinite(duration)) { Logf("npcplayback: invalid motion duration: %s", name); return false; }
    actor.SetBlendMode(slot, static_cast<eCWrapper_emfx2Motion::eEMotionBlendMode>(motion.blend));
    actor.SetIntroExtroBlending(slot, motion.intro, motion.extro);
    actor.SetTargetWeight(slot, motion.weight);
    // Keep skeletal/blend evaluation active; only the received timeline advances clip time.
    actor.SetPlaySpeed(slot, 0.0f);
    actor.PauseMotion(GEFalse, slot);
    actor.SetPlayTime(slot, std::min(duration * motion.phase, std::nextafter(duration, 0.0f)));
    return true;
}

bool IdleResourceName(char const* name)
{
    if (!name || !*name) return false;
    return (ContainsInsensitive(name, "ambient") || ContainsInsensitive(name, "idle")) &&
           !ContainsInsensitive(name, "combat") && !ContainsInsensitive(name, "hit") &&
           !ContainsInsensitive(name, "death") && !ContainsInsensitive(name, "dead") &&
           !ContainsInsensitive(name, "stumble");
}

bool ReactionResourceName(char const* name)
{
    if (!name || !*name) return false;
    return (ContainsInsensitive(name, "stumble") || ContainsInsensitive(name, "hit")) &&
           !ContainsInsensitive(name, "death") && !ContainsInsensitive(name, "dead");
}

eCResourceDataEntity* QueryNativeMotion(eCEntity* entity, gEAction action, gEPhase phase, bool reaction,
                                        std::string* lastCandidate, char const** failureReason)
{
    auto* admin = dynamic_cast<eCAnimationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCAnimationAdmin")));
    if (!admin) {
        if (failureReason) *failureReason = "native animation administration unavailable";
        return nullptr;
    }
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
                if (lastCandidate) *lastCandidate = name.GetText();
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

void PruneSkeletonCache(uint32_t now)
{
    for (auto it = sourceSkeletons.begin(); it != sourceSkeletons.end();)
        if (now - it->second.seen > 5000) it = sourceSkeletons.erase(it);
        else ++it;
    PruneSourceOptions(now);
}

void ForgetActorSkeleton(eCWrapper_emfx2Actor* actor)
{
    if (actor) sourceSkeletons.erase(actor);
}

void ClearSkeletonCache()
{
    sourceSkeletons.clear();
    ++publishedNameEpoch;
}

} // namespace g3t::npcplayback::detail

namespace g3t::npcplayback {

void ResetPublishedNames()
{
    ++detail::publishedNameEpoch;
}

bool CapturePose(eCEntity* entity, std::vector<pose::Bone>& bones, uint8_t& flags, PublishName publish)
{
    auto* animation = detail::Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!skeleton || !skeleton->mNodes.mData) return false;
    flags = animation->GetAnimationFactory().IsFacialAnimationStarted() ? pose::Facial : 0;
    auto const& nodes = skeleton->mNodes;
    if (nodes.mLength <= 0 || nodes.mLength > pose::MaxBones) {
        Logf("npcplayback: %s has an unsupported pose size: %d", entity->GetName().GetText(), nodes.mLength);
        return false;
    }
    bones.resize(static_cast<size_t>(nodes.mLength));
    auto& cache = detail::sourceSkeletons[actor];
    cache.seen = net::Now();
    char const* rawActorName = actor->GetActorName().GetText();
    char const* actorName = rawActorName ? rawActorName : "";
    if (cache.instance != skeleton || cache.nodes != nodes.mData ||
        cache.names.size() != static_cast<size_t>(nodes.mLength) || cache.actorName != actorName) {
        cache.instance = skeleton;
        cache.nodes = nodes.mData;
        cache.actorName = actorName;
        cache.names.clear();
        cache.hashes.clear();
        cache.names.reserve(static_cast<size_t>(nodes.mLength));
        cache.hashes.reserve(static_cast<size_t>(nodes.mLength));
        cache.publishedEpoch = 0;
        cache.publishedFn = nullptr;
        cache.valid = true;
        static_assert(pose::MaxBones <= 256, "Open-addressed hash table assumes <= 256 bones");
        uint32_t seenHashes[512] = {};
        bool seenOccupied[512] = {};
        for (int i = 0; i < nodes.mLength; ++i) {
            cache.names.emplace_back(actor->GetName(nodes.mData + i).GetText());
            const auto hash = net::HashName(cache.names.back().c_str());
            size_t slot = hash & 511u;
            while (seenOccupied[slot]) {
                if (seenHashes[slot] == hash) { cache.valid = false; break; }
                slot = (slot + 1) & 511u;
            }
            if (!seenOccupied[slot]) {
                seenOccupied[slot] = true;
                seenHashes[slot] = hash;
            }
            cache.hashes.push_back(hash);
        }
        if (!cache.valid) Logf("npcplayback: %s has ambiguous bone names", entity->GetName().GetText());
    }
    if (!cache.valid) {
        bones.clear();
        return false;
    }
    if (publish && (cache.publishedEpoch != detail::publishedNameEpoch || cache.publishedFn != publish)) {
        for (int i = 0; i < nodes.mLength; ++i) publish(cache.hashes[i], cache.names[i].c_str());
        cache.publishedEpoch = detail::publishedNameEpoch;
        cache.publishedFn = publish;
    }
    auto const* nodeData = nodes.mData;
    const int nodeCount = nodes.mLength;
    uint32_t const* hashes = cache.hashes.data();
    pose::Bone* outBones = bones.data();
    for (int i = 0; i < nodeCount; ++i) {
#if G3T_POSE_SSE2
        if (i + 1 < nodeCount)
            _mm_prefetch(reinterpret_cast<char const*>(nodeData + i + 1) + 0x44, _MM_HINT_T0);
#endif
        float rawMatrix[16];
        detail::ReadNodeMatrixRaw(nodeData, i, rawMatrix);
        if (!pose::EncodeMatrix4x4(hashes[i], rawMatrix, outBones[i])) {
            bones.clear();
            Logf("npcplayback: %s bone %d is outside the canonical pose encoding", entity->GetName().GetText(), i);
            return false;
        }
    }
    return true;
}

bool Capture(eCEntity* entity, net::NpcRecord& record, PublishName publish)
{
    auto* animation = detail::Animation(entity);
    if (!animation) return false;
    animation->SetForceAlwaysVisible(GETrue);
    if (!animation->GetActor()) return false;
    detail::Frame frame;
    if (!detail::CaptureFrame(entity, frame)) return false;
    for (int i = 0; i < detail::Slots; ++i) {
        detail::Motion const& source = frame.motions[i];
        auto& motion = record.motions[i];
        motion = net::NpcMotion{};
        if (!source.active) continue;
        motion.active = source.defaults ? 3 : 1;
        motion.name = net::HashName(source.name);
        motion.instance = source.instance;
        motion.phase = std::clamp(source.time / source.duration, 0.0f, 1.0f);
        motion.speed = source.speed;
        motion.weight = source.weight;
        motion.intro = source.intro;
        motion.extro = source.extro;
        motion.blend = static_cast<int32_t>(source.blend);
        std::memcpy(motion.options, source.options.data(), sizeof(motion.options));
        if (publish) publish(motion.name, source.name);
    }
    const uint8_t player = (record.flags >> 3) & 7u;
    const detail::Motion& first = frame.motions[0];
    if (net::IsHost() && first.active && combat_timing::RemoteCombat(record.flags, record.combatTarget) &&
        detail::MarkTimedCombatPlayer(animation->GetActor(), first.instance, player)) {
        Logf("combat: NPCATTACK stage=host-motion npc=%s player=%u instance=%u host_started=%u host_captured=%u "
             "motion=%s phase=%.4f duration_ms=%.1f native_speed=%.3f native_hit_phase=%.3f",
             entity->GetName().GetText(), player, first.instance, first.started, record.sent, first.name,
             record.motions[0].phase, first.duration * 1000.0f, first.speed, combat_timing::NativeHitPhase);
    }
    return true;
}

} // namespace g3t::npcplayback
