// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcplayback_skeleton.h"

#include <atomic>
#include <cstdint>

namespace g3t::npcplayback::detail {

using ActorProcessFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, bCMatrix const&, GEBool, GEBool, GEFloat);

struct CapturedOptions {
    MotionOptions words{};
    eCWrapper_emfx2Motion const* motion = nullptr;
    bool available = false;
    bool defaults = false;
    uint32_t instance = 0;
    uint32_t started = 0;
    uint8_t timedPlayer = 0;
};

struct HookCounters {
    std::atomic<uint32_t> blockedRoutines{0};
    std::atomic<uint32_t> blockedScripts{0};
    std::atomic<uint32_t> blockedMovement{0};
    std::atomic<uint32_t> blockedTasks{0};
    std::atomic<uint32_t> blockedAnimations{0};
    std::atomic<uint32_t> blockedEntities{0};
    std::atomic<uint32_t> blockedProperties{0};
    std::atomic<uint32_t> blockedEvents{0};
    std::atomic<uint32_t> blockedNpcTicks{0};
};

struct PresentationScope {
    explicit PresentationScope(bool value = true);
    ~PresentationScope();
    bool before;
};

struct NativePoseScope {
    explicit NativePoseScope(NativePose* pose);
    ~NativePoseScope();
    NativePose* before;
};

struct VisualScope {
    explicit VisualScope(eCEntity* entity);
    ~VisualScope();
    bool before;
    eCEntity* beforeEntity;
};

HookCounters& Counters();
NativePose* CurrentPresentingPose();
ActorProcessFn OriginalActorProcess();
void __fastcall ActorProcess(eCWrapper_emfx2Actor* self, void* edx, bCMatrix const& world,
                             GEBool skeleton, GEBool physics, GEFloat scale);

bool InstallHooks();
bool RegisterEntity(eCEntity* entity);
bool TaskBlocked(gCScriptRoutine_PS* self, bCString const& task);
void RecordBlockedTaskVerdict(eCEntity const* entity, char const* task);
bool LookupCapturedOptions(eCWrapper_emfx2Actor* actor, int slot, CapturedOptions& out);
bool MarkTimedCombatPlayer(eCWrapper_emfx2Actor* actor, uint32_t instance, uint8_t player);
uint32_t ActorMotionInstance(eCEntity* entity, int slot);
void ResetHooksState();

} // namespace g3t::npcplayback::detail
