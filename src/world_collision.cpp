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
#include <condition_variable>
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

std::condition_variable& PhysicOpCv()
{
    static std::condition_variable cv;
    return cv;
}

struct InFlightPhysicOp {
    eCEntity* entity = nullptr;
    DWORD threadId = 0;
};

std::unordered_set<eCEntity*> pinnedStatics;
std::unordered_set<eCEntity*> cachingOutStatics;
std::vector<InFlightPhysicOp> inFlightPhysicOps;
std::vector<eCEntity const*> readyRemoteNpcs;
std::vector<std::pair<uintptr_t, bool>> lastSphereReadiness;

bool HasOtherThreadInFlightLocked(eCEntity const* entity, DWORD selfThread)
{
    for (InFlightPhysicOp const& op : inFlightPhysicOps) {
        if (op.entity == entity && op.threadId != selfThread) return true;
    }
    return false;
}

void BeginPhysicOpLocked(eCEntity* entity, DWORD selfThread)
{
    inFlightPhysicOps.push_back({entity, selfThread});
}

void EndPhysicOp(eCEntity* entity, DWORD selfThread)
{
    {
        std::lock_guard<std::mutex> lock(Mutex());
        for (auto it = inFlightPhysicOps.begin(); it != inFlightPhysicOps.end(); ++it) {
            if (it->entity == entity && it->threadId == selfThread) {
                inFlightPhysicOps.erase(it);
                break;
            }
        }
    }
    PhysicOpCv().notify_all();
}

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
    const DWORD selfThread = GetCurrentThreadId();
    bool shouldRestore = false;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        const bool isPinned = pinnedStatics.find(entity) != pinnedStatics.end();
        if (!isPinned) return;
        const bool isCachingOut = cachingOutStatics.find(entity) != cachingOutStatics.end();
        shouldRestore = ShouldRestorePinnedOnExitRange(isPinned, isCachingOut, ReadStaticShapeState(entity));
        if (shouldRestore) BeginPhysicOpLocked(entity, selfThread);
    }
    if (!shouldRestore) return;
    entity->CreatePhysicObject();
    EndPhysicOp(entity, selfThread);
}

void __fastcall CacheOutInternalDetour(eCEntity* entity, void* edx, GEBool direct)
{
    bool tracked = false;
    bool shouldDestroy = false;
    if (entity && !entity->GetEntityFlags().Dynamic) {
        const DWORD selfThread = GetCurrentThreadId();
        std::unique_lock<std::mutex> lock(Mutex());
        PhysicOpCv().wait(lock, [&] { return !HasOtherThreadInFlightLocked(entity, selfThread); });
        cachingOutStatics.insert(entity);
        tracked = true;
        shouldDestroy = pinnedStatics.erase(entity) > 0 &&
                        ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity));
    }
    if (shouldDestroy) entity->DestroyPhysicObject();
    originalCacheOutInternal(entity, edx, direct);
    if (tracked) {
        {
            std::lock_guard<std::mutex> lock(Mutex());
            cachingOutStatics.erase(entity);
        }
        PhysicOpCv().notify_all();
    }
}

void UnpinAllOutsideLock()
{
    const DWORD selfThread = GetCurrentThreadId();
    std::vector<eCEntity*> candidates;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        candidates.assign(pinnedStatics.begin(), pinnedStatics.end());
        pinnedStatics.clear();
    }
    for (eCEntity* entity : candidates) {
        bool doDestroy = false;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (entity && pinnedStatics.find(entity) == pinnedStatics.end() &&
                cachingOutStatics.find(entity) == cachingOutStatics.end() &&
                ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity))) {
                BeginPhysicOpLocked(entity, selfThread);
                doDestroy = true;
            }
        }
        if (doDestroy) {
            entity->DestroyPhysicObject();
            EndPhysicOp(entity, selfThread);
        }
    }
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
    bool anyCoveringJobRunning = false;
    bool hasUncachedLocalStatic = false;
    bool hasMissingSolidPhysic = false;
    bool allLocalRange1Pinned = true;
    std::vector<eCEntity*> localRange1Statics;
};

constexpr float LocalGroundSupportDropCm = 350.0f;

struct IndexedStatic {
    eCEntity* entity = nullptr;
    float const* box = nullptr;
};

struct IndexedCell {
    eCPVSCellItem* cell = nullptr;
    float const* cellBox = nullptr;
    unsigned totalStatics = 0;
    bool jobRunning = false;
    bool staticsLoaded = false;
    std::vector<IndexedStatic> statics;
};

void LoadCellStatics(IndexedCell& indexed)
{
    if (indexed.staticsLoaded || !indexed.cell) return;
    indexed.staticsLoaded = true;
    auto const& statics = indexed.cell->GetStaticEntities();
    indexed.totalStatics = statics.GetCount();
    indexed.statics.reserve(indexed.totalStatics);
    for (GEU16 i = 0; i < statics.GetCount(); ++i) {
        eCEntity* entity = statics.GetAt(i);
        if (!entity) continue;
        indexed.statics.push_back({entity, reinterpret_cast<float const*>(&entity->GetWorldNodeBoundary())});
    }
}

void CollectPrefetcherCells(std::vector<eCPVSPrefetcher3*> const& prefetchers, std::vector<IndexedCell>& gridCells,
                            std::vector<IndexedCell>& permanentCells)
{
    for (eCPVSPrefetcher3* prefetcher : prefetchers) {
        if (!prefetcher) continue;
        const bool running = prefetcher->IsJobRunning() != GEFalse;
        auto* permanent = prefetcher->GetNonDeactivationCell();
        if (permanent &&
            std::find_if(permanentCells.begin(), permanentCells.end(),
                         [permanent](IndexedCell const& ref) { return ref.cell == permanent; }) == permanentCells.end()) {
            IndexedCell entry;
            entry.cell = permanent;
            entry.cellBox = reinterpret_cast<float const*>(&permanent->GetCellBox());
            entry.totalStatics = permanent->GetStaticEntities().GetCount();
            permanentCells.push_back(std::move(entry));
        }
        bTSmallArray<eCPVSCellItem*> cells;
        prefetcher->GetAllValidCells(cells);
        for (GEU16 i = 0; i < cells.GetCount(); ++i) {
            eCPVSCellItem* cell = cells.GetAt(i);
            if (!cell || cell == permanent) continue;
            auto it = std::find_if(gridCells.begin(), gridCells.end(),
                                   [cell](IndexedCell const& ref) { return ref.cell == cell; });
            if (it == gridCells.end()) {
                IndexedCell entry;
                entry.cell = cell;
                entry.cellBox = reinterpret_cast<float const*>(&cell->GetCellBox());
                entry.totalStatics = cell->GetStaticEntities().GetCount();
                entry.jobRunning = running;
                gridCells.push_back(std::move(entry));
            } else {
                it->jobRunning = it->jobRunning && running;
            }
        }
    }
}

void GatherSphereCandidates(std::vector<eCPVSPrefetcher3*> const& prefetchers, processing::Sphere const* spheres,
                            int sphereCount, std::unordered_set<eCEntity*>& desiredPinned,
                            std::vector<NpcCandidate>& candidates)
{
    pvs::ScopedCellLock cellLock;
    std::vector<IndexedCell> gridCells;
    std::vector<IndexedCell> permanentCells;
    CollectPrefetcherCells(prefetchers, gridCells, permanentCells);

    std::vector<eCEntity*> candidateEntities;
    auto scanCellEntities = [&](IndexedCell& indexed, bool isPermanent) {
        if (!isPermanent && !IntersectsAnyCircleXZ(indexed.cellBox, spheres, sphereCount, StaticCollisionMarginCm))
            return;
        LoadCellStatics(indexed);
        for (IndexedStatic const& item : indexed.statics) {
            if (!IntersectsAnySphere(item.box, spheres, sphereCount, StaticCollisionMarginCm)) continue;
            if (ShouldPinStaticCollision(ReadStaticShapeState(item.entity)))
                desiredPinned.insert(item.entity);
        }
        auto const& dynamics = indexed.cell->GetDynamicEntities();
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

    for (IndexedCell& entry : gridCells) scanCellEntities(entry, false);
    for (IndexedCell& permanent : permanentCells) scanCellEntities(permanent, true);

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

        auto inspectLocalStatics = [&](IndexedCell& indexed) {
            LoadCellStatics(indexed);
            for (IndexedStatic const& item : indexed.statics) {
                if (!BoxOverlapsSupportColumn(item.box, cand.pos[0], cand.pos[1], cand.pos[2], NpcColumnMarginCm))
                    continue;
                const StaticShapeState state = ReadStaticShapeState(item.entity);
                if (IsUncachedLocalStaticBlocker(state)) {
                    cand.hasUncachedLocalStatic = true;
                } else if (NeedsPinnedLocalPhysicObject(state)) {
                    desiredPinned.insert(item.entity);
                    cand.localRange1Statics.push_back(item.entity);
                } else if (IsSolidLocalStaticShape(state) && !state.hasPhysicObject) {
                    cand.hasMissingSolidPhysic = true;
                }
            }
        };

        bool anyCoveringReady = false;
        for (IndexedCell& entry : gridCells) {
            const bool coversCenter = BoxContainsXZ(entry.cellBox, cand.pos[0], cand.pos[2], 0.0f);
            const bool overlapsColumn = coversCenter || BoxContainsXZ(entry.cellBox, cand.pos[0], cand.pos[2], NpcColumnMarginCm);
            if (!overlapsColumn) continue;
            if (entry.jobRunning) cand.anyCoveringJobRunning = true;
            if (coversCenter) {
                ++cand.coveringCells;
                cand.coveringStaticEntities += entry.totalStatics;
                if (!entry.jobRunning) anyCoveringReady = true;
            }
            inspectLocalStatics(entry);
        }
        cand.coveringJobRunning = cand.coveringCells > 0 && !anyCoveringReady;
        for (IndexedCell& permanent : permanentCells) inspectLocalStatics(permanent);
        candidates.push_back(std::move(cand));
    }
}

void SyncPinnedPhysicObjects(std::unordered_set<eCEntity*> const& desiredPinned,
                             std::vector<NpcCandidate>& candidates)
{
    const DWORD selfThread = GetCurrentThreadId();
    std::vector<eCEntity*> toUnpin;
    std::vector<eCEntity*> toCreate;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        for (auto it = pinnedStatics.begin(); it != pinnedStatics.end();) {
            eCEntity* entity = *it;
            if (desiredPinned.find(entity) == desiredPinned.end()) {
                if (entity) toUnpin.push_back(entity);
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
            if (!state.hasPhysicObject) toCreate.push_back(entity);
        }
    }
    for (eCEntity* entity : toUnpin) {
        bool doDestroy = false;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (pinnedStatics.find(entity) == pinnedStatics.end() &&
                cachingOutStatics.find(entity) == cachingOutStatics.end() &&
                ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity))) {
                BeginPhysicOpLocked(entity, selfThread);
                doDestroy = true;
            }
        }
        if (doDestroy) {
            entity->DestroyPhysicObject();
            EndPhysicOp(entity, selfThread);
        }
    }
    for (eCEntity* entity : toCreate) {
        bool doCreate = false;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (pinnedStatics.find(entity) != pinnedStatics.end() &&
                cachingOutStatics.find(entity) == cachingOutStatics.end()) {
                const StaticShapeState state = ReadStaticShapeState(entity);
                if (ShouldPinStaticCollision(state) && !state.hasPhysicObject) {
                    BeginPhysicOpLocked(entity, selfThread);
                    doCreate = true;
                }
            }
        }
        if (doCreate) {
            entity->CreatePhysicObject();
            EndPhysicOp(entity, selfThread);
        }
    }
    std::lock_guard<std::mutex> lock(Mutex());
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

void EvaluateCandidateReadiness(std::vector<NpcCandidate> const& candidates, bool readyHooks,
                                std::vector<eCEntity const*> const& previousReady,
                                std::vector<eCEntity const*>& readyNow,
                                std::vector<std::pair<uintptr_t, bool>>& readinessSnapshot)
{
    auto& physics = eCPhysicsScene::GetInstance();
    const GEU32 rayHints = eETraceRayHints_TestStaticShapes | eETraceRayHints_IncludeWater;
    const bCVector rayDir(0.0f, -(GroundRayUpCm + LocalGroundSupportDropCm), 0.0f);

    readyNow.clear();
    readinessSnapshot.clear();
    readyNow.reserve(candidates.size());
    readinessSnapshot.reserve(candidates.size());

    for (NpcCandidate const& cand : candidates) {
        const bool wasReady = std::binary_search(previousReady.begin(), previousReady.end(), cand.entity);
        NpcCollisionGateInput gate;
        gate.hooksReady = readyHooks;
        gate.coveringCells = cand.coveringCells;
        gate.coveringStaticEntities = cand.coveringStaticEntities;
        gate.coveringJobRunning = cand.coveringJobRunning;
        gate.hasUncachedLocalStatic = cand.hasUncachedLocalStatic || cand.hasMissingSolidPhysic;
        gate.allLocalRange1Pinned = cand.allLocalRange1Pinned;
        if ((wasReady || !cand.anyCoveringJobRunning) &&
            gate.coveringCells > 0 && !(gate.coveringJobRunning && gate.coveringStaticEntities == 0) &&
            !gate.hasUncachedLocalStatic && gate.allLocalRange1Pinned) {
            const bCVector origin(cand.pos[0], cand.pos[1] + GroundRayUpCm, cand.pos[2]);
            gate.hasStaticGroundSupport = physics.TraceRayNearestHit(origin, rayDir, nullptr, rayHints) != GEFalse;
        }
        const bool ready = EvaluateNpcCollisionReady(gate) && (wasReady || !cand.anyCoveringJobRunning);
        if (ready) readyNow.push_back(cand.entity);
        readinessSnapshot.push_back({reinterpret_cast<uintptr_t>(cand.entity), ready});
    }

    std::sort(readyNow.begin(), readyNow.end());
    std::sort(readinessSnapshot.begin(), readinessSnapshot.end());
}
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
        UnpinAllOutsideLock();
        std::lock_guard<std::mutex> lock(Mutex());
        const bool hadReady = !readyRemoteNpcs.empty() || !lastSphereReadiness.empty();
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return hadReady;
    }
    const bool readyHooks = EnsureHooksInstalled();
    if (!readyHooks) {
        UnpinAllOutsideLock();
        std::lock_guard<std::mutex> lock(Mutex());
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return false;
    }

    std::vector<eCPVSPrefetcher3*> prefetchers;
    pvs::CollectReadyPrefetchers(prefetchers);

    std::unordered_set<eCEntity*> desiredPinned;
    std::vector<NpcCandidate> candidates;
    GatherSphereCandidates(prefetchers, spheres, sphereCount, desiredPinned, candidates);
    SyncPinnedPhysicObjects(desiredPinned, candidates);

    std::vector<eCEntity const*> previousReady;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        previousReady = readyRemoteNpcs;
    }

    std::vector<eCEntity const*> readyNow;
    std::vector<std::pair<uintptr_t, bool>> readinessSnapshot;
    EvaluateCandidateReadiness(candidates, readyHooks, previousReady, readyNow, readinessSnapshot);

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
    const DWORD selfThread = GetCurrentThreadId();
    std::unique_lock<std::mutex> lock(Mutex());
    PhysicOpCv().wait(lock, [&] { return !HasOtherThreadInFlightLocked(entity, selfThread); });
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
    UnpinAllOutsideLock();
    std::lock_guard<std::mutex> lock(Mutex());
    readyRemoteNpcs.clear();
    lastSphereReadiness.clear();
}

}
