// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcplayback.h"
#include "npclease_presentation.h"

#include <Game.h>
#include <Script.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::npcplayback::detail {

// Native emfx2Node stores its local bone bCMatrix at byte offset +0x44 in CP 1.75.
template <typename Node>
inline void ReadNodeMatrixRaw(Node const* nodes, int index, float (&out)[16])
{
    std::memcpy(out, reinterpret_cast<unsigned char const*>(nodes + index) + 0x44, sizeof(out));
}

template <typename Node>
inline bCMatrix ReadNodeMatrix(Node const* nodes, int index)
{
    bCMatrix matrix;
    std::memcpy(&matrix, reinterpret_cast<unsigned char const*>(nodes + index) + 0x44, sizeof(matrix));
    return matrix;
}

template <typename Node>
inline void WriteNodeMatrix(Node* nodes, int index, bCMatrix const& matrix)
{
    std::memcpy(reinterpret_cast<unsigned char*>(nodes + index) + 0x44, &matrix, sizeof(matrix));
}

constexpr int Slots = 8;
// Engine 1.75's actor PlayMotion reads seven 32-bit words. The SDK declares this descriptor without storage.
using MotionOptions = std::array<uint32_t, 7>;
static_assert(sizeof(MotionOptions) == 28, "Native motion options are seven 32-bit words");
static_assert(sizeof(uintptr_t) == 4, "The checked Gothic 3 native ABI is Win32");

struct NativeBone {
    int index = 0;
    bCMatrix matrix;
};

struct NativePose {
    std::vector<NativeBone> bones;
    void const* actor = nullptr;
    std::string actorName;
    void const* instance = nullptr;
    void const* nodes = nullptr;
    int count = 0;
    bool applied = false, evaluated = false, valid = true, evaluateOnly = false, allowNonPassive = false;
};

struct Motion {
    MotionOptions options{};
    uint32_t instance = 0;
    uint32_t started = 0;
    eCResourceAnimationMotion_PS* resource = nullptr;
    char name[96] = {};
    float time = 0.0f, duration = 0.0f, speed = 0.0f, weight = 0.0f, intro = 0.0f, extro = 0.0f;
    eCWrapper_emfx2Motion::eEMotionBlendMode blend{};
    bool active = false;
    bool defaults = false;
};

struct Frame {
    float position[3] = {}, rotation[4] = {};
    gEAniState aniState{};
    bool facial = false;
    Motion motions[Slots];
};

eCVisualAnimation_PS* Animation(eCEntity* entity);
template <typename Skeleton>
inline bool ValidSkeleton(Skeleton const* skeleton)
{
    return skeleton && skeleton->mNodes.mData && skeleton->mNodes.mLength > 0 && skeleton->mNodes.mLength <= 4096;
}
pose::Transform TransformOf(bCMatrix const& matrix, uint32_t name);
bCMatrix MatrixOf(pose::Transform const& transform);
bool SetFacialRig(eCVisualAnimation_PS& animation, bool facial);
float CurrentWeight(eCWrapper_emfx2Actor const& actor, int slot);
bool ApplyPose(eCWrapper_emfx2Actor& actor, NativePose& pose);
bool FullPose(NativePose const& pose);
npclease::PresentationRig Rig(eCVisualAnimation_PS& animation);

bool CaptureFrame(eCEntity* entity, Frame& frame);
bool PrepareTransforms(eCEntity* entity, eCVisualAnimation_PS& animation, State& state,
                       std::vector<pose::Transform> const& transforms, uint8_t flags,
                       FindName names, NativePose& result);
bool PresentMotion(eCVisualAnimation_PS& animation, eCWrapper_emfx2Actor& actor, int index,
                   char const* name, net::NpcMotion const& motion, bool restart);
bool IdleResourceName(char const* name);
bool ReactionResourceName(char const* name);
eCResourceDataEntity* QueryNativeMotion(eCEntity* entity, gEAction action, gEPhase phase, bool reaction,
                                        std::string* lastCandidate, char const** failureReason);

void PruneSkeletonCache(uint32_t now);
void ForgetActorSkeleton(eCWrapper_emfx2Actor* actor);
void ClearSkeletonCache();

} // namespace g3t::npcplayback::detail
