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
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
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

struct CachedGroundSupport {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32_t epoch = 0;
    bool hasSupport = false;
    bool jobWasRunning = false;
};

std::unordered_set<eCEntity*> pinnedStatics;
std::vector<InFlightPhysicOp> cachingOutOps;
std::vector<InFlightPhysicOp> inFlightPhysicOps;
std::vector<eCEntity const*> readyRemoteNpcs;
std::vector<std::pair<uintptr_t, bool>> lastSphereReadiness;
std::unordered_map<eCEntity const*, CachedGroundSupport> groundSupportCache;
std::atomic<uint32_t> staticCollisionEpoch{1};

bool HasOtherThreadInFlightLocked(eCEntity const* entity, DWORD selfThread)
{
    for (InFlightPhysicOp const& op : inFlightPhysicOps) {
        if (op.entity == entity && op.threadId != selfThread) return true;
    }
    return false;
}

bool HasOtherThreadCachingOutLocked(eCEntity const* entity, DWORD selfThread)
{
    for (InFlightPhysicOp const& op : cachingOutOps) {
        if (op.entity == entity && op.threadId != selfThread) return true;
    }
    return false;
}

bool IsCachingOutLocked(eCEntity const* entity)
{
    for (InFlightPhysicOp const& op : cachingOutOps) {
        if (op.entity == entity) return true;
    }
    return false;
}

void BeginPhysicOpLocked(eCEntity* entity, DWORD selfThread)
{
    inFlightPhysicOps.push_back({entity, selfThread});
}

void EraseOneOpLocked(std::vector<InFlightPhysicOp>& ops, eCEntity const* entity, DWORD selfThread)
{
    for (auto it = ops.begin(); it != ops.end(); ++it) {
        if (it->entity == entity && it->threadId == selfThread) {
            ops.erase(it);
            return;
        }
    }
}

void EndPhysicOp(eCEntity* entity, DWORD selfThread)
{
    {
        std::lock_guard<std::mutex> lock(Mutex());
        EraseOneOpLocked(inFlightPhysicOps, entity, selfThread);
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
    if (state.dynamic || state.killed || !state.enabled || !state.collisionEnabled)
        return state;
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
        const bool isCachingOut = IsCachingOutLocked(entity);
        shouldRestore = ShouldRestorePinnedOnExitRange(isPinned, isCachingOut, ReadStaticShapeState(entity));
        if (shouldRestore) BeginPhysicOpLocked(entity, selfThread);
    }
    staticCollisionEpoch.fetch_add(1, std::memory_order_release);
    if (!shouldRestore) return;
    entity->CreatePhysicObject();
    EndPhysicOp(entity, selfThread);
}

void __fastcall CacheOutInternalDetour(eCEntity* entity, void* edx, GEBool direct)
{
    bool tracked = false;
    bool shouldDestroy = false;
    const DWORD selfThread = GetCurrentThreadId();
    if (entity && !entity->GetEntityFlags().Dynamic) {
        std::unique_lock<std::mutex> lock(Mutex());
        PhysicOpCv().wait(lock, [&] {
            return !HasOtherThreadInFlightLocked(entity, selfThread) &&
                   !HasOtherThreadCachingOutLocked(entity, selfThread);
        });
        cachingOutOps.push_back({entity, selfThread});
        BeginPhysicOpLocked(entity, selfThread);
        tracked = true;
        shouldDestroy = pinnedStatics.erase(entity) > 0 &&
                        ShouldDestroyPinnedPhysicObject(ReadStaticShapeState(entity));
    }
    if (shouldDestroy) entity->DestroyPhysicObject();
    originalCacheOutInternal(entity, edx, direct);
    if (tracked) {
        staticCollisionEpoch.fetch_add(1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(Mutex());
            EraseOneOpLocked(cachingOutOps, entity, selfThread);
            EraseOneOpLocked(inFlightPhysicOps, entity, selfThread);
        }
        PhysicOpCv().notify_all();
    }
}

void UnpinPhysicObjects(std::vector<eCEntity*> const& candidates, DWORD selfThread)
{
    if (candidates.empty()) return;
    bool changed = false;
    for (eCEntity* entity : candidates) {
        if (!entity) continue;
        bool doDestroy = false;
        {
            std::unique_lock<std::mutex> lock(Mutex());
            PhysicOpCv().wait(lock, [&] {
                return !HasOtherThreadInFlightLocked(entity, selfThread) &&
                       !HasOtherThreadCachingOutLocked(entity, selfThread);
            });
            const bool wasPinned = pinnedStatics.erase(entity) > 0;
            if (wasPinned) changed = true;
            if (wasPinned && !IsCachingOutLocked(entity) &&
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
    if (changed) staticCollisionEpoch.fetch_add(1, std::memory_order_release);
}

void UnpinAllOutsideLock()
{
    const DWORD selfThread = GetCurrentThreadId();
    static std::vector<eCEntity*> candidates;
    candidates.clear();
    {
        std::lock_guard<std::mutex> lock(Mutex());
        if (pinnedStatics.empty()) return;
        candidates.assign(pinnedStatics.begin(), pinnedStatics.end());
    }
    UnpinPhysicObjects(candidates, selfThread);
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
    StaticShapeState state{};
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
    indexed.statics.clear();
    for (GEU16 i = 0; i < statics.GetCount(); ++i) {
        eCEntity* entity = statics.GetAt(i);
        if (!entity) continue;
        const StaticShapeState state = ReadStaticShapeState(entity);
        if (!ShouldPinStaticCollision(state) && !IsSolidLocalStaticShape(state)) continue;
        indexed.statics.push_back({entity, reinterpret_cast<float const*>(&entity->GetWorldNodeBoundary()), state});
    }
}

void CollectPrefetcherCells(std::vector<eCPVSPrefetcher3*> const& prefetchers, std::vector<IndexedCell>& gridCells,
                            std::vector<IndexedCell>& permanentCells)
{
    gridCells.clear();
    permanentCells.clear();
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

void GatherSphereCandidates(eCEntityAdmin* admin, std::vector<eCPVSPrefetcher3*> const& prefetchers,
                            processing::Sphere const* spheres, int sphereCount,
                            std::vector<eCEntity*>& desiredPinned, std::vector<NpcCandidate>& candidates)
{
    pvs::ScopedCellLock cellLock;
    static std::vector<IndexedCell> gridCells;
    static std::vector<IndexedCell> permanentCells;
    static std::vector<eCEntity*> candidateEntities;
    CollectPrefetcherCells(prefetchers, gridCells, permanentCells);

    desiredPinned.clear();
    candidateEntities.clear();
    candidates.clear();
    auto scanCellEntities = [&](IndexedCell& indexed, bool isPermanent) {
        if (!isPermanent && !IntersectsAnyCircleXZ(indexed.cellBox, spheres, sphereCount, StaticCollisionMarginCm))
            return;
        LoadCellStatics(indexed);
        for (IndexedStatic const& item : indexed.statics) {
            if (!IntersectsAnySphere(item.box, spheres, sphereCount, StaticCollisionMarginCm)) continue;
            if (ShouldPinStaticCollision(item.state))
                desiredPinned.push_back(item.entity);
        }
        auto const& dynamics = indexed.cell->GetDynamicEntities();
        for (GEU16 i = 0; i < dynamics.GetCount(); ++i) {
            eCEntity* entity = dynamics.GetAt(i);
            if (!entity || !entity->IsEnabled()) continue;
            if (!entity->GetPropertySet(eEPropertySetType_NPC)) continue;
            if (npcremoval::ExcludeFromProcessing(entity)) continue;
            bCBox const& bounds = entity->GetWorldNodeBoundary();
            if (admin->IntersectsProcessingRangeRegion(bounds)) continue;
            if (!processing::IncludesNpc(false, true, reinterpret_cast<float const*>(&bounds), spheres, sphereCount))
                continue;
            candidateEntities.push_back(entity);
        }
    };

    for (IndexedCell& entry : gridCells) scanCellEntities(entry, false);
    for (IndexedCell& permanent : permanentCells) scanCellEntities(permanent, true);

    std::sort(desiredPinned.begin(), desiredPinned.end());
    desiredPinned.erase(std::unique(desiredPinned.begin(), desiredPinned.end()), desiredPinned.end());

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
                StaticShapeState const& state = item.state;
                if (IsUncachedLocalStaticBlocker(state)) {
                    cand.hasUncachedLocalStatic = true;
                } else if (NeedsPinnedLocalPhysicObject(state)) {
                    desiredPinned.push_back(item.entity);
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

    std::sort(desiredPinned.begin(), desiredPinned.end());
    desiredPinned.erase(std::unique(desiredPinned.begin(), desiredPinned.end()), desiredPinned.end());
}

void SyncPinnedPhysicObjects(std::vector<eCEntity*> const& desiredPinned,
                             std::vector<NpcCandidate>& candidates)
{
    const DWORD selfThread = GetCurrentThreadId();
    static std::vector<eCEntity*> toUnpin;
    static std::vector<eCEntity*> toCreate;
    toUnpin.clear();
    toCreate.clear();
    {
        std::lock_guard<std::mutex> lock(Mutex());
        for (eCEntity* entity : pinnedStatics) {
            if (entity && !std::binary_search(desiredPinned.begin(), desiredPinned.end(), entity))
                toUnpin.push_back(entity);
        }
        for (eCEntity* entity : desiredPinned) {
            if (!entity || IsCachingOutLocked(entity)) continue;
            if (pinnedStatics.find(entity) != pinnedStatics.end()) {
                if (!entity->GetPhysicObject()) {
                    const StaticShapeState state = ReadStaticShapeState(entity);
                    if (ShouldPinStaticCollision(state)) toCreate.push_back(entity);
                }
                continue;
            }
            const StaticShapeState state = ReadStaticShapeState(entity);
            if (!ShouldPinStaticCollision(state)) continue;
            pinnedStatics.insert(entity);
            if (!state.hasPhysicObject) toCreate.push_back(entity);
        }
    }
    UnpinPhysicObjects(toUnpin, selfThread);
    if (!toCreate.empty()) staticCollisionEpoch.fetch_add(1, std::memory_order_release);
    for (eCEntity* entity : toCreate) {
        bool doCreate = false;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (pinnedStatics.find(entity) != pinnedStatics.end() && !IsCachingOutLocked(entity)) {
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
            if (!staticEnt || IsCachingOutLocked(staticEnt) ||
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
    const uint32_t epoch = staticCollisionEpoch.load(std::memory_order_acquire);

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
            bool usedCache = false;
            if (wasReady && !cand.anyCoveringJobRunning) {
                if (const auto cached = groundSupportCache.find(cand.entity); cached != groundSupportCache.end()) {
                    const float dx = cand.pos[0] - cached->second.x;
                    const float dy = cand.pos[1] - cached->second.y;
                    const float dz = cand.pos[2] - cached->second.z;
                    if (cached->second.epoch == epoch && !cached->second.jobWasRunning && cached->second.hasSupport &&
                        (dx * dx + dy * dy + dz * dz) < 25.0f) {
                        gate.hasStaticGroundSupport = true;
                        usedCache = true;
                    }
                }
            }
            if (!usedCache) {
                const bCVector origin(cand.pos[0], cand.pos[1] + GroundRayUpCm, cand.pos[2]);
                gate.hasStaticGroundSupport = physics.TraceRayNearestHit(origin, rayDir, nullptr, rayHints) != GEFalse;
                groundSupportCache[cand.entity] = {cand.pos[0], cand.pos[1], cand.pos[2], epoch,
                                                   gate.hasStaticGroundSupport, cand.anyCoveringJobRunning};
            }
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
        groundSupportCache.clear();
        std::lock_guard<std::mutex> lock(Mutex());
        const bool hadReady = !readyRemoteNpcs.empty() || !lastSphereReadiness.empty();
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return hadReady;
    }
    const bool readyHooks = EnsureHooksInstalled();
    if (!readyHooks) {
        UnpinAllOutsideLock();
        groundSupportCache.clear();
        std::lock_guard<std::mutex> lock(Mutex());
        readyRemoteNpcs.clear();
        lastSphereReadiness.clear();
        return false;
    }

    static std::vector<eCPVSPrefetcher3*> prefetchers;
    prefetchers.clear();
    pvs::CollectReadyPrefetchers(prefetchers);

    static std::vector<eCEntity*> desiredPinned;
    static std::vector<NpcCandidate> candidates;
    GatherSphereCandidates(admin, prefetchers, spheres, sphereCount, desiredPinned, candidates);
    SyncPinnedPhysicObjects(desiredPinned, candidates);

    static std::vector<eCEntity const*> previousReady;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        previousReady = readyRemoteNpcs;
    }

    static std::vector<eCEntity const*> readyNow;
    static std::vector<std::pair<uintptr_t, bool>> readinessSnapshot;
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
    staticCollisionEpoch.fetch_add(1, std::memory_order_release);
    groundSupportCache.erase(entity);
    const DWORD selfThread = GetCurrentThreadId();
    std::unique_lock<std::mutex> lock(Mutex());
    PhysicOpCv().wait(lock, [&] {
        return !HasOtherThreadInFlightLocked(entity, selfThread) &&
               !HasOtherThreadCachingOutLocked(entity, selfThread);
    });
    pinnedStatics.erase(entity);
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
    staticCollisionEpoch.fetch_add(1, std::memory_order_release);
    groundSupportCache.clear();
    std::lock_guard<std::mutex> lock(Mutex());
    readyRemoteNpcs.clear();
    lastSphereReadiness.clear();
}

}
