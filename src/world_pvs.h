// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Engine.h>
#include <array>
#include <cstddef>
#include <vector>
#include "hostsim_status.h"

namespace g3t::world::pvs {
bool GatePaused();
std::vector<std::array<float, 3>> const& SimulationAnchors();
std::size_t ActiveRegionCount();
void TickHostRegions();
void RefreshContexts();
void RetireHostRegions();
void FillRegionStatus(HostSimStatus& status);
void CaptureRegionCacheInventory(bTSmallArray<eCEntity*> const* range, void const* spheresData, unsigned sphereSize);
void StartDestroyDiagnostics();
}
