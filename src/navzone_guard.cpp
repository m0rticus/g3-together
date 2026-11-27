// SPDX-License-Identifier: GPL-3.0-or-later
#include "navzone_guard.h"

#include "ai.h"
#include "log.h"
#include "navzone_policy.h"
#include "patch.h"

#include <Game.h>
#include <Windows.h>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace g3t::navzone_guard {
namespace {
using DetectCollisionCircleExitFn = GEBool(__fastcall*)(gCNavZone_PS* self, void* edx, gCNavigation_PS* nav, bCVector* out);
using ZoneTestPointDccFn = GEBool(__fastcall*)(gCNavZone_PS* self, void* edx, bCVector const* point, eCEntity const* entity);
using PathTestPointDccFn = GEBool(__fastcall*)(gCNavPath_PS* self, void* edx, bCVector const* point, eCEntity const* entity);
using ZoneDccFn = bEResult(__fastcall*)(gCNavZone_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc);
using PathDccFn = bEResult(__fastcall*)(gCNavPath_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc);
using DccDestroyFn = void(__fastcall*)(gCDynamicCollisionCircle_PS* self, void* edx);
using MapDeregisterZoneFn = bEResult(__fastcall*)(gCNavigationMap* self, void* edx, gCNavZone_PS* zone);
using MapDeregisterPathFn = bEResult(__fastcall*)(gCNavigationMap* self, void* edx, gCNavPath_PS* path);

DetectCollisionCircleExitFn originalDetectCollisionCircleExit = nullptr;
ZoneTestPointDccFn originalZoneTestPointDcc = nullptr;
PathTestPointDccFn originalPathTestPointDcc = nullptr;
ZoneDccFn originalZoneRegisterDcc = nullptr;
ZoneDccFn originalZoneDeregisterDcc = nullptr;
PathDccFn originalPathRegisterDcc = nullptr;
PathDccFn originalPathDeregisterDcc = nullptr;
DccDestroyFn originalDccDestroy = nullptr;
MapDeregisterZoneFn originalMapDeregisterZone = nullptr;
MapDeregisterPathFn originalMapDeregisterPath = nullptr;

bool navZoneGuardSearched = false;
unsigned navZoneNullBlocked = 0;
unsigned invalidDccScrubbed = 0;
unsigned orphanDccRegisterBlocked = 0;
std::mutex registryMutex;
navzone_policy::DccRegistry registry;

static_assert(offsetof(gCNavZone_PS, __UNK_00E8) == 0xe8, "gCNavZone_PS::m_arrDCCPS data offset");
static_assert(offsetof(gCNavZone_PS, __UNK_00EC) == 0xec, "gCNavZone_PS::m_arrDCCPS count offset");
static_assert(offsetof(gCNavPath_PS, m_arrDCCPS) == 0xac, "gCNavPath_PS::m_arrDCCPS offset");

bTPtrArray<gCDynamicCollisionCircle_PS*>& ZoneDccArray(gCNavZone_PS* zone)
{
    return *reinterpret_cast<bTPtrArray<gCDynamicCollisionCircle_PS*>*>(&zone->__UNK_00E8);
}

void LogRemovedInvalidDcc(bool isPath, navzone_policy::DccEntryAction action, void const* zone, void const* dcc)
{
    if (ai::ShuttingDown()) return;
    if (++invalidDccScrubbed <= 8) {
        char const* reason = action == navzone_policy::DccEntryAction::RemoveNull ? "null"
                           : action == navzone_policy::DccEntryAction::RemoveRetired ? "retired"
                           : "orphan-entity";
        Logf("navzone: scrubbed %s DCC=%p from %s=%p count=%u",
             reason, dcc, isPath ? "navpath" : "navzone", zone, invalidDccScrubbed);
    }
}

template <typename ZoneT>
void ScrubDccArray(ZoneT* zone, bTPtrArray<gCDynamicCollisionCircle_PS*>& arr, bool isPath)
{
    for (GEInt i = arr.GetCount() - 1; i >= 0; --i) {
        gCDynamicCollisionCircle_PS* dcc = arr.GetAt(i);
        bool retired = false;
        if (dcc) {
            std::lock_guard<std::mutex> lock(registryMutex);
            retired = registry.IsRetired(dcc);
        }
        // Never dereference a null or retired/destroyed DCC pointer.
        eCEntity const* owner = (!dcc || retired) ? nullptr : dcc->GetEntity();
        const auto action = navzone_policy::ClassifyDccEntry(dcc, retired, owner != nullptr);
        if (action == navzone_policy::DccEntryAction::Keep) continue;
        arr.RemoveAt(i);
        if (dcc) {
            std::lock_guard<std::mutex> lock(registryMutex);
            registry.OnDeregister(zone, isPath, dcc);
        }
        LogRemovedInvalidDcc(isPath, action, zone, dcc);
    }
}

void DeregisterDccDirect(void* zone, bool isPath, gCDynamicCollisionCircle_PS* dcc)
{
    if (!zone || !dcc) return;
    if (isPath) {
        auto* path = static_cast<gCNavPath_PS*>(zone);
        if (originalPathDeregisterDcc) originalPathDeregisterDcc(path, nullptr, dcc);
        else path->DeregisterDCCPS(dcc);
    } else {
        auto* navZone = static_cast<gCNavZone_PS*>(zone);
        if (originalZoneDeregisterDcc) originalZoneDeregisterDcc(navZone, nullptr, dcc);
        else navZone->DeregisterDCCPS(dcc);
    }
}

void DeregisterDccFromZoneEntity(eCEntity const* zoneEntity, gCDynamicCollisionCircle_PS* dcc)
{
    if (!zoneEntity || !dcc) return;
    auto* mutableZone = const_cast<eCEntity*>(zoneEntity);
    if (auto* navZone = static_cast<gCNavZone_PS*>(mutableZone->GetPropertySet(eEPropertySetType_NavZone))) {
        DeregisterDccDirect(navZone, false, dcc);
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnDeregister(navZone, false, dcc);
    }
    if (auto* navPath = static_cast<gCNavPath_PS*>(mutableZone->GetPropertySet(eEPropertySetType_NavPath))) {
        DeregisterDccDirect(navPath, true, dcc);
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnDeregister(navPath, true, dcc);
    }
}

void RegisterDccInZoneEntity(eCEntity const* zoneEntity, gCDynamicCollisionCircle_PS* dcc)
{
    if (!zoneEntity || !dcc || !dcc->GetEntity()) return;
    auto* mutableZone = const_cast<eCEntity*>(zoneEntity);
    if (auto* navZone = static_cast<gCNavZone_PS*>(mutableZone->GetPropertySet(eEPropertySetType_NavZone))) {
        navZone->RegisterDCCPS(dcc);
    } else if (auto* navPath = static_cast<gCNavPath_PS*>(mutableZone->GetPropertySet(eEPropertySetType_NavPath))) {
        navPath->RegisterDCCPS(dcc);
    }
}

void DeregisterTrackedZones(gCDynamicCollisionCircle_PS* dcc, bool markRetired)
{
    if (!dcc) return;
    std::vector<navzone_policy::ZoneTarget> targets;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        targets = registry.TakeZonesForDcc(dcc, markRetired);
    }
    for (navzone_policy::ZoneTarget const& target : targets)
        DeregisterDccDirect(target.zone, target.isPath, dcc);
}

// gCNavigationMap::DetectEvades (Game.dll+0x134740) reads CurrentZoneEntityProxy->GetPropertySet(NavZone) without
// checking for null (e.g. when CurrentZoneEntityProxy points to a gCNavPath_PS entity) before calling
// gCNavZone_PS::DetectCollisionCircleExit (Game.dll+0x117c00), crashing at +0x117c09 reading [ecx+0xd4].
GEBool __fastcall SafeDetectCollisionCircleExit(gCNavZone_PS* self, void* edx, gCNavigation_PS* nav, bCVector* out)
{
    if (!self || !nav || !out) {
        if (!ai::ShuttingDown() && ++navZoneNullBlocked == 1)
            Log("navzone: blocked null-zone gCNavZone_PS::DetectCollisionCircleExit call");
        return GEFalse;
    }
    return originalDetectCollisionCircleExit ? originalDetectCollisionCircleExit(self, edx, nav, out) : GEFalse;
}

GEBool __fastcall SafeZoneTestPointAgainstDCCs(gCNavZone_PS* self, void* edx, bCVector const* point, eCEntity const* entity)
{
    if (!self || !point || !entity) return GETrue;
    ScrubDccArray(self, ZoneDccArray(self), false);
    return originalZoneTestPointDcc ? originalZoneTestPointDcc(self, edx, point, entity) : GETrue;
}

GEBool __fastcall SafePathTestPointAgainstDCCs(gCNavPath_PS* self, void* edx, bCVector const* point, eCEntity const* entity)
{
    if (!self || !point || !entity) return GETrue;
    ScrubDccArray(self, self->m_arrDCCPS, true);
    return originalPathTestPointDcc ? originalPathTestPointDcc(self, edx, point, entity) : GETrue;
}

bEResult __fastcall SafeZoneRegisterDcc(gCNavZone_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc)
{
    if (!self) return bEResult_False;
    if (!dcc) return bEResult_Ok;
    if (!dcc->GetEntity()) {
        if (!ai::ShuttingDown() && ++orphanDccRegisterBlocked <= 4)
            Logf("navzone: blocked orphan DCC=%p RegisterDCCPS on navzone=%p", dcc, self);
        return bEResult_Ok;
    }
    const bEResult result = originalZoneRegisterDcc ? originalZoneRegisterDcc(self, edx, dcc) : bEResult_False;
    std::lock_guard<std::mutex> lock(registryMutex);
    registry.OnRegister(self, false, dcc);
    return result;
}

bEResult __fastcall SafeZoneDeregisterDcc(gCNavZone_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc)
{
    if (!self) return bEResult_False;
    const bEResult result = originalZoneDeregisterDcc ? originalZoneDeregisterDcc(self, edx, dcc) : bEResult_False;
    if (dcc) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnDeregister(self, false, dcc);
    }
    return result;
}

bEResult __fastcall SafePathRegisterDcc(gCNavPath_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc)
{
    if (!self) return bEResult_False;
    if (!dcc) return bEResult_Ok;
    if (!dcc->GetEntity()) {
        if (!ai::ShuttingDown() && ++orphanDccRegisterBlocked <= 4)
            Logf("navzone: blocked orphan DCC=%p RegisterDCCPS on navpath=%p", dcc, self);
        return bEResult_Ok;
    }
    const bEResult result = originalPathRegisterDcc ? originalPathRegisterDcc(self, edx, dcc) : bEResult_False;
    std::lock_guard<std::mutex> lock(registryMutex);
    registry.OnRegister(self, true, dcc);
    return result;
}

bEResult __fastcall SafePathDeregisterDcc(gCNavPath_PS* self, void* edx, gCDynamicCollisionCircle_PS* dcc)
{
    if (!self) return bEResult_False;
    const bEResult result = originalPathDeregisterDcc ? originalPathDeregisterDcc(self, edx, dcc) : bEResult_False;
    if (dcc) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnDeregister(self, true, dcc);
    }
    return result;
}

void __fastcall SafeDccDestroy(gCDynamicCollisionCircle_PS* self, void* edx)
{
    if (!self) return;
    if (!ai::ShuttingDown()) {
        if (eCEntity* owner = self->GetEntity()) {
            if (auto* nav = static_cast<gCNavigation_PS*>(owner->GetPropertySet(eEPropertySetType_Navigation))) {
                DeregisterDccFromZoneEntity(nav->GetCurrentZoneEntityProxy().GetEntity(), self);
                DeregisterDccFromZoneEntity(nav->GetLastZoneEntityProxy().GetEntity(), self);
            }
        }
        DeregisterTrackedZones(self, true);
    }
    if (originalDccDestroy) originalDccDestroy(self, edx);
}

bEResult __fastcall SafeMapDeregisterZone(gCNavigationMap* self, void* edx, gCNavZone_PS* zone)
{
    if (zone) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnZoneRemoved(zone, false);
    }
    return originalMapDeregisterZone ? originalMapDeregisterZone(self, edx, zone) : bEResult_False;
}

bEResult __fastcall SafeMapDeregisterPath(gCNavigationMap* self, void* edx, gCNavPath_PS* path)
{
    if (path) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnZoneRemoved(path, true);
    }
    return originalMapDeregisterPath ? originalMapDeregisterPath(self, edx, path) : bEResult_False;
}

template <typename Fn, size_t N>
bool InstallHook(HMODULE game, char const* exportName, unsigned char const (&head)[N], void* detour, Fn& originalOut)
{
    auto* entry = reinterpret_cast<unsigned char*>(GetProcAddress(game, exportName));
    if (!entry || (std::memcmp(entry, head, sizeof(head)) != 0 && !patch::IsPatchedByUs(entry, head, sizeof(head))))
        return false;
    void* trampoline = ai::JumpTo(entry, detour, sizeof(head));
    if (!trampoline) return false;
    originalOut = reinterpret_cast<Fn>(trampoline);
    return true;
}
}

void MigrateZone(eCEntity const* previousZone, eCEntity const* lastZone, eCEntity const* newZone,
                 gCDynamicCollisionCircle_PS* navDcc, gCDynamicCollisionCircle_PS* entityDcc)
{
    if (ai::ShuttingDown()) return;
    const auto plan = navzone_policy::PlanZoneMigration(
        const_cast<eCEntity*>(previousZone), const_cast<eCEntity*>(lastZone),
        const_cast<eCEntity*>(newZone), navDcc, entityDcc);
    for (std::size_t zi = 0; zi < plan.deregisterZoneCount; ++zi) {
        auto* oldZone = static_cast<eCEntity const*>(plan.deregisterZones[zi]);
        for (std::size_t di = 0; di < plan.deregisterDccCount; ++di)
            DeregisterDccFromZoneEntity(oldZone, static_cast<gCDynamicCollisionCircle_PS*>(plan.deregisterDccs[di]));
    }
    if (plan.registerInNewZone) RegisterDccInZoneEntity(newZone, navDcc);
}

void OnEntityDestroyed(eCEntity* entity)
{
    if (!entity || ai::ShuttingDown()) return;
    if (auto* navZone = static_cast<gCNavZone_PS*>(entity->GetPropertySet(eEPropertySetType_NavZone))) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnZoneRemoved(navZone, false);
    }
    if (auto* navPath = static_cast<gCNavPath_PS*>(entity->GetPropertySet(eEPropertySetType_NavPath))) {
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.OnZoneRemoved(navPath, true);
    }
    auto* entityDcc = static_cast<gCDynamicCollisionCircle_PS*>(
        entity->GetPropertySet(eEPropertySetType_DynamicCollisionCircle));
    auto* nav = static_cast<gCNavigation_PS*>(entity->GetPropertySet(eEPropertySetType_Navigation));
    auto* navDcc = nav ? nav->GetDCC() : nullptr;
    if (!entityDcc && !navDcc) return;

    eCEntity const* currentZone = nav ? nav->GetCurrentZoneEntityProxy().GetEntity() : nullptr;
    eCEntity const* lastZone = nav ? nav->GetLastZoneEntityProxy().GetEntity() : nullptr;
    eCEntity const* probedZone = nullptr;
    if (auto* admin = static_cast<gCNavigationAdmin*>(
            eCModuleAdmin::GetInstance().FindModule(bCString("gCNavigationAdmin")))) {
        eCEntityProxy here;
        GEBool isPath = GEFalse;
        if (admin->GetZone(*entity, here, isPath, GEFalse) == bEResult_Ok) probedZone = here.GetEntity();
    }

    auto scrubDcc = [&](gCDynamicCollisionCircle_PS* dcc) {
        if (!dcc) return;
        DeregisterDccFromZoneEntity(currentZone, dcc);
        if (lastZone && lastZone != currentZone) DeregisterDccFromZoneEntity(lastZone, dcc);
        if (probedZone && probedZone != currentZone && probedZone != lastZone)
            DeregisterDccFromZoneEntity(probedZone, dcc);
        DeregisterTrackedZones(dcc, true);
    };
    scrubDcc(navDcc);
    if (entityDcc && entityDcc != navDcc) scrubDcc(entityDcc);
}

void OnWorldChange()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    registry.Clear();
}

void Install()
{
    if (navZoneGuardSearched || ai::ShuttingDown()) return;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return;
    navZoneGuardSearched = true;

    // CP 1.75 Game.dll+0x117c00: sub esp, 0xc; push ebx; push ebp; mov ebx, ecx (7 bytes).
    static const unsigned char exitHead[] = {0x83, 0xec, 0x0c, 0x53, 0x55, 0x8b, 0xd9};
    // CP 1.75 Game.dll+0x117760 / 0x111e80: push ebp; mov ebp, esp; and esp, -8 (6 bytes).
    static const unsigned char testDccHead[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    // CP 1.75 Game.dll+0x114940 / 0x111760: push ebx; mov ebx, [esp+8]; test ebx, ebx (7 bytes).
    static const unsigned char regDccHead[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x85, 0xdb};
    // CP 1.75 Game.dll+0x1149b0 / 0x1117d0: mov edx, [esp+4]; test edx, edx (6 bytes).
    static const unsigned char deregDccHead[] = {0x8b, 0x54, 0x24, 0x04, 0x85, 0xd2};
    // CP 1.75 Game.dll+0x1069d0: push esi; push edi; mov esi, ecx; xor edi, edi (6 bytes).
    static const unsigned char dccDestroyHead[] = {0x56, 0x57, 0x8b, 0xf1, 0x33, 0xff};
    // CP 1.75 Game.dll+0x122c80 / 0x122cf0: push esi; mov esi, ecx; mov eax, [esi+0x3c|0x48] (6 bytes).
    static const unsigned char mapDeregZoneHead[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x3c};
    static const unsigned char mapDeregPathHead[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x48};

    const bool exitOk = InstallHook(
        game, "?DetectCollisionCircleExit@gCNavZone_PS@@QAE_NPAVgCNavigation_PS@@AAVbCVector@@@Z",
        exitHead, reinterpret_cast<void*>(&SafeDetectCollisionCircleExit), originalDetectCollisionCircleExit);
    const bool zoneTestOk = InstallHook(
        game, "?TestPointAgainstDCCs@gCNavZone_PS@@QAE_NABVbCVector@@ABVeCEntity@@@Z",
        testDccHead, reinterpret_cast<void*>(&SafeZoneTestPointAgainstDCCs), originalZoneTestPointDcc);
    const bool pathTestOk = InstallHook(
        game, "?TestPointAgainstDCCs@gCNavPath_PS@@QAE_NABVbCVector@@ABVeCEntity@@@Z",
        testDccHead, reinterpret_cast<void*>(&SafePathTestPointAgainstDCCs), originalPathTestPointDcc);
    const bool zoneRegOk = InstallHook(
        game, "?RegisterDCCPS@gCNavZone_PS@@QAE?AW4bEResult@@PAVgCDynamicCollisionCircle_PS@@@Z",
        regDccHead, reinterpret_cast<void*>(&SafeZoneRegisterDcc), originalZoneRegisterDcc);
    const bool zoneDeregOk = InstallHook(
        game, "?DeregisterDCCPS@gCNavZone_PS@@QAE?AW4bEResult@@PAVgCDynamicCollisionCircle_PS@@@Z",
        deregDccHead, reinterpret_cast<void*>(&SafeZoneDeregisterDcc), originalZoneDeregisterDcc);
    const bool pathRegOk = InstallHook(
        game, "?RegisterDCCPS@gCNavPath_PS@@QAE?AW4bEResult@@PAVgCDynamicCollisionCircle_PS@@@Z",
        regDccHead, reinterpret_cast<void*>(&SafePathRegisterDcc), originalPathRegisterDcc);
    const bool pathDeregOk = InstallHook(
        game, "?DeregisterDCCPS@gCNavPath_PS@@QAE?AW4bEResult@@PAVgCDynamicCollisionCircle_PS@@@Z",
        deregDccHead, reinterpret_cast<void*>(&SafePathDeregisterDcc), originalPathDeregisterDcc);
    const bool dccDestroyOk = InstallHook(
        game, "?Destroy@gCDynamicCollisionCircle_PS@@UAEXXZ",
        dccDestroyHead, reinterpret_cast<void*>(&SafeDccDestroy), originalDccDestroy);
    const bool mapZoneOk = InstallHook(
        game, "?DeregisterNavZonePS@gCNavigationMap@@QAE?AW4bEResult@@PAVgCNavZone_PS@@@Z",
        mapDeregZoneHead, reinterpret_cast<void*>(&SafeMapDeregisterZone), originalMapDeregisterZone);
    const bool mapPathOk = InstallHook(
        game, "?DeregisterNavPathPS@gCNavigationMap@@QAE?AW4bEResult@@PAVgCNavPath_PS@@@Z",
        mapDeregPathHead, reinterpret_cast<void*>(&SafeMapDeregisterPath), originalMapDeregisterPath);

    Logf("navzone: guards installed exit=%d zone_dcc=%d path_dcc=%d reg=%d/%d dereg=%d/%d dcc_destroy=%d map=%d/%d",
         exitOk ? 1 : 0, zoneTestOk ? 1 : 0, pathTestOk ? 1 : 0,
         zoneRegOk ? 1 : 0, pathRegOk ? 1 : 0, zoneDeregOk ? 1 : 0, pathDeregOk ? 1 : 0,
         dccDestroyOk ? 1 : 0, mapZoneOk ? 1 : 0, mapPathOk ? 1 : 0);
}
}
