// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore.h"
#include "explore_internal.h"

#include "body.h"
#include "log.h"
#include "persist.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace g3t::explore {
namespace {
// Game.dll internals of this exact build (Steam 1037023, CP 1.75.14): gCHUDFileManager's singleton getter,
// SaveGame(name) and LoadGame(name), both __stdcall(this, name) (the prologue reads `this` from [ebp+8] and
// checks its fields; a __thiscall call made the game look for an empty file name). The first bytes are checked before every call, so a
// different build is refused instead of jumped into.
struct Internal { uintptr_t rva; char const* bytes; };
const Internal FileManagerGetter{0xa92d0, "b80100000084"};
const Internal SaveGameFn{0xa9e90, "558bec83e4f883ec1c535657"};
const Internal LoadGameFn{0xa9a10, "558bec83e4f88b450883ec5c83780400"};

void* Resolve(Internal const& fn)
{
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return nullptr;
    auto const* code = reinterpret_cast<unsigned char const*>(reinterpret_cast<uintptr_t>(game) + fn.rva);
    for (size_t i = 0; fn.bytes[2 * i]; ++i) {
        unsigned value = 0;
        sscanf_s(fn.bytes + 2 * i, "%2x", &value);
        if (code[i] != value) return nullptr;
    }
    return const_cast<unsigned char*>(code);
}

using Getter = void*(__cdecl*)();
using FileFn = bool(__stdcall*)(void* self, bCString const* name);

// One TSV field: UTF-8, without tabs or line breaks.
std::string Field(wchar_t const* text)
{
    char utf8[512] = {};
    if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    std::string s(utf8);
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}
}

void NativeSave(char const* name, LogFn log)
{
    char line[200];
    auto getter = reinterpret_cast<Getter>(Resolve(FileManagerGetter));
    // Once hooked (persist.cpp) the entry starts with our jump; calling it goes through the hook, like the menu.
    auto save = reinterpret_cast<FileFn>(persist::SaveTarget() ? persist::SaveTarget() : Resolve(SaveGameFn));
    if (!getter || !save) { log("REFUSED nativesave: Game.dll does not match this build"); return; }
    // Puppets are entities from the hero's template; saved with the world they make the game abort on load
    // ("current world has more than one hero!"). Remove them first; they come back with the next state.
    remote::DespawnAll("native save");
    const bCString slot(name);
    const bool ok = save(getter(), &slot);
    Format(line, "NATIVESAVE name=%s ok=%d folder=%s", name, ok ? 1 : 0, gCWorld::GetSaveGameFolder().GetText());
    log(line);
}

void NativeLoad(char const* name, LogFn log)
{
    char line[200];
    auto getter = reinterpret_cast<Getter>(Resolve(FileManagerGetter));
    auto load = reinterpret_cast<FileFn>(persist::LoadTarget() ? persist::LoadTarget() : Resolve(LoadGameFn));
    if (!getter || !load) { log("REFUSED nativeload: Game.dll does not match this build"); return; }
    const bCString slot(name);
    // Loading a slot that does not exist stops the session and leaves the game hanging: check first.
    gCWorld* world = gCWorld::GetCurrentWorld();
    if (!world || !world->ExistsSaveGame(slot)) { Format(line, "NATIVELOAD name=%s ok=0 exists=0", name); log(line); return; }
    Format(line, "NATIVELOAD name=%s calling=1", name);
    log(line);
    const bool ok = load(getter(), &slot);
    Format(line, "NATIVELOAD name=%s ok=%d", name, ok ? 1 : 0);
    log(line);
}

// QA, stage H3: every dialog option whose script contains `command` (Give, Attack, JoinNPC, Spawn, ...), with
// its owner's distance from the hero, nearest first: candidates for tests with real dialogs.
void InfoScan(char const* command, LogFn log)
{
    char line[600];
    gCInfoManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!manager || !hero) { log("REFUSED infoscan: no game"); return; }
    struct Row { float distance; std::string text; };
    std::vector<Row> rows;
    float h[3];
    detail::PositionOf(hero, h);
    auto& infos = manager->AccessInfos();
    // "type_<n>": every info of that condition type (17 = trade), one row each.
    const int condType = !std::strncmp(command, "type_", 5) ? std::atoi(command + 5) : -1;
    // "commands": every command name of every info's script, with how many lines use it and one example info.
    if (!std::strcmp(command, "commands")) {
        std::map<std::string, std::pair<int, std::string>> counts;
        for (int i = 0; i < infos.GetCount(); ++i) {
            gCInfo_PS* info = infos.AccessAt(i);
            gCInfoScript_PS* script = info ? info->AccessInfoScript().GetObject() : nullptr;
            for (int k = 0; script && k < script->AccessInfoScriptLines().GetCount(); ++k)
                if (auto* l = static_cast<gCInfoScriptLine_PS*>(script->AccessInfoScriptLines().AccessAt(k))) {
                    auto& c = counts[l->AccessCommand().GetText()];
                    if (!c.first++) c.second = info->AccessName().GetText();
                }
        }
        for (auto const& [name, c] : counts) {
            Format(line, "INFOSCAN command=%s lines=%d example=%s", name.c_str(), c.first, c.second.c_str());
            log(line);
        }
        Format(line, "INFOSCAN end command=commands matches=%zu", counts.size());
        log(line);
        return;
    }
    // "info_<name>": every script line of that info (command, entities, IDs), in order.
    char const* infoName = !std::strncmp(command, "info_", 5) ? command + 5 : nullptr;
    for (int i = 0; i < infos.GetCount(); ++i) {
        gCInfo_PS* info = infos.AccessAt(i);
        if (!info) continue;
        if (infoName) {
            gCInfoScript_PS* script = _stricmp(info->AccessName().GetText(), infoName) ? nullptr : info->AccessInfoScript().GetObject();
            for (int k = 0; script && k < script->AccessInfoScriptLines().GetCount(); ++k) {
                gCInfoScriptLine_PS* l = static_cast<gCInfoScriptLine_PS*>(script->AccessInfoScriptLines().AccessAt(k));
                if (!l) continue;
                Format(line, "INFOSCAN info=%s owner=%s line=%d cmd=%s e1=%s e2=%s id1=%s id2=%s type=%d quest=%s", infoName,
                       info->AccessOwner().GetText(), k, l->AccessCommand().GetText(), l->AccessEntity1().GetText(),
                       l->AccessEntity2().GetText(), l->AccessID1().GetText(), l->AccessID2().GetText(),
                       static_cast<int>(static_cast<gEInfoCondType const&>(info->AccessConditionType())), info->AccessQuest().GetText());
                rows.push_back({static_cast<float>(k), line});
            }
            continue;
        }
        if (condType >= 0) {
            if (static_cast<int>(static_cast<gEInfoCondType const&>(info->AccessConditionType())) != condType) continue;
            Entity owner = Entity::GetEntity(info->AccessOwner());
            float distance = 1e9f;
            if (owner.GetInstance()) { float p[3]; detail::PositionOf(owner.GetInstance(), p); distance = body::Distance(p, h) / 100.0f; }
            const std::string knows = detail::Join(info->AccessConditionPlayerKnows(), "|");
            Format(line, "INFOSCAN info=%s owner=%s distance_m=%.0f given=%d permanent=%d gold=%d itype=%d quest=%s parent=%s knows=%s",
                   info->AccessName().GetText(), info->AccessOwner().GetText(), distance, info->AccessInfoGiven() ? 1 : 0,
                   info->AccessPermanent() ? 1 : 0, static_cast<int>(info->AccessGoldCost()),
                   static_cast<int>(static_cast<gEInfoType const&>(info->AccessType())), info->AccessQuest().GetText(),
                   info->AccessParent().GetText(), knows == "-" ? "-" : knows.substr(0, 200).c_str());
            rows.push_back({distance, line});
            continue;
        }
        gCInfoScript_PS* script = info->AccessInfoScript().GetObject();
        if (!script) continue;
        auto& lines = script->AccessInfoScriptLines();
        for (int k = 0; k < lines.GetCount(); ++k) {
            gCInfoScriptLine_PS* l = static_cast<gCInfoScriptLine_PS*>(lines.AccessAt(k));
            if (!l || _stricmp(l->AccessCommand().GetText(), command) != 0) continue;
            Entity owner = Entity::GetEntity(info->AccessOwner());
            float distance = 1e9f;
            if (owner.GetInstance()) { float p[3]; detail::PositionOf(owner.GetInstance(), p); distance = body::Distance(p, h) / 100.0f; }
            Format(line, "INFOSCAN info=%s owner=%s distance_m=%.0f given=%d quest=%s line=%d e1=%s e2=%s id1=%s id2=%s",
                   info->AccessName().GetText(), info->AccessOwner().GetText(), distance, info->AccessInfoGiven() ? 1 : 0,
                   info->AccessQuest().GetText(), k, l->AccessEntity1().GetText(), l->AccessEntity2().GetText(),
                   l->AccessID1().GetText(), l->AccessID2().GetText());
            rows.push_back({distance, line});
        }
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.distance < b.distance; });
    for (size_t i = 0; i < rows.size() && i < 25; ++i) log(rows[i].text.c_str());
    Format(line, "INFOSCAN end command=%s matches=%zu", command, rows.size());
    log(line);
}

// Plan P0 (24.10.): every quest the game knows, with its journal folder (region or town), title in this game's language,
// type, status, experience, prerequisites and delivery targets: the base of the chapter checklist.
void QuestDump(char const* path, LogFn log)
{
    gCQuestManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
    if (!manager) { log("QUESTDUMP refused no game running"); return; }
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) { log("QUESTDUMP refused cannot write the file"); return; }
    std::fputs("name\tfolder\ttopic\ttype\tstatus\txp\tenclave_success\tdestination\tfinished_quests\tdelivery\r\n", file);
    auto const& quests = manager->GetQuests();
    int count = 0;
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        gCQuest_PS const* quest = quests.GetAt(qi).operator->();
        if (!quest) continue;
        const bCUnicodeString topic = quest->GetLogTopic();
        std::fprintf(file, "%s\t%s\t%s\t%d\t%d\t%u\t%s\t%s\t%s\t%s\r\n", quest->GetName().GetText(), quest->GetFolder().GetText(),
                     Field(reinterpret_cast<wchar_t const*>(topic.GetText())).c_str(), static_cast<int>(quest->GetType().GetValue()),
                     static_cast<int>(quest->GetStatus().GetValue()), static_cast<unsigned>(quest->GetExperiencePoints()),
                     quest->GetEnclaveSuccess().GetText(), quest->GetDestinationEntity().GetText(),
                     detail::Join(quest->GetFinishedQuests(), "|").c_str(),
                     detail::Join(quest->GetDeliveryEntities(), "|").c_str());
        ++count;
    }
    std::fclose(file);
    char line[120];
    Format(line, "QUESTDUMP quests=%d", count);
    log(line);
}

// QA (stage Q2): dialog options whose script gives experience (GiveXP) and whose only conditions are simple (no quest, no
// knowledge, no items): name, owner, condition type, parent, told; the owner's distance from the hero in metres.
void XpInfos(LogFn log)
{
    char line[400];
    gCInfoManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!manager || !hero) { log("REFUSED xpinfos: no game"); return; }
    auto& all = manager->AccessInfos();
    float h[3];
    detail::PositionOf(hero, h);
    unsigned found = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        gCInfo_PS* info = all.AccessAt(i);
        if (!info || info->AccessInfoGiven() || info->AccessQuest().GetLength() || info->AccessConditionPlayerKnows().GetCount() ||
            info->AccessCondItems().GetCount() || info->AccessCondSecondaryNPC().GetCount())
            continue;
        gCInfoScript_PS* script = info->AccessInfoScript().GetObject();
        int xp = 0;
        for (GEInt c = 0; script && c < script->GetCommandCount(); ++c)
            if (gCInfoScriptLine_PS* command = script->GetCommand(c))
                if (command->AccessCommand() == bCString("GiveXP")) xp = std::atoi(command->AccessID1().GetText());
        if (xp <= 0) continue;
        Entity owner = Entity::GetEntity(info->AccessOwner());
        float distance = -1.0f;
        if (owner.GetInstance()) {
            float p[3];
            detail::PositionOf(owner.GetInstance(), p);
            distance = body::Distance(p, h) / 100.0f;
        }
        Format(line, "XPINFO name=%s owner=%s xp=%d cond_type=%d parent=%s distance_m=%.0f", info->AccessName().GetText(),
               info->AccessOwner().GetText(), xp, static_cast<int>(static_cast<gEInfoCondType const&>(info->AccessConditionType())),
               info->AccessParent().GetLength() ? info->AccessParent().GetText() : "-", distance);
        log(line);
        ++found;
    }
    Format(line, "XPINFOS end found=%u", found);
    log(line);
}

// QA, stage E3: a dialog option's conditions and its script lines, to see why it cannot run.
void InfoDescribe(char const* name, LogFn log)
{
    char line[900];
    gCInfoManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    gCInfo_PS* info = manager ? manager->GetInfo(bCString(name)) : nullptr;
    if (!info) { Format(line, "INFODESC name=%s found=0", name); log(line); return; }
    Format(line, "INFODESC name=%s found=1 owner=%s parent=%s quest=%s given=%d permanent=%d cond_type=%d knows=%s knows_not=%s items=%s secondary=%s party=%s near=%s gold=%d",
           name, info->AccessOwner().GetText(), info->AccessParent().GetText(), info->AccessQuest().GetText(), info->AccessInfoGiven() ? 1 : 0,
           info->AccessPermanent() ? 1 : 0, static_cast<int>(static_cast<gEInfoCondType const&>(info->AccessConditionType())),
           detail::Join(info->AccessConditionPlayerKnows(), ",").c_str(),
           detail::Join(info->AccessConditionPlayerKnowsNot(), ",").c_str(),
           detail::Join(info->AccessCondItems(), ",").c_str(),
           detail::Join(info->AccessCondSecondaryNPC(), ",").c_str(),
           detail::Join(info->AccessCondPlayerPartyMember(), ",").c_str(),
           info->AccessConditionOwnerNearEntity().GetText(), static_cast<int>(info->AccessGoldCost()));
    log(line);
    if (gCInfoScript_PS* script = info->AccessInfoScript().GetObject()) {
        auto& lines = script->AccessInfoScriptLines();
        for (int k = 0; k < lines.GetCount(); ++k) {
            gCInfoScriptLine_PS* l = static_cast<gCInfoScriptLine_PS*>(lines.AccessAt(k));
            if (!l) continue;
            Format(line, "INFOLINE %d %s e1=%s e2=%s id1=%s id2=%s text=%s", k, l->AccessCommand().GetText(), l->AccessEntity1().GetText(),
                   l->AccessEntity2().GetText(), l->AccessID1().GetText(), l->AccessID2().GetText(), l->AccessText().GetID().GetText());
            log(line);
        }
    }
    log("INFODESC end");
}

// QA, stage Q: kill and defeat quests (or the one named), their status and, per target name, counter/amount.
void QuestCounters(char const* name, LogFn log)
{
    char line[600];
    gCQuestManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
    if (!manager) { log("REFUSED questcounters: no game running"); return; }
    auto const& quests = manager->GetQuests();
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        gCQuest_PS const* quest = quests.GetAt(qi).operator->();
        if (!quest) continue;
        const int type = static_cast<int>(quest->GetType().GetValue());
        if (name && *name) {
            if (_stricmp(quest->GetName().GetText(), name)) continue;
        } else if (type != gEQuestType_Kill && type != gEQuestType_Defeat) {
            continue;
        }
        auto const& targets = quest->GetDeliveryEntities();
        auto const& counter = quest->GetDeliveryCounter();
        auto const& amounts = quest->GetDeliveryAmounts();
        std::string list;
        for (GEInt i = 0; i < targets.GetCount(); ++i) {
            char item[160];
            Format(item, "%s%s:%d/%d", list.empty() ? "" : ",", targets.GetAt(i).GetText(),
                   i < counter.GetCount() ? static_cast<int>(counter.GetAt(i)) : -1,
                   i < amounts.GetCount() ? static_cast<int>(amounts.GetAt(i)) : -1);
            list += item;
        }
        Format(line, "QUESTCOUNTER name=%s type=%d status=%d targets=%s", quest->GetName().GetText(), type,
               static_cast<int>(quest->GetStatus().GetValue()), list.empty() ? "-" : list.c_str());
        log(line);
    }
    log("QUESTCOUNTER end");
}
}
