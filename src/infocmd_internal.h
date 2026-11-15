// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "guid.h"
#include "infocmd.h"
#include "net.h"
#include "npc_progression_policy.h"

#include <Game.h>
#include <Script.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace g3t::infocmd::detail {

constexpr uint32_t ReadAfter = 700;

using GuidKey = std::array<uint8_t, 16>;

inline GuidKey GuidKeyOf(uint8_t const guid[16])
{
    GuidKey k{};
    std::memcpy(k.data(), guid, 16);
    return k;
}

struct Pending {
    uint8_t guid[16] = {};
    std::string sector;
    uint32_t due = 0;
    bool moved = false;
    bool story = false;
    bool progressionOnly = false;
    std::string routine;
};

struct Fight {
    uint8_t attacker[16] = {};
    uint8_t player = 0;
    std::string reason;
    uint32_t until = 0;
    uint32_t joins = 0;
};

extern unsigned observed;
extern unsigned distinct;
extern unsigned snapshotsSent;
extern unsigned snapshotsApplied;
extern unsigned sectorsSent;
extern unsigned sectorsApplied;
extern unsigned fightsSent;
extern unsigned fightsStarted;
extern unsigned companionsKept;

extern std::string lastKey;
extern std::string lastText;
extern bool sharing;
extern bool givebackPlace;
extern bool keepCompanions;
extern bool flushHostReputation;
extern bool wasDialogRunning;
extern bool worldSeeded;
extern uint32_t bufferedEpoch;

extern std::vector<Pending> pending;
extern std::vector<Fight> fights;
extern std::map<GuidKey, npc_progression_policy::ObservedProgression> observedProgression;
extern std::set<GuidKey> syncedNpcs;
extern std::map<std::string, bool> syncedSectors;
extern std::map<GuidKey, net::WorldState> bufferedWorldStates;
extern std::map<std::string, bool> bufferedSectors;
extern std::set<GuidKey> activeDialogNpcs;

npc_progression_policy::ObservedProgression CaptureObserved(Entity const& e);
void ApplyFriendMemory(char const* entityName);
void QueueImmediateState(uint8_t const guid[16], bool progressionOnly = false);
void SyncClientBufferEpoch();
void SeedHostWorldState();
void FlushBufferedClientStates();
void TrackLocalDialogAndProgression();

npc_progression_policy::ObservedProgression Snapshot(Entity& e, net::WorldState& s, bool includeRoutine = true);
void Apply(net::WorldState const& s, uint8_t from);
void Send(net::WorldState const& s);
bool Join(eCEntity* self, bool join, bool follow = true, char const* questName = nullptr);

void StartFights();
void HookSectorRefresh();

} // namespace g3t::infocmd::detail
