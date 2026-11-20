// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <cstdio>

class eCEntity;

namespace g3t::npclease {
void Tick();
void OnWorldDeactivate();
void OnEntityDestroyed(eCEntity const* entity);
void CaptureCacheInventory();
bool NewLife(uint8_t const guid[16]);
bool LocalAuthority(eCEntity const* entity);
bool LeasedCombatHostile(eCEntity const* entity);
bool VettedLocalAuthority(eCEntity const* entity);
bool HandbackPending(uint8_t const guid[16]);
bool PreviouslyVetted(uint8_t const guid[16]);
void RelinquishToHost(eCEntity* entity, char const* reason = "role transfer to host");
// The client drives this NPC natively right now (provisional or host-confirmed). Safe to call with any GUID.
bool LocalOwner(uint8_t const guid[16]);
bool ProcessingAuthority(eCEntity const* entity);
void NoteHit(eCEntity* victim, eCEntity* source, char const* path, int before, int after);
char const* UnavailabilityReason(eCEntity* entity);
bool Fenced(eCEntity const* entity);
bool SuppressHostPublication(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16]);
bool IgnoreHostSnapshot(uint8_t const guid[16], uint32_t captured);
void ForgetReceivedNpcPose(uint8_t const guid[16]);
void ForgetReceivedNpcPoseBefore(uint8_t const guid[16], uint32_t incarnation, uint32_t sent);
void SyncMirrorPoseInterval(uint8_t const guid[16], uint32_t incarnation, uint32_t intervalMs);
uint8_t RemoteOwner(eCEntity const* entity);
uint8_t RemoteOwner(uint8_t const guid[16]);
uint32_t Life(uint8_t const guid[16]);
bool ContactAllowed(eCEntity const* victim, eCEntity const* source);
bool ActiveLeases();
// Retired save API: capture is empty and restore discards legacy client checkpoints.
SaveOverlay Capture();
bool Restore(SaveOverlay const& overlay, uint32_t expectedWorldEpoch = 0);
bool WriteSnapshot(FILE* file);
bool ReadSnapshot(FILE* file, SaveOverlay& overlay);
void RejectSnapshot(char const* reason);

namespace detail {
bool Player(eCEntity const* entity);
}
}
