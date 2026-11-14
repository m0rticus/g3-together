// SPDX-License-Identifier: GPL-3.0-or-later
#include "focus_diag.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace g3t::focus_diag {
namespace {
constexpr uint32_t WatchIntervalMs = 250;
constexpr uint32_t IdleBaselineMs = 5000;
constexpr uint32_t WatchWindowMs = 2000;

struct GuidKey {
    char text[40]{};
    bool operator==(GuidKey const& other) const { return std::strcmp(text, other.text) == 0; }
};

struct StoredNpc {
    GuidKey guid{};
    uintptr_t instance = 0;
    int weapon = 0;
    int ai = 0;
    uint16_t fraction = UINT16_MAX;
    char motion[96]{};
};

struct State {
    bool known = false, focused = false, paused = false, watching = false, baselineReady = false;
    uint32_t frameAt = 0, lostAt = 0, resumedAt = 0, sampledAt = 0;
    unsigned serial = 0, replacements = 0, details = 0, resourceResets = 0, resourceRestores = 0;
    std::vector<StoredNpc> previous;
    std::vector<StoredNpc> current;
    std::vector<GuidKey> weapons;
    std::vector<GuidKey> modes;
    std::vector<GuidKey> draws;
} state;

GuidKey MakeKey(char const* guid)
{
    GuidKey key{};
    if (guid) strncpy_s(key.text, guid, _TRUNCATE);
    return key;
}

void InsertUnique(std::vector<GuidKey>& list, GuidKey const& key)
{
    if (std::find(list.begin(), list.end(), key) == list.end()) list.push_back(key);
}

StoredNpc const* FindPrevious(GuidKey const& key)
{
    auto const it = std::lower_bound(state.previous.begin(), state.previous.end(), key,
        [](StoredNpc const& entry, GuidKey const& target) { return std::strcmp(entry.guid.text, target.text) < 0; });
    return (it != state.previous.end() && it->guid == key) ? &*it : nullptr;
}
}

void Reset()
{
    state.known = state.focused = state.paused = state.watching = state.baselineReady = false;
    state.frameAt = state.lostAt = state.resumedAt = state.sampledAt = 0;
    state.serial = state.replacements = state.details = state.resourceResets = state.resourceRestores = 0;
    state.previous.clear();
    state.current.clear();
    state.weapons.clear();
    state.modes.clear();
    state.draws.clear();
}

Header AdvanceFrame(uint32_t now, bool focused, bool paused, bool inWorld)
{
    Header header{};
    header.gap = state.known ? now - state.frameAt : 0;
    header.lost = state.known && state.focused && !focused;
    header.regained = state.known && !state.focused && focused;
    header.resumed = state.known && state.paused && !paused;
    header.longGap = state.known && header.gap >= 1000;
    const bool pauseEdge = state.known && !state.paused && paused;
    if (header.lost) state.lostAt = now;
    if (header.lost || header.regained || header.resumed || header.longGap) {
        header.serial = ++state.serial;
        header.awayMs = header.regained ? now - state.lostAt : 0;
        if (header.regained || header.resumed || header.longGap) {
            state.watching = true;
            state.resumedAt = now;
            state.weapons.clear();
            state.modes.clear();
            state.draws.clear();
            state.replacements = state.details = 0;
            state.sampledAt = now - WatchIntervalMs;
        }
    }
    state.known = true;
    state.focused = focused;
    state.paused = paused;
    state.frameAt = now;
    header.watching = state.watching;
    if (!inWorld) return header;
    if (header.lost || pauseEdge) {
        header.sampleDue = true;
        return header;
    }
    if (!focused) return header;
    const uint32_t interval = state.watching ? WatchIntervalMs : IdleBaselineMs;
    header.sampleDue = !state.baselineReady || (now - state.sampledAt >= interval);
    return header;
}

void BeginSample(uint32_t now)
{
    state.sampledAt = now;
    state.current.clear();
    if (state.current.capacity() < state.previous.size()) state.current.reserve(state.previous.size());
}

NpcChange RecordNpc(NpcSample const& sample)
{
    NpcChange change{};
    StoredNpc stored{};
    stored.guid = MakeKey(sample.guid);
    stored.instance = sample.instance;
    stored.weapon = sample.weapon;
    stored.ai = sample.ai;
    stored.fraction = sample.fraction;
    if (sample.motion) strncpy_s(stored.motion, sample.motion, _TRUNCATE);

    if (state.watching) {
        if (StoredNpc const* prev = FindPrevious(stored.guid)) {
            const bool weapon = stored.weapon != prev->weapon;
            const bool mode = stored.ai != prev->ai;
            const bool draw = (std::strstr(stored.motion, "Draw") || std::strstr(stored.motion, "Equip")) &&
                (std::strcmp(stored.motion, prev->motion) != 0 ||
                 (stored.fraction != UINT16_MAX && prev->fraction != UINT16_MAX &&
                  stored.fraction + 3276 < prev->fraction));
            if (weapon) InsertUnique(state.weapons, stored.guid);
            if (mode) InsertUnique(state.modes, stored.guid);
            if (draw) InsertUnique(state.draws, stored.guid);
            if (stored.instance != prev->instance) ++state.replacements;
            if ((weapon || mode || draw) && state.details++ < 8) {
                change.logDetail = true;
                change.prevWeapon = prev->weapon;
                change.prevAi = prev->ai;
                change.draw = draw;
            }
        }
    }
    state.current.push_back(stored);
    return change;
}

bool FinishSample(uint32_t now, SettleReport& out)
{
    std::sort(state.current.begin(), state.current.end(),
        [](StoredNpc const& a, StoredNpc const& b) { return std::strcmp(a.guid.text, b.guid.text) < 0; });
    state.previous.swap(state.current);
    state.baselineReady = true;
    if (!state.watching || now - state.resumedAt < WatchWindowMs) return false;
    out.serial = state.serial;
    out.windowMs = now - state.resumedAt;
    out.sampledNpcs = state.previous.size();
    out.weaponChanged = state.weapons.size();
    out.aiChanged = state.modes.size();
    out.drawMotion = state.draws.size();
    out.actorReplacements = state.replacements;
    state.watching = false;
    return true;
}

unsigned CountResourceReset() { return ++state.resourceResets; }
unsigned CountResourceRestore() { return ++state.resourceRestores; }
}
