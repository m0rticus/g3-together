// SPDX-License-Identifier: GPL-3.0-or-later
#include "trade.h"
#include "trade_internal.h"

#include "goods_schedule.h"
#include "guid.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "offer.h"
#include "persist.h"
#include "remote.h"
#include "telemetry.h"
#include "tradeview.h"
#include "ai.h"

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
using detail::Content;
using detail::Wares;
std::map<std::string, std::string> names;
char const* const Gold = trade_policy::GoldTemplateName;
Wares received[net::MaxPlayers];
int32_t filledGold[net::MaxPlayers] = {};
Content sentItems;
int32_t sentGold = -1;
uint64_t epoch = 0;
uint32_t serial = 0, nextSerial = 0;
GoodsSchedule goodsSchedule;
DWORD lastScan = 0;
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

std::string StackKey(Entity& entity, int stack)
{
    const Template item = entity.Inventory.GetTemplate(stack);
    return trade_policy::StackKeyBytes(&static_cast<bCPropertyID const&>(item).GetCore(),
                                       static_cast<uint16_t>(entity.Inventory.GetQuality(stack) & trade_policy::QualityMask));
}

void Read(Entity& self, Content& items, int32_t& gold)
{
    items.clear();
    gold = 0;
    int64_t goldTotal = 0;
    std::map<std::string, int64_t> itemTotals;
    for (int i = 0; i < self.Inventory.GetStackCount(); ++i) {
        if (!TradableStack(self, i)) continue;
        const int amount = self.Inventory.GetAmount(i);
        if (amount <= 0) continue;
        Entity item = self.Inventory.GetTemplateItem(i);
        if (!std::strcmp(item.GetName().GetText(), Gold)) {
            goldTotal += amount;
            continue;
        }
        const std::string key = StackKey(self, i);
        names[key] = item.GetName().GetText();
        itemTotals[key] += amount;
    }
    gold = static_cast<int32_t>(std::clamp<int64_t>(goldTotal, 0, INT32_MAX));
    for (auto const& [key, total] : itemTotals)
        items[key] = static_cast<int>(std::min<int64_t>(total, INT32_MAX));
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

void CreateGoldMerchandise(Entity& puppet, int amount)
{
    eCTemplateEntityProxy gold;
    gold.SetEntity(bCString(Gold));
    CreateMerchandise(puppet, gold, 0, amount);
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
        item.SetEntity(guid::Property(raw));
        CreateMerchandise(puppet, item, trade_policy::StackQuality(key), amount);
    }
    CreateGoldMerchandise(puppet, goods.gold);
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
        const std::string key = trade_policy::StackKeyBytes(entries[i].id, entries[i].quality);
        names[key] = entries[i].name;
        goods.items[key] += entries[i].amount;
    }
    ++goodsReceived;
    if (player == partner && (windowOpen || puppetDialog) && !fillOff)
        if (eCEntity* body = remote::PuppetOf(player)) {
            Entity puppet(body);
            Fill(puppet, goods);
        }
}

GEInt GE_STDCALL GoldCost(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalCost ? originalCost(spu, self, other, args) : 0;
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
        ai::TrackPointer(reinterpret_cast<void*>(&cost->m_funcScript),
                         reinterpret_cast<void*>(originalCost),
                         reinterpret_cast<void*>(&GoldCost), "trade OnPlayerGetGoldCost script");
        costHooked = true;
        Log("trade: player-only free price hook installed");
    }
}

bool BuildExchange(gCInventory_PS* heroInventory, gCInventory_PS* puppetInventory, int id, net::OfferNote& note)
{
    if (id <= 0 || id >= net::MaxPlayers || persist::CheckpointBusy() || !Available() || !net::Connected() || !offer::Idle() ||
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
            if (amount < 0) return false;
            if (!amount) continue;
            if (!TradableStack(entity, i)) return false;
            const int signedAmount = side ? amount : -amount;
            Entity item = entity.Inventory.GetTemplateItem(i);
            if (!std::strcmp(item.GetName().GetText(), Gold)) gold += signedAmount;
            else {
                if (amount > trade_policy::MaxStackAmount) return false;
                const std::string key = StackKey(entity, i);
                names[key] = item.GetName().GetText();
                delta[key] += signedAmount;
            }
        }
    }
    if (gold < -trade_policy::MaxGoldAmount || gold > trade_policy::MaxGoldAmount) return false;
    note.gold = static_cast<int32_t>(gold);
    for (auto const& [key, amount] : delta) {
        if (!amount) continue;
        if (note.count >= net::MaxOfferEntries || amount < -trade_policy::MaxStackAmount || amount > trade_policy::MaxStackAmount) return false;
        auto& entry = note.entries[note.count++];
        std::memcpy(entry.item, key.data(), 16);
        entry.quality = trade_policy::StackQuality(key);
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
        telemetry::OnTradeOffer("exchange_refused", net::LocalId(), static_cast<uint8_t>(id), note.serial, note.gold, note.count, "build_or_reserve_failed", 0);
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
    if (ai::ShuttingDown()) return originalTransfer ? originalTransfer(from, to, type) : GETrue;
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
    if (ai::ShuttingDown()) return true;
    auto* bytes = static_cast<uint8_t*>(controls);
    gCInventory_PS *playerInventory = nullptr, *partnerInventory = nullptr;
    std::memcpy(&playerInventory, bytes + 0x24, sizeof(playerInventory));
    std::memcpy(&partnerInventory, bytes + 0x7c, sizeof(partnerInventory));
    const int id = partnerInventory ? remote::PuppetOwner(partnerInventory->GetEntity()) : 0;
    if (!id) return playerValue >= partnerValue && playerValue > 0 && partnerValue > 0;
    return !persist::CheckpointBusy() && Available() && net::Connected() && offer::Idle() && net::Remotes()[id].connected && received[id].known &&
           (HasSelection(playerInventory) || HasSelection(partnerInventory));
}

void HookNative()
{
    tradeview::Install();
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
    uint8_t originalGate[sizeof(gate)];
    std::memcpy(originalGate, site, sizeof(originalGate));
    std::memset(site, 0x90, sizeof(gate));
    site[0] = 0x57; site[1] = 0x53; site[2] = 0x56; site[3] = 0xe8;
    const int32_t can = static_cast<int32_t>(reinterpret_cast<intptr_t>(&NativeCanExchange) - reinterpret_cast<intptr_t>(site + 8));
    std::memcpy(site + 4, &can, 4);
    uint8_t originalCalls[2][4];
    uint8_t installedCalls[2][4];
    for (int i = 0; i < 2; ++i) {
        const uintptr_t call = calls[i];
        std::memcpy(originalCalls[i], base + call + 1, sizeof(originalCalls[i]));
        const int32_t offset = static_cast<int32_t>(reinterpret_cast<intptr_t>(&NativeTransfer) - reinterpret_cast<intptr_t>(base + call + 5));
        std::memcpy(base + call + 1, &offset, 4);
        std::memcpy(installedCalls[i], base + call + 1, sizeof(installedCalls[i]));
    }
    DWORD ignored = 0;
    if (!VirtualProtect(base + start, end - start, old, &ignored)) Logf("trade: cannot restore native code protection (error %lu)", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), base + start, end - start);
    uint8_t installedGate[sizeof(gate)];
    std::memcpy(installedGate, site, sizeof(installedGate));
    ai::TrackPatch(site, originalGate, installedGate, sizeof(gate), "trade native exchange gate");
    for (int i = 0; i < 2; ++i)
        ai::TrackPatch(base + calls[i] + 1, originalCalls[i], installedCalls[i], sizeof(originalCalls[i]),
                       "trade native transfer call");
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

bool TradableItem(Entity const& item)
{
    return item.GetInstance() && !item.Item.IsMissionItem() &&
           item.Item.GetCategory() != gEItemCategory_Skill &&
           item.Item.GetCategory() != gEItemCategory_Spellbook &&
           std::strcmp(item.GetName().GetText(), "Fist");
}

bool TradableStack(Entity& self, int stack)
{
    if (self.Inventory.GetSlotFromStack(stack) != gESlot_None) return false;
    return TradableItem(self.Inventory.GetTemplateItem(stack));
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
        if (amount <= 0) continue;
        if (entries.size() == net::MaxGoodsEntries) {
            if (!overflowWarned) Log("trade: inventory exceeds advertised stock capacity; remaining stacks are not offered");
            overflowWarned = true;
            break;
        }
        net::GoodsEntry entry{};
        std::memcpy(entry.id, key.data(), 16);
        entry.quality = trade_policy::StackQuality(key);
        entry.amount = std::min(amount, trade_policy::MaxStackAmount);
        strncpy_s(entry.name, names[key].c_str(), _TRUNCATE);
        entries.push_back(entry);
    }
    // A partial broadcast can have reached some recipients even if queuing another failed.
    // Never reuse its serial for a later, possibly different snapshot.
    if (++nextSerial == 0) ++nextSerial;
    if (net::SendGoods(Epoch(), nextSerial, gold, entries.data(), static_cast<int>(entries.size()))) {
        serial = nextSerial;
        ++goodsSent;
        sentItems = items;
        sentGold = gold;
        goodsSchedule.Published(GetTickCount(), net::JoinCount(), net::WorldEntryCount());
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
    const int talkingPuppet = npc ? remote::PuppetOwner(npc) : 0;
    if (talkingPuppet) {
        if (!puppetDialog || partner != talkingPuppet) OnOpen(npc);
        puppetDialog = true;
        dialogOverSince = 0;
    } else if (talking) {
        puppetDialog = false;
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
    windowOpen = partner && puppetDialog && trading;
    if (!puppetDialog && !windowOpen) partner = 0;
    const DWORD now = GetTickCount();
    if (now - lastScan < 250) return;
    lastScan = now;
    eCDynamicEntity* body = gCSession::GetInstance().GetHero();
    if (!body || !net::Connected()) return;
    Entity hero(body);
    Content items;
    int32_t gold = 0;
    Read(hero, items, gold);
    if (goodsSchedule.Due(now, items != sentItems || gold != sentGold, net::JoinCount(), net::WorldEntryCount())) Publish();
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
    telemetry::OnContainerAccess("player_trade_open", gCSession::GetInstance().GetHero(), body, true, received[id].known ? "goods_known" : "goods_unknown");
    return true;
}

void OnWorldChange()
{
    offer::OnWorldChange();
    const uint64_t previous = Epoch();
    epoch = 0;
    if (Epoch() <= previous) epoch = previous + 1;
    for (auto& goods : received) goods = Wares{};
    for (auto& gold : filledGold) gold = 0;
    sentItems.clear();
    names.clear();
    sentGold = -1;
    serial = nextSerial = 0;
    goodsSchedule = GoodsSchedule{};
    lastScan = 0;
    partner = 0;
    puppetDialog = windowOpen = deliveryFailed = false;
    dialogOverSince = 0;
}

void OnPeerLeft(uint8_t player)
{
    if (!player || player >= net::MaxPlayers) return;
    offer::OnPeerLeft(player);
    received[player] = Wares{};
    filledGold[player] = 0;
    if (partner == player) {
        partner = 0;
        windowOpen = false;
    }
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
    CreateGoldMerchandise(puppet, filledGold[player]);
}

bool Exchange(eCEntity* body, char const* name, int amount, bool take)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    const int id = remote::PuppetOwner(body);
    if (!hero || !id || amount <= 0) return false;
    Entity entity(take ? body : hero);
    std::string itemName = name ? name : "";
    int quality = -1;
    if (!trade_policy::ParseQualitySuffix(itemName, quality)) return false;
    gCInventory_PS* inventory = Inventory(entity.GetInstance());
    if (!inventory) return false;
    ClearSelection(Inventory(hero));
    ClearSelection(Inventory(body));
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        Entity item = entity.Inventory.GetTemplateItem(i);
        if (item.GetInstance() && itemName == item.GetName().GetText() && TradableStack(entity, i) &&
            (quality < 0 || (entity.Inventory.GetQuality(i) & trade_policy::QualityMask) == static_cast<unsigned>(quality)) &&
            entity.Inventory.GetAmount(i) >= amount) {
            inventory->SetTransactionAmount(i, amount);
            return BeginExchange(Inventory(hero), Inventory(body), id);
        }
    }
    return false;
}

namespace detail {
void ReadInventory(Entity& self, Content& items, int32_t& gold) { Read(self, items, gold); }

SyncSnapshot GetSyncSnapshot()
{
    return {goodsSent, serial, sentItems.size(), sentGold, goodsReceived, partner,
            fills, swaps, nativeHooked, costHooked, windowOpen, heroReleases};
}

bool CopyKnownGoods(int player, Content& itemsOut, int32_t& goldOut, uint32_t& serialOut)
{
    if (player == 0) {
        itemsOut = sentItems;
        goldOut = sentGold;
        serialOut = serial;
        return true;
    }
    if (player > 0 && player < net::MaxPlayers && received[player].known) {
        itemsOut = received[player].items;
        goldOut = received[player].gold;
        serialOut = received[player].serial;
        return true;
    }
    return false;
}

std::string ItemNameForKey(std::string const& key)
{
    const auto it = names.find(key);
    return it == names.end() ? std::string{} : it->second;
}

void SetFillOff(bool off) { fillOff = off; }
} // namespace detail
}
