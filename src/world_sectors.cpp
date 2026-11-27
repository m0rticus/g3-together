// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_sectors.h"

#include "ai.h"
#include "log.h"
#include "persist.h"
#include "saving_store.h"
#include "sector_sync_policy.h"
#include "telemetry.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::sectors {
namespace {

using SectorEnableFn = GEBool(__fastcall*)(gCSector*, void*, GEBool);
using ChangeSectorStatusFn = void(__stdcall*)(void*, void*, void*);

SectorEnableFn originalSectorEnable = nullptr;
ChangeSectorStatusFn originalChangeStatus = nullptr;
bool hooksTried = false;
bool applying = false;

sector_sync::HostState hostState;
sector_sync::ClientState clientState;
sector_sync::StateMap deferredClientLive;
uint32_t nextRequestAt = 0;
unsigned sentCount = 0;
unsigned appliedCount = 0;

struct CachedHostSector {
    gCSector* sector = nullptr;
    bool enabled = false;
};
std::vector<CachedHostSector> cachedHostSectors;
GEInt cachedHostSectorCount = -1;

void RefreshHostSectorCache()
{
    cachedHostSectors.clear();
    cachedHostSectorCount = -1;
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world) return;
    const GEInt count = world->GetSectorCount();
    cachedHostSectorCount = count;
    cachedHostSectors.reserve(static_cast<size_t>(count > 0 ? count : 0));
    for (GEInt i = 0; i < count; ++i) {
        gCSector* sector = world->GetSectorAt(i);
        if (!sector) continue;
        char const* name = sector->GetFileBaseName().GetText();
        if (!name || !*name) continue;
        cachedHostSectors.push_back({sector, sector->IsEnabled() != GEFalse});
    }
}

bool HostSectorsDirty()
{
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world) return false;
    if (world->GetSectorCount() != cachedHostSectorCount) return true;
    for (auto const& entry : cachedHostSectors) {
        if ((entry.sector->IsEnabled() != GEFalse) != entry.enabled) return true;
    }
    return false;
}

gCSectorPersistence_PS* SectorPersistence()
{
    gCWorld* world = gCWorld::GetCurrentWorld();
    eCEntity* worldEntity = world ? world->GetWorldEntity() : nullptr;
    if (!worldEntity) worldEntity = Entity::GetWorldEntity().GetInstance();
    if (!worldEntity) return nullptr;
    return static_cast<gCSectorPersistence_PS*>(worldEntity->GetPropertySet(eEPropertySetType_SectorPersistence));
}

void RefreshPersistence()
{
    if (auto* ps = SectorPersistence()) ps->CollectSectors();
}

sector_sync::StateMap CaptureWorldSectors()
{
    sector_sync::StateMap out;
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world) return out;
    const GEInt count = world->GetSectorCount();
    for (GEInt i = 0; i < count; ++i) {
        gCSector* sector = world->GetSectorAt(i);
        if (!sector) continue;
        char const* name = sector->GetFileBaseName().GetText();
        if (!name || !*name) continue;
        out[name] = sector->IsEnabled() != GEFalse;
    }
    return out;
}

void BroadcastLive(char const* name, bool enabled, char const* source)
{
    if (!name || !*name) return;
    if (std::strlen(name) > sector_sync::MaxWireNameLen) {
        Logf("sectors: sector name exceeds %zu wire bytes (%s); skipped", sector_sync::MaxWireNameLen, name);
        return;
    }
    if (net::Connected()) {
        net::WorldState s{};
        s.kind = sector_sync::WorldStateKind;
        strncpy_s(s.routine, name, _TRUNCATE);
        s.enclave = enabled ? 1 : 0;
        net::SendWorldState(s, 0);
        ++sentCount;
    }
    Logf("sectors: host broadcast source=%s sector=%s enabled=%d", source ? source : "live", name, enabled ? 1 : 0);
    telemetry::OnSectorChange(source ? source : "host", 1, name, enabled);
}

GEBool __fastcall SectorEnableDetour(gCSector* self, void*, GEBool enable)
{
    if (ai::ShuttingDown() || !self) return originalSectorEnable ? originalSectorEnable(self, nullptr, enable) : GEFalse;
    const bool running = gCSession::GetInstance().IsGameRunning() && !persist::NativeLoading();
    if (!running) return originalSectorEnable(self, nullptr, enable);
    if (!applying && net::IsHost() && !hostState.seeded)
        hostState.Seed(CaptureWorldSectors());
    const bool wasEnabled = self->IsEnabled() != GEFalse;
    const GEBool ok = originalSectorEnable(self, nullptr, enable);
    const bool nowEnabled = self->IsEnabled() != GEFalse;
    if (wasEnabled != nowEnabled) {
        RefreshPersistence();
        if (!applying && net::IsHost()) {
            char const* name = self->GetFileBaseName().GetText();
            if (name && *name && hostState.RecordLive(name, nowEnabled))
                BroadcastLive(name, nowEnabled, "native");
            RefreshHostSectorCache();
        }
        world::RequestProcessingUpdate();
    }
    return ok;
}

void __stdcall ChangeSectorStatusDetour(void* self, void* sender, void* arg)
{
    if (!ai::ShuttingDown() && !net::IsHost() && net::Joined()) {
        Log("sectors: console ToggleSector/ToggleFolder refused on client (host-authoritative)");
        return;
    }
    if (originalChangeStatus) originalChangeStatus(self, sender, arg);
}

void InstallHooks()
{
    if (hooksTried) return;
    hooksTried = true;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) {
        Log("sectors: Game.dll unavailable; sector hooks off (polling fallback active)");
        return;
    }
    auto* enableEntry = reinterpret_cast<unsigned char*>(GetProcAddress(game, "?Enable@gCSector@@QAE_N_N@Z"));
    static constexpr unsigned char enablePrefix[] = {0x51, 0x56, 0x8b, 0xf1, 0x68};
    if (enableEntry && std::memcmp(enableEntry, enablePrefix, sizeof(enablePrefix)) == 0) {
        originalSectorEnable = reinterpret_cast<SectorEnableFn>(
            ai::JumpTo(enableEntry, reinterpret_cast<void*>(&SectorEnableDetour), 9));
    }
    auto* consoleEntry = reinterpret_cast<unsigned char*>(
        GetProcAddress(game, "?OnChangeSectorStatus@gCGameApp@@QAGXPAVbCObjectRefBase@@PAVeCConsoleEventArg@@@Z"));
    static constexpr unsigned char consolePrefix[] = {0x55, 0x56, 0x8b, 0x74, 0x24, 0x14};
    if (consoleEntry && std::memcmp(consoleEntry, consolePrefix, sizeof(consolePrefix)) == 0) {
        originalChangeStatus = reinterpret_cast<ChangeSectorStatusFn>(
            ai::JumpTo(consoleEntry, reinterpret_cast<void*>(&ChangeSectorStatusDetour), 6));
    }
    Logf("sectors: Game.dll hooks installed enable=%d console=%d",
         originalSectorEnable ? 1 : 0, originalChangeStatus ? 1 : 0);
}

bool CaptureSaveExtension(std::vector<uint8_t>& bytes)
{
    RefreshPersistence();
    const auto states = CaptureWorldSectors();
    return sector_sync::EncodeExtension(states, bytes);
}

bool RestoreSaveExtension(uint32_t savedVersion, std::vector<uint8_t> const& bytes)
{
    sector_sync::StateMap saved;
    bool present = false;
    if (!sector_sync::DecodeExtension(savedVersion, bytes, saved, present)) {
        Logf("sectors: invalid world_sectors save extension version=%u bytes=%zu", savedVersion, bytes.size());
        return false;
    }
    if (present) {
        unsigned changed = 0;
        if (gCWorld* world = gCWorld::GetCurrentWorld()) {
            applying = true;
            for (auto const& [name, enabled] : saved) {
                gCSector* sector = world->GetSector(bCString(name.c_str()));
                if (!sector) continue;
                if ((sector->IsEnabled() != GEFalse) != enabled) {
                    sector->Enable(enabled ? GETrue : GEFalse);
                    ++changed;
                }
            }
            applying = false;
        }
        if (changed) {
            RefreshPersistence();
            world::RequestProcessingUpdate();
        }
        Logf("sectors: restored save extension entries=%zu changed=%u", saved.size(), changed);
    }
    hostState.Seed(CaptureWorldSectors());
    RefreshHostSectorCache();
    return true;
}

void HandleSnapshotRequest(uint8_t from)
{
    if (!net::IsHost() || !net::InWorld(1) || !net::PeerJoined(from) ||
        persist::NativeLoading() || persist::HeroPending())
        return;
    if (!hostState.seeded) hostState.Seed(CaptureWorldSectors());
    const uint32_t generation = net::PeerGeneration(from);
    const auto current = CaptureWorldSectors();
    sector_sync::StateMap const* snapshot = hostState.BeginReplay(from, generation, current);
    if (!snapshot) return;

    net::WorldState begin{};
    begin.kind = sector_sync::WorldStateKind;
    begin.npcClass = sector_sync::FlagBaseline;
    bool ok = net::SendWorldStateTo(from, begin);

    uint32_t sentEntries = 0;
    for (auto const& [name, enabled] : *snapshot) {
        if (!ok) break;
        if (name.size() > sector_sync::MaxWireNameLen) {
            Logf("sectors: skipping oversized sector name (%s) in baseline", name.c_str());
            continue;
        }
        net::WorldState entry{};
        entry.kind = sector_sync::WorldStateKind;
        entry.npcClass = sector_sync::FlagBaseline;
        strncpy_s(entry.routine, name.c_str(), _TRUNCATE);
        entry.enclave = enabled ? 1 : 0;
        ok = net::SendWorldStateTo(from, entry);
        if (ok) ++sentEntries;
    }

    if (ok) {
        net::WorldState end{};
        end.kind = sector_sync::WorldStateKind;
        end.npcClass = sector_sync::FlagBaseline | sector_sync::FlagEnd;
        sector_sync::PackCount(end.guid, sentEntries);
        ok = net::SendWorldStateTo(from, end);
    }
    if (ok) {
        hostState.MarkReplaySent(from, generation);
        Logf("sectors: full baseline (%u entries) sent to player %u", sentEntries, from);
    } else {
        Logf("sectors: full baseline send failed player=%u; admission retry required", from);
    }
}

void CommitClientBaseline()
{
    if (net::IsHost() || !clientState.admissionStarted || clientState.admissionApplied || !clientState.complete) return;
    if (!gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero() || persist::NativeLoading())
        return;
    sector_sync::StateMap commitMap;
    if (!clientState.TakeCommitMap(commitMap)) return;
    unsigned changed = 0;
    for (auto const& [name, enabled] : commitMap) {
        if (ApplySector(name.c_str(), enabled, "baseline")) ++changed;
    }
    Logf("sectors: authoritative baseline applied entries=%zu changed=%u", commitMap.size(), changed);
}

} // namespace

void Start()
{
    InstallHooks();
    persist::SaveExtensions::Register("world_sectors", sector_sync::ExtensionType, sector_sync::ExtensionVersion,
                                      &CaptureSaveExtension, &RestoreSaveExtension);
}

bool ApplySector(char const* name, bool enabled, char const* source)
{
    if (!name || !*name) return false;
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world || !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading()) {
        if (!net::IsHost()) deferredClientLive[name] = enabled;
        return false;
    }
    gCSector* sector = world->GetSector(bCString(name));
    if (!sector) {
        Logf("sectors: unknown sector %s (source=%s)", name, source ? source : "-");
        return false;
    }
    const bool wasEnabled = sector->IsEnabled() != GEFalse;
    if (wasEnabled != enabled) {
        applying = true;
        sector->Enable(enabled ? GETrue : GEFalse);
        applying = false;
        RefreshPersistence();
        world::RequestProcessingUpdate();
        ++appliedCount;
        Logf("sectors: sector %s %s (source=%s)", name, enabled ? "on" : "off", source ? source : "-");
        telemetry::OnSectorChange(source ? source : "apply", 0, name, enabled);
    }
    if (net::IsHost()) {
        if (!hostState.seeded) hostState.Seed(CaptureWorldSectors());
        hostState.RecordLive(name, sector->IsEnabled() != GEFalse);
    } else {
        clientState.known[name] = sector->IsEnabled() != GEFalse;
    }
    return wasEnabled != enabled;
}

bool HandleWire(uint8_t from, net::WorldState const& s)
{
    if (s.kind != sector_sync::WorldStateKind) return false;
    if ((s.npcClass & sector_sync::HandshakeMask) != 0) {
        if (s.npcClass == sector_sync::FlagRequest) {
            if (net::IsHost()) HandleSnapshotRequest(from);
            return true;
        }
        if (net::IsHost() || from != 1 || !clientState.admissionStarted) return true;
        const uint32_t countHint = sector_sync::UnpackCount(s.guid);
        const bool accepted = clientState.OnBaselineMessage(s.npcClass, s.routine, s.enclave != 0, countHint);
        if (accepted && s.npcClass == (sector_sync::FlagBaseline | sector_sync::FlagCatchup) &&
            clientState.admissionApplied) {
            ApplySector(s.routine, s.enclave != 0, "catchup");
        }
        return true;
    }
    if (s.npcClass != 0 || !s.routine[0]) return true;
    if (net::IsHost()) {
        if (!net::InWorld(from) || !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading()) return true;
        gCWorld* world = gCWorld::GetCurrentWorld();
        if (!world || !world->GetSector(bCString(s.routine))) return true;
        return false; // Let infocmd::OnWorldState relay except `from` and call ApplySector on host.
    }
    if (from != 1) return true;
    clientState.OnLiveDelta(s.routine, s.enclave != 0);
    if (clientState.admissionApplied) ApplySector(s.routine, s.enclave != 0, "host");
    return true;
}

void BroadcastIfChanged(char const* name, bool enabled)
{
    if (!net::IsHost() || !name || !*name) return;
    if (!hostState.seeded) hostState.Seed(CaptureWorldSectors());
    if (hostState.RecordLive(name, enabled))
        BroadcastLive(name, enabled, "infocmd");
}

void SyncToClients()
{
    if (!net::IsHost() || !net::Connected() || !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading())
        return;
    if (!hostState.seeded) hostState.Seed(CaptureWorldSectors());
    const auto current = CaptureWorldSectors();
    const auto deltas = hostState.Reconcile(current);
    for (auto const& [name, enabled] : deltas) BroadcastLive(name.c_str(), enabled, "reconcile");
}

void PrepareAdmission()
{
    if (net::IsHost()) return;
    clientState.PrepareAdmission();
    deferredClientLive.clear();
    nextRequestAt = 0;
}

bool AdmissionStarted() { return clientState.admissionStarted; }

bool AdmissionReady()
{
    if (net::IsHost()) return true;
    return clientState.admissionStarted && clientState.admissionApplied;
}

bool FinishAdmission(uint8_t player)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers) return false;
    if (!hostState.seeded) hostState.Seed(CaptureWorldSectors());
    sector_sync::DeltaList catchup;
    if (!hostState.FinishReplay(player, net::PeerGeneration(player), CaptureWorldSectors(), catchup))
        return false;
    for (auto const& [name, enabled] : catchup) {
        if (name.size() > sector_sync::MaxWireNameLen) continue;
        net::WorldState entry{};
        entry.kind = sector_sync::WorldStateKind;
        entry.npcClass = sector_sync::FlagBaseline | sector_sync::FlagCatchup;
        strncpy_s(entry.routine, name.c_str(), _TRUNCATE);
        entry.enclave = enabled ? 1 : 0;
        if (!net::SendWorldStateTo(player, entry)) return false;
    }
    return true;
}

void OnWorldChange()
{
    hostState.Reset();
    clientState.Reset();
    deferredClientLive.clear();
    cachedHostSectors.clear();
    cachedHostSectorCount = -1;
    nextRequestAt = 0;
}

void Tick()
{
    if (ai::ShuttingDown()) return;
    if (net::IsHost()) {
        if (!gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero() ||
            persist::NativeLoading() || persist::HeroPending())
            return;
        if (!hostState.seeded) {
            hostState.Seed(CaptureWorldSectors());
            RefreshHostSectorCache();
            return;
        }
        if (!HostSectorsDirty()) return;
        const auto deltas = hostState.Reconcile(CaptureWorldSectors());
        RefreshHostSectorCache();
        if (!deltas.empty()) {
            RefreshPersistence();
            for (auto const& [name, enabled] : deltas)
                BroadcastLive(name.c_str(), enabled, "reconcile");
            world::RequestProcessingUpdate();
        }
        return;
    }
    if (!net::Joined()) return;
    if (clientState.admissionStarted && !clientState.admissionApplied) {
        if (!clientState.complete) {
            const uint32_t now = GetTickCount();
            if (!nextRequestAt || static_cast<int32_t>(now - nextRequestAt) >= 0) {
                nextRequestAt = now + 1000;
                net::WorldState req{};
                req.kind = sector_sync::WorldStateKind;
                req.npcClass = sector_sync::FlagRequest;
                net::SendWorldStateTo(1, req);
            }
            return;
        }
        CommitClientBaseline();
    }
    if (clientState.admissionApplied && !deferredClientLive.empty() &&
        gCSession::GetInstance().IsGameRunning() && !persist::NativeLoading()) {
        sector_sync::StateMap flush;
        flush.swap(deferredClientLive);
        for (auto const& [name, enabled] : flush) ApplySector(name.c_str(), enabled, "deferred");
    }
}

void Counts(unsigned& sent, unsigned& applied)
{
    sent = sentCount;
    applied = appliedCount;
}

} // namespace g3t::sectors
