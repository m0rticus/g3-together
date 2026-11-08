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
// The client drives this NPC natively right now (provisional or host-confirmed). Safe to call with any GUID.
bool LocalOwner(uint8_t const guid[16]);
bool ProcessingAuthority(eCEntity const* entity);
void NoteHit(eCEntity* victim, eCEntity* source, char const* path, int before, int after);
char const* UnavailabilityReason(eCEntity* entity);
bool Fenced(eCEntity const* entity);
bool SuppressHostPublication(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16], uint32_t captured);
uint8_t RemoteOwner(eCEntity const* entity);
uint32_t Life(uint8_t const guid[16]);
bool ContactAllowed(eCEntity const* victim, eCEntity const* source);
bool ActiveLeases();
// Retired save API: capture is empty and restore discards legacy client checkpoints.
SaveOverlay Capture();
bool Restore(SaveOverlay const& overlay, uint32_t expectedWorldEpoch = 0);
bool WriteSnapshot(FILE* file);
bool ReadSnapshot(FILE* file, SaveOverlay& overlay);
void RejectSnapshot(char const* reason);
}
