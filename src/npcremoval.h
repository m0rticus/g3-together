// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class eCEntity;
namespace g3t::npcremoval {
void Start();
void Tick();
void OnWorldDeactivate();
void CaptureCacheInventory();  // engine thread only; raw applied pointers, not resolved GUIDs
bool ExcludeFromProcessing(eCEntity const* entity);
bool Suppressed(eCEntity const* entity);
bool Suppressed(uint8_t const guid[16]);
}
