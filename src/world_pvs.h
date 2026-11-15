// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Engine.h>
#include <array>
#include <cstddef>
#include <vector>
#include "hostsim_status.h"

namespace g3t::world::pvs {
struct ScopedCellLock {
    bCCriticalSectionWin32& lock = eCPVSCellItem::GetCriticalSection();
    ScopedCellLock() { lock.Acquire(); }
    ~ScopedCellLock() { lock.Release(); }
    ScopedCellLock(ScopedCellLock const&) = delete;
    ScopedCellLock& operator=(ScopedCellLock const&) = delete;
};

bool GatePaused();
bool InstallDatabaseIndexFix();
std::vector<std::array<float, 3>> const& SimulationAnchors();
std::vector<std::array<float, 3>> const& ReadySimulationAnchors();
void CollectReadyPrefetchers(std::vector<eCPVSPrefetcher3*>& out);
std::size_t ActiveRegionCount();
void TickHostRegions();
void RefreshContexts();
void RetireHostRegions();
void FillRegionStatus(HostSimStatus& status);
void CaptureRegionCacheInventory(bTSmallArray<eCEntity*> const* range, void const* spheresData, unsigned sphereSize);
void StartDestroyDiagnostics();
}
