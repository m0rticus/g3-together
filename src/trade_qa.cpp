// SPDX-License-Identifier: GPL-3.0-or-later
#include "trade_internal.h"

#include "log.h"
#include "offer.h"
#include "remote.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace g3t::trade {
using namespace detail;

void Report(LogFn log)
{
    const SyncSnapshot snap = GetSyncSnapshot();
    char line[320];
    Format(line, "TRADESYNC goods_sent=%u serial=%u own_items=%zu own_gold=%d goods_received=%u partner=%d fills=%u swaps=%u"
           " native_hooked=%d price_hooked=%d window=%d settled=%d hero_releases=%u",
           snap.goodsSent, snap.serial, snap.ownItems, snap.ownGold, snap.goodsReceived, snap.partner, snap.fills,
           snap.swaps, snap.nativeHooked ? 1 : 0, snap.costHooked ? 1 : 0, snap.windowOpen ? 1 : 0,
           ReadyForSave() ? 1 : 0, snap.heroReleases);
    log(line);
    offer::Report(log);
}

void GoodsReport(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.find('_');
    const std::string who = arg.substr(0, cut);
    std::string only = cut == std::string::npos ? "" : arg.substr(cut + 1);
    int quality = -1;
    if (!trade_policy::ParseQualitySuffix(only, quality)) {
        log("REFUSED playergoods: invalid quality");
        return;
    }
    Content items;
    int32_t gold = 0;
    uint32_t seenSerial = 0;
    bool known = false;
    if (who.compare(0, 6, "puppet") == 0) {
        int puppetId = 0;
        eCEntity* body = trade_policy::ParsePuppetId(who.c_str(), net::MaxPlayers, puppetId) ? remote::PuppetOf(puppetId) : nullptr;
        if (body) {
            Entity puppet(body);
            ReadInventory(puppet, items, gold);
            known = true;
        }
    } else if (!who.empty() && std::isdigit(static_cast<unsigned char>(who[0]))) {
        char* end = nullptr;
        const long parsed = std::strtol(who.c_str(), &end, 10);
        if (end != who.c_str() && *end == '\0' && parsed >= 0 && parsed < net::MaxPlayers)
            known = CopyKnownGoods(static_cast<int>(parsed), items, gold, seenSerial);
    }
    char line[900];
    if (!known) { Format(line, "PLAYERGOODS %s unknown", who.c_str()); log(line); return; }
    int amount = 0;
    for (auto const& [key, n] : items)
        if (ItemNameForKey(key) == only && (quality < 0 || trade_policy::StackQuality(key) == quality)) amount += n;
    Format(line, "PLAYERGOODS %s serial=%u item=%s:%d gold=%d stacks=%zu", who.c_str(), seenSerial,
           only.empty() ? "-" : only.c_str(), amount, gold, items.size());
    log(line);
}

void FillOff(LogFn log) { SetFillOff(true); log("TRADEFILLOFF ok"); }

void Price(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.find('_');
    if (cut == std::string::npos) { log("REFUSED tradeprice: <partner>_<Item>"); return; }
    const std::string who = arg.substr(0, cut), name = arg.substr(cut + 1);
    eCEntity* body = nullptr;
    if (who.compare(0, 6, "puppet") == 0) {
        int puppetId = 0;
        if (trade_policy::ParsePuppetId(who.c_str(), net::MaxPlayers, puppetId))
            body = remote::PuppetOf(puppetId);
    } else {
        body = Entity::GetEntity(bCString(who.c_str())).GetInstance();
    }
    eCDynamicEntity* heroBody = gCSession::GetInstance().GetHero();
    if (!body || !heroBody) { log("REFUSED tradeprice: no hero or partner"); return; }
    Entity hero(heroBody), other(body);
    auto stackOf = [&](Entity& entity) {
        for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
            Entity item = entity.Inventory.GetTemplateItem(i);
            if (item.GetInstance() && name == item.GetName().GetText()) return i;
        }
        return -1;
    };
    const int theirs = stackOf(other), mine = stackOf(hero);
    const int buy = theirs < 0 ? -1 : GetScriptAdminExt().CallScript(bCString("OnPlayerGetGoldCost"), heroBody, body, theirs);
    const int sell = mine < 0 ? -1 : GetScriptAdminExt().CallScript(bCString("OnPlayerGetGoldCost"), body, heroBody, mine);
    const int value = theirs < 0 ? -1 : other.Inventory.GetTemplateItem(theirs).Item.GetGoldValue();
    char line[220];
    Format(line, "TRADEPRICE partner=%s item=%s buy=%d sell=%d value=%d", who.c_str(), name.c_str(), buy, sell, value);
    log(line);
}

void GiftCommand(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.find('_');
    if (cut == std::string::npos || arg.compare(0, 6, "puppet")) { log("REFUSED tradegift: puppet<id>_<amount> (n for take)"); return; }
    const std::string who = arg.substr(0, cut);
    int id = 0;
    if (!trade_policy::ParsePuppetId(who.c_str(), net::MaxPlayers, id)) { log("REFUSED tradegift: invalid player"); return; }
    const char* text = arg.c_str() + cut + 1;
    const bool take = *text == 'n';
    if (take) ++text;
    char* end = nullptr;
    const long amount = std::strtol(text, &end, 10);
    if (end == text || *end || amount < 1 || amount > trade_policy::MaxGoldAmount) { log("REFUSED tradegift: invalid amount"); return; }
    const bool ok = Exchange(remote::PuppetOf(id), trade_policy::GoldTemplateName, static_cast<int>(amount), take);
    log(ok ? "TRADEGIFT native_exchange=1" : "REFUSED tradegift: exchange unavailable");
}

void GiveQuality(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.rfind('_');
    if (cut == std::string::npos) { log("REFUSED givequality: <Item>_<quality>"); return; }
    char const* qText = arg.c_str() + cut + 1;
    if (!*qText || !std::isdigit(static_cast<unsigned char>(*qText))) {
        log("REFUSED givequality: invalid hero, item or quality");
        return;
    }
    char* end = nullptr;
    const unsigned long parsedQuality = std::strtoul(qText, &end, 10);
    if (end == qText || *end != '\0' || parsedQuality >= trade_policy::MaxQuality) {
        log("REFUSED givequality: invalid hero, item or quality");
        return;
    }
    const unsigned quality = static_cast<unsigned>(parsedQuality);
    const Template item(arg.substr(0, cut).c_str());
    eCDynamicEntity* body = gCSession::GetInstance().GetHero();
    if (!body || !item.IsValid()) { log("REFUSED givequality: invalid hero, item or quality"); return; }
    Entity hero(body);
    const int stack = hero.Inventory.CreateItems(item, quality, 1);
    char line[200];
    Format(line, "GIVEQUALITY name=%s quality=%u stack=%d got=%u", arg.substr(0, cut).c_str(), quality, stack,
           stack >= 0 ? hero.Inventory.GetQuality(stack) : 0u);
    log(line);
}

void QualityCount(char const* name, LogFn log)
{
    if (!name || !*name) { log("REFUSED qualitycount: missing item name"); return; }
    eCDynamicEntity* body = gCSession::GetInstance().GetHero();
    if (!body) { log("REFUSED qualitycount: no hero"); return; }
    Entity hero(body);
    std::map<unsigned, int> byQuality;
    int total = 0;
    for (int i = 0; i < hero.Inventory.GetStackCount(); ++i) {
        Entity item = hero.Inventory.GetTemplateItem(i);
        if (item.GetInstance() && !std::strcmp(item.GetName().GetText(), name)) {
            byQuality[hero.Inventory.GetQuality(i)] += hero.Inventory.GetAmount(i);
            total += hero.Inventory.GetAmount(i);
        }
    }
    std::string line = "QUALITYCOUNT name=" + std::string(name) + " total=" + std::to_string(total);
    for (auto const& [q, n] : byQuality) line += " q" + std::to_string(q) + "=" + std::to_string(n);
    log(line.c_str());
}

void CloseCommand(LogFn log)
{
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    if (!manager || !manager->ClosePage()) { log("REFUSED tradeclose: no open page"); return; }
    log("TRADECLOSE ok=1");
}

} // namespace g3t::trade
