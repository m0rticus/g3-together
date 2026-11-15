// SPDX-License-Identifier: GPL-3.0-or-later
#include "story_internal.h"

#include "body.h"
#include "combat.h"
#include "log.h"
#include "npcs.h"
#include "remote.h"
#include "trade_policy.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::story {
namespace {
using namespace detail;

// QA: an NPC by name, or "puppet<id>" for that player's puppet (its template name, PC_Hero, finds the hero itself).
Entity Named(char const* name)
{
    if (!name || !*name) return Entity();
    int puppetId = 0;
    if (std::strncmp(name, "puppet", 6) == 0)
        return trade_policy::ParsePuppetId(name, net::MaxPlayers, puppetId) ? Entity(remote::PuppetOf(puppetId)) : Entity();
    return Entity::GetEntity(bCString(name));
}
} // namespace

// QA: the local hero's knowledge of a story event; set or clear it as a dialog would.
void Event(char const* action, char const* name, LogFn log)
{
    char line[200];
    if (!EventsReady()) { log("REFUSED event: no hero"); return; }
    Entity hero(gCSession::GetInstance().GetHero());
    const bCString event(name);
    if (!std::strcmp(action, "setevent")) hero.PlayerMemory.SetGameEvent(event);
    else if (!std::strcmp(action, "clearevent")) hero.PlayerMemory.ClearGameEvent(event);
    Format(line, "EVENT name=%s known=%d", name, hero.PlayerMemory.IsSetGameEvent(event) ? 1 : 0);
    log(line);
}

void Report(LogFn log)
{
    char line[260];
    for (Domain const& d : domains) {
        size_t longest = 0;
        for (auto const& entry : d.known) longest = entry.first.size() > longest ? entry.first.size() : longest;
        Format(line, "STORY kind=%s longest=%zu disabled=%d primed=%d known=%zu sent=%u received=%u applied=%u forced=%u claims=%u requests=%u skipped=%u zeroed=%zu reloads=%u",
                  d.label, longest, disabled, d.primed, d.known.size(), d.sent, d.received, d.applied, d.forced, d.claims, d.requests, d.skipped,
                  d.zeroed.size(), reloads);
        log(line);
    }
    log("STORY end");
}

void ChapterOff(LogFn log)
{
    chapterSharing = false;
    log("CHAPTERSYNCOFF ok=1");
}

void Disable(LogFn log)
{
    disabled = true;
    log("STORYOFF ok=1");
}

void ZerosOff(LogFn log)
{
    fullZeros = false;
    log("STORYZEROSOFF ok=1 (the full state leaves out what was cleared)");
}

void ReloadOff(LogFn log)
{
    reloadReset = false;
    log("STORYRELOADOFF ok=1");
}

void Info(char const* name, bool give, LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    gCInfo_PS* info = manager ? manager->GetInfo(bCString(name)) : nullptr;
    if (!info) { Format(line, "INFO name=%s missing=1", name); log(line); return; }
    if (give) info->AccessInfoGiven() = true;
    Format(line, "INFO name=%s given=%d permanent=%d owner=%s", name, info->AccessInfoGiven() ? 1 : 0,
              info->AccessPermanent() ? 1 : 0, info->AccessOwner().GetText());
    log(line);
}

void InfoCandidates(LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    int found = 0;
    if (manager) {
        auto& all = manager->AccessInfos();
        for (int i = 0; i < all.GetCount() && found < 20; ++i) {
            gCInfo_PS* info = all.AccessAt(i);
            if (!info || info->AccessInfoGiven() || info->AccessPermanent() || info->AccessOwner().IsEmpty()) continue;
            char const* name = info->AccessName().GetText();
            // Names the QA command file accepts: [A-Za-z0-9_-], and short enough for one message entry.
            bool plain = std::strlen(name) < sizeof(net::StateEntry::name);
            for (char const* c = name; *c && plain; ++c) plain = std::isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-';
            if (!plain) continue;
            Format(line, "INFOS name=%s owner=%s", name, info->AccessOwner().GetText());
            log(line);
            ++found;
        }
        Format(line, "INFOS total=%d", all.GetCount());
        log(line);
    }
    log("INFOS end");
}

// QA diagnostics: hold a dialog the way talking to an NPC does (info manager between hero and NPC).
void Talk(char const* npcName, LogFn log)
{
    gCInfoManager_PS* manager = Infos();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!manager || !hero || !npc.GetInstance()) { log("REFUSED talk: no game, hero or NPC"); return; }
    const GEBool started = manager->StartInfoManager(*hero, *npc.GetInstance());
    // The options on offer; ":xp" marks one whose script gives experience (GiveXP), for stage Q2.
    std::string offered;
    for (GEInt i = 0; i < manager->GetAvailableInfoCount(); ++i) {
        gCInfo_PS* info = manager->GetAvailableInfo(i);
        if (!info) continue;
        bool xp = false;
        gCInfoScript_PS* script = info->AccessInfoScript().GetObject();
        for (GEInt c = 0; script && c < script->GetCommandCount(); ++c)
            if (gCInfoScriptLine_PS* command = script->GetCommand(c)) xp = xp || command->AccessCommand() == bCString("GiveXP");
        offered += std::string(offered.empty() ? "" : ",") + info->AccessName().GetText() + (xp ? ":xp" : "");
    }
    char line[900];
    Format(line, "TALK npc=%s started=%d running=%d available=%d npc_task=%s infos=%s", npcName, started ? 1 : 0,
              manager->AccessIsRunning() ? 1 : 0, manager->GetAvailableInfoCount(), npc.Routine.GetCurrentTask().GetText(),
              offered.empty() ? "-" : offered.c_str());
    log(line);
}

void TalkState(LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    if (!manager) { log("REFUSED talkstate: no game"); return; }
    gCInfo_PS* current = manager->GetCurrentInfo();
    gCEntity* partner = manager->GetDialogPartnerNPC();
    Format(line, "TALKSTATE running=%d available=%d current=%s partner=%s waiting=%d", manager->AccessIsRunning() ? 1 : 0,
              manager->GetAvailableInfoCount(), current ? current->AccessName().GetText() : "-",
              partner ? partner->GetName().GetText() : "-", manager->AccessWaitForScript() ? 1 : 0);
    log(line);
}

void EndTalk(LogFn log)
{
    gCInfoManager_PS* manager = Infos();
    if (!manager) { log("REFUSED endtalk: no game"); return; }
    manager->EndInfoManager(manager->GetCurrentInfo());
    TalkState(log);
}

// QA: address an NPC the way the player's talk action does (the AssessTalk script, NPC first).
void AssessTalk(char const* npcName, LogFn log)
{
    char line[260];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!hero || !npc.GetInstance()) { log("REFUSED assesstalk: no hero or NPC"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTalk"), npc.GetInstance(), hero, 0);
    Format(line, "ASSESSTALK npc=%s result=%d npc_task=%s npc_aimode=%d hero_task=%s has_info=%d frozen=%d npc_talking=%d npc_talked_to=%d",
              npcName, result, npc.Routine.GetCurrentTask().GetText(), body::AIModeOf(npc.GetInstance()),
              Entity(hero).Routine.GetCurrentTask().GetText(), npc.HasInfo(Entity(hero)) ? 1 : 0, npc.NPC.IsFrozen() ? 1 : 0,
              npc.Dialog.IsTalking() ? 1 : 0, npc.Dialog.IsBeingTalkedTo() ? 1 : 0);
    log(line);
}

// QA (host): end the opening raid so that the people of Ardea calm down and can talk.
void KillRaiders(LogFn log)
{
    char line[120];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int killed = 0;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount() && hero; ++i) {
        Entity npc = all.GetAt(i);
        if (!npc.GetInstance() || npc.IsDead() || std::strncmp(npc.GetName().GetText(), "Orc_", 4) != 0) continue;
        const bCVector& a = npc.GetInstance()->GetWorldPosition();
        const bCVector& b = hero->GetWorldPosition();
        const float at[3] = {a.GetX(), a.GetY(), a.GetZ()}, from[3] = {b.GetX(), b.GetY(), b.GetZ()};
        if (body::Distance(at, from) > 30000.0f) continue;
        // Not Entity::Kill (removes the body while others still target it: crash) and not a death task set
        // from outside (crashed in Script.dll mid-fight): a lethal hit, judged by the game.
        if (combat::LethalHit(npc.GetInstance())) ++killed;
    }
    Format(line, "KILLRAIDERS killed=%d", killed);
    log(line);
}

// QA: put the hero 1.5 m from an NPC, on the side he came from, as a player walks up before talking.
void Approach(char const* npcName, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!hero || !npc.GetInstance()) { log("REFUSED approach: no hero or NPC"); return; }
    const bCVector& n = npc.GetInstance()->GetWorldPosition();
    const bCVector& h = hero->GetWorldPosition();
    const float target[3] = {n.GetX(), n.GetY(), n.GetZ()}, from[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const float before = body::Distance(target, from);
    const float dx = before > 1.0f ? (from[0] - target[0]) / before : 1.0f, dz = before > 1.0f ? (from[2] - target[2]) / before : 0.0f;
    const float position[3] = {target[0] + 150.0f * dx, target[1], target[2] + 150.0f * dz};
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, position, rotation));
    const bCVector& p = hero->GetWorldPosition();
    const float now[3] = {p.GetX(), p.GetY(), p.GetZ()};
    Format(line, "APPROACH npc=%s before_cm=%.0f after_cm=%.0f", npcName, before, body::Distance(target, now));
    log(line);
}

// QA: NPCs within `radius` cm (default 2000, the distance from which AssessActivePerception lets an NPC with an important
// info walk up to the player and talk) that have an important info for this machine's hero, as the game asks it.
void Important(char const* radius, LogFn log)
{
    char line[300];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("REFUSED important: no hero"); return; }
    const float within = radius && *radius ? static_cast<float>(std::atof(radius)) : 2000.0f;
    const bCVector& h = hero->GetWorldPosition();
    const float from[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const bTObjArray<Entity> list = Entity::GetNPCs();
    for (int i = 0; i < list.GetCount(); ++i) {
        Entity npc = list.GetAt(i);
        if (!npc.GetInstance() || npc.IsDead() || remote::IsPuppet(npc.GetInstance())) continue;
        const bCVector& n = npc.GetInstance()->GetWorldPosition();
        const float at[3] = {n.GetX(), n.GetY(), n.GetZ()};
        const float distance = body::Distance(at, from);
        if (distance > within || !npc.HasImportantInfo(Entity(hero))) continue;
        Format(line, "IMPORTANT npc=%s distance=%.0f mirrored=%d task=%s", npc.GetName().GetText(), distance,
                  npcs::IsMirrored(npc.GetInstance()) ? 1 : 0, npc.Routine.GetCurrentTask().GetText());
        log(line);
    }
    log("IMPORTANT end");
}

} // namespace g3t::story
