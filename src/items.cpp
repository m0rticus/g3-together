// SPDX-License-Identifier: GPL-3.0-or-later
#include "items.h"

#include "body.h"
#include "guid.h"
#include "log.h"
#include "net.h"

#include <Game.h>
#include <Windows.h>
#include <Script.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace g3t::items {
namespace {
float DistanceToHero(eCEntity* entity)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !entity) return 1e9f;
    const bCVector& a = entity->GetWorldPosition();
    const bCVector& b = hero->GetWorldPosition();
    const float p[3] = {a.GetX(), a.GetY(), a.GetZ()}, q[3] = {b.GetX(), b.GetY(), b.GetZ()};
    return body::Distance(p, q);
}

void Line(char const* tag, char const* guid, Entity& item, LogFn log)
{
    char line[300];
    Entity owner = item.GetInstance() ? item.GetOwner() : Entity();
    Format(line, "%s guid=%s exists=%d name=%s item=%d owner=%s distance_cm=%.0f", tag, guid, item.GetInstance() != nullptr,
              item.GetInstance() ? item.GetName().GetText() : "-", item.GetInstance() && item.IsItem() ? 1 : 0,
              owner.GetInstance() ? owner.GetName().GetText() : "-", DistanceToHero(item.GetInstance()));
    log(line);
}
}

void List(LogFn log)
{
    char line[300], guid[48];
    const bTObjArray<Entity> all = Entity::GetEntities();
    int items = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        if (!entity.GetInstance() || !entity.IsItem() || entity.GetOwner().GetInstance()) continue;
        const float distance = DistanceToHero(entity.GetInstance());
        if (distance > 3000.0f) continue;
        guid::FormatEntity(entity.GetInstance(), guid, sizeof(guid));
        Format(line, "ITEM guid=%s name=%s distance_cm=%.0f", guid, entity.GetName().GetText(), distance);
        log(line);
        ++items;
    }
    Format(line, "ITEMS entities=%d world_items_near=%d", all.GetCount(), items);
    log(line);
    log("ITEMS end");
}

void Take(char const* guid, LogFn log)
{
    _GUID raw;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !guid::Parse(guid, raw)) { log("REFUSED itemtake: no hero or bad GUID"); return; }
    Entity item = guid::Resolve(raw);
    if (!item.GetInstance()) { log("REFUSED itemtake: no such entity"); return; }
    const GEInt stack = Entity(hero).Inventory.InsertStack(item);
    char line[120];
    Format(line, "ITEMTAKE guid=%s stack=%d", guid, stack);
    log(line);
    Entity again = guid::Resolve(raw);
    Line("ITEMAFTER", guid, again, log);
}

void Decay(char const* guid, LogFn log)
{
    _GUID raw;
    if (!guid::Parse(guid, raw)) { log("REFUSED itemdecay: bad GUID"); return; }
    Entity item = guid::Resolve(raw);
    if (!item.GetInstance()) { log("REFUSED itemdecay: no such entity"); return; }
    item.Decay();
    Entity again = guid::Resolve(raw);
    Line("ITEMDECAY", guid, again, log);
}

void Describe(char const* guid, LogFn log)
{
    _GUID raw;
    if (!guid::Parse(guid, raw)) { log("REFUSED iteminfo: bad GUID"); return; }
    Entity item = guid::Resolve(raw);
    Line("ITEMINFO", guid, item, log);
}
}

namespace g3t::items {
namespace {
// The item a hero's _AI_TakeItem is working on: its GUID disappears once the game puts it into a stack.
uint8_t takingGuid[16] = {};
char takingName[64] = "";
bool taking = false;
uint32_t takingAmount = 1;
std::map<std::string, int> carriedBefore;  // the hero's inventory when the take began: item template -> amount

std::map<std::string, int> Carried(eCDynamicEntity* hero)
{
    std::map<std::string, int> out;
    Entity self(hero);
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        Entity item = self.Inventory.GetTemplateItem(i);
        if (item.GetInstance()) out[item.GetName().GetText()] += self.Inventory.GetAmount(i);
    }
    return out;
}
unsigned takesBegun = 0;
unsigned tookCount = 0, ignoredRemotePickups = 0;
char lastTook[200] = "-";
}

void BeforeTake(gCScriptProcessingUnit* spu)
{
    eCEntity* item = spu ? spu->GetTargetEntity() : nullptr;
    if (!item) item = spu ? spu->GetOtherEntity() : nullptr;
    taking = item != nullptr;
    if (!taking) return;
    ++takesBegun;
    const eCEntityProxy proxy(item);
    std::memcpy(takingGuid, &proxy.GetEntityID().GetCore(), 16);
    strncpy_s(takingName, Entity(item).GetName().GetText(), _TRUNCATE);
    takingAmount = Entity(item).Item.GetAmount();
    carriedBefore = Carried(gCSession::GetInstance().GetHero());
}

void AfterTake(gCScriptProcessingUnit*)
{
    if (!taking) return;
    _GUID raw;
    std::memcpy(&raw, takingGuid, 16);
    if (guid::Resolve(raw).GetInstance()) return;  // not taken yet (the animation runs over frames)
    taking = false;
    ++tookCount;
    // What actually arrived can differ from the world item (a gold stack turns into coins): compare.
    for (auto const& [name, amount] : Carried(gCSession::GetInstance().GetHero())) {
        auto it = carriedBefore.find(name);
        const int gained = amount - (it == carriedBefore.end() ? 0 : it->second);
        if (gained <= 0) continue;
        strncpy_s(takingName, name.c_str(), _TRUNCATE);
        takingAmount = static_cast<uint32_t>(gained);
        break;
    }
    char guid[48];
    guid::Format(takingGuid, guid, sizeof(guid));
    Format(lastTook, "guid=%s name=%s amount=%u", guid, takingName, takingAmount);
    Logf("items: hero took %s (personal world pickup; other players' copies unchanged)", lastTook);
}

namespace {
void OnItem(uint8_t from, net::ItemNote const& note)
{
    ++ignoredRemotePickups;
    Logf("items: ignored shared-world pickup message from player %u (op=%u); loose pickups are personal", from, note.op);
}
}

void Start()
{
    net::SetItemHandler(&OnItem);
    Log("items: loose world pickups are personal; chest/corpse loot and NPC merchant stock are personal too");
}

void NoteOwnDrop(char const* name)
{
    Logf("items: this hero dropped %s when knocked out (personal world item)", name);
}

void Pick(char const* guid, LogFn log)
{
    _GUID raw;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !guid::Parse(guid, raw)) { log("REFUSED itempick: no hero or bad GUID"); return; }
    Entity item = guid::Resolve(raw);
    if (!item.GetInstance()) { log("REFUSED itempick: no such entity"); return; }
    Entity self(hero);
    self.NPC.SetCurrentTarget(item);
    self.Routine.FullStop();
    self.Routine.SetTask(bCString("PS_Normal_Take"));
    char line[200];
    Format(line, "ITEMPICK guid=%s task=%s", guid, self.Routine.GetCurrentTask().GetText());
    log(line);
}

void TookReport(LogFn log)
{
    char line[320];
    Format(line, "ITEMTOOK count=%u sent=0 received=%u granted=0 denials=0 removed=0 returned=0 mode=personal last=%s",
           tookCount, ignoredRemotePickups, lastTook);
    log(line);
}
}

namespace g3t::items {
void Count(char const* templateName, LogFn log)
{
    char line[160];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    const GEInt amount = hero ? Entity(hero).Inventory.GetItemAmount(Template(templateName)) : -1;
    Format(line, "INVCOUNT name=%s amount=%d", templateName, amount);
    log(line);
}

void Disable(LogFn log)
{
    log("ITEMSYNCOFF ok=1 (loose world pickups are already personal)");
}

unsigned Returned() { return 0; }
unsigned TakesBegun() { return takesBegun; }
unsigned TakesDone() { return tookCount; }
}

namespace g3t::items {
// QA diagnostic: entities near the hero that carry an inventory and are not living NPCs (chests, corpses),
// with their contents as "template:amount" lists.
void Containers(LogFn log)
{
    char guid[48];
    std::string line;
    const bTObjArray<Entity> all = Entity::GetEntities();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int found = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        eCEntity* instance = entity.GetInstance();
        if (!instance || instance == hero || entity.IsItem() || !instance->GetPropertySet(eEPropertySetType_Inventory)) continue;
        const bool npc = instance->GetPropertySet(eEPropertySetType_NPC) != nullptr;
        if (npc && !entity.IsDead()) continue;
        if (DistanceToHero(instance) > 4000.0f) continue;
        guid::FormatEntity(instance, guid, sizeof(guid));
        line = std::string("CONTAINER guid=") + guid + " name=" + entity.GetName().GetText() + (npc ? " kind=corpse" : " kind=other") +
               " items=";
        std::map<std::string, int> contents;
        for (int s = 0; s < entity.Inventory.GetStackCount(); ++s) {
            Entity item = entity.Inventory.GetTemplateItem(s);
            if (item.GetInstance()) contents[item.GetName().GetText()] += entity.Inventory.GetAmount(s);
        }
        bool first = true;
        for (auto const& [name, amount] : contents) {
            line += (first ? "" : ",") + name + ":" + std::to_string(amount);
            first = false;
        }
        if (first) line += "-";
        log(line.c_str());
        ++found;
    }
    log("CONTAINERS end");
}
// QA negative controls of the play-evening fixes (27.10.): each switches its fix off (on: clientfameon).
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "owndropoff")) {
        log("SWITCH owndropoff ok=1 (not applicable to personal world pickups)");
        return true;
    }
    return false;
}
}
