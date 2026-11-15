// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_collision.h"

#include "ai.h"
#include "log.h"
#include "npcremoval.h"
#include "world_collision_policy.h"
#include "world_pvs.h"

#include <Engine.h>
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace g3t::world::collision {
namespace {
using ExitRangeFn = void(__fastcall*)(eCEntity*, void*);
using CacheOutInternalFn = void(__fastcall*)(eCEntity*, void*, GEBool);

ExitRangeFn originalExitRange = nullptr;
CacheOutInternalFn originalCacheOutInternal = nullptr;
bool hooksTried = false;
bool hooksActive = false;

std::mutex& Mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<eCEntity*> pinnedStatics;
std::unordered_set<eCEntity*> cachingOutStatics;
std::vector<eCEntity const*> readyRemoteNpcs;
std::vector<std::pair<uintptr_t, bool>> lastSphereReadiness;

struct CellLock {
    bCCriticalSectionWin32& lock = eCPVSCellItem::GetCriticalSection();
    CellLock() { lock.Acquire(); }
    ~CellLock() { lock.Release(); }
};

StaticShapeState ReadStaticShapeState(eCEntity* entity)
{
    StaticShapeState state;
    if (!entity) return state;
    auto const& flags = entity->GetEntityFlags();
    state.dynamic = flags.Dynamic != 0;
    state.killed = flags.m_Killed != 0;
    state.enabled = flags.m_Enabled != 0;
    state.collisionEnabled = flags.m_CollisionEnabled != 0;
    state.cachedIn = flags.m_CachedIn != 0;
    state.cachingInOut = flags.m_CachingInOut != 0;
    state.inProcessingRange = flags.m_InProcessingRange != 0;
    state.hasPhysicObject = entity->GetPhysicObject() != nullptr;
    if (auto* ps = static_cast<eCCollisionShape_PS*>(entity->GetPropertySet(eEPropertySetType_CollisionShape))) {
        state.hasShapePs = true;
        state.rangeIsProcessing = ps->GetRange() == eEPhysicRangeType_ProcessingRange;
        state.disableCollision = ps->GetDisableCollision() != GEFalse;
        state.disableResponse = ps->GetDisableResponse() != GEFalse;
        state.physicRelevant = ps->IsPhysicRelevant() != GEFalse;
    }
    return state;
}

void __fastcall OnExitProcessingRangeDetour(eCEntity* entity, void* edx)
{
    originalExitRange(entity, edx);
    if (!entity || ai::ShuttingDown() || pvs::GatePaused()) return;
    std::lock_guard<std::mutex> lock(Mutex());
    const bool isPinned = pinnedStatics.find(entity) != pinnedStatics.end();
    if (!isPinned) return;
    const bool isCachingOut = cachingOutStatics.find(entity) != cachingOutStatics.end();
    if (ShouldRestorePinnedOnExitRange(isPinned, isCachingOut, ReadStaticShapeState(entity)))
        entity->CreatePhysicObject();
}

void __fastcall CacheOutInternalDetour(eCEntity* entity, void* edx, GEBool direct)
{
    bool tracked = false;
    if (entity && !entity->GetEntityFlags().Dynamic) {
        std::lock_guard<std::mutex> lock(Mutex());
        cachingOutStatics.insert(entity);
        tracked = true;
        if (pinnedStatics.erase(entity) > 0 && ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity)))
            entity->DestroyPhysicObject();
    }
    originalCacheOutInternal(entity, edx, direct);
    if (tracked) {
        std::lock_guard<std::mutex> lock(Mutex());
        cachingOutStatics.erase(entity);
    }
}

void UnpinAllLocked()
{
    for (eCEntity* entity : pinnedStatics) {
        if (entity && cachingOutStatics.find(entity) == cachingOutStatics.end() &&
            ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity))) {
            entity->DestroyPhysicObject();
        }
    }
    pinnedStatics.clear();
}

bool IntersectsAnySphere(float const box[6], processing::Sphere const* spheres, int sphereCount, float margin)
{
    for (int i = 0; i < sphereCount; ++i)
        if (BoxIntersectsSphere(box, spheres[i].at, spheres[i].radius + margin)) return true;
    return false;
}

bool IntersectsAnyCircleXZ(float const box[6], processing::Sphere const* spheres, int sphereCount, float margin)
{
    for (int i = 0; i < sphereCount; ++i)
        if (BoxIntersectsCircleXZ(box, spheres[i].at[0], spheres[i].at[2], spheres[i].radius + margin)) return true;
    return false;
}

struct NpcCandidate {
    eCEntity* entity = nullptr;
    float pos[3] = {};
    unsigned coveringCells = 0;
    unsigned coveringStaticEntities = 0;
    bool coveringJobRunning = false;
    bool hasUncachedLocalStatic = false;
    bool allLocalRange1Pinned = true;
    std::vector<eCEntity*> localRange1Statics;
};

struct GridCellRef {
    eCPVSCellItem* cell = nullptr;
    bool jobRunning = false;
};
}

bool EnsureHooksInstalled()
{
    if (hooksTried) return hooksActive && pvs::InstallDatabaseIndexFix();
    hooksTried = true;
    if (!pvs::InstallDatabaseIndexFix()) return false;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* exitCode = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?OnExitProcessingRange@eCEntity@@MAEXXZ")) : nullptr;
    auto* cacheOutCode = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CacheOutInternal@eCEntity@@IAEX_N@Z")) : nullptr;
    constexpr unsigned char expectedExit[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x3c};
    constexpr unsigned char expectedCacheOut[] = {0x57, 0x8b, 0xf9, 0x8b, 0x47, 0x3c};
    if (!exitCode || std::memcmp(exitCode, expectedExit, sizeof(expectedExit)) ||
        !cacheOutCode || std::memcmp(cacheOutCode, expectedCacheOut, sizeof(expectedCacheOut))) {
        Log("FAIL world: static collision lifecycle hooks have unexpected Engine.dll prologues; remote NPC gating closed");
        return false;
    }
    originalExitRange = reinterpret_cast<ExitRangeFn>(
        ai::JumpTo(exitCode, reinterpret_cast<void*>(&OnExitProcessingRangeDetour), sizeof(expectedExit)));
    originalCacheOutInternal = reinterpret_cast<CacheOutInternalFn>(
        ai::JumpTo(cacheOutCode, reinterpret_cast<void*>(&CacheOutInternalDetour), sizeof(expectedCacheOut)));
    hooksActive = originalExitRange != nullptr && originalCacheOutInternal != nullptr;
    Log(hooksActive ? "world: static collision lifecycle hooks installed for remote simulation spheres" :
                      "FAIL world: static collision lifecycle hooks could not be installed");
    return hooksActive;
}

bool SyncSphereStaticCollision(eCEntityAdmin* admin, processing::Sphere const* spheres, int sphereCount)
{
    if (!admin || sphereCount <= 0 || !spheres || pvs::GatePaused() || ai::ShuttingDown()) {
        std::lock_guard<std::mutex> lock(Mutex());
        UnpinAllLocked();
        const bool hadReady = !readyRemoteNpcs.empty() || !lastSphereReadiness.empty();
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return hadReady;
    }
    const bool readyHooks = EnsureHooksInstalled();
    if (!readyHooks) {
        std::lock_guard<std::mutex> lock(Mutex());
        UnpinAllLocked();
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return false;
    }

    std::vector<eCPVSPrefetcher3*> prefetchers;
    pvs::CollectReadyPrefetchers(prefetchers);

    std::unordered_set<eCEntity*> desiredPinned;
    std::vector<NpcCandidate> candidates;
    {
        CellLock cellLock;
        std::vector<GridCellRef> gridCells;
        std::vector<eCPVSCellItem*> permanentCells;
        for (eCPVSPrefetcher3* prefetcher : prefetchers) {
            if (!prefetcher) continue;
            const bool running = prefetcher->IsJobRunning() != GEFalse;
            auto* permanent = prefetcher->GetNonDeactivationCell();
            if (permanent && std::find(permanentCells.begin(), permanentCells.end(), permanent) == permanentCells.end())
                permanentCells.push_back(permanent);
            bTSmallArray<eCPVSCellItem*> cells;
            prefetcher->GetAllValidCells(cells);
            for (GEU16 i = 0; i < cells.GetCount(); ++i) {
                eCPVSCellItem* cell = cells.GetAt(i);
                if (!cell || cell == permanent) continue;
                auto it = std::find_if(gridCells.begin(), gridCells.end(),
                                       [cell](GridCellRef const& ref) { return ref.cell == cell; });
                if (it == gridCells.end()) gridCells.push_back({cell, running});
                else it->jobRunning = it->jobRunning && running;
            }
        }

        std::vector<eCEntity*> candidateEntities;
        auto scanCellEntities = [&](eCPVSCellItem* cell, bool isPermanent) {
            auto const* cellBox = reinterpret_cast<float const*>(&cell->GetCellBox());
            if (!isPermanent && !IntersectsAnyCircleXZ(cellBox, spheres, sphereCount, StaticCollisionMarginCm))
                return;
            auto const& statics = cell->GetStaticEntities();
            for (GEU16 i = 0; i < statics.GetCount(); ++i) {
                eCEntity* entity = statics.GetAt(i);
                if (!entity) continue;
                auto const* box = reinterpret_cast<float const*>(&entity->GetWorldNodeBoundary());
                if (!IntersectsAnySphere(box, spheres, sphereCount, StaticCollisionMarginCm)) continue;
                if (ShouldPinStaticCollision(ReadStaticShapeState(entity)))
                    desiredPinned.insert(entity);
            }
            auto const& dynamics = cell->GetDynamicEntities();
            for (GEU16 i = 0; i < dynamics.GetCount(); ++i) {
                eCEntity* entity = dynamics.GetAt(i);
                if (!entity || entity->IsKilled() || !entity->IsEnabled()) continue;
                if (npcremoval::ExcludeFromProcessing(entity)) continue;
                if (!entity->GetPropertySet(eEPropertySetType_NPC)) continue;
                bCBox const& bounds = entity->GetWorldNodeBoundary();
                if (!processing::IncludesNpc(false, true, reinterpret_cast<float const*>(&bounds), spheres, sphereCount))
                    continue;
                candidateEntities.push_back(entity);
            }
        };

        for (GridCellRef const& entry : gridCells) scanCellEntities(entry.cell, false);
        for (eCPVSCellItem* permanent : permanentCells) scanCellEntities(permanent, true);

        std::sort(candidateEntities.begin(), candidateEntities.end());
        candidateEntities.erase(std::unique(candidateEntities.begin(), candidateEntities.end()), candidateEntities.end());
        candidates.reserve(candidateEntities.size());

        for (eCEntity* npc : candidateEntities) {
            NpcCandidate cand;
            cand.entity = npc;
            bCVector const& wp = npc->GetWorldPosition();
            cand.pos[0] = wp.GetX();
            cand.pos[1] = wp.GetY();
            cand.pos[2] = wp.GetZ();

            auto inspectLocalStatics = [&](eCPVSCellItem* cell) {
                auto const& statics = cell->GetStaticEntities();
                for (GEU16 i = 0; i < statics.GetCount(); ++i) {
                    eCEntity* staticEnt = statics.GetAt(i);
                    if (!staticEnt) continue;
                    auto const* sbox = reinterpret_cast<float const*>(&staticEnt->GetWorldNodeBoundary());
                    if (!BoxOverlapsSupportColumn(sbox, cand.pos[0], cand.pos[1], cand.pos[2], NpcColumnMarginCm))
                        continue;
                    const StaticShapeState state = ReadStaticShapeState(staticEnt);
                    if (IsUncachedLocalStaticBlocker(state)) {
                        cand.hasUncachedLocalStatic = true;
                    } else if (NeedsPinnedLocalPhysicObject(state)) {
                        desiredPinned.insert(staticEnt);
                        cand.localRange1Statics.push_back(staticEnt);
                    }
                }
            };

            bool anyCoveringReady = false;
            for (GridCellRef const& entry : gridCells) {
                auto const* cellBox = reinterpret_cast<float const*>(&entry.cell->GetCellBox());
                if (!BoxContainsXZ(cellBox, cand.pos[0], cand.pos[2], 0.0f)) continue;
                ++cand.coveringCells;
                cand.coveringStaticEntities += entry.cell->GetStaticEntities().GetCount();
                if (!entry.jobRunning) anyCoveringReady = true;
                inspectLocalStatics(entry.cell);
            }
            cand.coveringJobRunning = cand.coveringCells > 0 && !anyCoveringReady;
            for (eCPVSCellItem* permanent : permanentCells) inspectLocalStatics(permanent);
            candidates.push_back(std::move(cand));
        }
    }

    {
        std::lock_guard<std::mutex> lock(Mutex());
        for (auto it = pinnedStatics.begin(); it != pinnedStatics.end();) {
            eCEntity* entity = *it;
            if (desiredPinned.find(entity) == desiredPinned.end()) {
                if (entity && cachingOutStatics.find(entity) == cachingOutStatics.end() &&
                    ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity))) {
                    entity->DestroyPhysicObject();
                }
                it = pinnedStatics.erase(it);
            } else {
                ++it;
            }
        }
        for (eCEntity* entity : desiredPinned) {
            if (!entity || cachingOutStatics.find(entity) != cachingOutStatics.end()) continue;
            const StaticShapeState state = ReadStaticShapeState(entity);
            if (!ShouldPinStaticCollision(state)) continue;
            pinnedStatics.insert(entity);
            if (!state.hasPhysicObject) entity->CreatePhysicObject();
        }
        for (NpcCandidate& cand : candidates) {
            for (eCEntity* staticEnt : cand.localRange1Statics) {
                if (!staticEnt || cachingOutStatics.find(staticEnt) != cachingOutStatics.end() ||
                    staticEnt->GetPhysicObject() == nullptr) {
                    cand.allLocalRange1Pinned = false;
                    break;
                }
            }
        }
    }

    auto& physics = eCPhysicsScene::GetInstance();
    const GEU32 rayHints = eETraceRayHints_TestStaticShapes | eETraceRayHints_IncludeWater;
    const bCVector rayDir(0.0f, GroundRayDownCm, 0.0f);

    std::vector<eCEntity const*> readyNow;
    std::vector<std::pair<uintptr_t, bool>> readinessSnapshot;
    readyNow.reserve(candidates.size());
    readinessSnapshot.reserve(candidates.size());

    for (NpcCandidate const& cand : candidates) {
        NpcCollisionGateInput gate;
        gate.hooksReady = readyHooks;
        gate.coveringCells = cand.coveringCells;
        gate.coveringStaticEntities = cand.coveringStaticEntities;
        gate.coveringJobRunning = cand.coveringJobRunning;
        gate.hasUncachedLocalStatic = cand.hasUncachedLocalStatic;
        gate.allLocalRange1Pinned = cand.allLocalRange1Pinned;
        if (gate.coveringCells > 0 && !(gate.coveringJobRunning && gate.coveringStaticEntities == 0) &&
            !gate.hasUncachedLocalStatic && gate.allLocalRange1Pinned) {
            const bCVector origin(cand.pos[0], cand.pos[1] + GroundRayUpCm, cand.pos[2]);
            gate.hasStaticGroundSupport = physics.TraceRayNearestHit(origin, rayDir, nullptr, rayHints) != GEFalse;
        }
        const bool ready = EvaluateNpcCollisionReady(gate);
        if (ready) readyNow.push_back(cand.entity);
        readinessSnapshot.push_back({reinterpret_cast<uintptr_t>(cand.entity), ready});
    }

    std::sort(readyNow.begin(), readyNow.end());
    std::sort(readinessSnapshot.begin(), readinessSnapshot.end());

    std::lock_guard<std::mutex> lock(Mutex());
    const bool changed = DidReadinessChange(lastSphereReadiness, readinessSnapshot);
    readyRemoteNpcs = std::move(readyNow);
    lastSphereReadiness = std::move(readinessSnapshot);
    return changed;
}

bool IsNpcCollisionReady(eCEntity const* entity)
{
    if (!entity) return false;
    std::lock_guard<std::mutex> lock(Mutex());
    return std::binary_search(readyRemoteNpcs.begin(), readyRemoteNpcs.end(), entity);
}

void OnEntityDestroyed(eCEntity* entity)
{
    if (!entity) return;
    std::lock_guard<std::mutex> lock(Mutex());
    pinnedStatics.erase(entity);
    cachingOutStatics.erase(entity);
    auto it = std::lower_bound(readyRemoteNpcs.begin(), readyRemoteNpcs.end(), entity);
    if (it != readyRemoteNpcs.end() && *it == entity) readyRemoteNpcs.erase(it);
    const uintptr_t key = reinterpret_cast<uintptr_t>(entity);
    auto snapIt = std::lower_bound(lastSphereReadiness.begin(), lastSphereReadiness.end(),
                                   std::make_pair(key, false),
                                   [](auto const& a, auto const& b) { return a.first < b.first; });
    if (snapIt != lastSphereReadiness.end() && snapIt->first == key)
        lastSphereReadiness.erase(snapIt);
}

void Retire()
{
    std::lock_guard<std::mutex> lock(Mutex());
    UnpinAllLocked();
    cachingOutStatics.clear();
    readyRemoteNpcs.clear();
    lastSphereReadiness.clear();
}

}
