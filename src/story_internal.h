// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include "story.h"
#include "story_policy.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace g3t::story::detail {

static_assert(net::StateQuests == 1 &&
              net::StateInfos == 2 &&
              net::StateEvents == 3 &&
              net::StateQuestRuntime == 4 &&
              net::StateWorldClock == 5 &&
              net::StateReputation == 6,
              "Story domain kinds must map densely to 1..6");

constexpr size_t DomainCount = 6;
constexpr uint8_t AllDomainsSentMask = static_cast<uint8_t>((1u << DomainCount) - 1u);
static_assert(AllDomainsSentMask == 0x3f, "All 6 story domains must be sent before live cutover");
constexpr char ChapterEntry[] = "G3T_Chapter";

using States = std::unordered_map<std::string, uint32_t>;

struct Domain {
    uint8_t kind;
    char const* label;
    bool (*ready)();
    void (*snapshot)(States&);
    bool (*apply)(char const* name, uint32_t value);  // true if the game's own function did it, false if set directly
    States known;  // host: values last sent; client: the host's values as far as this client knows them
    // Host: names that went back to 0 while the game ran here. The full state sends their zeros: a client that loads the
    // host's save got them as set, since the save is older (Codex review 22.10. R8). All zeros would be thousands of infos.
    std::unordered_set<std::string> zeroed;
    bool primed = false;
    bool wasConnected = false;
    policy::Baseline baseline;
    DWORD nextRequest = 0;
    unsigned sent = 0, received = 0, applied = 0, forced = 0, claims = 0, requests = 0, skipped = 0;
};

extern Domain domains[DomainCount];
extern bool disabled;
extern bool fullZeros;
extern bool reloadReset;
extern bool chapterSharing;
extern bool namesReady;
extern unsigned reloads;
extern std::unordered_map<std::string, std::string> questNames;
extern std::unordered_map<std::string, std::string> infoNames;
extern std::unordered_map<std::string, uint32_t> seenInfoEnds;

bool Running();
gCQuestManager_PS* Quests();
uint8_t StatusOf(gCQuest_PS* quest);
gCInfoManager_PS* Infos();
bool EventsReady();
bool ClockApply(char const* name, uint32_t value);

Domain* Find(uint8_t kind);
bool BuildNames();
void Track(Domain& d, std::string const& name, uint32_t value);

} // namespace g3t::story::detail
