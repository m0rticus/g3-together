// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/world_collision_policy.h"
#include "../src/processing.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

using namespace g3t::world::collision;

namespace {
void TestDatabaseFreeIndexFix()
{
    std::array<unsigned char, 0x76> validCode{};
    const unsigned char head[] = {
        0x8a, 0x41, 0x10, 0x84, 0xc0, 0x75, 0x52, 0x83, 0x39, 0x00,
        0x74, 0x08, 0x8b, 0x51, 0x08, 0x3b, 0x51, 0x0c, 0x75, 0x2e
    };
    const unsigned char tail[] = {
        0xb0, 0x01, 0xc6, 0x41, 0x10, 0x00, 0x0f, 0xb6, 0xc0, 0xc3
    };
    std::memcpy(validCode.data(), head, sizeof(head));
    std::memcpy(validCode.data() + 0x6c, tail, sizeof(tail));
    assert(VerifyFreeIndexSignature(validCode.data()));
    assert(!VerifyFreeIndexSignature(nullptr));

    auto badHead = validCode;
    badHead[0] ^= 0xff;
    assert(!VerifyFreeIndexSignature(badHead.data()));

    auto badTail = validCode;
    badTail[0x6d] = 0x00;
    assert(!VerifyFreeIndexSignature(badTail.data()));

    // Reproduce the playtest database corruption scenario:
    // slot 0 = main camera, slot 1 = live remote region camera, slot 2 = temporary engine camera.
    // Retiring slot 2 pushes 2 into freeRing; PopFreeIndex must return 2 (never 1, which clobbers slot 1).
    int mainCam = 10, remoteCam = 20;
    std::array<void*, 4> slots{&mainCam, &remoteCam, nullptr, nullptr};
    std::array<int32_t, 4> ring{2, 0, 0, 0};
    NativeIndexPtrMap map{};
    map.freeRing = ring.data();
    map.ringCapacity = static_cast<uint32_t>(ring.size());
    map.ringHead = 0;
    map.ringTail = 1;
    map.ringFull = 0;
    map.data = slots.data();
    map.countAndFlags = 0x80000002u;
    map.allocatedSlots = 3;
    map.dataCapacity = 4;

    assert(PopFreeIndex(map) == 2);
    assert(map.ringHead == 1 && map.ringTail == 1 && map.ringFull == 0);
    assert(slots[1] == &remoteCam);

    // Wrap-around on full ring + skipping stale occupied indices before falling back to linear scan.
    int tempCam = 30;
    slots[2] = &tempCam;
    ring = {1, 2, 0, 3}; // slots 1, 2, 0 are occupied; slot 3 is free at wrapped index
    map.ringHead = 2;
    map.ringTail = 2;
    map.ringFull = 1;
    map.allocatedSlots = 4;
    // Order from ringHead=2: ring[2]=0 (occupied), ring[3]=3 (free!) -> wraps ringHead to 0, returns 3.
    assert(PopFreeIndex(map) == 3);
    assert(map.ringHead == 0 && map.ringFull == 0);

    // Remaining ring[0]=1 and ring[1]=2 are both occupied; linear scan finds no nullptr and clears bit 31.
    int fourthCam = 40;
    slots[3] = &fourthCam;
    map.countAndFlags = 0x80000004u;
    assert(PopFreeIndex(map) == -1);
    assert(map.ringHead == 2);
    assert((map.countAndFlags & 0x80000000u) == 0);
}

void TestRegionBackoffAndAnchorReadiness()
{
    assert(ShouldAttemptRegionRelease(false, 1000u, 6000u));
    assert(!ShouldAttemptRegionRelease(true, 1000u, 6000u));
    assert(!ShouldAttemptRegionRelease(true, 5999u, 6000u));
    assert(ShouldAttemptRegionRelease(true, 6000u, 6000u));
    // Wrap-around across uint32_t max.
    assert(!ShouldAttemptRegionRelease(true, 0xffffff00u, 0x00000100u));
    assert(ShouldAttemptRegionRelease(true, 0x00000150u, 0x00000100u));

    assert(!IsRemoteRegionReady(true, true, true, true));
    assert(!IsRemoteRegionReady(false, false, true, true));
    assert(!IsRemoteRegionReady(false, true, false, true));
    assert(!IsRemoteRegionReady(false, true, true, false));
    assert(IsRemoteRegionReady(false, true, true, true));

    assert(!ShouldIncludeSimulationAnchor(true, false, true));
    assert(ShouldIncludeSimulationAnchor(true, true, false));
    assert(!ShouldIncludeSimulationAnchor(false, true, false));
    assert(ShouldIncludeSimulationAnchor(false, false, true));
}

void TestStaticPinningAndLifecycle()
{
    StaticShapeState reddockCave{};
    reddockCave.cachedIn = true;
    reddockCave.hasShapePs = true;
    reddockCave.rangeIsProcessing = true;
    reddockCave.physicRelevant = true;
    assert(ShouldPinStaticCollision(reddockCave));
    assert(NeedsPinnedLocalPhysicObject(reddockCave));
    assert(!IsUncachedLocalStaticBlocker(reddockCave));

    // Range == 2 terrain creates its physics object on OnCacheIn natively; no Range == 1 pin needed.
    StaticShapeState surfaceTerrain = reddockCave;
    surfaceTerrain.rangeIsProcessing = false;
    assert(!ShouldPinStaticCollision(surfaceTerrain));
    assert(!NeedsPinnedLocalPhysicObject(surfaceTerrain));

    // Uncached or mid-transition cave mesh blocks nearby NPCs and must not have CreatePhysicObject called yet.
    StaticShapeState streamingCave = reddockCave;
    streamingCave.cachedIn = false;
    assert(!ShouldPinStaticCollision(streamingCave));
    assert(IsUncachedLocalStaticBlocker(streamingCave));

    StaticShapeState cachingCave = reddockCave;
    cachingCave.cachingInOut = true;
    assert(!ShouldPinStaticCollision(cachingCave));
    assert(IsUncachedLocalStaticBlocker(cachingCave));

    // Non-response trigger volume does not block NPC ground readiness.
    StaticShapeState triggerZone = streamingCave;
    triggerZone.disableResponse = true;
    assert(!IsUncachedLocalStaticBlocker(triggerZone));

    // Host walks past 40m native ROI while remote player stays at Reddock:
    // OnExitProcessingRange destroys m_pPhysicObject, and our hook immediately restores it before OnProcess.
    StaticShapeState exitedNativeRange = reddockCave;
    exitedNativeRange.inProcessingRange = false;
    exitedNativeRange.hasPhysicObject = false;
    assert(ShouldRestorePinnedOnExitRange(true, false, exitedNativeRange));
    assert(!ShouldRestorePinnedOnExitRange(false, false, exitedNativeRange));
    assert(!ShouldRestorePinnedOnExitRange(true, true, exitedNativeRange));

    // CacheOutInternal / retirement destroys pinned m_pPhysicObject only when outside native processing range.
    StaticShapeState pinnedOutsideNative = reddockCave;
    pinnedOutsideNative.inProcessingRange = false;
    pinnedOutsideNative.hasPhysicObject = true;
    assert(ShouldDestroyPinnedPhysicObject(pinnedOutsideNative));

    StaticShapeState stillInsideNative = pinnedOutsideNative;
    stillInsideNative.inProcessingRange = true;
    assert(!ShouldDestroyPinnedPhysicObject(stillInsideNative));
}

void TestReddockUndergroundNpcGateAndReadinessRebuild()
{
    // Reddock underground NPC (Norris at ~74515, 2390, -15400 beneath surface hill Y ~3230).
    const float caveBox[6] = {72000.0f, 1800.0f, -18000.0f, 77000.0f, 3200.0f, -13000.0f};
    const float remoteSphereCentre[3] = {74800.0f, 3230.0f, -15100.0f};
    assert(BoxIntersectsSphere(caveBox, remoteSphereCentre, 8250.0f + StaticCollisionMarginCm));
    assert(BoxOverlapsSupportColumn(caveBox, 74515.0f, 2390.0f, -15400.0f, NpcColumnMarginCm));

    // Sky-high unrelated decoration far above the NPC's head (+10m) is ignored by the support column.
    const float highBannerBox[6] = {74450.0f, 3500.0f, -15450.0f, 74550.0f, 3800.0f, -15350.0f};
    assert(!BoxOverlapsSupportColumn(highBannerBox, 74515.0f, 2390.0f, -15400.0f, NpcColumnMarginCm));

    NpcCollisionGateInput gate{};
    gate.hooksReady = true;
    gate.coveringCells = 1;
    gate.coveringStaticEntities = 48;
    gate.coveringJobRunning = false;
    gate.hasUncachedLocalStatic = false;
    gate.allLocalRange1Pinned = true;
    gate.hasStaticGroundSupport = true;
    assert(EvaluateNpcCollisionReady(gate));

    // 1. Missing lifecycle hooks -> fail-closed.
    auto noHooks = gate;
    noHooks.hooksReady = false;
    assert(!EvaluateNpcCollisionReady(noHooks));

    // 2. No valid PVS cell covering the NPC -> blocked.
    auto noCell = gate;
    noCell.coveringCells = 0;
    assert(!EvaluateNpcCollisionReady(noCell));

    // 3. Empty streaming shell cell while PVS job is still running -> blocked.
    auto emptyShell = gate;
    emptyShell.coveringStaticEntities = 0;
    emptyShell.coveringJobRunning = true;
    assert(!EvaluateNpcCollisionReady(emptyShell));

    // 4. Cave static mesh in cell still uncached -> blocked (prevents Floordetection upward teleport to hill).
    auto uncachedCave = gate;
    uncachedCave.hasUncachedLocalStatic = true;
    assert(!EvaluateNpcCollisionReady(uncachedCave));

    // 5. Range == 1 cave static lacks m_pPhysicObject -> blocked (prevents falling through cave floor).
    auto unpinnedCave = gate;
    unpinnedCave.allLocalRange1Pinned = false;
    assert(!EvaluateNpcCollisionReady(unpinnedCave));

    // 6. Downward static ray hits nothing (void below underground cave) -> blocked.
    auto noGround = gate;
    noGround.hasStaticGroundSupport = false;
    assert(!EvaluateNpcCollisionReady(noGround));

    // Stationary sphere readiness transition triggers an incremental processing rebuild.
    const std::vector<std::pair<uintptr_t, bool>> before{{0x1001u, false}, {0x1002u, false}};
    const std::vector<std::pair<uintptr_t, bool>> afterStreamIn{{0x1001u, true}, {0x1002u, true}};
    assert(DidReadinessChange(before, afterStreamIn));
    assert(!DidReadinessChange(afterStreamIn, afterStreamIn));
}
}

int main()
{
    TestDatabaseFreeIndexFix();
    TestRegionBackoffAndAnchorReadiness();
    TestStaticPinningAndLifecycle();
    TestReddockUndergroundNpcGateAndReadinessRebuild();
    return 0;
}
