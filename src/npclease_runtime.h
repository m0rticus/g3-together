// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <cstdio>

class eCEntity;

namespace g3t::npclease {
void Tick();
void OnWorldDeactivate();
void CaptureCacheInventory();
bool NewLife(uint8_t const guid[16]);
bool LocalAuthority(eCEntity const* entity);
char const* UnavailabilityReason(eCEntity* entity);
bool Fenced(eCEntity const* entity);
bool SuppressHostPublication(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16], uint32_t captured);
uint8_t RemoteOwner(eCEntity const* entity);
uint32_t Life(uint8_t const guid[16]);
bool ContactAllowed(eCEntity const* victim, eCEntity const* source);
bool ActiveLeases();
// Canonical state only; no entity access, IO, native calls or live owner clocks.
SaveOverlay Capture();
// Pass the saving world's epoch when restoring before its first network publication.
bool Restore(SaveOverlay const& overlay, uint32_t expectedWorldEpoch = 0);
bool WriteSnapshot(FILE* file);
bool ReadSnapshot(FILE* file, SaveOverlay& overlay);
void RejectSnapshot(char const* reason);
}
