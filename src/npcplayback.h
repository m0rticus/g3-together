// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include "npcplayback_hit_policy.h"
#include <string>
#include <unordered_map>

class bCMatrix;
class eCEntity;

namespace g3t::npcplayback {
using LogFn = void (*)(char const*);
using PublishName = void (*)(uint32_t, char const*);
using FindName = char const* (*)(uint32_t);
struct PoseBlendState {
    bool active = false;
    uint32_t startedAtMs = 0;
    float durationSec = 0.0f;
    void const* actor = nullptr;
    std::string actorName;
    void const* skeleton = nullptr;
    void const* nodes = nullptr;
    std::vector<pose::Transform> fromTransforms;
};

struct State {
    std::vector<pose::Bone> heldPose;
    float heldPosition[3]{}, heldRotation[4]{0, 0, 0, 1};
    uint8_t heldFlags = 0;
    bool holding = false;
    uint32_t instances[net::NpcMotionSlots] = {};
    uint32_t suppressed[net::NpcMotionSlots] = {};
    uint32_t effects[net::NpcMotionSlots] = {};
    uint32_t incarnation = 0;
    uint32_t timedInstance = 0;
    uint16_t pendingHit = 0, acknowledgedHit = 0;
    uint32_t localStart = 0;
    uint32_t acknowledgedAt = 0;
    float localDuration = 0.0f;
    float localRecoverDuration = 0.0f;
    float localPlaySpeed = 1.0f;
    uint8_t localReaction = 0;
    bool localInRecover = false;
    char localMotion[96] = {};
    char localRecoverMotion[96] = {};
    net::NpcMotion localOptions;
    npcplayback_hit_policy::HitKnockbackState knockback;
    PoseBlendState poseBlend;
    void const* actor = nullptr;
    std::string actorName;
    void const* nodes = nullptr;
    int nodeCount = 0;
    std::unordered_map<uint32_t, int> boneIndices;
    bool initialized = false;
};
class HitOrigin {
public:
    HitOrigin(eCEntity* entity, uint8_t player, uint32_t sequence);
    ~HitOrigin();
    HitOrigin(HitOrigin const&) = delete;
    HitOrigin& operator=(HitOrigin const&) = delete;
private:
    eCEntity* before;
    uint8_t beforePlayer;
    uint32_t beforeSequence;
};
bool Start(bool client);
bool Available();
bool IsCharacter(eCEntity const* entity);
bool IsPassive(eCEntity const* entity);
bool RememberNativePresentation(eCEntity* entity);
bool CosmeticHit(eCEntity* entity, uint32_t swing); // resource-only flinch, never native damage or scripts
void Silence(eCEntity* entity);  // stop and clear task/state restored directly by a native save or processing-range entry
bool GuardForLease(eCEntity* entity); // register every native property fence before suspending either donor
uint32_t MotionInstance(eCEntity* entity, int slot = 0);
void BeginHit(eCEntity const* entity);
char const* HitTask(eCEntity const* entity);
void Stats(LogFn log);
void UpdatePresentation(eCEntity* entity);
bool HoldUnavailable(eCEntity* entity, State& state);
void ReleaseUnavailable(eCEntity* entity);
bool ReleaseNativePresentation(eCEntity* entity);
bool Capture(eCEntity* entity, net::NpcRecord& record, PublishName publish, bool allowOffscreenCapture = true);
bool CapturePose(eCEntity* entity, std::vector<pose::Bone>& bones, uint8_t& flags, PublishName publish);
void SyncHostCaptureVisibility(eCEntity* const* activeEntities, size_t count);
void ResetPublishedNames();
uint32_t PoseInterval(net::NpcRecord const& record);
bool Replay(eCEntity* entity, State& state, net::NpcRecord const& record, FindName names, uint32_t now);
bool ReplayFrozenPose(eCEntity* entity, State& state, std::vector<pose::Bone> const& bones, uint8_t flags);
bool LocalHit(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options, uint32_t hit, uint32_t now,
              float playSpeed = 1.0f, eCEntity const* attacker = nullptr, uint8_t reaction = 0);
bool ResolveHitMotion(eCEntity* entity, eCEntity const* attacker, uint8_t reaction, char const* currentMotion,
                      char outMotion[96], float& outPlaySpeed);
void ResetLocalHitState(State& state);
void CancelLocalHit(State& state, uint32_t now, eCEntity* entity = nullptr);
bool EstimateHostHitKnockback(eCEntity* entity, eCEntity const* attacker, uint8_t reaction, net::HitResult& outResult);
void ConfirmLocalHitKnockback(State& state, net::HitResult const& result, uint32_t now);
namespace detail {
struct NativePose;
float StartLocalHitKnockback(eCEntity* entity, eCEntity const* attacker, State& state, char const* motion, uint32_t hit,
                             uint32_t now);
bool StartLocalHitPresentation(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options,
                               uint32_t hit, uint32_t now, float playSpeed, eCEntity const* attacker, uint8_t reaction);
void CaptureEntityPoseBlend(eCEntity* entity, State& state, uint32_t now, float durationSec);
bool EvaluateLocalActorPose(eCEntity* entity, NativePose& outPose);
bool ApplyPoseBlend(State& state, NativePose& targetPose, uint32_t now);
bool VerifyLocalHitPresentation(eCEntity* entity, State const& state, net::NpcRecord const& record, FindName names,
                                char const* activeLocalClip, bool local, bool echo, float localPhase,
                                float& maxReplayPhaseError);
}
void SetEntityPose(eCEntity* entity, bCMatrix const& pose);
void Tick();
void OnWorldDeactivate();
void OnEntityDestroyed(eCEntity* entity);
void ForgetRemoved(uint8_t const guid[16]);
namespace detail { void ForgetDestroyedEntity(eCEntity const* entity); }
}
