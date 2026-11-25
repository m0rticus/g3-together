// SPDX-License-Identifier: GPL-3.0-or-later
#include "infocmd_internal.h"

#include "log.h"
#include "world_sectors.h"
#include "xpshare.h"

#include <string>

namespace g3t::infocmd {
using namespace detail;

void Off(LogFn log) { flags.sharing = false; log("INFOCMDOFF ok=1"); }
void StoryCompanionOff(LogFn log) { flags.keepCompanions = false; log("STORYCOMPANIONOFF ok=1"); }
void GivebackPlaceOff(LogFn log) { flags.givebackPlace = false; log("GIVEBACKPOSOFF ok=1"); }
void DialogXpOff(LogFn log) { xpshare::Off(log); log("DIALOGXPOFF ok=1"); }

void StoryMoveOff(LogFn log)
{
    flags.refreshSync = false;
    const RefreshStats refresh = GetRefreshStats();
    char line[120];
    Format(line, "STORYMOVEOFF ok=1 calls=%u sent=%u", refresh.calls, refresh.sent);
    log(line);
}

void StoryMoves(LogFn log)
{
    const RefreshStats refresh = GetRefreshStats();
    char line[120];
    Format(line, "STORYMOVES hooked=%d calls=%u sent=%u sync=%d companions_kept=%u keep=%d",
           refresh.hooked ? 1 : 0, refresh.calls, refresh.sent, flags.refreshSync ? 1 : 0,
           stats.companionsKept, flags.keepCompanions ? 1 : 0);
    log(line);
}

void Report(LogFn log)
{
    unsigned sectorSent = 0, sectorApplied = 0;
    sectors::Counts(sectorSent, sectorApplied);
    char line[600];
    Format(line, "INFOCMD observed=%u distinct=%u sent=%u applied=%u sectors_sent=%u sectors_applied=%u fights_sent=%u fights_started=%u last=%s",
           stats.observed, stats.distinct, stats.snapshotsSent, stats.snapshotsApplied, stats.sectorsSent + sectorSent,
           stats.sectorsApplied + sectorApplied, stats.fightsSent, stats.fightsStarted, stats.lastText.c_str());
    log(line);
}

void JoinQa(char const* argument, LogFn log)
{
    char line[200];
    std::string arg = argument ? argument : "";
    const size_t cut = arg.rfind('_');
    if (cut == std::string::npos || net::IsHost()) { log("REFUSED join: use Name_1 or Name_0 on a client"); return; }
    Entity npc = Entity::GetEntity(bCString(arg.substr(0, cut).c_str()));
    const bool join = arg.substr(cut + 1) == "1";
    const bool changed = npc.GetInstance() && Join(npc.GetInstance(), join);
    Format(line, "JOIN name=%s join=%d found=%d changed=%d", arg.substr(0, cut).c_str(), join ? 1 : 0,
           npc.GetInstance() ? 1 : 0, changed ? 1 : 0);
    log(line);
}

void PartyEnableQa(char const* argument, LogFn log)
{
    char line[160];
    std::string name = argument ? argument : "";
    const bool mob = name.size() > 4 && name.compare(name.size() - 4, 4, "_mob") == 0;
    if (mob) name.resize(name.size() - 4);
    Entity npc = Entity::GetEntity(bCString(name.c_str()));
    if (!npc.GetInstance()) { log("REFUSED partyenable: no such NPC"); return; }
    if (mob) npc.Dialog.MobEnabled = GETrue;
    else npc.Dialog.PartyEnabled = GETrue;
    Changed(npc.GetInstance());
    Format(line, "PARTYENABLE name=%s party_enabled=%d mob_enabled=%d", name.c_str(), npc.Dialog.PartyEnabled ? 1 : 0,
           npc.Dialog.MobEnabled ? 1 : 0);
    log(line);
}

} // namespace g3t::infocmd
