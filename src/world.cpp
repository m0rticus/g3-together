// SPDX-License-Identifier: GPL-3.0-or-later
#include "world.h"
#include "world_collision.h"
#include "world_pvs.h"
#include "log.h"

#include "body.h"
#include "ai.h"
#include "config.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "processing.h"
#include "remote.h"
#include "world_retirement.h"
#include "world_processing_policy.h"
#include "movement.h"

#include <Engine.h>
#include <Game.h>
#include <Script.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include <Windows.h>
#include <intrin.h>

namespace g3t::world {
namespace {
retirement::Gate retirementGate;
unsigned nativeStops = 0, gateResumes = 0;
uint32_t gateStoppedAt = 0, suspendedLogged = 0;

// Every entity the engine processes around the camera, static ones included (doors, ore veins, campfires);
// Entity::GetEntities lists only dynamic ones (NPCs, items: 243 at the start, no door).
bTSmallArray<eCEntity*> const* Processed()
{
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    return scene ? &scene->GetAccessToEntityAdmin().GetEntitiesInProcessingRange() : nullptr;
}

}

bTSmallArray<eCEntity*> const* ProcessedEntities() { return Processed(); }

ProcessingMembership ProcessingOf(eCEntity const* entity)
{
    ProcessingMembership result;
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    if (!entity || !scene) return result;
    auto& admin = scene->GetAccessToEntityAdmin();
    const auto contains = [entity](bTSmallArray<eCEntity*> const& list) {
        for (GEU16 i = 0; i < list.GetCount(); ++i) if (list.GetAt(i) == entity) return true;
        return false;
    };
    result.range = contains(admin.GetEntitiesInProcessingRange());
    result.processable = contains(admin.GetProcessableEntities());
    return result;
}

namespace {
bool MarkProcessingDirty(eCEntityAdmin* admin, char const** why = nullptr)
{
    auto* engine = GetModuleHandleA("Engine.dll");
    auto* refresh = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?FourceFullROIUpdate@eCEntityAdmin@@QAEXXZ")) : nullptr;
    if (refresh && refresh == reinterpret_cast<unsigned char*>(engine) + 0x231e00 &&
        QueueProcessingRebuild(reinterpret_cast<unsigned char*>(admin), refresh)) return true;
    if (why) *why = "engine ROI refresh signature mismatch";
    static bool warned = false;
    if (!warned) {
        warned = true;
        Log("FAIL world: unsupported incremental processing refresh; destructive full ROI clear refused");
    }
    return false;
}
}

void RequestProcessingUpdate()
{
    if (retirementGate.Paused() || ai::ShuttingDown()) return;
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    if (scene) {
        if (MarkProcessingDirty(&scene->GetAccessToEntityAdmin()))
            movement::Touch(remote::ControlledBody(), "ROI-dirty", "membership-preserved", _ReturnAddress());
    }
    else Log("world: cannot refresh NPC processing range; scene administration is unavailable");
}

bool QueueEntityProcessing(eCEntity* entity, char const** why)
{
    if (why) *why = "queued";
    if (!entity) { if (why) *why = "no entity"; return false; }
    if (retirementGate.Paused()) { if (why) *why = "retirement gate paused"; return false; }
    if (ai::ShuttingDown()) { if (why) *why = "shutting down"; return false; }
    auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
    if (!scene) {
        if (why) *why = "scene administration unavailable";
        Log("FAIL world: cannot queue native entity processing; scene administration unavailable");
        return false;
    }
    auto& admin = scene->GetAccessToEntityAdmin();
    if (!MarkProcessingDirty(&admin, why)) return false;
    admin.AddDirtyProcessingRangeEntity(entity, GETrue);
    return true;
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
    // Reached only from the validated LEA/PUSH/CALL sites of the engine's two dynamic-entity range paths.
    static_assert(offsetof(eCEntity, m_WorldNodeBoundary) == 0xe8, "native dynamic-collection box offset");
    auto const* entity = reinterpret_cast<eCEntity const*>(reinterpret_cast<char const*>(&box) - 0xe8);
    if (npcremoval::ExcludeFromProcessing(entity)) return false;
    if (native) return true;
    if (!net::IsHost()) return npclease::ProcessingAuthority(entity);
    const bool npc = entity->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    ++checks;
    const bool included = processing::IncludesNpc(native, npc, reinterpret_cast<float const*>(&box), spheres.data(), sphereCount) &&
                          collision::IsNpcCollisionReady(entity);
    if (included) ++expanded;
    return included;
}

bool __fastcall NpcIntersects(eCEntityAdmin* admin, void*, bCBox const& box)
{
    if (retirementGate.Paused() || ai::ShuttingDown()) return admin ? admin->IntersectsProcessingRangeRegion(box) : GEFalse;
    return NpcIntersectsAt(admin, box, npcRangeChecks, npcRangeExpanded);
}

bool __fastcall NpcIntersectsUpdate(eCEntityAdmin* admin, void*, bCBox const& box)
{
    if (retirementGate.Paused() || ai::ShuttingDown()) return admin ? admin->IntersectsProcessingRangeRegion(box) : GEFalse;
    return NpcIntersectsAt(admin, box, npcUpdateChecks, npcUpdateExpanded);
}

void HookSpheres()
{
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* collect = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?CollectDynamicProcessingRangeEntity@eCEntityAdmin@@IAEXPAVeCEntity@@AAV?$bTSmallArray@PAVeCEntity@@@@1@Z")) : nullptr;
    auto* update = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?UpdateProcessingRangeEntity@eCEntityAdmin@@QAEXPAVeCEntity@@AAV?$bTSmallArray@PAVeCEntity@@@@1@Z")) : nullptr;
    if (!update && engine) {
        auto* candidate = reinterpret_cast<unsigned char*>(engine) + 0x231520;
        constexpr unsigned char arguments[] = {0x8b, 0x7c, 0x24, 0x10, 0x8b, 0x4f, 0x3c};
        if (!std::memcmp(candidate + 0x3d, arguments, sizeof(arguments))) update = candidate;
    }
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
    const int patched = 2;
    unsigned char originalBytes[2][5];
    unsigned char installedBytes[2][5];
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
        std::memcpy(originalBytes[i], sites[i].call, 5);
        const int32_t displacement = static_cast<int32_t>(reinterpret_cast<intptr_t>(sites[i].detour) - reinterpret_cast<intptr_t>(sites[i].call + 5));
        std::memcpy(sites[i].call + 1, &displacement, sizeof(displacement));
        std::memcpy(installedBytes[i], sites[i].call, 5);
    }
    for (int i = 0; i < patched; ++i) {
        DWORD unused = 0;
        if (!VirtualProtect(sites[i].call, 5, old[i], &unused))
            Logf("world: cannot restore NPC range call protection (Windows error %lu)", GetLastError());
        FlushInstructionCache(GetCurrentProcess(), sites[i].call, 5);
        ai::TrackPatch(sites[i].call, originalBytes[i], installedBytes[i], 5, "world NPC range call");
    }
    spheresHooked = true;
    Log("world: NPC-only range calls hooked (collection and per-entity update); static culling, native ROI and camera unchanged");
}

// Rebuild the NPC collection when a covered player moves by the engine's own ROI threshold.
void PlaceSpheres(eCEntityAdmin* admin, eCEntity* camera)
{
    lastAdmin = admin;
    auto const& simulationAnchors = pvs::ReadySimulationAnchors();
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
    }
    bool changed = n != sphereCount;
    for (int i = 0; i < n && !changed; ++i)
        changed = body::Distance(wanted[i].at, spheres[i].at) > step || wanted[i].radius != spheres[i].radius;
    if (changed) {
        if (!MarkProcessingDirty(admin)) return;
        spheres.assign(wanted.begin(), wanted.begin() + n);
        sphereCount = n;
        ++sphereRebuilds;
    }
    spheresActive = sphereCount > 0;
}

void __fastcall ElasticProcess(eCEntityAdmin* admin, void*)
{
    if (retirementGate.Paused() || ai::ShuttingDown()) {
        if (originalProcess) originalProcess(admin);
        return;
    }
    eCEntity* camera = nullptr;
    camera = reinterpret_cast<eCEntity*>(&eCApplication::GetInstance().GetCurrentCamera());  // Process reads the same one
    if (net::IsHost()) {
        PlaceSpheres(admin, camera);
        if (collision::SyncSphereStaticCollision(admin, spheres.data(), sphereCount))
            MarkProcessingDirty(admin);
    }
    originalProcess(admin);
}

void RetireWorld(bool bySession);

void __fastcall StopSession(gCSession* session, void* edx)
{
    if (!ai::ShuttingDown()) {
        const bool running = session && session->IsGameRunning();
        RetireWorld(true);
        gateStoppedAt = net::Now();
        if (++nativeStops <= 24)
            Logf("world: native gCSession::Stop #%u (session running=%d): host simulation suspended until the session runs again",
                 nativeStops, running ? 1 : 0);
    }
    if (originalStopSession) originalStopSession(session, edx);
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

bool Extended() { return !retirementGate.Paused() && (elasticActive || spheresActive); }

bool HostProcesses(float const at[3], float margin)
{
    if (retirementGate.Paused() || !spheresActive || !lastAdmin) return false;
    if (body::Distance(at, Field(lastAdmin, 0x24)) <= *Field(lastAdmin, 0x10) - margin) return true;  // the camera's own
    const float step = *Field(lastAdmin, 0x40);
    for (int i = 0; i < sphereCount; ++i)
        if (body::Distance(at, spheres[i].at) <= spheres[i].radius - step - margin) return true;
    return false;
}

// The native Stop that follows the world activation callback of a New Game leaves the gate paused for the whole world
// (no callback arrives afterwards): lift that one pause once the native session runs again, as in a loaded world.
void ResumeAfterNativeStop()
{
    auto& session = gCSession::GetInstance();
    if (!session.IsGameRunning() || !session.GetHero() || !retirementGate.ResumeAfterStop(true)) return;
    ++gateResumes;
    Logf("world: host simulation resumed after native session stop #%u (suspended %u ms)", nativeStops, net::Now() - gateStoppedAt);
}

HostSimStatus HostSimulationStatus()
{
    HostSimStatus status;
    status.hooked = spheresHooked;
    status.paused = retirementGate.Paused();
    status.resumable = retirementGate.Resumable();
    status.spheresActive = spheresActive;
    status.spheres = static_cast<unsigned>(sphereCount);
    status.stops = nativeStops;
    status.resumes = gateResumes;
    if (lastAdmin) status.nativeRoiCm = *Field(lastAdmin, 0x10);
    if (auto const* range = Processed()) status.processed = range->GetCount();
    pvs::FillRegionStatus(status);
    return status;
}

void Tick()
{
    Start();
    if (ai::ShuttingDown()) return;
    if (retirementGate.Paused()) ResumeAfterNativeStop();
    if (retirementGate.Paused()) {
        if (net::IsHost() && GetConfig().hostSimSpheres && spheresHooked && net::Now() - suspendedLogged >= 5000) {
            suspendedLogged = net::Now();
            Logf("world: spheres suspended (native Stop #%u, resumable=%d, session running=%d)", nativeStops,
                 retirementGate.Resumable() ? 1 : 0, gCSession::GetInstance().IsGameRunning() ? 1 : 0);
        }
        return;
    }
    HookElastic();
    if (!net::IsHost()) return;
    if (spheresHooked && originalStopSession && gCSession::GetInstance().IsGameRunning()) {
        pvs::TickHostRegions();
        pvs::RefreshContexts();
    }
    if (GetConfig().hostSimSpheres && net::Now() - elasticLogged >= 5000) {
        elasticLogged = net::Now();
        if (spheresActive)
            Logf("world: spheres %d around players (first %.0f,%.0f,%.0f radius %.0f cm), %u rebuilds, %u times too far, regions=%zu anchors=%zu",
                 sphereCount, spheres[0].at[0], spheres[0].at[1], spheres[0].at[2], spheres[0].radius, sphereRebuilds, sphereTooFar,
                 pvs::ActiveRegionCount(), pvs::SimulationAnchors().size());
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

void RetireWorld(bool bySession)
{
    if (!retirementGate.Begin(bySession)) {
        Log("world-cache: nested deactivation; retirement already in progress");
        return;
    }
    struct FinishRetirement { ~FinishRetirement() { retirementGate.Finish(); } } finish;
    CaptureCacheInventory();
    collision::Retire();
    pvs::RetireHostRegions();
    spheres.clear();
    sphereCount = 0;
    spheresActive = false;
    elasticActive = false;
    lastAdmin = nullptr;
    CaptureCacheInventory();
}

void OnWorldDeactivate() { RetireWorld(false); }

void OnWorldActivate() { retirementGate.Activate(); }

void CaptureCacheInventory()
{
    pvs::CaptureRegionCacheInventory(Processed(), spheres.empty() ? nullptr : spheres.data(), static_cast<unsigned>(spheres.size()));
}

bool pvs::GatePaused() { return retirementGate.Paused(); }

void Start()
{
    pvs::InstallDatabaseIndexFix();
    pvs::StartDestroyDiagnostics();
    collision::EnsureHooksInstalled();
}
}
