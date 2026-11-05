// SPDX-License-Identifier: GPL-3.0-or-later
#include "world.h"
#include "log.h"
#include "hud.h"
#include "i18n.h"

#include "body.h"
#include "ai.h"
#include "config.h"
#include "net.h"
#include "npcs.h"
#include "processing.h"
#include "qa.h"
#include "remote.h"

#include <Engine.h>
#include <Game.h>
#include <Script.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <array>
#include <algorithm>
#include <vector>
#include <Windows.h>

namespace g3t::world {
namespace {
struct StreamProbeState {
    std::unique_ptr<eCCameraBase> camera;
    std::unique_ptr<eCPVSPrefetcher3> prefetcher;
    float position[3] = {};
    float radius = 0.0f;
    uint32_t frames = 0;
    bool failed = false;
    uint32_t retryAt = 0;
};
StreamProbeState streamProbe;
std::vector<StreamProbeState> hostRegions;
std::vector<std::array<float, 3>> simulationAnchors;
uint32_t anchorsCheckedAt = 0;
unsigned remoteRegionFailures = 0;
uint32_t regionErrorShownAt = 0;

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

using CacheOutFn = GEBool(__fastcall*)(eCPVSPrefetcher3*, void*, eCEntity*);
CacheOutFn originalCacheOut = nullptr;
using DestroyEntityFn = void(__fastcall*)(eCEntity*, void*);
DestroyEntityFn originalDestroyEntity = nullptr;
unsigned remoteEntityEvictions = 0;
using ContextCacheOutFn = GEBool(__stdcall*)(eCGeometrySpatialContext*);
using ContextCallbackGetter = ContextCacheOutFn(__thiscall*)(eCPVSPrefetcher3 const*);
ContextCallbackGetter getContextCallback = nullptr;
ContextCacheOutFn previousContextCallback = nullptr;
eCPVSPrefetcher3* contextCacheOwner = nullptr;
unsigned contextsRetained = 0;
bool contextRefreshPending = false;

bool RemoteJobsPending()
{
    if (streamProbe.prefetcher && streamProbe.prefetcher->IsJobRunning()) return true;
    for (auto& region : hostRegions)
        if (region.prefetcher && region.prefetcher->IsJobRunning()) return true;
    return false;
}

void RefreshContexts()
{
    if (!contextRefreshPending || RemoteJobsPending()) return;
    eCApplication::GetInstance().GetCurrentCamera().GetFrustumDatabase().ForcePrefetch();
    contextRefreshPending = false;
}

bool NeedsContext(StreamProbeState& region, bCBox const& context)
{
    if (!region.camera || !region.prefetcher || region.failed) return false;
    bTSmallArray<eCPVSCellItem*> cells;
    region.prefetcher->GetValidCells(region.camera->GetFrustumDatabase(), cells);
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        auto const& cell = cells.GetAt(i)->GetCellBox();
        // Execute expands context bounds by one grid cell before testing the active rectangle.
        if (context.GetMax().GetX() >= cell.GetMin().GetX() - cell.GetWidth() &&
            context.GetMin().GetX() <= cell.GetMax().GetX() + cell.GetWidth() &&
            context.GetMax().GetZ() >= cell.GetMin().GetZ() - cell.GetDepth() &&
            context.GetMin().GetZ() <= cell.GetMax().GetZ() + cell.GetDepth()) return true;
    }
    return false;
}

GEBool __stdcall MayReleaseContext(eCGeometrySpatialContext* context)
{
    if (RemoteJobsPending()) {
        contextRefreshPending = true;
        ++contextsRetained;
        return GEFalse;
    }
    if (NeedsContext(streamProbe, context->GetContextBox())) { ++contextsRetained; return GEFalse; }
    for (auto& region : hostRegions)
        if (NeedsContext(region, context->GetContextBox())) { ++contextsRetained; return GEFalse; }
    return previousContextCallback ? previousContextCallback(context) : GETrue;
}

GEBool __stdcall KeepSharedContext(eCGeometrySpatialContext*)
{
    // Spatial contexts are global, even when the requesting prefetcher has a private cell map.
    // Only the main prefetcher may retire them, after checking every live remote region.
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
    // The SDK getter has the wrong signature; the checked export takes no argument and returns this callback.
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
        contextCacheOwner->SetContextCacheOutCallback(previousContextCallback);
    else Log("WARN world: spatial-context callback changed externally; not overwriting it");
    contextCacheOwner = nullptr;
    previousContextCallback = nullptr;
    contextRefreshPending = false;
}

void EvictRemoteEntity(StreamProbeState& region, eCEntity* entity)
{
    if (!region.camera || !region.prefetcher || !entity) return;
    const uint32_t index = region.camera->GetFrustumDatabase().GetFrustumDatabaseIndex();
    if (index == 0xffffffffu) return;
    region.prefetcher->RemoveNewAddedEntity(entity);
    bTSmallArray<eCPVSCellItem*> cells;
    region.prefetcher->GetAllValidCells(cells);
    auto* permanent = region.prefetcher->GetNonDeactivationCell();
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        auto* cell = cells.GetAt(i);
        if (cell && cell != permanent && cell->ExistsEntity(entity)) {
            cell->CacheOutEntity(entity, index);
            ++remoteEntityEvictions;
        }
    }
    if (permanent && permanent->ExistsEntity(entity)) {
        permanent->CacheOutEntity(entity, index);
        ++remoteEntityEvictions;
    }
}

GEBool __fastcall CacheOut(eCPVSPrefetcher3* self, void* edx, eCEntity* entity)
{
    auto* renderer = RenderAdmin();
    if (renderer && self == &renderer->GetPVSPrefetcher3()) {
        EvictRemoteEntity(streamProbe, entity);
        for (auto& region : hostRegions) EvictRemoteEntity(region, entity);
    } else {
        if (self == streamProbe.prefetcher.get()) { EvictRemoteEntity(streamProbe, entity); return GETrue; }
        for (auto& region : hostRegions)
            if (self == region.prefetcher.get()) { EvictRemoteEntity(region, entity); return GETrue; }
    }
    return originalCacheOut(self, edx, entity);
}

void __fastcall DestroyEntityDiagnostic(eCEntity* entity, void* edx)
{
    uint32_t flags = 0, databases = 0;
    std::memcpy(&flags, reinterpret_cast<unsigned char const*>(entity) + 0x3c, sizeof(flags));
    std::memcpy(&databases, reinterpret_cast<unsigned char const*>(entity) + 0x16c, sizeof(databases));
    if ((flags & 0x100) || databases) {
        unsigned cells = 0, permanent = 0;
        if (auto* renderer = RenderAdmin()) {
            bTSmallArray<eCPVSCellItem*> all;
            auto& pvs = renderer->GetPVSPrefetcher3();
            pvs.GetAllValidCells(all);
            for (GEU16 i = 0; i < all.GetCount(); ++i) cells += all.GetAt(i)->ExistsEntity(entity) ? 1 : 0;
            auto* cell = pvs.GetNonDeactivationCell();
            permanent = cell && cell->ExistsEntity(entity);
        }
        Logf("world: cached destruction entity=%p name=%s flags=%08x databases=%08x global_cells=%u permanent=%u",
             entity, entity->GetName().GetText(), flags, databases, cells, permanent);
    }
    originalDestroyEntity(entity, edx);
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
    if (originalCacheOut && qa::Enabled() && !originalDestroyEntity) {
        auto* destroy = reinterpret_cast<unsigned char*>(GetProcAddress(engine, "?Destroy@eCEntity@@UAEXXZ"));
        const unsigned char prefix[] = {0x51, 0x56, 0x57, 0x8b, 0xf1};
        if (destroy && !std::memcmp(destroy, prefix, sizeof(prefix))) {
            originalDestroyEntity = reinterpret_cast<DestroyEntityFn>(
                ai::JumpTo(destroy, reinterpret_cast<void*>(&DestroyEntityDiagnostic), sizeof(prefix)));
        }
        if (!originalDestroyEntity) Log("QA world: cached-destruction diagnostics could not be installed");
    }
    return originalCacheOut != nullptr;
}

void ReleaseRegion(StreamProbeState& region);

void CompletePrefetcher(eCPVSPrefetcher3& prefetcher)
{
    eCPVSPrefetcherJob* job = nullptr;
    std::memcpy(&job, reinterpret_cast<unsigned char const*>(&prefetcher) + 0xc, sizeof(job));
    if (!job) return;
    // WaitForEmptyThreadQueue cancels and discards a job; partially cached entities must be committed to cells first.
    job->DisableTimeSlice();
    while (!job->IsFinished()) {
        job->Process();
        if (!job->IsFinished()) SwitchToThread();
    }
    prefetcher.WaitForEmptyThreadQueue();
}

void ResetStreamProbe()
{
    ReleaseRegion(streamProbe);
}

void TickStreamProbe()
{
    if (!streamProbe.camera || !streamProbe.prefetcher || streamProbe.failed) return;
    auto* renderer = RenderAdmin();
    if (!renderer) {
        Log("world: stream probe stopped: render system administration is unavailable");
        streamProbe.failed = true;
        return;
    }
    eCFrustumDatabase& database = streamProbe.camera->GetFrustumDatabase();
    if (!streamProbe.prefetcher->Process(database)) {
        Log("world: stream probe stopped: the prefetcher refused its database");
        streamProbe.failed = true;
        return;
    }
    ++streamProbe.frames;
}

void ReleaseRegion(StreamProbeState& region)
{
    if (!region.camera) { region = StreamProbeState{}; return; }
    if (auto* renderer = RenderAdmin()) {
        if (region.prefetcher) CompletePrefetcher(*region.prefetcher);
        CompletePrefetcher(renderer->GetPVSPrefetcher3());
        if (!renderer->FlushThreadQueues()) Log("world: remote region render queues did not flush before release");
        auto& database = region.camera->GetFrustumDatabase();
        const uint32_t index = database.GetFrustumDatabaseIndex();
        if (index != 0xffffffffu) {
            // Retirement queues cell cache-out work; drain it while both native owners are still alive.
            database.RefreshFrustumData();
            if (region.prefetcher) {
                region.prefetcher->ClearCells(database);
                region.prefetcher->WaitForEmptyThreadQueue();
            }
            if (!renderer->UnregisterDatabase(database))
                Logf("FAIL world: remote camera database %u could not be unregistered", index);
            renderer->GetPVSPrefetcher3().WaitForEmptyThreadQueue();
            if (!renderer->FlushThreadQueues()) Log("FAIL world: remote cell cache-out queues did not flush");
            Logf("world: retired remote camera database %u before destroying its camera", index);
        }
        region.camera.reset();
        region.prefetcher.reset();
        if (!renderer->FlushThreadQueues()) Log("FAIL world: remote camera destruction queues did not flush");
    } else {
        Log("FAIL world: remote camera retirement has no render system administration");
    }
    region = StreamProbeState{};
    contextRefreshPending = true;
}

bool PlaceRegion(StreamProbeState& region, float const at[3], float radius)
{
    if (region.failed) {
        if (static_cast<int32_t>(net::Now() - region.retryAt) < 0) return false;
        ReleaseRegion(region);
    }
    if (!region.camera) {
        auto* renderer = RenderAdmin();
        if (!renderer || !InstallRemoteCacheOut() || !InstallContextOwnership(renderer->GetPVSPrefetcher3())) {
            region.failed = true;
            region.retryAt = net::Now() + 5000;
            ++remoteRegionFailures;
            RegionError();
            return false;
        }
        // The game's grid-removal path assumes one database per prefetcher outside the editor.
        auto prefetcher = std::make_unique<eCPVSPrefetcher3>();
        if (prefetcher->Create() != bEResult_Ok) {
            region.failed = true;
            region.retryAt = net::Now() + 5000;
            ++remoteRegionFailures;
            Log("FAIL world: cannot create an isolated remote prefetcher");
            RegionError();
            return false;
        }
        prefetcher->SetContextCacheOutCallback(&KeepSharedContext);
        auto camera = std::make_unique<eCCameraBase>();
        if (camera->Create(GEFalse, GETrue) != bEResult_Ok ||
            camera->Init(bCRect(0, 0, 32, 32), 60.0f, 1.0f, radius) != bEResult_Ok ||
            camera->GetFrustumDatabase().GetFrustumDatabaseIndex() == 0xffffffffu) {
            region.failed = true;
            region.retryAt = net::Now() + 5000;
            ++remoteRegionFailures;
            Log("FAIL world: cannot register a remote simulation camera; no client-simulation fallback");
            RegionError();
            return false;
        }
        region.camera = std::move(camera);
        region.prefetcher = std::move(prefetcher);
        region.radius = 0.0f;
    }
    const bool moved = body::Distance(at, region.position) >= 500.0f || region.radius != radius;
    if (moved) {
        contextRefreshPending = true;
        region.camera->SetClipDepth(bCVector2(1.0f, radius));
        region.camera->SetPosition(bCVector(at[0], at[1], at[2]));
        auto& database = region.camera->GetFrustumDatabase();
        database.Update();
        database.ForcePrefetch();
        std::memcpy(region.position, at, sizeof(region.position));
        region.radius = radius;
    }
    auto* renderer = RenderAdmin();
    if (!renderer || !region.prefetcher || !region.prefetcher->Process(region.camera->GetFrustumDatabase())) {
        region.failed = true;
        region.retryAt = net::Now() + 5000;
        ++remoteRegionFailures;
        Log("FAIL world: remote simulation prefetch refused; no client-simulation fallback");
        RegionError();
        return false;
    }
    ++region.frames;
    return true;
}

void TickHostRegions()
{
    auto* renderer = RenderAdmin();
    if (!renderer) return;
    auto const& camera = eCApplication::GetInstance().GetCurrentCamera();
    const bCVector& p = camera.GetPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    if (!anchorsCheckedAt || net::Now() - anchorsCheckedAt >= 250) {
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
    struct Group { std::array<float, 3> low, high; };
    std::vector<Group> groups;
    for (auto const& anchor : simulationAnchors) {
        if (body::Distance(here, anchor.data()) + GetConfig().hostRoi + 1000.0f < camera.GetClipDepth().GetY()) continue;
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
    }
    for (size_t i = groups.size(); i < hostRegions.size(); ++i) ReleaseRegion(hostRegions[i]);
    hostRegions.resize(groups.size());
    for (size_t id = 0; id < groups.size(); ++id) {
        auto& region = hostRegions[id];
        float centre[3];
        for (int k = 0; k < 3; ++k) centre[k] = (groups[id].low[k] + groups[id].high[k]) * 0.5f;
        const float radius = GetConfig().hostRoi + body::Distance(groups[id].low.data(), groups[id].high.data()) * 0.5f + 1500.0f;
        PlaceRegion(region, centre, radius);
    }
}

void PositionOf(eCEntity* entity, float out[3])
{
    const bCVector& at = entity->GetWorldPosition();
    out[0] = at.GetX();
    out[1] = at.GetY();
    out[2] = at.GetZ();
}

// Every entity the engine processes around the camera, static ones included (doors, ore veins, campfires);
// Entity::GetEntities lists only dynamic ones (NPCs, items: 243 at the start, no door).
bTSmallArray<eCEntity*> const* Processed()
{
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    return scene ? &scene->GetAccessToEntityAdmin().GetEntitiesInProcessingRange() : nullptr;
}

}

bTSmallArray<eCEntity*> const* ProcessedEntities() { return Processed(); }

void RequestProcessingUpdate()
{
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    if (scene) scene->GetAccessToEntityAdmin().FourceFullROIUpdate();
    else Log("world: cannot refresh NPC processing range; scene administration is unavailable");
}

// Extra ranges are only for NPC collection. Moving the camera, resizing the engine ROI, or globally replacing its
// culling functions also changes static-world processing and can leave low-detail geometry over detailed buildings.
constexpr float ElasticMargin = 4000.0f;   // cm beyond the farthest player
constexpr float ElasticMax = 15000.0f;     // cm, largest radius
using ProcessFn = void(__thiscall*)(eCEntityAdmin*);
ProcessFn originalProcess = nullptr;
using StopSessionFn = void(__fastcall*)(gCSession*, void*);
StopSessionFn originalStopSession = nullptr;
bool elasticTried = false, elasticActive = false;
unsigned elasticFrames = 0, elasticTooFar = 0;
float elasticRadius = 0.0f, elasticCentre[3] = {};
char const* elasticWhy = "not yet";
uint32_t elasticLogged = 0;

bool ElasticSphere(eCEntity* camera, float centre[3], float& radius)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !camera) { elasticWhy = "no hero"; return false; }
    float points[net::MaxPlayers][3];
    int n = 0;
    const bCVector& h = remote::ControlledBody() ? remote::ControlledBody()->GetWorldPosition() : hero->GetWorldPosition();
    points[n][0] = h.GetX(); points[n][1] = h.GetY(); points[n][2] = h.GetZ(); ++n;
    net::Remote const* remotes = net::Remotes();
    for (int id = 2; id < net::MaxPlayers; ++id) {
        net::Remote const& r = remotes[id];
        if (!r.connected || !(r.latest.flags & 1) || r.latest.world != remote::CurrentWorld()) continue;
        std::memcpy(points[n++], r.latest.position, sizeof(float) * 3);
    }
    if (n < 2) { elasticWhy = "alone"; return false; }
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) lo[k] = hi[k] = points[0][k];
    for (int i = 1; i < n; ++i)
        for (int k = 0; k < 3; ++k) lo[k] = std::fmin(lo[k], points[i][k]), hi[k] = std::fmax(hi[k], points[i][k]);
    for (int k = 0; k < 3; ++k) centre[k] = (lo[k] + hi[k]) * 0.5f;
    float farthest = 0.0f;
    for (int i = 0; i < n; ++i) farthest = std::fmax(farthest, body::Distance(centre, points[i]));
    radius = std::fmax(GetConfig().hostRoi, farthest + ElasticMargin);
    // The prefetcher's sphere around the real camera: a centre that leaves a player outside it would process nothing there.
    const float loaded = *reinterpret_cast<float const*>(reinterpret_cast<char const*>(camera) + 0xbc);
    float fromCamera = 0.0f;
    const float* at = reinterpret_cast<float const*>(reinterpret_cast<char const*>(camera) + 0x8c);
    for (int i = 0; i < n; ++i) fromCamera = std::fmax(fromCamera, body::Distance(at, points[i]));
    if (radius > ElasticMax || (loaded > 0.0f && fromCamera > loaded - 1000.0f)) { ++elasticTooFar; elasticWhy = "too far apart"; return false; }
    elasticWhy = "on";
    return true;
}

using processing::Sphere;
std::vector<Sphere> spheres;
int sphereCount = 0;
bool spheresHooked = false, spheresActive = false;
unsigned sphereRebuilds = 0, sphereTooFar = 0;
unsigned npcRangeChecks = 0, npcRangeExpanded = 0, npcUpdateChecks = 0, npcUpdateExpanded = 0;
eCEntityAdmin* lastAdmin = nullptr;

float const* Field(eCEntityAdmin const* admin, int offset) { return reinterpret_cast<float const*>(reinterpret_cast<char const*>(admin) + offset); }

bool NpcIntersectsAt(eCEntityAdmin* admin, bCBox const& box, unsigned& checks, unsigned& expanded)
{
    const bool native = admin->IntersectsProcessingRangeRegion(box);
    if (native) return true;
    // Reached only from the validated LEA/PUSH/CALL sites of the engine's two dynamic-entity range paths.
    static_assert(offsetof(eCEntity, m_WorldNodeBoundary) == 0xe8, "native dynamic-collection box offset");
    auto const* entity = reinterpret_cast<eCEntity const*>(reinterpret_cast<char const*>(&box) - 0xe8);
    const bool npc = entity->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    ++checks;
    const bool included = processing::IncludesNpc(native, npc, reinterpret_cast<float const*>(&box), spheres.data(), sphereCount);
    if (included) ++expanded;
    return included;
}

bool __fastcall NpcIntersects(eCEntityAdmin* admin, void*, bCBox const& box)
{
    return NpcIntersectsAt(admin, box, npcRangeChecks, npcRangeExpanded);
}

bool __fastcall NpcIntersectsUpdate(eCEntityAdmin* admin, void*, bCBox const& box)
{
    return NpcIntersectsAt(admin, box, npcUpdateChecks, npcUpdateExpanded);
}

void HookSpheres()
{
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* collect = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CollectDynamicProcessingRangeEntity@eCEntityAdmin@@IAEXPAVeCEntity@@AAV?$bTSmallArray@PAVeCEntity@@@@1@Z")) : nullptr;
    auto* update = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?UpdateProcessingRangeEntity@eCEntityAdmin@@QAEXPAVeCEntity@@AAV?$bTSmallArray@PAVeCEntity@@@@1@Z")) : nullptr;
    auto* intersects = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?IntersectsProcessingRangeRegion@eCEntityAdmin@@QAE_NABVbCBox@@@Z")) : nullptr;
    // Engine 1.75 (Engine.dll SHA-256 ac0fb505...): the CALL of IntersectsProcessingRangeRegion in both dynamic-entity paths.
    //   Collect+0x8d: 8d 87 e8 00 00 00 (lea eax,[edi+e8]) 50 (push eax) 8b cd (mov ecx,ebp) e8 <rel32>
    //   Update+0x68:  8d 97 e8 00 00 00 (lea edx,[edi+e8]) 52 (push edx) 8b cd (mov ecx,ebp) e8 <rel32>
    // Both are patched, or neither. With only the collection path patched, a moving NPC outside the native range was dropped
    // by the engine's per-entity update and re-added by the next collection, and every re-entry refilled its hit points and
    // reset its routine (stuns and hits were lost). These are the only callers besides the static-entity collection.
    static const unsigned char collectBefore[] = {0x8d, 0x87, 0xe8, 0x00, 0x00, 0x00, 0x50, 0x8b, 0xcd};
    static const unsigned char updateBefore[] = {0x8d, 0x97, 0xe8, 0x00, 0x00, 0x00, 0x52, 0x8b, 0xcd};
    struct Site { unsigned char* call; unsigned char const* before; size_t length; void* detour; char const* name; };
    Site sites[] = {
        {collect ? collect + 0x8d : nullptr, collectBefore, sizeof(collectBefore), reinterpret_cast<void*>(&NpcIntersects), "collection"},
        {update ? update + 0x68 : nullptr, updateBefore, sizeof(updateBefore), reinterpret_cast<void*>(&NpcIntersectsUpdate), "update"},
    };
    for (Site const& s : sites) {
        int32_t displacement = 0;
        if (s.call) std::memcpy(&displacement, s.call + 1, sizeof(displacement));
        if (!s.call || !intersects || std::memcmp(s.call - s.length, s.before, s.length) || s.call[0] != 0xe8 ||
            s.call + 5 + displacement != intersects) {
            Logf("world: NPC range %s call site did not match; native world culling retained, NPC extension off", s.name);
            return;
        }
    }
    DWORD old[2] = {};
    // QA negative control (only with the QA bot): patch the collection path alone, as the first fix did.
    const int patched = qa::Enabled() && std::getenv("G3T_QA_COLLECTION_HOOK_ONLY") ? 1 : 2;
    if (patched == 1) Log("world: QA control: only the collection call site is patched");
    for (int i = 0; i < patched; ++i) {
        if (!VirtualProtect(sites[i].call, 5, PAGE_EXECUTE_READWRITE, &old[i])) {
            Logf("world: cannot patch NPC range call (Windows error %lu); native culling retained", GetLastError());
            for (int k = 0; k < i; ++k) {
                const int32_t original = static_cast<int32_t>(reinterpret_cast<intptr_t>(intersects) - reinterpret_cast<intptr_t>(sites[k].call + 5));
                std::memcpy(sites[k].call + 1, &original, sizeof(original));
                DWORD unused = 0;
                VirtualProtect(sites[k].call, 5, old[k], &unused);
                FlushInstructionCache(GetCurrentProcess(), sites[k].call, 5);
            }
            return;
        }
        const int32_t displacement = static_cast<int32_t>(reinterpret_cast<intptr_t>(sites[i].detour) - reinterpret_cast<intptr_t>(sites[i].call + 5));
        std::memcpy(sites[i].call + 1, &displacement, sizeof(displacement));
    }
    for (int i = 0; i < patched; ++i) {
        DWORD unused = 0;
        if (!VirtualProtect(sites[i].call, 5, old[i], &unused))
            Logf("world: cannot restore NPC range call protection (Windows error %lu)", GetLastError());
        FlushInstructionCache(GetCurrentProcess(), sites[i].call, 5);
    }
    spheresHooked = true;
    Log("world: NPC-only range calls hooked (collection and per-entity update); static culling, native ROI and camera unchanged");
}

// Rebuild the NPC collection when a covered player moves by the engine's own ROI threshold.
void PlaceSpheres(eCEntityAdmin* admin, eCEntity* camera)
{
    lastAdmin = admin;
    std::vector<Sphere> wanted(simulationAnchors.size() + 3);
    int n = 0;
    float const* at = camera ? reinterpret_cast<float const*>(reinterpret_cast<char const*>(camera) + 0x8c) : nullptr;
    const float step = *Field(admin, 0x40);
    elasticActive = false;
    if (spheresHooked && at && net::IsHost() && gCSession::GetInstance().IsGameRunning()) {
        if (GetConfig().hostRoi > *Field(admin, 0x10)) {
            std::memcpy(wanted[n].at, at, sizeof(wanted[n].at));
            wanted[n++].radius = GetConfig().hostRoi + step;
        }
        float centre[3], radius = 0.0f;
        if (GetConfig().hostSimElastic && ElasticSphere(camera, centre, radius)) {
            std::memcpy(wanted[n].at, centre, sizeof(wanted[n].at));
            wanted[n++].radius = radius + step;
            std::memcpy(elasticCentre, centre, sizeof(elasticCentre));
            elasticRadius = radius;
            elasticActive = true;
            ++elasticFrames;
        }
        for (auto const& anchor : simulationAnchors) {
            std::memcpy(wanted[n].at, anchor.data(), sizeof(wanted[n].at));
            wanted[n++].radius = GetConfig().hostRoi + step;
        }
        if (streamProbe.camera && !streamProbe.failed) {
            std::memcpy(wanted[n].at, streamProbe.position, sizeof(wanted[n].at));
            wanted[n++].radius = streamProbe.radius + step;
        }
    }
    bool changed = n != sphereCount;
    for (int i = 0; i < n && !changed; ++i)
        changed = body::Distance(wanted[i].at, spheres[i].at) > step || wanted[i].radius != spheres[i].radius;
    auto* bytes = reinterpret_cast<unsigned char*>(admin);
    if (changed) {
        spheres.assign(wanted.begin(), wanted.begin() + n);
        sphereCount = n;
        bytes[0xfd] = 1;
        ++sphereRebuilds;
    }
    spheresActive = sphereCount > 0;
}

void __fastcall ElasticProcess(eCEntityAdmin* admin, void*)
{
    eCEntity* camera = nullptr;
    camera = reinterpret_cast<eCEntity*>(&eCApplication::GetInstance().GetCurrentCamera());  // Process reads the same one
    PlaceSpheres(admin, camera);
    originalProcess(admin);
}

void __fastcall StopSession(gCSession* session, void* edx)
{
    OnWorldDeactivate();
    originalStopSession(session, edx);
}

void HookElastic()
{
    if (elasticTried) return;
    elasticTried = true;
    auto* stop = reinterpret_cast<unsigned char*>(GetProcAddress(GetModuleHandleA("Game.dll"), "?Stop@gCSession@@QAEXXZ"));
    const unsigned char stopPrefix[] = {0x51, 0x53, 0x55, 0x8b, 0x2d};
    if (!stop || std::memcmp(stop, stopPrefix, sizeof(stopPrefix)) ||
        stop[9] != 0x56 || stop[10] != 0x33 || stop[11] != 0xdb) {
        Log("FAIL world: unsupported session teardown; remote simulation cameras cannot be retained safely");
        return;
    }
    originalStopSession = reinterpret_cast<StopSessionFn>(ai::JumpTo(stop, reinterpret_cast<void*>(&StopSession), 9));
    if (!originalStopSession) { Log("FAIL world: cannot guard remote camera lifetime"); return; }
    auto* code = reinterpret_cast<unsigned char*>(GetProcAddress(GetModuleHandleA("Engine.dll"), "?Process@eCEntityAdmin@@QAEXXZ"));
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (!code || std::memcmp(code, prologue, sizeof(prologue))) {
        Logf("world: eCEntityAdmin::Process %s; host_sim stays off", code ? "has another prologue" : "not found");
        return;
    }
    originalProcess = reinterpret_cast<ProcessFn>(ai::JumpTo(code, reinterpret_cast<void*>(&ElasticProcess)));
    if (originalProcess) HookSpheres();
    Logf("world: eCEntityAdmin::Process %s", originalProcess ? "wrapped (NPC-only ranges)" : "could not be wrapped");
}

bool Extended() { return elasticActive || spheresActive; }

bool HostProcesses(float const at[3], float margin)
{
    if (!spheresActive || !lastAdmin) return false;
    if (body::Distance(at, Field(lastAdmin, 0x24)) <= *Field(lastAdmin, 0x10) - margin) return true;  // the camera's own
    const float step = *Field(lastAdmin, 0x40);
    for (int i = 0; i < sphereCount; ++i)
        if (body::Distance(at, spheres[i].at) <= spheres[i].radius - step - margin) return true;
    return false;
}

void Tick()
{
    if (!net::IsHost()) return;
    HookElastic();
    if (spheresHooked && originalStopSession && gCSession::GetInstance().IsGameRunning()) {
        TickHostRegions();
        TickStreamProbe();
        RefreshContexts();
    }
    if (GetConfig().hostSimSpheres && net::Now() - elasticLogged >= 5000) {
        elasticLogged = net::Now();
        if (spheresActive)
            Logf("world: spheres %d around players (first %.0f,%.0f,%.0f radius %.0f cm), %u rebuilds, %u times too far", sphereCount,
                 spheres[0].at[0], spheres[0].at[1], spheres[0].at[2], spheres[0].radius, sphereRebuilds, sphereTooFar);
        else if (spheresHooked)
            Logf("world: spheres none now (%u times too far)", sphereTooFar);
    }
    if (GetConfig().hostSimElastic && net::Now() - elasticLogged >= 5000) {
        elasticLogged = net::Now();
        if (elasticActive)
            Logf("world: elastic range centre %.0f,%.0f,%.0f radius %.0f cm (%u frames)", elasticCentre[0], elasticCentre[1], elasticCentre[2],
                 elasticRadius, elasticFrames);
        else if (originalProcess)
            Logf("world: elastic range off now (%s; %u times too far apart)", elasticWhy, elasticTooFar);
    }
}

void OnWorldDeactivate()
{
    for (auto& region : hostRegions) ReleaseRegion(region);
    hostRegions.clear();
    simulationAnchors.clear();
    anchorsCheckedAt = 0;
    spheres.clear();
    sphereCount = 0;
    lastAdmin = nullptr;
    ResetStreamProbe();
    RestoreContextOwnership();
}

void StreamProbe(char const* argument, LogFn log)
{
    if (!qa::Enabled() || !net::IsHost() || !gCSession::GetInstance().IsGameRunning()) {
        log("REFUSED streamprobe needs=qa_running_host");
        return;
    }
    if (argument && !std::strcmp(argument, "off")) {
        ResetStreamProbe();
        RequestProcessingUpdate();
        log("STREAMPROBE active=0");
        return;
    }
    if (argument && *argument && std::strcmp(argument, "report")) {
        float at[3] = {}, radius = 0.0f;
        char tail = '\0';
        if (std::sscanf(argument, "%f_%f_%f_%f%c", &at[0], &at[1], &at[2], &radius, &tail) != 4 ||
            !std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2]) ||
            !std::isfinite(radius) || radius < 1000.0f || radius > 15000.0f) {
            log("REFUSED streamprobe expected=X_Y_Z_radius radius_cm=1000..15000");
            return;
        }
        ResetStreamProbe();
        if (!PlaceRegion(streamProbe, at, radius)) {
            log("REFUSED streamprobe reason=remote_region_setup_failed");
            return;
        }
        RequestProcessingUpdate();
    }
    if (!streamProbe.camera) { log("STREAMPROBE active=0"); return; }
    auto* renderer = RenderAdmin();
    if (!renderer) { log("REFUSED streamprobe reason=no_render_admin"); return; }
    auto& database = streamProbe.camera->GetFrustumDatabase();
    bTSmallArray<eCPVSCellItem*> cells;
    if (!streamProbe.prefetcher) { log("REFUSED streamprobe reason=no_private_prefetcher"); return; }
    streamProbe.prefetcher->GetValidCells(database, cells);
    unsigned nearbyNpcs = 0, processed = 0, navigation = 0, floors = 0;
    if (auto* entities = Processed())
        for (GEU16 i = 0; i < entities->GetCount(); ++i) {
            eCEntity* entity = entities->GetAt(i);
            if (!entity || !entity->GetPropertySet(eEPropertySetType_NPC)) continue;
            ++processed;
            float at[3];
            PositionOf(entity, at);
            if (body::Distance(at, streamProbe.position) > streamProbe.radius) continue;
            ++nearbyNpcs;
            if (entity->GetPropertySet(eEPropertySetType_Navigation) && Entity(entity).Navigation.GetCurrentZone().GetInstance())
                ++navigation;
            if (auto* movement = static_cast<gCCharacterMovement_PS*>(entity->GetPropertySet(eEPropertySetType_CharacterMovement))) {
                const float ground = movement->GetDistToGround(entity->GetWorldPosition(), GETrue);
                if (std::isfinite(ground) && std::fabs(ground) < 2000.0f) ++floors;
            }
        }
    char line[320];
    Format(line, "STREAMPROBE active=1 failed=%d database=%u cells=%d frames=%u job=%d processed_npcs=%u near_npcs=%u nav_npcs=%u floor_npcs=%u "
                 "position=%.1f,%.1f,%.1f radius=%.0f",
           streamProbe.failed ? 1 : 0, database.GetFrustumDatabaseIndex(), cells.GetCount(), streamProbe.frames,
           streamProbe.prefetcher->IsJobRunning() ? 1 : 0, processed, nearbyNpcs, navigation, floors, streamProbe.position[0],
           streamProbe.position[1], streamProbe.position[2], streamProbe.radius);
    log(line);
}

void ProcessingReport(LogFn log)
{
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* cull = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CullProcessingRangeRegion@eCEntityAdmin@@QAE?AW4eEProcessingRangeCull@1@ABVbCBox@@@Z")) : nullptr;
    auto* old = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CullOldProcessingRangeRegion@eCEntityAdmin@@QAE?AW4eEProcessingRangeCull@1@ABVbCBox@@@Z")) : nullptr;
    auto* intersects = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?IntersectsProcessingRangeRegion@eCEntityAdmin@@QAE_NABVbCBox@@@Z")) : nullptr;
    unsigned regions = 0;
    for (auto const& region : hostRegions) regions += region.camera && !region.failed;
    char line[480];
    auto& camera = eCApplication::GetInstance().GetCurrentCamera();
    bCVector const& cameraPosition = camera.GetPosition();
    Format(line, "WORLDPROCESS npc_only=%d native_roi_cm=%.0f npc_roi_cm=%.0f spheres=%d checks=%u expanded=%u "
                 "update_checks=%u update_expanded=%u static_cull_original=%d old_cull_original=%d intersects_original=%d "
                 "camera=%.2f,%.2f,%.2f camera_far=%.0f remote_regions=%u remote_failures=%u",
           spheresHooked, scene ? scene->GetAccessToEntityAdmin().GetROISphere().GetRadius() : -1.0f,
           GetConfig().hostRoi, sphereCount, npcRangeChecks, npcRangeExpanded, npcUpdateChecks, npcUpdateExpanded,
           cull && cull[0] == 0x8b, old && old[0] == 0x8b, intersects && intersects[0] == 0x8b,
           cameraPosition.GetX(), cameraPosition.GetY(), cameraPosition.GetZ(), camera.GetClipDepth().GetY(), regions, remoteRegionFailures);
    log(line);
    bTSmallArray<eCPVSCellItem*> cells;
    if (auto* renderer = RenderAdmin()) renderer->GetPVSPrefetcher3().GetValidCells(camera.GetFrustumDatabase(), cells);
    unsigned statics = 0, dynamics = 0;
    for (GEU16 i = 0; i < cells.GetCount(); ++i) {
        statics += cells.GetAt(i)->GetStaticEntities().GetCount();
        dynamics += cells.GetAt(i)->GetDynamicEntities().GetCount();
    }
    Format(line, "WORLDRENDER main_database=%u cells=%d statics=%u dynamics=%u solids=%d alpha=%d remote_regions=%u evictions=%u contexts_retained=%u",
           camera.GetFrustumDatabase().GetFrustumDatabaseIndex(), cells.GetCount(), statics, dynamics,
           camera.GetFrustumDatabase().GetSolidElementCount(), camera.GetFrustumDatabase().GetAlphaElementCount(),
           regions, remoteEntityEvictions, contextsRetained);
    log(line);
}

namespace {
bool listAll = false;  // QA: usables all -> every entity with an Interaction set that is no NPC

bool Usable(eCEntity* entity)
{
    if (!entity || !entity->GetPropertySet(eEPropertySetType_Interaction)) return false;
    if (entity->GetPropertySet(eEPropertySetType_NPC)) return false;
    if (listAll) return true;
    if (entity->GetPropertySet(eEPropertySetType_Item)) return false;
    Entity e(entity);
    const bCString use = e.Interaction.UseFunc;
    return use.GetLength() > 0 || entity->GetPropertySet(eEPropertySetType_Door);
}

// The nearest entity of that name to the controlled body (names of interact objects repeat: every door is a door).
eCEntity* Nearest(char const* name)
{
    eCEntity* body = remote::ControlledBody();
    if (!body) return nullptr;
    float from[3];
    PositionOf(body, from);
    eCEntity* best = nullptr;
    float bestDistance = 1e12f;
    // A GUID ("8-4-4-4-12") picks that one entity: chests all share one name.
    const bool byGuid = std::strlen(name) == 36 && name[8] == '-' && name[13] == '-';
    bTSmallArray<eCEntity*> const* every = Processed();
    for (GEU16 i = 0; every && i < every->GetCount(); ++i) {
        eCEntity* instance = every->GetAt(i);
        if (!instance) continue;
        if (byGuid) {
            const eCEntityProxy proxy(instance);
            _GUID raw;
            std::memcpy(&raw, &proxy.GetEntityID().GetCore(), 16);
            char text[40];
            Format(text, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", raw.Data1, raw.Data2, raw.Data3, raw.Data4[0], raw.Data4[1],
                      raw.Data4[2], raw.Data4[3], raw.Data4[4], raw.Data4[5], raw.Data4[6], raw.Data4[7]);
            if (_stricmp(text, name) != 0) continue;
        } else if (std::strcmp(instance->GetName().GetText(), name) != 0) {
            continue;
        }
        float p[3];
        PositionOf(instance, p);
        const float d = body::Distance(p, from);
        if (d < bestDistance) best = instance, bestDistance = d;
    }
    return best;
}

void Describe(eCEntity* entity, char* line, size_t size)
{
    Entity e(entity);
    float p[3];
    PositionOf(entity, p);
    const bool door = entity->GetPropertySet(eEPropertySetType_Door) != nullptr;
    const bool lock = entity->GetPropertySet(eEPropertySetType_Lock) != nullptr;
    const bCString use = e.Interaction.UseFunc;
    Format(line, size, "name=%s pos=%.0f_%.0f_%.0f usefunc=%s usetype=%d item=%d door=%d open=%d lock=%d locked=%d user=%s", entity->GetName().GetText(), p[0], p[1],
              p[2], *use.GetText() ? use.GetText() : "-", static_cast<int>(e.Interaction.GetUseType()), entity->GetPropertySet(eEPropertySetType_Item) ? 1 : 0, door ? 1 : 0, door && e.Door.IsOpen() ? 1 : 0, lock ? 1 : 0, lock && e.Lock.IsLocked() ? 1 : 0,
              e.Interaction.GetUser().GetInstance() ? e.Interaction.GetUser().GetName().GetText() : "-");
}
}

void Usables(char const* radiusText, LogFn log)
{
    eCEntity* body = remote::ControlledBody();
    if (!body) { log("USABLE end count=0 no_hero=1"); return; }
    listAll = radiusText && !std::strncmp(radiusText, "all", 3);
    if (listAll) radiusText += 3;
    const float radius = radiusText && *radiusText ? static_cast<float>(std::atof(radiusText)) : 3000.0f;
    float from[3];
    PositionOf(body, from);
    char line[400], text[360];
    int count = 0, total = 0, nearby = 0, interaction = 0, doors = 0;
    bTSmallArray<eCEntity*> const* every = Processed();
    total = every ? every->GetCount() : -1;
    for (GEU16 i = 0; every && i < every->GetCount() && count < 200; ++i) {
        eCEntity* instance = every->GetAt(i);
        if (!instance) continue;
        float p[3];
        PositionOf(instance, p);
        const float d = body::Distance(p, from);
        if (d > radius) continue;
        ++nearby;
        interaction += instance->GetPropertySet(eEPropertySetType_Interaction) != nullptr;
        doors += instance->GetPropertySet(eEPropertySetType_Door) != nullptr;
        if (!Usable(instance)) continue;
        Describe(instance, text, sizeof(text));
        Format(line, "USABLE %s distance=%.0f", text, d);
        log(line);
        ++count;
    }
    Format(line, "USABLE end count=%d entities=%d nearby=%d interaction=%d doors=%d", count, total, nearby, interaction, doors);
    log(line);
}

void UseInfo(char const* name, LogFn log)
{
    char line[560], text[360];  // text and the hero's part: more than 400 (sprintf_s ends the game on overflow)
    eCEntity* target = Nearest(name);
    if (!target) { log("REFUSED useinfo: no such entity"); return; }
    Describe(target, text, sizeof(text));
    eCEntity* hero = remote::ControlledBody();
    Entity self(hero);
    float at[3] = {};
    if (hero) PositionOf(hero, at);
    Entity focus = hero ? self.Focus.GetFocusEntity() : Entity();
    // The game's own rule for what the hero may choose (-1: never; an interact object with a user, re/out/p1-focus.c).
    const GEInt priority = hero ? GetScriptAdminExt().CallScript(bCString("GetFocusEntityPriority"), hero, target, 0) : -2;
    const eCEntityProxy proxy(target);
    bCPropertyID::bSCore g;  // for the loot QA (contents, loottake) on an interact object (treasurediag)
    std::memcpy(&g, &proxy.GetEntityID().GetCore(), 16);
    Format(line, "USEINFO %s hero_task=%s hero_state=%s hero_pos=%.0f_%.0f_%.0f hero_ani=%d focus=%s focus_is_it=%d focus_priority=%d "
              "guid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", text, self.Routine.GetCurrentTask().GetText(),
              self.Routine.GetCurrentState().GetText(), at[0], at[1], at[2], hero ? static_cast<int>(self.Routine.AniState) : -1,
              focus.GetInstance() ? focus.GetName().GetText() : "-", focus.GetInstance() == target ? 1 : 0, priority, g.Data1, g.Data2,
              g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    log(line);
}

void FindEntities(char const* prefix, LogFn log)
{
    char line[200];
    int count = 0;
    bTSmallArray<eCEntity*> const* every = Processed();
    const size_t length = std::strlen(prefix);
    for (GEU16 i = 0; every && i < every->GetCount() && count < 30; ++i) {
        eCEntity* instance = every->GetAt(i);
        if (!instance || std::strncmp(instance->GetName().GetText(), prefix, length) != 0) continue;
        float p[3];
        PositionOf(instance, p);
        Format(line, "FINDENT name=%s pos=%.0f_%.0f_%.0f", instance->GetName().GetText(), p[0], p[1], p[2]);
        log(line);
        ++count;
    }
    Format(line, "FINDENTS end count=%d", count);
    log(line);
}

void FindAll(char const* name, LogFn log)
{
    char line[200];
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    bTPtrArray<eCEntity*> found;
    if (scene) scene->GetEntitiesByName(bCString(name), found);
    int count = 0;
    for (GEInt i = 0; i < found.GetCount() && count < 40; ++i) {
        eCEntity* instance = found.GetAt(i);
        if (!instance) continue;
        float p[3];
        PositionOf(instance, p);
        Entity e(instance);
        Format(line, "FINDALL name=%s pos=%.0f_%.0f_%.0f dead=%d", name, p[0], p[1], p[2], e.IsDead() ? 1 : 0);
        log(line);
        ++count;
    }
    Format(line, "FINDALL end count=%d", count);
    log(line);
}

void Unlock(char const* name, LogFn log)
{
    char line[200];
    eCEntity* target = Nearest(name);
    if (!target || !target->GetPropertySet(eEPropertySetType_Lock)) { log("REFUSED unlock: no such entity with a lock"); return; }
    Entity e(target);
    const bool before = e.Lock.IsLocked() != GEFalse;
    e.Lock.Status = gELockStatus_Unlocked;
    Format(line, "UNLOCK name=%s was_locked=%d locked=%d", name, before ? 1 : 0, e.Lock.IsLocked() ? 1 : 0);
    log(line);
}

namespace {
// Stand in front of the entity, facing it, as a player who walks up to it; the anchor point if it has one.
Entity Approach(eCEntity* hero, eCEntity* target)
{
    // Stand 1.2 m in front of it, facing it, as a player who walks up to it.
    float from[3], at[3];
    PositionOf(hero, from);
    PositionOf(target, at);
    float dx = from[0] - at[0], dz = from[2] - at[2];
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length < 1.0f) dx = 0.0f, dz = 1.0f;
    else dx /= length, dz /= length;
    float place[3] = {at[0] + dx * 120.0f, from[1], at[2] + dz * 120.0f};
    // The object's anchor point is where PS_Interact walks the user to first (_AI_Goto_Ext); from anywhere else the
    // walk did not always arrive (a bed: 25 s in PS_Interact). Start there.
    Entity anchor = Entity(target).Interaction.GetAnchorPoint();
    if (anchor.GetInstance() && anchor.GetInstance() != target) {
        float a[3];
        PositionOf(anchor.GetInstance(), a);
        if (body::Distance(a, at) < 1000.0f) {
            place[0] = a[0], place[1] = a[1], place[2] = a[2];
            dx = a[0] - at[0], dz = a[2] - at[2];
            const float l = std::sqrt(dx * dx + dz * dz);
            if (l > 1.0f) dx /= l, dz /= l;
        }
    }
    const float yaw = std::atan2(-dx, -dz);
    const float rotation[4] = {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)};
    Entity(hero).MoveTo(body::Pose(*hero, place, rotation));
    return anchor;
}
}

void Use(char const* name, LogFn log)
{
    char line[400];
    eCEntity* hero = remote::ControlledBody();
    eCEntity* target = Nearest(name);
    if (!hero || !target) { log("REFUSED use: no hero or no such entity"); return; }
    Entity anchor = Approach(hero, target);
    Entity self(hero);
    Entity object(target);
    self.Focus.SetFocusEntity(object);
    self.NPC.SetCurrentTarget(object);
    self.Routine.SetTask(bCString("PS_Interact"));
    const bCString use = object.Interaction.UseFunc;
    Format(line, "USE name=%s usefunc=%s hero_task=%s anchor=%s", name, use.GetText(), self.Routine.GetCurrentTask().GetText(),
              anchor.GetInstance() ? anchor.GetName().GetText() : "-");
    log(line);
}
}

namespace g3t::world {
// QA: stand in front of an entity and face it, without using it (the game's own focus then picks it).
void Face(char const* name, LogFn log)
{
    eCEntity* hero = remote::ControlledBody();
    eCEntity* target = Nearest(name);
    if (!hero || !target) { log("REFUSED face: no hero or no such entity"); return; }
    Approach(hero, target);
    char line[200];
    Format(line, "FACE name=%s", name);
    log(line);
}
}
