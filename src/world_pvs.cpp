// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_pvs.h"

#include "ai.h"
#include "body.h"
#include "config.h"
#include "crashlog.h"
#include "guards.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcplayback.h"
#include "npcs.h"
#include "persist.h"
#include "ranged.h"
#include "relation.h"
#include "remote.h"
#include "telemetry.h"
#include "world_collision.h"
#include "world_collision_policy.h"
#include "world_retirement.h"

#include <Engine.h>
#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace g3t::world::pvs {
namespace {
struct RemoteRegionState {
    std::unique_ptr<eCCameraBase> camera;
    std::unique_ptr<eCPVSPrefetcher3> prefetcher;
    float position[3] = {};
    float radius = 0.0f;
    uint32_t frames = 0;
    bool failed = false;
    uint32_t retryAt = 0;
    uint32_t anchorRetryAt = 0;
};

struct PublishedRegion {
    eCPVSPrefetcher3* prefetcher = nullptr;
    uint32_t dbIndex = 0xffffffffu;
};

struct PublishedSnapshot {
    std::array<PublishedRegion, 32> items{};
    std::size_t count = 0;
};

std::mutex& PublishedRegionsMutex()
{
    static std::mutex mutex;
    return mutex;
}

PublishedSnapshot publishedRegions;

std::vector<RemoteRegionState> hostRegions;
unsigned regionLogs = 0;
std::atomic<unsigned> regionOwners{0};
std::vector<std::array<float, 3>> simulationAnchors;
std::vector<std::array<float, 3>> readySimulationAnchors;
uint32_t anchorsCheckedAt = 0;
unsigned remoteRegionFailures = 0;
uint32_t regionErrorShownAt = 0;
bool freeIndexTried = false;
bool freeIndexPatched = false;

using CacheOutFn = GEBool(__fastcall*)(eCPVSPrefetcher3*, void*, eCEntity*);
CacheOutFn originalCacheOut = nullptr;
std::atomic<unsigned> remoteEntityEvictions{0};
using ContextCacheOutFn = GEBool(__stdcall*)(eCGeometrySpatialContext*);
using ContextCallbackGetter = ContextCacheOutFn(__thiscall*)(eCPVSPrefetcher3 const*);
ContextCallbackGetter getContextCallback = nullptr;
std::atomic<ContextCacheOutFn> previousContextCallback{nullptr};
eCPVSPrefetcher3* contextCacheOwner = nullptr;
std::atomic<unsigned> contextsRetained{0};
std::atomic<bool> contextRefreshPending{false};

PublishedSnapshot LoadPublishedRegions()
{
    std::lock_guard<std::mutex> lock(PublishedRegionsMutex());
    return publishedRegions;
}

void PublishActiveRegions(RemoteRegionState const* retiring = nullptr)
{
    PublishedSnapshot next;
    for (auto const& region : hostRegions) {
        if (&region == retiring || !region.camera || !region.prefetcher) continue;
        if (next.count >= next.items.size()) break;
        next.items[next.count++] = {
            region.prefetcher.get(),
            region.camera->GetFrustumDatabase().GetFrustumDatabaseIndex(),
        };
    }
    std::lock_guard<std::mutex> lock(PublishedRegionsMutex());
    publishedRegions = next;
}

using DestroyEntityFn = void(__fastcall*)(eCEntity*, void*);
DestroyEntityFn originalDestroyEntity = nullptr;
bool destroyHookTried = false;
thread_local bool reportingDestroy = false;

void RegionError()
{
    if (!regionErrorShownAt || net::Now() - regionErrorShownAt >= 30000) {
        hud::Message(hud::Warning, i18n::Text(i18n::NpcRegionUnavailable));
        regionErrorShownAt = net::Now() ? net::Now() : 1;
    }
}

eCRenderSystemAdmin* RenderAdmin()
{
    return static_cast<eCRenderSystemAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCRenderSystemAdmin")));
}

uint32_t EntityDatabases(eCEntity const* entity)
{
    if (!entity) return 0;
    uint32_t databases = 0;
    std::memcpy(&databases, reinterpret_cast<unsigned char const*>(entity) + 0x16c, sizeof(databases));
    return databases;
}

bool RemoteJobsPending(PublishedSnapshot const& snapshot)
{
    for (std::size_t i = 0; i < snapshot.count; ++i)
        if (snapshot.items[i].prefetcher && snapshot.items[i].prefetcher->IsJobRunning()) return true;
    return false;
}

bool NeedsContext(eCPVSPrefetcher3& prefetcher, bCBox const& context)
{
    ScopedCellLock lock;
    bTSmallArray<eCPVSCellItem*> cells;
    prefetcher.GetAllValidCells(cells);
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        auto* cellItem = cells.GetAt(i);
        if (!cellItem) continue;
        auto const& cell = cellItem->GetCellBox();
        if (context.GetMax().GetX() >= cell.GetMin().GetX() - cell.GetWidth() &&
            context.GetMin().GetX() <= cell.GetMax().GetX() + cell.GetWidth() &&
            context.GetMax().GetZ() >= cell.GetMin().GetZ() - cell.GetDepth() &&
            context.GetMin().GetZ() <= cell.GetMax().GetZ() + cell.GetDepth()) return true;
    }
    return false;
}

GEBool __stdcall MayReleaseContext(eCGeometrySpatialContext* context)
{
    auto previous = previousContextCallback.load();
    if (GatePaused() || ai::ShuttingDown()) {
        if (regionOwners.load()) return GEFalse;
        return previous ? previous(context) : GETrue;
    }
    const PublishedSnapshot snapshot = LoadPublishedRegions();
    if (RemoteJobsPending(snapshot)) {
        contextRefreshPending.store(true, std::memory_order_relaxed);
        contextsRetained.fetch_add(1, std::memory_order_relaxed);
        return GEFalse;
    }
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        if (snapshot.items[i].prefetcher && NeedsContext(*snapshot.items[i].prefetcher, context->GetContextBox())) {
            contextsRetained.fetch_add(1, std::memory_order_relaxed);
            return GEFalse;
        }
    }
    return previous ? previous(context) : GETrue;
}

GEBool __stdcall KeepSharedContext(eCGeometrySpatialContext*)
{
    return GEFalse;
}

bool InstallContextOwnership(eCPVSPrefetcher3& prefetcher)
{
    if (contextCacheOwner == &prefetcher) return true;
    if (contextCacheOwner) { Log("FAIL world: spatial-context cache owner changed without retirement"); return false; }
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* entry = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?GetContextCacheOutCallback@eCPVSPrefetcher3@@QBEP6G_NPAVeCGeometrySpatialContext@@@ZXZ")) : nullptr;
    const unsigned char expected[] = {0x8b, 0x41, 0x4c, 0xc3};
    if (!entry || std::memcmp(entry, expected, sizeof(expected))) {
        Log("FAIL world: unsupported spatial-context callback ABI; remote streaming refused");
        return false;
    }
    getContextCallback = reinterpret_cast<ContextCallbackGetter>(entry);
    previousContextCallback = getContextCallback(&prefetcher);
    contextCacheOwner = &prefetcher;
    prefetcher.SetContextCacheOutCallback(&MayReleaseContext);
    return true;
}

void RestoreContextOwnership()
{
    if (!contextCacheOwner) return;
    if (getContextCallback(contextCacheOwner) == &MayReleaseContext)
        contextCacheOwner->SetContextCacheOutCallback(previousContextCallback.load());
    else Log("WARN world: spatial-context callback changed externally; not overwriting it");
    contextCacheOwner = nullptr;
    previousContextCallback = nullptr;
    contextRefreshPending.store(false, std::memory_order_relaxed);
}

void EvictFromPrefetcher(eCPVSPrefetcher3& prefetcher, eCEntity* entity, uint32_t index)
{
    ScopedCellLock lock;
    prefetcher.RemoveNewAddedEntity(entity);
    bTSmallArray<eCPVSCellItem*> cells;
    prefetcher.GetAllValidCells(cells);
    auto* permanent = prefetcher.GetNonDeactivationCell();
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        auto* cell = cells.GetAt(i);
        if (cell && cell != permanent && cell->ExistsEntity(entity)) {
            cell->CacheOutEntity(entity, index);
            remoteEntityEvictions.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (permanent && permanent->ExistsEntity(entity)) {
        permanent->CacheOutEntity(entity, index);
        remoteEntityEvictions.fetch_add(1, std::memory_order_relaxed);
    }
}

void EvictPublishedSlot(PublishedSnapshot const& snapshot, eCPVSPrefetcher3* mainPrefetcher, eCEntity* entity,
                        uint32_t dbIndex)
{
    if (!entity || dbIndex >= 32) return;
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        if (snapshot.items[i].prefetcher) EvictFromPrefetcher(*snapshot.items[i].prefetcher, entity, dbIndex);
    }
    if (mainPrefetcher) EvictFromPrefetcher(*mainPrefetcher, entity, dbIndex);
}

GEBool __fastcall CacheOut(eCPVSPrefetcher3* self, void* edx, eCEntity* entity)
{
    auto* renderer = RenderAdmin();
    auto* mainPrefetcher = renderer ? &renderer->GetPVSPrefetcher3() : nullptr;
    const PublishedSnapshot snapshot = LoadPublishedRegions();
    if (mainPrefetcher && self == mainPrefetcher) {
        for (std::size_t i = 0; i < snapshot.count; ++i)
            EvictPublishedSlot(snapshot, nullptr, entity, snapshot.items[i].dbIndex);
    } else {
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            if (self == snapshot.items[i].prefetcher) {
                EvictPublishedSlot(snapshot, mainPrefetcher, entity, snapshot.items[i].dbIndex);
                return GETrue;
            }
        }
    }
    return originalCacheOut(self, edx, entity);
}

int32_t __fastcall FixedGetFreeIndex(collision::NativeIndexPtrMap* map, void*)
{
    return map ? collision::PopFreeIndex(*map) : -1;
}

bool InstallRemoteCacheOut()
{
    if (originalCacheOut) return true;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* code = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CacheOut@eCPVSPrefetcher3@@QAE_NPAVeCEntity@@@Z")) : nullptr;
    const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (!code || std::memcmp(code, expected, sizeof(expected))) {
        Log("FAIL world: native entity cache-out interception is unavailable; remote streaming refused");
        return false;
    }
    originalCacheOut = reinterpret_cast<CacheOutFn>(ai::JumpTo(code, reinterpret_cast<void*>(&CacheOut), sizeof(expected)));
    if (!originalCacheOut) Log("FAIL world: native remote cache-out hook could not be installed");
    return originalCacheOut != nullptr;
}

bool CompletePrefetcher(eCPVSPrefetcher3& prefetcher, char const* phase)
{
    eCPVSPrefetcherJob* job = nullptr;
    std::memcpy(&job, reinterpret_cast<unsigned char const*>(&prefetcher) + 0xc, sizeof(job));
    if (!job) {
        prefetcher.WaitForEmptyThreadQueue();
        Logf("world: %s prefetcher has no active job", phase);
        return true;
    }
    job->DisableTimeSlice();
    const DWORD deadline = GetTickCount() + 5000;
    unsigned iterations = 0;
    while (!job->IsFinished() && static_cast<int32_t>(deadline - GetTickCount()) > 0 && iterations < 1000) {
        job->Process();
        ++iterations;
        if (!job->IsFinished()) SwitchToThread();
    }
    if (!job->IsFinished()) {
        Logf("FAIL world: %s prefetch job unfinished after %u bounded iterations", phase, iterations);
        return false;
    }
    prefetcher.WaitForEmptyThreadQueue();
    Logf("world: %s prefetch job completed after %u iterations", phase, iterations);
    return true;
}

struct CellInventory {
    unsigned cells = 0, statics = 0, dynamics = 0;
    eCPVSCellItem* permanent = nullptr;
    bool Empty() const { return !statics && !dynamics; }
};

bool HasReadyAnchorCell(eCPVSPrefetcher3& prefetcher, float const anchor[3])
{
    ScopedCellLock lock;
    bTSmallArray<eCPVSCellItem*> cells;
    prefetcher.GetAllValidCells(cells);
    auto* permanent = prefetcher.GetNonDeactivationCell();
    const bool jobDone = !prefetcher.IsJobRunning();
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        auto* cell = cells.GetAt(i);
        if (!cell || cell == permanent) continue;
        auto const* box = reinterpret_cast<float const*>(&cell->GetCellBox());
        if (!collision::BoxContainsXZ(box, anchor[0], anchor[2], 0.0f)) continue;
        if (jobDone || cell->GetStaticEntities().GetCount() > 0) return true;
    }
    return false;
}

CellInventory Inventory(eCPVSPrefetcher3* prefetcher)
{
    CellInventory result;
    if (!prefetcher) return result;
    ScopedCellLock lock;
    bTSmallArray<eCPVSCellItem*> cells;
    prefetcher->GetAllValidCells(cells);
    result.permanent = prefetcher->GetNonDeactivationCell();
    auto add = [&](eCPVSCellItem* cell) {
        if (!cell) return;
        ++result.cells;
        result.statics += cell->GetStaticEntities().GetCount();
        result.dynamics += cell->GetDynamicEntities().GetCount();
    };
    for (GEU16 i = 0; i < cells.GetCount(); ++i)
        if (cells.GetAt(i) != result.permanent) add(cells.GetAt(i));
    add(result.permanent);
    return result;
}

std::size_t SlotEntityCount(eCPVSPrefetcher3* prefetcher, uint32_t index)
{
    if (!prefetcher || index >= 32) return 0;
    ScopedCellLock lock;
    bTSmallArray<eCPVSCellItem*> cells;
    prefetcher->GetAllValidCells(cells);
    auto scanCell = [](eCPVSCellItem const& cell, uint32_t bit) {
        std::size_t count = 0;
        auto const& statics = cell.GetStaticEntities();
        for (GEU16 i = 0; i < statics.GetCount(); ++i)
            if (EntityDatabases(statics.GetAt(i)) & bit) ++count;
        auto const& dynamics = cell.GetDynamicEntities();
        for (GEU16 i = 0; i < dynamics.GetCount(); ++i)
            if (EntityDatabases(dynamics.GetAt(i)) & bit) ++count;
        return count;
    };
    return retirement::CountSlotEntities(cells.GetCount(), [&](std::size_t i) { return cells.GetAt(static_cast<GEU16>(i)); },
                                         prefetcher->GetNonDeactivationCell(), index, scanCell);
}

void LogRegion(RemoteRegionState const& region, char const* phase)
{
    auto cells = Inventory(region.prefetcher.get());
    Logf("world-cache: %s camera=%p prefetcher=%p database=%u cells=%u static=%u dynamic=%u permanent=%p failed=%d",
         phase, region.camera.get(), region.prefetcher.get(),
         region.camera ? region.camera->GetFrustumDatabase().GetFrustumDatabaseIndex() : 0xffffffffu,
         cells.cells, cells.statics, cells.dynamics, cells.permanent, region.failed);
}

struct RegionRetirement {
    RemoteRegionState& region;
    eCRenderSystemAdmin& renderer;
    std::vector<eCPVSPrefetcher3*> siblingPrefetchers;

    bool CompleteJobs()
    {
        if (region.prefetcher) {
            region.prefetcher->EnablePrefetcher(GEFalse);
            if (!CompletePrefetcher(*region.prefetcher, "retired remote")) return false;
        }
        for (eCPVSPrefetcher3* sibling : siblingPrefetchers) {
            if (sibling && !CompletePrefetcher(*sibling, "sibling remote")) return false;
        }
        return CompletePrefetcher(renderer.GetPVSPrefetcher3(), "main renderer");
    }
    bool FlushQueues()
    {
        if (renderer.FlushThreadQueues()) return true;
        Log("FAIL world: remote retirement render queues did not flush; preserving owners");
        return false;
    }
    bool ClearCells()
    {
        auto& database = region.camera->GetFrustumDatabase();
        const uint32_t index = database.GetFrustumDatabaseIndex();
        if (index == 0xffffffffu) return true;
        if (index >= 32) {
            Logf("FAIL world: unsupported remote database index %u; preserving owners", index);
            return false;
        }
        database.RefreshFrustumData();
        auto drainPrefetcher = [](eCPVSPrefetcher3& prefetcher, eCFrustumDatabase& db, uint32_t slot) {
            ScopedCellLock lock;
            bTSmallArray<eCPVSCellItem*> cells;
            prefetcher.GetAllValidCells(cells);
            // Release every grid and permanent cell across active prefetchers BEFORE losing the registered database slot index.
            if (!retirement::DrainSlots(cells.GetCount(), [&](std::size_t i) { return cells.GetAt(static_cast<GEU16>(i)); },
                                        prefetcher.GetNonDeactivationCell(), slot))
                return false;
            prefetcher.ClearCells(db);
            return true;
        };
        return retirement::DrainPrefetcherGroup(region.prefetcher.get(), siblingPrefetchers.data(),
                                                siblingPrefetchers.size(), renderer.GetPVSPrefetcher3(), database,
                                                index, drainPrefetcher);
    }
    bool CellsEmpty()
    {
        LogRegion(region, "drained");
        const uint32_t index = region.camera->GetFrustumDatabase().GetFrustumDatabaseIndex();
        const auto privateCells = Inventory(region.prefetcher.get());
        const std::size_t privateSlot = index < 32 ? SlotEntityCount(region.prefetcher.get(), index) : 0;
        std::size_t siblingSlot = 0;
        if (index < 32) {
            for (eCPVSPrefetcher3* sibling : siblingPrefetchers) siblingSlot += SlotEntityCount(sibling, index);
        }
        const std::size_t mainSlot = index < 32 ? SlotEntityCount(&renderer.GetPVSPrefetcher3(), index) : 0;
        if (privateCells.Empty() && privateSlot == 0 && siblingSlot == 0 && mainSlot == 0) return true;
        Logf("FAIL world: PVS slot %u still owns entities (private=%u/%u private_slot=%zu sibling_slot=%zu main_slot=%zu); database/camera/prefetcher preserved",
             index, privateCells.statics, privateCells.dynamics, privateSlot, siblingSlot, mainSlot);
        return false;
    }
    bool Unregister()
    {
        auto& database = region.camera->GetFrustumDatabase();
        const uint32_t index = database.GetFrustumDatabaseIndex();
        if (index == 0xffffffffu) return true;
        const bool unregistered = renderer.UnregisterDatabase(database);
        const uint32_t after = database.GetFrustumDatabaseIndex();
        if (unregistered && after == 0xffffffffu) return true;
        Logf("FAIL world: remote database %u unregister result=%d after=%u; preserving owners", index, unregistered, after);
        return false;
    }
    void DestroyOwners()
    {
        PublishActiveRegions(&region);
        region.camera.reset();
        region.prefetcher.reset();
        if (!renderer.FlushThreadQueues()) Log("FAIL world: remote camera destruction queues did not flush");
        regionOwners.fetch_sub(1);
        region = RemoteRegionState{};
    }
};

void ReleaseRegion(RemoteRegionState& region)
{
    if (!region.camera && !region.prefetcher) { region = RemoteRegionState{}; return; }
    LogRegion(region, "retiring");
    auto* renderer = RenderAdmin();
    bool retired = false;
    if (renderer && region.camera) {
        std::vector<eCPVSPrefetcher3*> siblings;
        siblings.reserve(hostRegions.size());
        for (auto const& other : hostRegions) {
            if (&other != &region && other.prefetcher) siblings.push_back(other.prefetcher.get());
        }
        RegionRetirement operations{region, *renderer, std::move(siblings)};
        retired = retirement::Retire(operations);
    } else Log("FAIL world: remote retirement has no render administration or camera");
    if (!retired) {
        region.failed = true;
        region.retryAt = net::Now() + 5000;
        Log("FAIL world: retirement unsafe; no owner destroyed");
    } else Log("world-cache: retired remote slot before destroying camera/prefetcher");
    contextRefreshPending.store(true, std::memory_order_relaxed);
}

bool MarkRegionFailed(RemoteRegionState& region, char const* logMessage = nullptr)
{
    region.failed = true;
    region.retryAt = net::Now() + 5000;
    ++remoteRegionFailures;
    if (logMessage) Log(logMessage);
    RegionError();
    return false;
}

bool PlaceRegion(RemoteRegionState& region, float const at[3], float radius)
{
    if (GatePaused() || ai::ShuttingDown()) return false;
    if (region.failed) {
        if (static_cast<int32_t>(net::Now() - region.retryAt) < 0) return false;
        ReleaseRegion(region);
        if (region.failed) return false;
    }
    if (!region.camera) {
        auto* renderer = RenderAdmin();
        if (!renderer || !InstallDatabaseIndexFix() || !InstallRemoteCacheOut() ||
            !InstallContextOwnership(renderer->GetPVSPrefetcher3())) {
            return MarkRegionFailed(region);
        }
        auto prefetcher = std::make_unique<eCPVSPrefetcher3>();
        if (prefetcher->Create() != bEResult_Ok) {
            return MarkRegionFailed(region, "FAIL world: cannot create an isolated remote prefetcher");
        }
        prefetcher->SetContextCacheOutCallback(&KeepSharedContext);
        auto camera = std::make_unique<eCCameraBase>();
        if (camera->Create(GEFalse, GETrue) != bEResult_Ok ||
            camera->Init(bCRect(0, 0, 32, 32), 60.0f, 1.0f, radius) != bEResult_Ok ||
            camera->GetFrustumDatabase().GetFrustumDatabaseIndex() == 0xffffffffu) {
            auto& database = camera->GetFrustumDatabase();
            if (database.GetFrustumDatabaseIndex() != 0xffffffffu) {
                renderer->UnregisterDatabase(database);
                renderer->FlushThreadQueues();
            }
            return MarkRegionFailed(region, "FAIL world: cannot register a remote simulation camera; no client-simulation fallback");
        }
        region.camera = std::move(camera);
        region.prefetcher = std::move(prefetcher);
        regionOwners.fetch_add(1);
        region.radius = 0.0f;
        PublishActiveRegions();
        Logf("world: remote simulation region created (database %u) for a player %.0f,%.0f,%.0f radius %.0f cm",
             region.camera->GetFrustumDatabase().GetFrustumDatabaseIndex(), at[0], at[1], at[2], radius);
    }
    auto* renderer = RenderAdmin();
    const uint32_t now = net::Now();
    const bool moved = body::Distance(at, region.position) >= 500.0f || region.radius != radius;
    const bool coveredByMain = renderer && HasReadyAnchorCell(renderer->GetPVSPrefetcher3(), at);
    const bool anchorMissing = region.prefetcher && !region.prefetcher->IsJobRunning() &&
                               !HasReadyAnchorCell(*region.prefetcher, at) && !coveredByMain &&
                               (!region.anchorRetryAt || static_cast<int32_t>(now - region.anchorRetryAt) >= 0);
    if (moved || anchorMissing) {
        if (moved && region.frames && regionLogs < 24) {
            ++regionLogs;
            Logf("world: remote simulation region moved to %.0f,%.0f,%.0f radius %.0f cm (%u frames)", at[0], at[1], at[2], radius,
                 region.frames);
        }
        if (moved) contextRefreshPending.store(true, std::memory_order_relaxed);
        region.camera->SetClipDepth(bCVector2(1.0f, radius));
        region.camera->SetPosition(bCVector(at[0], at[1], at[2]));
        auto& database = region.camera->GetFrustumDatabase();
        database.Update();
        database.ForcePrefetch();
        std::memcpy(region.position, at, sizeof(region.position));
        region.radius = radius;
        region.anchorRetryAt = (now ? now : 1) + 500u;
    }
    if (!renderer || !region.prefetcher || !region.prefetcher->Process(region.camera->GetFrustumDatabase())) {
        return MarkRegionFailed(region, "FAIL world: remote simulation prefetch refused; no client-simulation fallback");
    }
    ++region.frames;
    return true;
}

void __fastcall DestroyEntity(eCEntity* entity, void* edx)
{
    if (entity && entity->GetEntityFlags().m_CachedIn && !reportingDestroy) {
        reportingDestroy = true;
        static_assert(offsetof(eCEntity, m_EntityFlags) == 0x3c, "native entity flag offset");
        uint32_t flags = 0;
        std::memcpy(&flags, &entity->GetEntityFlags(), sizeof(flags));
        const uint32_t databases = EntityDatabases(entity);
        Logf("world-cache: destroy cached entity=%p flags=%08x databases=%08x paused=%d owners=%u shutdown=%d",
             entity, flags, databases, GatePaused(), regionOwners.load(), ai::ShuttingDown());
        crashlog::LogCacheInventory("cached entity destruction");
        crashlog::NativeStack("eCEntity::Destroy cached-in invariant");
        reportingDestroy = false;
    }
    hud::OnEntityDestroyed(entity);
    npcs::OnEntityDestroyed(entity);
    ranged::ClearPuppetOverlay(entity);
    npcplayback::OnEntityDestroyed(entity);
    npclease::OnEntityDestroyed(entity);
    telemetry::OnEntityDestroyed(entity);
    guards::OnEntityDestroyed(entity);
    relation::OnEntityDestroyed(entity);
    collision::OnEntityDestroyed(entity);
    originalDestroyEntity(entity, edx);
}
}

bool InstallDatabaseIndexFix()
{
    if (freeIndexTried) return freeIndexPatched;
    freeIndexTried = true;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* code = engine ? reinterpret_cast<unsigned char*>(engine) + 0x1d7c80 : nullptr;
    if (!collision::VerifyFreeIndexSignature(code)) {
        Log("FAIL world-cache: unsupported bTIndexPtrMap::GetFreeIndex; remote slot protection unavailable");
        return false;
    }
    freeIndexPatched = ai::JumpTo(code, reinterpret_cast<void*>(&FixedGetFreeIndex), 5) != nullptr;
    Log(freeIndexPatched ? "world-cache: native database free-index fix installed" :
                           "FAIL world-cache: native database free-index fix could not be installed");
    return freeIndexPatched;
}

std::vector<std::array<float, 3>> const& SimulationAnchors() { return simulationAnchors; }

std::vector<std::array<float, 3>> const& ReadySimulationAnchors() { return readySimulationAnchors; }

void CollectReadyPrefetchers(std::vector<eCPVSPrefetcher3*>& out)
{
    out.clear();
    if (auto* renderer = RenderAdmin()) out.push_back(&renderer->GetPVSPrefetcher3());
    for (auto const& region : hostRegions)
        if (!region.failed && region.camera && region.prefetcher) out.push_back(region.prefetcher.get());
}

std::size_t ActiveRegionCount() { return hostRegions.size(); }

void RefreshContexts()
{
    if (!contextRefreshPending.load(std::memory_order_relaxed) || RemoteJobsPending(LoadPublishedRegions())) return;
    eCApplication::GetInstance().GetCurrentCamera().GetFrustumDatabase().ForcePrefetch();
    contextRefreshPending.store(false, std::memory_order_relaxed);
}

namespace {
struct AnchorGroup {
    std::array<float, 3> low, high;
};

void RefreshSimulationAnchors()
{
    if (anchorsCheckedAt && net::Now() - anchorsCheckedAt < 250) return;
    anchorsCheckedAt = net::Now();
    simulationAnchors.clear();
    auto add = [](bCVector const& position) {
        simulationAnchors.push_back({position.GetX(), position.GetY(), position.GetZ()});
    };
    auto const* remotes = net::Remotes();
    for (int id = 2; id < net::MaxPlayers; ++id) {
        auto const& peer = remotes[id];
        if (peer.connected && (peer.latest.flags & 1) && peer.latest.world == remote::CurrentWorld())
            simulationAnchors.push_back({peer.latest.position[0], peer.latest.position[1], peer.latest.position[2]});
    }
    persist::AppendPendingAdmissionAnchors(simulationAnchors);
    std::vector<eCEntity*> guides;
    for (int id = 1; id < net::MaxPlayers; ++id) {
        eCEntity* player = id == 1 ? remote::ControlledBody() : remote::PuppetOf(static_cast<uint8_t>(id));
        if (!player || !player->GetPropertySet(eEPropertySetType_Party)) continue;
        if (auto* guide = Entity(player).Party.GetPartyLeader().GetInstance()) guides.push_back(guide);
    }
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* entity = npc.GetInstance();
        if (!entity || entity->GetPropertySet(eEPropertySetType_PlayerMemory) || remote::IsPuppet(entity)) continue;
        if (std::find(guides.begin(), guides.end(), entity) != guides.end()) { add(entity->GetWorldPosition()); continue; }
        if (!entity->GetPropertySet(eEPropertySetType_Party) || npc.Party.Waiting) continue;
        if (entity->GetPropertySet(eEPropertySetType_DamageReceiver) && npc.DamageReceiver.HitPoints <= 0) continue;
        const uint8_t owner = npcs::CompanionOwner(entity);
        if (owner == 1 || (owner > 1 && remotes[owner].connected && net::InWorld(owner))) add(entity->GetWorldPosition());
    }
}

void ClusterAnchorGroups(float const here[3], float mainFarClip, std::vector<AnchorGroup>& groups,
                         std::vector<size_t>& anchorGroups)
{
    groups.clear();
    anchorGroups.assign(simulationAnchors.size(), static_cast<size_t>(-1));
    for (size_t idx = 0; idx < simulationAnchors.size(); ++idx) {
        auto const& anchor = simulationAnchors[idx];
        if (body::Distance(here, anchor.data()) + GetConfig().hostRoi + 1000.0f < mainFarClip) continue;
        size_t group = groups.size();
        for (size_t previous = 0; previous < groups.size(); ++previous) {
            float centre[3];
            for (int k = 0; k < 3; ++k) centre[k] = (groups[previous].low[k] + groups[previous].high[k]) * 0.5f;
            if (body::Distance(centre, anchor.data()) <= GetConfig().hostRoi * 0.5f) { group = previous; break; }
        }
        if (group == groups.size()) {
            groups.push_back({anchor, anchor});
        } else {
            for (int k = 0; k < 3; ++k) {
                groups[group].low[k] = std::min(groups[group].low[k], anchor[k]);
                groups[group].high[k] = std::max(groups[group].high[k], anchor[k]);
            }
        }
        anchorGroups[idx] = group;
    }
}

void SyncActiveRegions(std::vector<AnchorGroup> const& groups)
{
    for (size_t i = groups.size(); i < hostRegions.size(); ++i) {
        if (collision::ShouldAttemptRegionRelease(hostRegions[i].failed, net::Now(), hostRegions[i].retryAt))
            ReleaseRegion(hostRegions[i]);
    }
    size_t retained = groups.size();
    for (size_t i = groups.size(); i < hostRegions.size(); ++i)
        if (hostRegions[i].camera || hostRegions[i].prefetcher) retained = i + 1;
    hostRegions.resize(retained);
    PublishActiveRegions();
    for (size_t id = 0; id < groups.size(); ++id) {
        float centre[3];
        for (int k = 0; k < 3; ++k) centre[k] = (groups[id].low[k] + groups[id].high[k]) * 0.5f;
        const float radius = GetConfig().hostRoi + body::Distance(groups[id].low.data(), groups[id].high.data()) * 0.5f + 1500.0f;
        PlaceRegion(hostRegions[id], centre, radius);
    }
    PublishActiveRegions();
}

void UpdateReadySimulationAnchors(eCPVSPrefetcher3& mainPrefetcher, std::vector<size_t> const& anchorGroups)
{
    readySimulationAnchors.clear();
    readySimulationAnchors.reserve(simulationAnchors.size());
    for (size_t idx = 0; idx < simulationAnchors.size(); ++idx) {
        auto const& anchor = simulationAnchors[idx];
        const size_t group = anchorGroups[idx];
        const bool withinMain = group == static_cast<size_t>(-1);
        const bool mainCellReady = HasReadyAnchorCell(mainPrefetcher, anchor.data());
        const bool mainReady = withinMain && mainCellReady;
        bool remoteReady = false;
        if (!withinMain && group < hostRegions.size()) {
            auto const& region = hostRegions[group];
            // Main PVS cells still hold sole ownership near the host; reuse their ready coverage while remote cells populate.
            const bool cellReady = (region.prefetcher && HasReadyAnchorCell(*region.prefetcher, anchor.data())) ||
                                   mainCellReady;
            remoteReady = collision::IsRemoteRegionReady(region.failed, region.camera != nullptr,
                                                         region.prefetcher != nullptr, cellReady);
        }
        if (collision::ShouldIncludeSimulationAnchor(withinMain, mainReady, remoteReady))
            readySimulationAnchors.push_back(anchor);
    }
}
} // namespace

void TickHostRegions()
{
    auto* renderer = RenderAdmin();
    if (!renderer) return;
    auto const& camera = eCApplication::GetInstance().GetCurrentCamera();
    const bCVector& p = camera.GetPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    RefreshSimulationAnchors();
    static std::vector<AnchorGroup> groups;
    static std::vector<size_t> anchorGroups;
    groups.clear();
    anchorGroups.clear();
    ClusterAnchorGroups(here, camera.GetClipDepth().GetY(), groups, anchorGroups);
    SyncActiveRegions(groups);
    UpdateReadySimulationAnchors(renderer->GetPVSPrefetcher3(), anchorGroups);
}

void RetireHostRegions()
{
    for (auto& region : hostRegions) ReleaseRegion(region);
    for (size_t i = 0; i < hostRegions.size();) {
        if (!hostRegions[i].camera && !hostRegions[i].prefetcher && !hostRegions[i].failed)
            hostRegions.erase(hostRegions.begin() + i);
        else
            ++i;
    }
    simulationAnchors.clear();
    readySimulationAnchors.clear();
    anchorsCheckedAt = 0;
    PublishActiveRegions();
    if (hostRegions.empty()) RestoreContextOwnership();
}

void FillRegionStatus(HostSimStatus& status)
{
    status.anchors = static_cast<unsigned>(simulationAnchors.size());
    for (auto const& region : hostRegions) {
        if (region.failed) ++status.regionsFailed;
        else if (region.camera) ++status.regions;
        const CellInventory cells = Inventory(region.prefetcher.get());
        status.regionCells += cells.cells;
        status.regionStatics += cells.statics;
        status.regionDynamics += cells.dynamics;
    }
}

void CaptureRegionCacheInventory(bTSmallArray<eCEntity*> const* range, void const* spheresData, unsigned sphereSize)
{
    unsigned cameras = 0, prefetchers = 0, populated = 0;
    for (auto const& region : hostRegions) {
        cameras += region.camera != nullptr;
        prefetchers += region.prefetcher != nullptr;
        auto cells = Inventory(region.prefetcher.get());
        populated += cells.statics + cells.dynamics;
    }
    crashlog::CacheObjects(crashlog::CacheKind::Cameras, cameras, hostRegions.empty() ? nullptr : hostRegions.front().camera.get());
    crashlog::CacheObjects(crashlog::CacheKind::Prefetchers, prefetchers, hostRegions.empty() ? nullptr : hostRegions.front().prefetcher.get());
    crashlog::CacheObjects(crashlog::CacheKind::PrivateEntities, populated, contextCacheOwner);
    crashlog::CacheObjects(crashlog::CacheKind::Spheres, sphereSize, spheresData);
    crashlog::CacheObjects(crashlog::CacheKind::Anchors, static_cast<unsigned>(simulationAnchors.size()),
                           simulationAnchors.empty() ? nullptr : simulationAnchors.data());
    crashlog::CacheObjects(crashlog::CacheKind::ProcessingRange, range ? range->GetCount() : 0, range);
}

void StartDestroyDiagnostics()
{
    if (destroyHookTried || ai::ShuttingDown()) return;
    destroyHookTried = true;
    auto engine = GetModuleHandleA("Engine.dll");
    auto* code = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine, "?Destroy@eCEntity@@UAEXXZ")) : nullptr;
    const unsigned char expected[] = {0x51, 0x56, 0x57, 0x8b, 0xf1};
    if (!code || std::memcmp(code, expected, sizeof(expected))) {
        Log("FAIL world-cache: unsupported eCEntity::Destroy entry; native invariant diagnostics unavailable");
        return;
    }
    originalDestroyEntity = reinterpret_cast<DestroyEntityFn>(ai::JumpTo(code, reinterpret_cast<void*>(&DestroyEntity), sizeof(expected)));
    Log(originalDestroyEntity ? "world-cache: native destruction diagnostics installed" :
                                "FAIL world-cache: native destruction diagnostics hook could not be installed");
}
}
