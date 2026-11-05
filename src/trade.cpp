// SPDX-License-Identifier: GPL-3.0-or-later
#include "trade.h"

#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "offer.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace g3t::trade {
namespace {
using Content = std::map<std::string, int>;
std::map<std::string, std::string> names;
char const* const Gold = "It_Gold";
struct Wares {
    bool known = false;
    uint64_t epoch = 0;
    uint32_t serial = 0;
    int32_t gold = 0;
    Content items;
};
Wares received[net::MaxPlayers];
int32_t filledGold[net::MaxPlayers] = {};
Content sentItems;
int32_t sentGold = -1;
uint64_t epoch = 0;
uint32_t serial = 0;
DWORD lastScan = 0, lastSent = 0;
int partner = 0;
bool puppetDialog = false, windowOpen = false, fillOff = false, overflowWarned = false;
DWORD dialogOverSince = 0;
unsigned goodsSent = 0, goodsReceived = 0, fills = 0, swaps = 0, heroReleases = 0;
gFScript originalCost = nullptr;
bool costHooked = false, nativeHooked = false, nativeFailed = false;
bool deliveryFailed = false;
using TransferFn = GEBool (__thiscall *)(gCInventory_PS*, gCInventory_PS*, gEStackType);
TransferFn originalTransfer = nullptr;

gCInventory_PS* Inventory(eCEntity* body)
{
    return body ? static_cast<gCInventory_PS*>(body->GetPropertySet(eEPropertySetType_Inventory)) : nullptr;
}

std::string KeyOf(void const* id, uint16_t quality)
{
    std::string key(static_cast<char const*>(id), 16);
    key.append(reinterpret_cast<char const*>(&quality), 2);
    return key;
}

std::string StackKey(Entity& entity, int stack)
{
    const Template item = entity.Inventory.GetTemplate(stack);
    return KeyOf(&static_cast<bCPropertyID const&>(item).GetCore(), static_cast<uint16_t>(entity.Inventory.GetQuality(stack) & 1023));
}

uint16_t Quality(std::string const& key)
{
    uint16_t value = 0;
    std::memcpy(&value, key.data() + 16, 2);
    return value;
}

bool Tradable(Entity& self, int stack)
{
    if (self.Inventory.GetSlotFromStack(stack) != gESlot_None) return false;
    Entity item = self.Inventory.GetTemplateItem(stack);
    return item.GetInstance() && !item.Item.IsMissionItem() && item.Item.GetCategory() != gEItemCategory_Skill &&
           std::strcmp(item.GetName().GetText(), "Fist");
}

void Read(Entity& self, Content& items, int32_t& gold)
{
    items.clear();
    gold = 0;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (!Tradable(self, i)) continue;
        Entity item = self.Inventory.GetTemplateItem(i);
        if (!std::strcmp(item.GetName().GetText(), Gold)) {
            gold += self.Inventory.GetAmount(i);
            continue;
        }
        const std::string key = StackKey(self, i);
        names[key] = item.GetName().GetText();
        items[key] += self.Inventory.GetAmount(i);
    }
}

int Selected(gCInventory_PS* inventory, int stack, int amount)
{
    const int selected = inventory->GetTransactionAmount(stack);
    return selected < 0 ? amount : std::min(amount, selected);
}

void ClearSelection(gCInventory_PS* inventory)
{
    for (int i = 0; inventory && i < inventory->GetStackCount(); ++i) inventory->SetTransactionAmount(i, 0);
}

bool HasSelection(gCInventory_PS* inventory)
{
    for (int i = 0; inventory && i < inventory->GetStackCount(); ++i)
        if (inventory->GetTransactionAmount(i)) return true;
    return false;
}

void CreateMerchandise(Entity& puppet, eCTemplateEntityProxy& item, uint16_t quality, int amount)
{
    gCInventory_PS* inventory = Inventory(puppet.GetInstance());
    if (inventory && item.GetEntity() && amount > 0 &&
        inventory->CreateItems(item.GetEntity(), quality, static_cast<GEU32>(amount), gEStackType_Merchandise) < 0) {
        Log("trade: failed to create advertised player merchandise");
        hud::Message(hud::Warning, i18n::Text(i18n::TradeUnavailable));
    }
}

void Fill(Entity& puppet, Wares const& goods)
{
    gCInventory_PS* inventory = Inventory(puppet.GetInstance());
    if (!inventory) return;
    // Keep selections by item identity, not indices: the owner may consume or equip an item while this page is open.
    Content selection;
    for (int i = 0; i < puppet.Inventory.GetStackCount(); ++i)
        if (puppet.Inventory.GetSlotFromStack(i) == gESlot_None)
            selection[StackKey(puppet, i)] += Selected(inventory, i, puppet.Inventory.GetAmount(i));
    for (int i = puppet.Inventory.GetStackCount() - 1; i >= 0; --i)
        if (puppet.Inventory.GetSlotFromStack(i) == gESlot_None) puppet.Inventory.DeleteStack(i);
    for (auto const& [key, amount] : goods.items) {
        _GUID raw;
        std::memcpy(&raw, key.data(), 16);
        eCTemplateEntityProxy item;
        item.SetEntity(bCPropertyID(bCGuid(raw)));
        CreateMerchandise(puppet, item, Quality(key), amount);
    }
    eCTemplateEntityProxy gold;
    gold.SetEntity(bCString(Gold));
    CreateMerchandise(puppet, gold, 0, goods.gold);
    for (int i = 0; i < puppet.Inventory.GetStackCount(); ++i) {
        const auto selected = selection.find(StackKey(puppet, i));
        if (selected != selection.end() && puppet.Inventory.GetSlotFromStack(i) == gESlot_None)
            inventory->SetTransactionAmount(i, std::min(selected->second, puppet.Inventory.GetAmount(i)));
    }
    if (const int id = remote::PuppetOwner(puppet.GetInstance())) filledGold[id] = goods.gold;
    ++fills;
}

void OnGoods(uint8_t player, uint64_t epochIn, uint32_t serialIn, int32_t gold, net::GoodsEntry const* entries, int count)
{
    Wares& goods = received[player];
    if (!epochIn || (goods.known && (epochIn < goods.epoch || (epochIn == goods.epoch && serialIn <= goods.serial)))) return;
    goods = Wares{true, epochIn, serialIn, gold, {}};
    for (int i = 0; i < count; ++i) {
        const std::string key = KeyOf(entries[i].id, entries[i].quality);
        names[key] = entries[i].name;
        goods.items[key] += entries[i].amount;
    }
    ++goodsReceived;
    if (player == partner && !fillOff)
        if (eCEntity* body = remote::PuppetOf(player)) {
            Entity puppet(body);
            Fill(puppet, goods);
        }
}

GEInt GE_STDCALL GoldCost(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    Entity counterpart, owner;
    if (self) counterpart = *static_cast<Entity*>(self);
    else if (spu) counterpart.AttachTo(spu->GetSelfEntity());
    if (other) owner = *static_cast<Entity*>(other);
    else if (spu) owner.AttachTo(spu->GetOtherEntity());
    if (nativeHooked && (remote::PuppetOwner(counterpart.GetInstance()) || remote::PuppetOwner(owner.GetInstance()))) return 0;
    return originalCost(spu, self, other, args);
}

void HookCost()
{
    if (costHooked) return;
    gSScript* cost = nullptr;
    if (GetScriptAdminExt().GetScripts().GetAt(bCString("OnPlayerGetGoldCost"), cost) && cost && cost->m_funcScript) {
        originalCost = cost->m_funcScript;
        cost->m_funcScript = &GoldCost;
        costHooked = true;
        Log("trade: player-only free price hook installed");
    }
}

bool BuildExchange(gCInventory_PS* heroInventory, gCInventory_PS* puppetInventory, int id, net::OfferNote& note)
{
    if (id <= 0 || id >= net::MaxPlayers || !Available() || !net::Connected() || !offer::Idle() ||
        !net::Remotes()[id].connected || !received[id].known) return false;
    note = net::OfferNote{};
    note.buyer = net::LocalId();
    note.buyerEpoch = Epoch();
    note.seller = static_cast<uint8_t>(id);
    note.sellerEpoch = received[id].epoch;
    Content delta;
    int64_t gold = 0;
    gCInventory_PS* inventories[] = {heroInventory, puppetInventory};
    for (int side = 0; side < 2; ++side) {
        gCInventory_PS* inventory = inventories[side];
        if (!inventory) return false;
        Entity entity(inventory->GetEntity());
        for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
            const int amount = Selected(inventory, i, entity.Inventory.GetAmount(i));
            if (!amount) continue;
            if (!Tradable(entity, i)) return false;
            const int signedAmount = side ? amount : -amount;
            Entity item = entity.Inventory.GetTemplateItem(i);
            if (!std::strcmp(item.GetName().GetText(), Gold)) gold += signedAmount;
            else {
                if (amount > 1000000) return false;
                const std::string key = StackKey(entity, i);
                names[key] = item.GetName().GetText();
                delta[key] += signedAmount;
            }
        }
    }
    if (gold < -10000000 || gold > 10000000) return false;
    note.gold = static_cast<int32_t>(gold);
    for (auto const& [key, amount] : delta) {
        if (!amount) continue;
        if (note.count >= net::MaxOfferEntries || amount < -1000000 || amount > 1000000) return false;
        auto& entry = note.entries[note.count++];
        std::memcpy(entry.item, key.data(), 16);
        entry.quality = Quality(key);
        entry.amount = amount;
        strncpy_s(entry.name, names[key].c_str(), _TRUNCATE);
    }
    return note.count || note.gold;
}

bool BeginExchange(gCInventory_PS* heroInventory, gCInventory_PS* puppetInventory, int id)
{
    net::OfferNote note{};
    const bool built = BuildExchange(heroInventory, puppetInventory, id, note);
    ClearSelection(heroInventory);
    ClearSelection(puppetInventory);
    if (!built || !offer::Reserve(note)) {
        hud::Message(hud::Warning, i18n::Text(i18n::TradeUnavailable));
        Log("trade: native exchange refused before sending");
        return false;
    }
    if (!offer::Send(note)) {
        if (!offer::Deliver(note, false)) deliveryFailed = true;
        hud::Message(hud::Warning, i18n::Text(i18n::TradeUnavailable));
        Log("trade: no route for native exchange; reserved goods restored");
        return false;
    }
    ++swaps;
    Publish();
    return true;
}

GEBool __fastcall NativeTransfer(gCInventory_PS* from, void*, gCInventory_PS* to, gEStackType type)
{
    const int fromPlayer = from ? remote::PuppetOwner(from->GetEntity()) : 0;
    const int toPlayer = to ? remote::PuppetOwner(to->GetEntity()) : 0;
    if (!fromPlayer && !toPlayer) return originalTransfer(from, to, type);
    // The native button calls hero -> partner, then partner -> hero. Handle the whole basket at the first call only.
    if (from && from->GetEntity() == gCSession::GetInstance().GetHero() && toPlayer)
        BeginExchange(from, to, toPlayer);
    return GETrue;
}

bool __stdcall NativeCanExchange(void* controls, int playerValue, int partnerValue)
{
    auto* bytes = static_cast<uint8_t*>(controls);
    gCInventory_PS *playerInventory = nullptr, *partnerInventory = nullptr;
    std::memcpy(&playerInventory, bytes + 0x24, sizeof(playerInventory));
    std::memcpy(&partnerInventory, bytes + 0x7c, sizeof(partnerInventory));
    const int id = partnerInventory ? remote::PuppetOwner(partnerInventory->GetEntity()) : 0;
    if (!id) return playerValue >= partnerValue && playerValue > 0 && partnerValue > 0;
    return Available() && net::Connected() && offer::Idle() && net::Remotes()[id].connected && received[id].known &&
           (HasSelection(playerInventory) || HasSelection(partnerInventory));
}

void HookNative()
{
    if (nativeHooked || nativeFailed) return;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("Game.dll"));
    if (!base) return;
    // CP 1.75 Game.dll: native Trade button's value gate and its two complete-basket transfer calls.
    // Verified against c7191a6a...; leave every NPC on the original gate and transfer implementation.
    const uint8_t gate[] = {0x3b,0xdf,0x7c,0x0c,0x85,0xdb,0x7e,0x08,0x85,0xff,0x7e,0x04,0xb0,0x01,0xeb,0x02,0x32,0xc0};
    const uintptr_t calls[] = {0x8a123, 0x8a12d};
    originalTransfer = reinterpret_cast<TransferFn>(GetProcAddress(reinterpret_cast<HMODULE>(base),
        "?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z"));
    bool valid = originalTransfer && !std::memcmp(base + 0x8144a, gate, sizeof(gate));
    for (const uintptr_t call : calls) {
        int32_t offset = 0;
        std::memcpy(&offset, base + call + 1, 4);
        valid = valid && base[call] == 0xe8 && base + call + 5 + offset == base + 0xb7170;
    }
    if (!valid) {
        nativeFailed = true;
        Log("trade: unsupported native trade code; player trading disabled, NPC trading unchanged");
        hud::Message(hud::Warning, i18n::Text(i18n::TradeNativeUnavailable));
        return;
    }
    // One protected range, so failure cannot leave only half of the integration installed.
    constexpr uintptr_t start = 0x8144a, end = 0x8a132;
    DWORD old = 0;
    if (!VirtualProtect(base + start, end - start, PAGE_EXECUTE_READWRITE, &old)) {
        nativeFailed = true;
        Logf("trade: cannot protect native trade hooks (error %lu)", GetLastError());
        hud::Message(hud::Warning, i18n::Text(i18n::TradeNativeUnavailable));
        return;
    }
    uint8_t* site = base + start;
    std::memset(site, 0x90, sizeof(gate));
    site[0] = 0x57; site[1] = 0x53; site[2] = 0x56; site[3] = 0xe8;
    const int32_t can = static_cast<int32_t>(reinterpret_cast<intptr_t>(&NativeCanExchange) - reinterpret_cast<intptr_t>(site + 8));
    std::memcpy(site + 4, &can, 4);
    for (const uintptr_t call : calls) {
        const int32_t offset = static_cast<int32_t>(reinterpret_cast<intptr_t>(&NativeTransfer) - reinterpret_cast<intptr_t>(base + call + 5));
        std::memcpy(base + call + 1, &offset, 4);
    }
    DWORD ignored = 0;
    if (!VirtualProtect(base + start, end - start, old, &ignored)) Logf("trade: cannot restore native code protection (error %lu)", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), base + start, end - start);
    nativeHooked = true;
    Log("trade: native free-exchange button and transaction hooks installed; NPC path preserved");
}

bool OnResult(net::OfferNote const& note, bool accepted)
{
    const bool ok = offer::Deliver(note, accepted);
    deliveryFailed = !ok;
    return ok;
}
}

uint64_t Epoch()
{
    if (!epoch) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        epoch = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }
    return epoch;
}

bool Available() { return nativeHooked && costHooked && !deliveryFailed; }
bool ReadyForSave() { return !deliveryFailed && offer::Idle(); }

uint32_t Publish()
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !net::Connected() || !gCSession::GetInstance().IsGameRunning()) return serial;
    Entity self(hero);
    Content items;
    int32_t gold = 0;
    Read(self, items, gold);
    std::vector<net::GoodsEntry> entries;
    for (auto const& [key, amount] : items) {
        if (entries.size() == net::MaxGoodsEntries) {
            if (!overflowWarned) Log("trade: inventory exceeds advertised stock capacity; remaining stacks are not offered");
            overflowWarned = true;
            break;
        }
        net::GoodsEntry entry{};
        std::memcpy(entry.id, key.data(), 16);
        entry.quality = Quality(key);
        entry.amount = amount;
        strncpy_s(entry.name, names[key].c_str(), _TRUNCATE);
        entries.push_back(entry);
    }
    if (net::SendGoods(Epoch(), serial + 1, gold, entries.data(), static_cast<int>(entries.size()))) {
        ++serial;
        ++goodsSent;
        sentItems = items;
        sentGold = gold;
        lastSent = GetTickCount();
    }
    return serial;
}

void Start()
{
    net::SetGoodsHandler(&OnGoods);
    offer::SetResultHandler(&OnResult);
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    HookNative();
    HookCost();
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    const bool talking = infos && infos->AccessIsRunning();
    eCEntity* npc = talking ? infos->GetDialogPartnerNPC() : nullptr;
    if (npc && remote::PuppetOwner(npc)) {
        if (!puppetDialog) OnOpen(npc);
        puppetDialog = true;
        dialogOverSince = 0;
    }
    if (puppetDialog && !talking) {
        const DWORD now = GetTickCount();
        if (!dialogOverSince) dialogOverSince = now;
        if (now - dialogOverSince > 500) {
            puppetDialog = false;
            if (eCDynamicEntity* body = gCSession::GetInstance().GetHero()) {
                Entity hero(body);
                if (!std::strcmp(hero.Routine.GetCurrentTask().GetText(), "ZS_Talk")) {
                    hero.Routine.FullStop();
                    hero.Routine.ContinueRoutine();
                    ++heroReleases;
                }
            }
        }
    }
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    const bool trading = manager && manager->GetActivePage() == gEGamePage_Trade;
    if (windowOpen && !trading) {
        if (eCEntity* body = remote::PuppetOf(partner)) {
            ClearSelection(Inventory(body));
            Entity puppet(body);
            if (!fillOff) Fill(puppet, received[partner]);
        }
    }
    windowOpen = partner && trading;
    const DWORD now = GetTickCount();
    if (now - lastScan < 250) return;
    lastScan = now;
    eCDynamicEntity* body = gCSession::GetInstance().GetHero();
    if (!body || !net::Connected()) return;
    Entity hero(body);
    Content items;
    int32_t gold = 0;
    Read(hero, items, gold);
    if (items != sentItems || gold != sentGold || now - lastSent > 2000) Publish();
}

bool OnOpen(eCEntity* body)
{
    const int id = remote::PuppetOwner(body);
    if (!id) return false;
    partner = id;
    if (Available() && !fillOff) {
        Entity puppet(body);
        ClearSelection(Inventory(body));
        Fill(puppet, received[id]);
    }
    Logf("trade: native player exchange opened with %d, goods known=%d", id, received[id].known ? 1 : 0);
    return true;
}

void OnWorldChange()
{
    const uint64_t previous = Epoch();
    epoch = 0;
    if (Epoch() <= previous) epoch = previous + 1;
    for (auto& goods : received) goods = Wares{};
    for (auto& gold : filledGold) gold = 0;
    sentItems.clear();
    sentGold = -1;
    serial = 0;
    lastScan = lastSent = 0;
    partner = 0;
    puppetDialog = windowOpen = deliveryFailed = false;
    dialogOverSince = 0;
}

void RestoreGold(eCEntity* body, int player)
{
    if (!body || player <= 0 || player >= net::MaxPlayers) return;
    Entity puppet(body);
    for (int i = puppet.Inventory.GetStackCount() - 1; i >= 0; --i) {
        Entity item = puppet.Inventory.GetTemplateItem(i);
        if (item.GetInstance() && !std::strcmp(item.GetName().GetText(), Gold) && puppet.Inventory.GetSlotFromStack(i) == gESlot_None)
            puppet.Inventory.DeleteStack(i);
    }
    eCTemplateEntityProxy gold;
    gold.SetEntity(bCString(Gold));
    CreateMerchandise(puppet, gold, 0, filledGold[player]);
}

void Report(LogFn log)
{
    char line[320];
    Format(line, "TRADESYNC goods_sent=%u serial=%u own_items=%zu own_gold=%d goods_received=%u partner=%d fills=%u swaps=%u"
           " native_hooked=%d price_hooked=%d window=%d settled=%d hero_releases=%u", goodsSent, serial, sentItems.size(), sentGold,
           goodsReceived, partner, fills, swaps, nativeHooked ? 1 : 0, costHooked ? 1 : 0, windowOpen ? 1 : 0, ReadyForSave() ? 1 : 0,
           heroReleases);
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
    if (const size_t q = only.rfind("-q"); q != std::string::npos) {
        quality = std::atoi(only.c_str() + q + 2);
        only.resize(q);
    }
    Content items;
    int32_t gold = 0;
    uint32_t seenSerial = 0;
    bool known = true;
    if (who.compare(0, 6, "puppet") == 0) {
        eCEntity* body = remote::PuppetOf(std::atoi(who.c_str() + 6));
        known = body != nullptr;
        if (body) { Entity puppet(body); Read(puppet, items, gold); }
    } else {
        const int id = std::atoi(who.c_str());
        if (!id) items = sentItems, gold = sentGold, seenSerial = serial;
        else if (id > 0 && id < net::MaxPlayers && received[id].known)
            items = received[id].items, gold = received[id].gold, seenSerial = received[id].serial;
        else known = false;
    }
    char line[900];
    if (!known) { Format(line, "PLAYERGOODS %s unknown", who.c_str()); log(line); return; }
    int amount = 0;
    for (auto const& [key, n] : items)
        if (names[key] == only && (quality < 0 || Quality(key) == quality)) amount += n;
    Format(line, "PLAYERGOODS %s serial=%u item=%s:%d gold=%d stacks=%zu", who.c_str(), seenSerial,
           only.empty() ? "-" : only.c_str(), amount, gold, items.size());
    log(line);
}

void FillOff(LogFn log) { fillOff = true; log("TRADEFILLOFF ok"); }

void Price(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.find('_');
    if (cut == std::string::npos) { log("REFUSED tradeprice: <partner>_<Item>"); return; }
    const std::string who = arg.substr(0, cut), name = arg.substr(cut + 1);
    eCEntity* body = who.compare(0, 6, "puppet") == 0 ? remote::PuppetOf(std::atoi(who.c_str() + 6)) : Entity::GetEntity(bCString(who.c_str())).GetInstance();
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

bool Exchange(eCEntity* body, char const* name, int amount, bool take)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    const int id = remote::PuppetOwner(body);
    if (!hero || !id || amount <= 0) return false;
    Entity entity(take ? body : hero);
    std::string itemName = name ? name : "";
    int quality = -1;
    if (const size_t q = itemName.rfind("-q"); q != std::string::npos) {
        quality = std::atoi(itemName.c_str() + q + 2);
        itemName.resize(q);
    }
    gCInventory_PS* inventory = Inventory(entity.GetInstance());
    ClearSelection(Inventory(hero));
    ClearSelection(Inventory(body));
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        Entity item = entity.Inventory.GetTemplateItem(i);
        if (item.GetInstance() && itemName == item.GetName().GetText() && Tradable(entity, i) &&
            (quality < 0 || entity.Inventory.GetQuality(i) == static_cast<unsigned>(quality)) &&
            entity.Inventory.GetAmount(i) >= amount) {
            inventory->SetTransactionAmount(i, amount);
            return BeginExchange(Inventory(hero), Inventory(body), id);
        }
    }
    return false;
}

void GiftCommand(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.find('_');
    if (cut == std::string::npos || arg.compare(0, 6, "puppet")) { log("REFUSED tradegift: puppet<id>_<amount> (n for take)"); return; }
    const int id = std::atoi(arg.c_str() + 6);
    if (id <= 0 || id >= net::MaxPlayers) { log("REFUSED tradegift: invalid player"); return; }
    const char* text = arg.c_str() + cut + 1;
    const bool take = *text == 'n';
    if (take) ++text;
    char* end = nullptr;
    const long amount = std::strtol(text, &end, 10);
    if (end == text || *end || amount < 1 || amount > 10000000) { log("REFUSED tradegift: invalid amount"); return; }
    const bool ok = Exchange(remote::PuppetOf(id), Gold, static_cast<int>(amount), take);
    log(ok ? "TRADEGIFT native_exchange=1" : "REFUSED tradegift: exchange unavailable");
}

void GiveQuality(char const* argument, LogFn log)
{
    const std::string arg = argument ? argument : "";
    const size_t cut = arg.rfind('_');
    if (cut == std::string::npos) { log("REFUSED givequality: <Item>_<quality>"); return; }
    const Template item(arg.substr(0, cut).c_str());
    const unsigned quality = static_cast<unsigned>(std::atoi(arg.c_str() + cut + 1));
    eCDynamicEntity* body = gCSession::GetInstance().GetHero();
    if (!body || !item.IsValid() || quality >= 1024) { log("REFUSED givequality: invalid hero, item or quality"); return; }
    Entity hero(body);
    const int stack = hero.Inventory.CreateItems(item, quality, 1);
    char line[200];
    Format(line, "GIVEQUALITY name=%s quality=%u stack=%d got=%u", arg.substr(0, cut).c_str(), quality, stack,
           stack >= 0 ? hero.Inventory.GetQuality(stack) : 0u);
    log(line);
}

void QualityCount(char const* name, LogFn log)
{
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
}
