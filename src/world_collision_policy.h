// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace g3t::world::collision {

constexpr float StaticCollisionMarginCm = 2000.0f;
constexpr float NpcColumnMarginCm = 200.0f;
constexpr float GroundRayUpCm = 150.0f;
constexpr float GroundRayDownCm = -20000.0f;
constexpr float LocalCeilingMarginCm = 300.0f;

// Exact CP 1.75 Engine.dll bTIndexPtrMap layout (0x24 bytes, e.g. eCRenderSystemAdmin + 0x258).
// Native GetFreeIndex (Engine.dll + 0x1d7c80) accidentally returns bTCircularBuffer::Pop()'s bool (1)
// instead of the popped slot index, clobbering slot 1 whenever another slot (>= 2) is recycled.
struct NativeIndexPtrMap {
    int32_t* freeRing = nullptr;   // +0x00
    uint32_t ringCapacity = 0;     // +0x04
    uint32_t ringHead = 0;         // +0x08
    uint32_t ringTail = 0;         // +0x0c
    uint8_t ringFull = 0;          // +0x10
    uint8_t pad11[3] = {};         // +0x11..0x13
    void** data = nullptr;         // +0x14
    uint32_t countAndFlags = 0;    // +0x18 (bit 31 = dirty flag cleared on linear scan)
    int32_t allocatedSlots = 0;    // +0x1c
    int32_t dataCapacity = 0;      // +0x20
};
static_assert(sizeof(void*) != 4 || sizeof(NativeIndexPtrMap) == 0x24, "CP 1.75 Win32 bTIndexPtrMap size");

inline bool VerifyFreeIndexSignature(unsigned char const* code)
{
    if (!code) return false;
    constexpr unsigned char expectedHead[] = {
        0x8a, 0x41, 0x10, 0x84, 0xc0, 0x75, 0x52, 0x83, 0x39, 0x00,
        0x74, 0x08, 0x8b, 0x51, 0x08, 0x3b, 0x51, 0x0c, 0x75, 0x2e
    };
    constexpr unsigned char expectedBrokenTail[] = {
        0xb0, 0x01, 0xc6, 0x41, 0x10, 0x00, 0x0f, 0xb6, 0xc0, 0xc3
    };
    return std::memcmp(code, expectedHead, sizeof(expectedHead)) == 0 &&
           std::memcmp(code + 0x6c, expectedBrokenTail, sizeof(expectedBrokenTail)) == 0;
}

inline int32_t PopFreeIndex(NativeIndexPtrMap& map)
{
    while (map.ringFull != 0 || (map.freeRing != nullptr && map.ringHead != map.ringTail)) {
        if (!map.freeRing || map.ringCapacity == 0 || map.ringHead >= map.ringCapacity) {
            map.ringHead = 0;
            map.ringTail = 0;
            map.ringFull = 0;
            break;
        }
        const int32_t slot = map.freeRing[map.ringHead];
        map.ringHead = (map.ringHead + 1u < map.ringCapacity) ? (map.ringHead + 1u) : 0u;
        map.ringFull = 0;
        if (slot >= 0 && slot < map.allocatedSlots && map.data && map.data[slot] == nullptr)
            return slot;
    }
    map.countAndFlags &= 0x7fffffffu;
    if (map.data && map.allocatedSlots > 0) {
        for (int32_t i = 0; i < map.allocatedSlots; ++i)
            if (map.data[i] == nullptr) return i;
    }
    return -1;
}

inline bool ShouldAttemptRegionRelease(bool failed, uint32_t now, uint32_t retryAt)
{
    return !failed || static_cast<int32_t>(now - retryAt) >= 0;
}

inline bool IsRemoteRegionReady(bool failed, bool hasCamera, bool hasPrefetcher, bool anchorCellReady)
{
    return !failed && hasCamera && hasPrefetcher && anchorCellReady;
}

inline bool ShouldIncludeSimulationAnchor(bool withinMainPreload, bool mainCellReady, bool remoteRegionReady)
{
    return withinMainPreload ? mainCellReady : remoteRegionReady;
}

inline bool BoxContainsXZ(float const box[6], float x, float z, float margin)
{
    return x >= box[0] - margin && x <= box[3] + margin &&
           z >= box[2] - margin && z <= box[5] + margin;
}

inline bool BoxIntersectsCircleXZ(float const box[6], float cx, float cz, float radius)
{
    const float clampedX = cx < box[0] ? box[0] : (cx > box[3] ? box[3] : cx);
    const float clampedZ = cz < box[2] ? box[2] : (cz > box[5] ? box[5] : cz);
    const float dx = cx - clampedX;
    const float dz = cz - clampedZ;
    return dx * dx + dz * dz <= radius * radius;
}

inline bool BoxIntersectsSphere(float const box[6], float const centre[3], float radius)
{
    float distSq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float lo = box[k];
        const float hi = box[k + 3];
        const float clamped = centre[k] < lo ? lo : (centre[k] > hi ? hi : centre[k]);
        const float d = centre[k] - clamped;
        distSq += d * d;
    }
    return distSq <= radius * radius;
}

inline bool BoxOverlapsSupportColumn(float const box[6], float x, float y, float z, float xzMargin)
{
    return BoxContainsXZ(box, x, z, xzMargin) &&
           box[1] <= y + LocalCeilingMarginCm &&
           box[4] >= y + GroundRayDownCm;
}

struct StaticShapeState {
    bool dynamic = false;
    bool killed = false;
    bool enabled = true;
    bool collisionEnabled = true;
    bool cachedIn = false;
    bool cachingInOut = false;
    bool inProcessingRange = false;
    bool hasShapePs = false;
    bool rangeIsProcessing = false;
    bool disableCollision = false;
    bool disableResponse = false;
    bool physicRelevant = false;
    bool hasPhysicObject = false;
};

inline bool ShouldPinStaticCollision(StaticShapeState const& s)
{
    return !s.dynamic && !s.killed && s.enabled && s.collisionEnabled &&
           s.cachedIn && !s.cachingInOut && s.hasShapePs &&
           s.rangeIsProcessing && !s.disableCollision && s.physicRelevant;
}

inline bool ShouldRestorePinnedOnExitRange(bool isPinned, bool isCachingOut, StaticShapeState const& s)
{
    return isPinned && !isCachingOut && !s.dynamic && !s.killed &&
           s.enabled && s.collisionEnabled && s.cachedIn && !s.cachingInOut &&
           !s.hasPhysicObject;
}

inline bool ShouldDestroyPinnedPhysicObject(StaticShapeState const& s)
{
    return !s.inProcessingRange && s.hasPhysicObject;
}

inline bool IsSolidLocalStaticShape(StaticShapeState const& s)
{
    return !s.dynamic && !s.killed && s.enabled && s.collisionEnabled &&
           s.hasShapePs && !s.disableCollision && !s.disableResponse && s.physicRelevant;
}

inline bool IsUncachedLocalStaticBlocker(StaticShapeState const& s)
{
    return IsSolidLocalStaticShape(s) && (!s.cachedIn || s.cachingInOut);
}

inline bool NeedsPinnedLocalPhysicObject(StaticShapeState const& s)
{
    return IsSolidLocalStaticShape(s) && s.cachedIn && !s.cachingInOut && s.rangeIsProcessing;
}

struct NpcCollisionGateInput {
    bool hooksReady = false;
    unsigned coveringCells = 0;
    unsigned coveringStaticEntities = 0;
    bool coveringJobRunning = false;
    bool hasUncachedLocalStatic = false;
    bool allLocalRange1Pinned = true;
    bool hasStaticGroundSupport = false;
};

inline bool EvaluateNpcCollisionReady(NpcCollisionGateInput const& in)
{
    if (!in.hooksReady || in.coveringCells == 0) return false;
    if (in.coveringJobRunning && in.coveringStaticEntities == 0) return false;
    if (in.hasUncachedLocalStatic || !in.allLocalRange1Pinned) return false;
    return in.hasStaticGroundSupport;
}

inline bool DidReadinessChange(std::vector<std::pair<uintptr_t, bool>> const& previous,
                               std::vector<std::pair<uintptr_t, bool>> const& current)
{
    return previous != current;
}

}
