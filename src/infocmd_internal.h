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
    bool explicitRoutine = false;
    std::string routine;
};

struct Fight {
    uint8_t attacker[16] = {};
    uint8_t player = 0;
    std::string reason;
    uint32_t until = 0;
    uint32_t generation = 0;
};

struct Stats {
    unsigned observed = 0;
    unsigned distinct = 0;
    unsigned snapshotsSent = 0;
    unsigned snapshotsApplied = 0;
    unsigned sectorsSent = 0;
    unsigned sectorsApplied = 0;
    unsigned fightsSent = 0;
    unsigned fightsStarted = 0;
    unsigned companionsKept = 0;
    gCInfo_PS const* lastInfo = nullptr;
    std::string lastKey;
    std::string lastText = "-";
};

struct Flags {
    bool sharing = true;
    bool givebackPlace = true;
    bool keepCompanions = true;
    bool refreshSync = true;
    bool flushHostReputation = false;
};

struct RefreshStats {
    bool hooked = false;
    unsigned calls = 0;
    unsigned sent = 0;
};

extern Stats stats;
extern Flags flags;
extern std::vector<Pending> pending;
extern std::vector<Fight> fights;

npc_progression_policy::ObservedProgression CaptureObserved(Entity const& e);
void ApplyFriendMemory(char const* entityName);
void QueueImmediateState(uint8_t const guid[16], bool progressionOnly = false);
void NoteObservedProgression(GuidKey const& key, npc_progression_policy::ObservedProgression const& obs);
void NoteSyncedNpc(GuidKey const& key);
void NoteSyncedSector(char const* sector, bool enabled);
void BufferClientNpcState(GuidKey const& key, net::WorldState const& s);
void BufferClientSectorState(char const* sector, bool enabled);
void ResetProgressionState();
void SeedHostWorldState();
void FlushBufferedClientStates();
void TrackLocalDialogAndProgression();

npc_progression_policy::ObservedProgression Snapshot(Entity& e, net::WorldState& s, bool includeRoutine = true);
void Apply(net::WorldState const& s, uint8_t from);
void Send(net::WorldState const& s);
bool Join(eCEntity* self, bool join, bool follow = true, char const* questName = nullptr, uint8_t memberType = 0);

void StartFights();
void HookSectorRefresh();
RefreshStats GetRefreshStats();

} // namespace g3t::infocmd::detail
