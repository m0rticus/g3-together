// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease_runtime.h"
#include "npclease_client.h"
#include "npclease_guards.h"
#include "npcown_policy.h"
#include <array>
#include <unordered_map>

namespace g3t::npclease::detail {
struct Entry {
    Lease lease;
    Eligibility interest;
    ClientWindow window;
    uint32_t lastRequest = 0, lastState = 0, lastControl = 0, localDeadline = 0;
    uint32_t highestEpoch = 0;
    uint32_t echo = 0, welcome = 0, world = 0, recallAt = 0;
    uint32_t previousGeneration = 0;
    uint32_t appliedRevision = 0;
    uint32_t presentationFloor = 0;
    uint32_t lastHit = 0;
    uint32_t rangeChangedAt = 0, processableChangedAt = 0;
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
    uint32_t yieldedAt = 0, cutAt = 0, handbackUntil = 0, hostSeenAt = 0, requestEvery = 500, retryAt = 0;
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
extern uint32_t authoritySerial;
extern unsigned granted, recalled, recovered, refused, stateSent, stateReceived, discovered;
extern bool installed, installFailed;
extern npcown::RefusalLedger refusals, waiting;
extern npcown::TransitionLog transitions;
extern npcown::OwnStats ownTotals;
extern uint32_t sessionReadyAt, ownReportAt;

GuidKey Key(uint8_t const guid[16]);
GuidKey Key(eCEntity const* entity);
Entry* Find(eCEntity const* entity);
bool HostWins(GuidKey const& key);
bool CapacityAvailable();
bool Running();
void PublishGuard(GuidKey const& key, Entry const& entry);
void SyncGuardHostClaims();
npcown::RoleFacts RoleOf(eCEntity* entity, bool prototype);
char const* EligibilityReason(eCEntity* entity, char const* name = nullptr, bool prototype = false);
char const* DiscoveryReason(Checkpoint const& state);
bool Eligible(eCEntity* entity);
bool Processed(eCEntity const* entity);
bool NativeReady(eCEntity* entity);
bool CanResumePresentation(eCEntity* entity);
bool TransferableStatus(Checkpoint const& state);
uint8_t TargetPlayer(eCEntity* target);
bool CaptureNative(eCEntity* entity, Checkpoint& state, bool allowRoleTransition = false);
void Stop(eCEntity* entity);
bool Import(eCEntity* entity, Checkpoint const& state);
bool Activate(eCEntity* entity, Entry& entry, uint8_t const identity[16]);
bool CurrentPeer(uint8_t player, uint32_t generation);
Message Control(GuidKey const& key, Entry const& entry, Kind kind);
bool Send(GuidKey const& key, Entry& entry, Kind kind);
void Diagnostic(char const* action, GuidKey const& key, Entry const& entry, char const* why);
void Refusal(GuidKey const& key, char const* name, char const* reason);
void OwnEvent(GuidKey const& key, char const* name, npcown::Event event, char const* detail);
void OwnEventAlways(GuidKey const& key, char const* name, npcown::Event event, char const* detail);
bool Exact(Message const& note, Entry const& entry);
void ReceiveHost(uint8_t from, Message const& note);
void TickHost(uint32_t now);
void ReceiveClient(Message const& note);
void TickClient(uint32_t now);
}
