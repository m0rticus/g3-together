// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include "npclease_client.h"
#include "npclease_guards.h"
#include <array>
#include <cstdio>
#include <unordered_map>

class eCEntity;

namespace g3t::npclease {
void Tick();
void OnWorldDeactivate();
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
struct Entry {
    Lease lease;
    Eligibility interest;
    ClientWindow window;
    uint32_t lastRequest = 0, lastState = 0, lastControl = 0, localDeadline = 0;
    uint32_t echo = 0, welcome = 0, world = 0, recallAt = 0;
    uint32_t previousGeneration = 0;
    uint32_t appliedRevision = 0;
    uint32_t presentationFloor = 0;
    uint32_t unavailableAt = 0;
    uint32_t lastHit = 0;
    uint32_t activationAt = 0, ownerReportAt = 0, rangeChangedAt = 0, processableChangedAt = 0;
    eCEntity* observedEntity = nullptr;
    bool observedRange = false, observedProcessable = false;
    bool nativeActivated = false;
    bool hasPresentationFloor = false;
    uint8_t previousOwner = 0;
    eCEntity* applied = nullptr;
    bool active = false, prepared = false, cut = false, committed = false, hasBaseline = false;
    bool captureFailed = false;
    bool adopted = false;
    // Local ownership is independent of host confirmation.
    bool local = false, confirmed = false, vetted = false, vettedInSession = false, hasOfferedState = false;
    Checkpoint offeredState{};
    uint32_t ownedAt = 0, yieldedAt = 0, cutAt = 0, handbackUntil = 0, hostSeenAt = 0, requestEvery = 500, retryAt = 0;
    char const* cutReason = nullptr;
    eCEntity* ownedEntity = nullptr;
    bool silentLogged = false;
    char lastTask[48]{};
    uint32_t reactionAt = 0;
    char sentRoutine[64]{};
};
using GuidKey = std::array<uint8_t, 16>;
struct GuidHash {
    size_t operator()(GuidKey const& key) const;
};
extern std::unordered_map<GuidKey, Entry, GuidHash> entries;
extern GuardRegistry guardStates;
extern bool installed, installFailed;
extern unsigned refused, stateSent;
extern npcown::RefusalLedger refusals, waiting;
extern npcown::TransitionLog transitions;
extern npcown::OwnStats ownTotals;
extern uint32_t sessionReadyAt, ownReportAt;

GuidKey Key(uint8_t const guid[16]);
GuidKey Key(eCEntity const* entity);
Entry* Find(eCEntity const* entity);
bool Player(eCEntity const* entity);
bool HostWins(GuidKey const& key);
bool CapacityAvailable();
bool Running();
void PublishGuard(GuidKey const& key, Entry const& entry);
void SyncGuardHostClaims();
npcown::RoleFacts RoleOf(eCEntity* entity, bool prototype);
char const* EligibilityReason(eCEntity* entity, char const* name = nullptr, bool prototype = false);
bool Eligible(eCEntity* entity);
bool Processed(eCEntity const* entity);
bool NativeReady(eCEntity* entity);
bool CanResumePresentation(eCEntity* entity);
bool TransferableStatus(Checkpoint const& state);
uint8_t TargetPlayer(eCEntity* target);
bool CaptureNative(eCEntity* entity, Checkpoint& state, bool allowRoleTransition = false);
void Stop(eCEntity* entity);
bool Activate(eCEntity* entity, Entry& entry, uint8_t const identity[16]);
Message Control(GuidKey const& key, Entry const& entry, Kind kind);
bool Send(GuidKey const& key, Entry& entry, Kind kind);
void Diagnostic(char const* action, GuidKey const& key, Entry const& entry, char const* why);
void Refusal(GuidKey const& key, char const* name, char const* reason);
void OwnEvent(GuidKey const& key, char const* name, npcown::Event event, char const* detail);
void OwnEventAlways(GuidKey const& key, char const* name, npcown::Event event, char const* detail);
bool Exact(Message const& note, Entry const& entry);
Checkpoint HostInterestState(Checkpoint const& baseline, Entry const& entry, eCEntity* native, bool frozen);
void SyncOfferedHostState(Entry& entry, eCEntity* native, Checkpoint const& priorState, bool hadBaseline, bool frozen);
void RejectToClient(uint8_t client, Message const& note, eCEntity* native);
void ReceiveClient(Message const& note);
void TickClient(uint32_t now);
}
}
