// SPDX-License-Identifier: GPL-3.0-or-later
#include "personal_ledger.h"

#include "ai.h"
#include "guid.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "remote.h"
#include "world.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

namespace g3t::containers::ledger {

namespace personal = saving::personal;

struct ObserverState final : gCInventoryListener {
    std::string key;
    gCInventory_PS* inventory = nullptr;
    gCInventory_PS* retiredInventory = nullptr;
    eCEntity* owner = nullptr;
    personal::ChangeTracker changes;
    unsigned depth = 0;
    uint32_t generation = 0;
    bool active = false;

    void Changed() { if (active && !ai::ShuttingDown()) changes.Changed(); }
    void OnStackCreate(GEInt, gCInventoryStack const*) override { Changed(); }
    void OnStackChange(GEInt, gCInventoryStack const*) override { Changed(); }
    void OnStackDelete(GEInt, gCInventoryStack const*) override { Changed(); }
    void OnSortInventory() override { Changed(); }
    void OnDestroyInventory() override;
};

namespace {
personal::Snapshot personalLedger;
size_t personalLedgerBytes = personal::HeaderBytes;
std::set<std::string> personalPending;
std::set<std::string> personalErrors;
std::set<std::string> personalRestoreErrors;
std::set<std::string> personalUncertain;
bool personalErrorOverflow = false;
std::set<std::string> restoredPersonal, openedPersonalTrade, seenPersonalTradePage;

constexpr size_t MaxPersonalObservers = 65536;
std::vector<std::unique_ptr<ObserverState>> personalObservers;
std::vector<ObserverState*> reusablePersonalObservers;
std::map<std::string, ObserverState*> currentPersonalObservers;
size_t destroyedPersonalObservers = 0;
uint32_t observerGeneration = 1;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }
gCInventory_PS* InventoryOf(eCEntity* entity) { return entity ? static_cast<gCInventory_PS*>(entity->GetPropertySet(eEPropertySetType_Inventory)) : nullptr; }
Entity ResolveKey(std::string const& key) { return guid::Resolve(reinterpret_cast<uint8_t const*>(key.data())); }

void ClearTrackingState()
{
    personalPending.clear(); personalErrors.clear(); personalRestoreErrors.clear(); personalUncertain.clear();
    personalErrorOverflow = false;
    restoredPersonal.clear(); openedPersonalTrade.clear(); seenPersonalTradePage.clear();
}

void NoteCaptureError(ObserverState* observer, std::string const& key, char const* message, Entity& entity)
{
    observer->changes.Refreshed();
    if (!personalErrors.count(key)) {
        if (personalErrors.size() < personal::MaxInventoryRows) {
            personalErrors.insert(key);
            Logf(message, NameOf(entity).c_str());
        } else {
            personalErrorOverflow = true;
        }
    }
}

std::string TemplateKey(Entity& entity, int stack)
{
    const Template item = entity.Inventory.GetTemplate(stack);
    bCPropertyID const& id = item;
    return std::string(reinterpret_cast<char const*>(&id.GetCore()), 16);
}

bool Counted(Entity& entity, int stack, bool allowEquipped = false)
{
    const gESlot slot = entity.Inventory.GetSlotFromStack(stack);
    if (!allowEquipped && slot != gESlot_None) return false;
    auto* inventory = InventoryOf(entity.GetInstance());
    if (!inventory || !inventory->GetStack(stack) || entity.Inventory.GetAmount(stack) <= 0) return false;
    if (slot != gESlot_None && entity.Inventory.GetCategory(stack) == gEItemCategory_None) return false;
    Entity item = entity.Inventory.GetTemplateItem(stack);
    if (!item.GetInstance() || !item.GetInstance()->GetPropertySet(eEPropertySetType_Item)) return false;
    if (!std::strcmp(item.GetName().GetText(), "Fist")) return false;
    return personal::Id(TemplateKey(entity, stack));
}

bool StackMerchandise(Entity& entity, gCInventory_PS const& inventory, int index, personal::Kind kind)
{
    auto const* stack = inventory.GetStack(index);
    if (!stack) return false;
    const gEStackType type = stack->GetType();
    if (type == gEStackType_Merchandise || entity.Inventory.IsStackMerchandise(index)) return true;
    return kind == personal::Kind::Trader && type != gEStackType_Normal && type != gEStackType_Treasure;
}

bool PersonalCounted(Entity& entity, int index)
{
    const bool npc = entity.GetInstance() && entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    personal::Kind kind = personal::Kind::Corpse;
    const bool haveKind = npc && PersonalKind(entity, kind);
    const bool allowEquipped = npc && haveKind && kind == personal::Kind::Corpse;
    if (!Counted(entity, index, allowEquipped)) return false;
    if (npc) {
        const gEItemCategory category = entity.Inventory.GetCategory(index);
        auto const* inventory = InventoryOf(entity.GetInstance());
        if ((category == gEItemCategory_Skill || category == gEItemCategory_Spellbook) &&
            (!inventory || !StackMerchandise(entity, *inventory, index, kind))) return false;
    }
    return true;
}

void DetachAndRecycleObserver(ObserverState* observer, eCEntity* instance)
{
    if (observer->inventory && instance &&
        instance->GetPropertySet(eEPropertySetType_Inventory) == observer->inventory) {
        observer->inventory->RemoveListener(observer);
    }
    observer->active = false;
    observer->inventory = nullptr;
    observer->retiredInventory = nullptr;
    observer->owner = nullptr;
    if (!observer->depth) {
        observer->generation = 0;
        reusablePersonalObservers.push_back(observer);
    }
}

void ReclaimCompletedObservers()
{
    destroyedPersonalObservers = 0;
    for (auto it = currentPersonalObservers.begin(); it != currentPersonalObservers.end();) {
        ObserverState* observer = it->second;
        if (observer->depth) {
            if (observer->changes.destroyed) ++destroyedPersonalObservers;
            ++it;
            continue;
        }
        Entity entity = ResolveKey(it->first);
        eCEntity* instance = entity.GetInstance();
        auto* currentInv = InventoryOf(instance);
        const bool liveMatch = observer->inventory && currentInv == observer->inventory;
        if (liveMatch) { ++it; continue; }
        DetachAndRecycleObserver(observer, instance);
        it = currentPersonalObservers.erase(it);
    }
}

void RetirePersonalObservers()
{
    if (++observerGeneration == 0) ++observerGeneration;
    for (auto const& entry : currentPersonalObservers) {
        Entity entity = ResolveKey(entry.first);
        DetachAndRecycleObserver(entry.second, entity.GetInstance());
    }
    currentPersonalObservers.clear();
    destroyedPersonalObservers = 0;
}

bool ObservePersonal(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!instance) return false;
    const std::string key = EntityKey(instance);
    auto* inventory = InventoryOf(instance);
    if (!inventory) return false;
    auto existing = currentPersonalObservers.find(key);
    if (existing != currentPersonalObservers.end()) {
        ObserverState* observer = existing->second;
        if (observer->inventory == inventory && observer->changes.CanRefresh()) return true;
        DetachAndRecycleObserver(observer, instance);
        currentPersonalObservers.erase(existing);
    }
    if (reusablePersonalObservers.empty() || destroyedPersonalObservers > 0) ReclaimCompletedObservers();
    ObserverState* observer = nullptr;
    if (!reusablePersonalObservers.empty()) {
        observer = reusablePersonalObservers.back();
        reusablePersonalObservers.pop_back();
    } else {
        if (personalObservers.size() >= MaxPersonalObservers) {
            if (!personalErrorOverflow) {
                personalErrorOverflow = true;
                Log("WARN containers: personal inventory observer limit reached");
            }
            return false;
        }
        personalObservers.push_back(std::make_unique<ObserverState>());
        observer = personalObservers.back().get();
    }
    observer->key = key;
    observer->inventory = inventory;
    observer->retiredInventory = nullptr;
    observer->owner = instance;
    observer->changes = {};
    observer->depth = 0;
    observer->generation = observerGeneration;
    observer->active = true;
    currentPersonalObservers[key] = observer;
    inventory->AddListener(observer);
    return true;
}

bool ResolveStackTemplates(Entity& entity, eCEntity* instance,
                           std::vector<personal::Stack> const& stacks,
                           std::vector<eCTemplateEntity*>& templates)
{
    templates.clear();
    templates.reserve(stacks.size());
    for (auto const& stack : stacks) {
        _GUID raw;
        std::memcpy(&raw, stack.item.data(), 16);
        eCTemplateEntityProxy proxy;
        proxy.SetEntity(guid::Property(raw));
        eCTemplateEntity* item = proxy.GetEntity();
        auto* itemProperty = item ? static_cast<gCItem_PS*>(item->GetPropertySet(eEPropertySetType_Item)) : nullptr;
        if (!itemProperty) return false;
        const gEItemCategory category = itemProperty->GetCategory();
        if (instance->GetPropertySet(eEPropertySetType_NPC) && stack.type == personal::Stock::Normal &&
            (category == gEItemCategory_Skill || category == gEItemCategory_Spellbook)) return false;
        if (stack.type == personal::Stock::Normal) {
            for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
                if (!PersonalCounted(entity, i) &&
                    (entity.Inventory.GetSlotFromStack(i) == gESlot_None ||
                     entity.Inventory.GetSlotFromStack(i) == gESlot_Ammo) &&
                    TemplateKey(entity, i) == stack.item)
                    return false;
            }
        }
        templates.push_back(item);
    }
    return true;
}

using StackKey = std::tuple<std::string, uint32_t, personal::Stock>;
using EquippedSlot = std::pair<StackKey, gESlot>;

StackKey StackKeyOf(Entity& entity, int index, personal::Stock stock)
{
    return {TemplateKey(entity, index), personal::MaskQuality(entity.Inventory.GetQuality(index)), stock};
}

void CaptureEquippedSlots(Entity& entity, gCInventory_PS const& inventory,
                          personal::Kind kind, std::vector<EquippedSlot>& slots)
{
    slots.clear();
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        const gESlot slot = entity.Inventory.GetSlotFromStack(i);
        if (slot == gESlot_None || !PersonalCounted(entity, i)) continue;
        const personal::Stock stock = StackMerchandise(entity, inventory, i, kind)
            ? personal::Stock::Merchandise : personal::Stock::Normal;
        slots.emplace_back(StackKeyOf(entity, i, stock), slot);
    }
}

void RestoreMatchingSlot(Entity& entity, int created, personal::Stack const& stack,
                         std::vector<EquippedSlot>& slots)
{
    if (created < 0 || slots.empty()) return;
    const StackKey key{stack.item, personal::MaskQuality(stack.quality), stack.type};
    for (auto it = slots.begin(); it != slots.end();) {
        if (it->first != key) { ++it; continue; }
        const gESlot slot = it->second;
        it = slots.erase(it);
        if (entity.Inventory.FindStackIndex(slot) >= 0) continue;
        const int amount = entity.Inventory.GetAmount(created);
        if (amount <= 0) break;
        if (amount > 1 && slot != gESlot_Ammo) {
            const int split = entity.Inventory.SplitStack(created, 1);
            if (split >= 0) {
                entity.Inventory.EquipStackToSlot(split, slot);
                continue;
            }
        }
        entity.Inventory.EquipStackToSlot(created, slot);
        break;
    }
}

void ApplyPersonalMetadata(Entity& entity, gCInventory_PS& inventory, uint8_t flags)
{
    inventory.AccessGeneratedPlunder() = (flags & personal::GeneratedPlunder) != 0;
    inventory.AccessGeneratedTrade() = (flags & personal::GeneratedTrade) != 0;
    if (flags & personal::HasLock)
        entity.Lock.Status = (flags & personal::Locked) ? gELockStatus_Locked : gELockStatus_Unlocked;
    if (flags & personal::HasDoor) {
        const bool open = entity.Door.IsOpen() != GEFalse;
        if ((flags & personal::DoorOpen) && !open) entity.Door.Open(GEFalse);
        else if (!(flags & personal::DoorOpen) && open) entity.Door.Close(GEFalse);
    }
}

void ReadPersonalFlags(Entity& entity, personal::Inventory& row)
{
    if (entity.Inventory.IsGeneratedPlunder()) row.flags |= personal::GeneratedPlunder;
    if (entity.Inventory.IsGeneratedTrade()) row.flags |= personal::GeneratedTrade;
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_Door)) {
        row.flags |= personal::HasDoor;
        if (entity.Door.IsOpen()) row.flags |= personal::DoorOpen;
    }
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_Lock)) {
        row.flags |= personal::HasLock;
        if (entity.Lock.IsLocked()) row.flags |= personal::Locked;
    }
}

bool PreservedCorpseStack(Entity& entity, gCInventory_PS const& inventory, int index,
                          personal::Kind kind, bool preserveEquipped)
{
    return preserveEquipped && entity.Inventory.GetSlotFromStack(index) != gESlot_None &&
           !StackMerchandise(entity, inventory, index, kind);
}

void CollectPersonalStacks(Entity& entity, gCInventory_PS const& inventory, personal::Kind kind,
                           uint32_t maxAmount, bool maskQuality, personal::Inventory& row)
{
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        if (!PersonalCounted(entity, i)) continue;
        const int amount = entity.Inventory.GetAmount(i);
        if (amount <= 0) continue;
        const uint32_t clamped = amount > static_cast<int>(maxAmount) ? maxAmount : static_cast<uint32_t>(amount);
        const personal::Stock stock = StackMerchandise(entity, inventory, i, kind)
            ? personal::Stock::Merchandise
            : personal::Stock::Normal;
        const uint32_t quality = maskQuality ? personal::MaskQuality(entity.Inventory.GetQuality(i))
                                             : entity.Inventory.GetQuality(i);
        row.stacks.push_back({TemplateKey(entity, i), clamped, quality, stock});
    }
}

int CreateAndEquipStack(Entity& entity, gCInventory_PS& inventory, eCTemplateEntity* item,
                        personal::Stack const& stack, uint32_t amount, bool restoreSlot,
                        std::vector<EquippedSlot>& remainingSlots)
{
    const gEStackType type = stack.type == personal::Stock::Merchandise ? gEStackType_Merchandise : gEStackType_Normal;
    const int beforeCount = entity.Inventory.GetStackCount();
    int created = inventory.CreateItems(item, stack.quality, static_cast<GEU32>(amount), type);
    if (created < 0) return -1;
    if (created < beforeCount && entity.Inventory.GetAmount(created) > static_cast<int>(amount)) {
        const int split = entity.Inventory.SplitStack(created, static_cast<GEInt>(amount));
        if (split >= 0) created = split;
    }
    if (restoreSlot) RestoreMatchingSlot(entity, created, stack, remainingSlots);
    return created;
}

bool ReadRawPersonal(Entity& entity, personal::Kind kind, gCInventory_PS const& inventory,
                     personal::Inventory& row, std::vector<eCTemplateEntity*>& templates)
{
    row = {};
    row.kind = kind;
    ReadPersonalFlags(entity, row);
    templates.clear();
    CollectPersonalStacks(entity, inventory, kind, (std::numeric_limits<uint32_t>::max)(), false, row);
    return ResolveStackTemplates(entity, entity.GetInstance(), row.stacks, templates);
}

void RollbackPersonal(Entity& entity, gCInventory_PS& inventory,
                      personal::Inventory const& before,
                      std::vector<eCTemplateEntity*> const& beforeTemplates,
                      std::vector<EquippedSlot> const* beforeSlots = nullptr)
{
    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i) {
        if (!PersonalCounted(entity, i)) continue;
        if (const gESlot slot = entity.Inventory.GetSlotFromStack(i); slot != gESlot_None) {
            inventory.UnlinkStackFromSlot(slot);
            if (entity.Inventory.GetSlotFromStack(i) != gESlot_None) continue;
        }
        entity.Inventory.DeleteStack(i);
    }
    std::map<StackKey, int64_t> remaining;
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        if (!PersonalCounted(entity, i)) continue;
        const int amount = entity.Inventory.GetAmount(i);
        if (amount <= 0) continue;
        const personal::Stock stock = StackMerchandise(entity, inventory, i, before.kind)
            ? personal::Stock::Merchandise
            : personal::Stock::Normal;
        remaining[{TemplateKey(entity, i), personal::MaskQuality(entity.Inventory.GetQuality(i)), stock}] += amount;
    }
    std::vector<EquippedSlot> remainingSlots = beforeSlots ? *beforeSlots : std::vector<EquippedSlot>{};
    for (size_t i = 0; i < before.stacks.size() && i < beforeTemplates.size(); ++i) {
        auto const& stack = before.stacks[i];
        const StackKey key{stack.item, personal::MaskQuality(stack.quality), stack.type};
        int64_t need = stack.amount;
        if (auto it = remaining.find(key); it != remaining.end() && it->second > 0) {
            const int64_t already = std::min(need, it->second);
            need -= already;
            it->second -= already;
        }
        if (need <= 0) continue;
        CreateAndEquipStack(entity, inventory, beforeTemplates[i], stack, static_cast<uint32_t>(need),
                            before.kind == personal::Kind::Corpse, remainingSlots);
    }
    ApplyPersonalMetadata(entity, inventory, before.flags);
}

bool ReadPersonal(Entity& entity, personal::Inventory& row)
{
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return false;
    auto* inventory = InventoryOf(entity.GetInstance());
    if (!inventory) return false;
    personal::Inventory result;
    result.kind = kind;
    ReadPersonalFlags(entity, result);
    CollectPersonalStacks(entity, *inventory, kind, personal::MaxAmount, true, result);
    bool truncated = false;
    personal::NormalizeStacks(result.stacks, &truncated);
    if (truncated)
        Logf("WARN containers: capped personal inventory stacks for %s at %zu",
             NameOf(entity).c_str(), personal::MaxStacks);
    if (!personal::Valid(result)) return false;
    row = std::move(result);
    return true;
}

bool CompatiblePersonalKind(personal::Kind saved, personal::Kind live)
{
    return saved == live || (saved == personal::Kind::Trader && live == personal::Kind::Corpse);
}

bool ApplyPersonal(Entity& entity, personal::Inventory const& row)
{
    personal::Kind kind;
    if (!PersonalKind(entity, kind) || !CompatiblePersonalKind(row.kind, kind)) return false;
    eCEntity* instance = entity.GetInstance();
    auto* inventory = InventoryOf(instance);
    if (!inventory ||
        ((row.flags & personal::HasDoor) != 0) != (instance->GetPropertySet(eEPropertySetType_Door) != nullptr) ||
        ((row.flags & personal::HasLock) != 0) != (instance->GetPropertySet(eEPropertySetType_Lock) != nullptr)) return false;

    std::vector<eCTemplateEntity*> templates;
    if (!ResolveStackTemplates(entity, instance, row.stacks, templates)) return false;

    personal::Inventory before;
    std::vector<eCTemplateEntity*> beforeTemplates;
    if (!ReadRawPersonal(entity, kind, *inventory, before, beforeTemplates)) return false;
    std::vector<EquippedSlot> beforeSlots;
    CaptureEquippedSlots(entity, *inventory, before.kind, beforeSlots);
    std::vector<EquippedSlot> remainingSlots = beforeSlots;

    const bool preserveCorpseEquipped = row.kind == personal::Kind::Trader && kind == personal::Kind::Corpse;
    std::map<StackKey, int64_t> preservedTotals;
    if (preserveCorpseEquipped) {
        for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
            if (!PersonalCounted(entity, i) || !PreservedCorpseStack(entity, *inventory, i, kind, true)) continue;
            const int amount = entity.Inventory.GetAmount(i);
            if (amount > 0) preservedTotals[StackKeyOf(entity, i, personal::Stock::Normal)] += amount;
        }
    }

    auto rollbackAndFail = [&]() { RollbackPersonal(entity, *inventory, before, beforeTemplates, &beforeSlots); return false; };

    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i) {
        if (!PersonalCounted(entity, i) || PreservedCorpseStack(entity, *inventory, i, kind, preserveCorpseEquipped)) continue;
        if (const gESlot slot = entity.Inventory.GetSlotFromStack(i); slot != gESlot_None) {
            inventory->UnlinkStackFromSlot(slot);
            if (entity.Inventory.GetSlotFromStack(i) != gESlot_None) return rollbackAndFail();
        }
        if (!entity.Inventory.DeleteStack(i)) return rollbackAndFail();
    }
    for (size_t i = 0; i < row.stacks.size(); ++i) {
        auto const& stack = row.stacks[i];
        if (CreateAndEquipStack(entity, *inventory, templates[i], stack, stack.amount,
                                kind == personal::Kind::Corpse && !preserveCorpseEquipped, remainingSlots) < 0)
            return rollbackAndFail();
    }
    ApplyPersonalMetadata(entity, *inventory, row.flags);

    personal::Inventory after;
    if (!ReadPersonal(entity, after) || after.kind != kind || row.flags != after.flags) return rollbackAndFail();
    std::map<StackKey, int64_t> totals;
    for (auto const& stack : row.stacks) totals[{stack.item, stack.quality, stack.type}] += stack.amount;
    for (auto const& entry : preservedTotals) totals[entry.first] += entry.second;
    for (auto const& stack : after.stacks) totals[{stack.item, stack.quality, stack.type}] -= stack.amount;
    for (auto const& entry : totals) if (entry.second != 0) return rollbackAndFail();
    return true;
}

bool AwaitingDeath(Entity& entity, personal::Inventory const& row)
{
    return row.kind == personal::Kind::Corpse && entity.GetInstance() &&
           entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) && !entity.IsDead() && !entity.IsDown();
}
} // namespace

void ObserverState::OnDestroyInventory()
{
    if (!active || ai::ShuttingDown()) return;
    changes.Destroyed();
    retiredInventory = inventory;
    inventory = nullptr;
    active = false;
    ++destroyedPersonalObservers;
    if (depth) {
        personalUncertain.insert(key);
        Log("WARN containers: personal inventory unloaded during active mutation; keeping last committed snapshot");
    }
    if (personalLedger.count(key)) personalPending.insert(key);
}

bool HasInventory(eCEntity* entity) { return entity && entity->GetPropertySet(eEPropertySetType_Inventory); }

bool IsContainer(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!HasInventory(instance) || entity.IsItem() || instance == gCSession::GetInstance().GetHero()) return false;
    return !instance->GetPropertySet(eEPropertySetType_NPC) || entity.IsDead() || entity.IsDown();
}

bool IsChest(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return HasInventory(instance) && !entity.IsItem() && !instance->GetPropertySet(eEPropertySetType_NPC) &&
           instance != gCSession::GetInstance().GetHero();
}
bool IsTrader(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!HasInventory(instance) || !instance->GetPropertySet(eEPropertySetType_NPC) ||
        entity.IsDead() || entity.IsDown() || instance == gCSession::GetInstance().GetHero())
        return false;
    Entity const sets[5] = {entity.Inventory.GetTreasureSet1(), entity.Inventory.GetTreasureSet2(),
                            entity.Inventory.GetTreasureSet3(), entity.Inventory.GetTreasureSet4(),
                            entity.Inventory.GetTreasureSet5()};
    for (Entity set : sets)
        if (set.GetInstance()) {
            const gETreasureDistribution kind = set.TreasureSet.TreasureDistribution;
            if (kind == gETreasureDistribution_Trade_Generate || kind == gETreasureDistribution_Trade_Refresh) return true;
        }
    return false;
}
bool Shared(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return HasInventory(instance) && !entity.IsItem() && instance != gCSession::GetInstance().GetHero() && !remote::IsPuppet(instance);
}
bool Corpse(Entity& entity) { return entity.GetInstance() && entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) && (entity.IsDead() || entity.IsDown()); }
bool PersonalKind(Entity& entity, personal::Kind& kind)
{
    if (!Shared(entity)) return false;
    if (Corpse(entity)) kind = personal::Kind::Corpse;
    else if (IsTrader(entity)) kind = personal::Kind::Trader;
    else if (IsChest(entity)) kind = personal::Kind::Chest;
    else return false;
    return true;
}
std::string EntityKey(eCEntity* entity) { uint8_t raw[16]; guid::CopyEntity(entity, raw); return Key(raw); }
std::string NameOf(Entity& entity) { return entity.GetInstance() ? entity.GetName().GetText() : "?"; }

bool PersonalAdmitted() { return !net::Connected() || net::IsHost() || net::InWorld(net::LocalId()); }
bool IsPending(std::string const& key) { return personalPending.count(key) != 0; }
bool IsRestored(std::string const& key) { return restoredPersonal.count(key) != 0; }
void MarkTradeOpened(std::string const& key) { openedPersonalTrade.insert(key); }

void TrackTradeClose()
{
    if (openedPersonalTrade.empty() || !PersonalAdmitted()) return;
    gCGUIManager* manager = gCSession::GetInstance().GetGUIManager();
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    const bool tradePageOpen = manager && manager->GetActivePage() == gEGamePage_Trade;
    const bool dialogRunning = infos && infos->AccessIsRunning();
    eCEntity* partner = tradePageOpen && dialogRunning ? infos->GetDialogPartnerNPC() : nullptr;
    const std::string active = partner ? EntityKey(partner) : std::string();
    for (auto it = openedPersonalTrade.begin(); it != openedPersonalTrade.end();) {
        if (*it == active) { seenPersonalTradePage.insert(*it); ++it; continue; }
        if (!seenPersonalTradePage.count(*it) && tradePageOpen && dialogRunning) { ++it; continue; }
        Entity entity = ResolveKey(*it);
        if (entity.GetInstance()) RememberPersonal(entity);
        restoredPersonal.erase(*it);
        seenPersonalTradePage.erase(*it);
        it = openedPersonalTrade.erase(it);
    }
}

bool PersonalTeardown(Entity& entity)
{
    if (!entity.GetInstance()) return false;
    auto observer = currentPersonalObservers.find(EntityKey(entity.GetInstance()));
    return observer != currentPersonalObservers.end() && observer->second->changes.destroyed &&
           observer->second->owner == entity.GetInstance() &&
           entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory) == observer->second->retiredInventory;
}

bool EnsurePersonal(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!instance) return true;
    if (destroyedPersonalObservers > 0) ReclaimCompletedObservers();
    if (PersonalTeardown(entity)) return false;
    const std::string key = EntityKey(instance);
    personalUncertain.erase(key);
    auto pending = personalPending.find(key);
    if (pending == personalPending.end()) return true;
    auto row = personalLedger.find(key);
    if (row == personalLedger.end()) return false;
    if (AwaitingDeath(entity, row->second)) return false;
    if (!ApplyPersonal(entity, row->second)) {
        if (personalRestoreErrors.insert(key).second)
            Logf("WARN containers: personal restore skipped for %s; keeping live inventory", NameOf(entity).c_str());
        const size_t removedBytes = personal::EncodedRowBytes(row->second);
        personalLedgerBytes = personalLedgerBytes > personal::HeaderBytes + removedBytes
            ? personalLedgerBytes - removedBytes
            : personal::HeaderBytes;
        personalLedger.erase(row);
        personalPending.erase(pending);
        ObservePersonal(entity);
        return true;
    }
    if (!ObservePersonal(entity))
        Logf("WARN containers: cannot observe restored inventory for %s", NameOf(entity).c_str());
    personalRestoreErrors.erase(key);
    restoredPersonal.insert(key);
    personalPending.erase(pending);
    return true;
}

bool RememberPersonal(Entity& entity)
{
    if (!EnsurePersonal(entity)) return false;
    if (!entity.GetInstance()) return false;
    const std::string key = EntityKey(entity.GetInstance());
    if (!ObservePersonal(entity)) return false;
    ObserverState* observer = currentPersonalObservers[key];
    if (observer->depth || !observer->changes.CanRefresh()) return false;
    personal::Inventory row;
    if (!personal::Id(key) || !ReadPersonal(entity, row)) {
        NoteCaptureError(observer, key, "WARN containers: skipping personal inventory capture for %s", entity);
        return false;
    }
    auto old = personalLedger.find(key);
    const size_t oldSize = old == personalLedger.end() ? 0 : personal::EncodedRowBytes(old->second);
    const size_t newSize = personal::EncodedRowBytes(row);
    if ((old == personalLedger.end() && personalLedger.size() >= personal::MaxInventoryRows) ||
        personalLedgerBytes - oldSize + newSize > personal::Limit) {
        NoteCaptureError(observer, key, "WARN containers: personal inventory ledger limit reached for %s", entity);
        return false;
    }
    personalLedgerBytes = personalLedgerBytes - oldSize + newSize;
    personalErrors.erase(key);
    personalUncertain.erase(key);
    personalLedger[key] = std::move(row);
    observer->changes.Refreshed();
    return true;
}

bool PendingAwaitingDeath(std::string const& key, Entity& entity)
{
    if (!personalPending.count(key)) return false;
    auto row = personalLedger.find(key);
    return row != personalLedger.end() && AwaitingDeath(entity, row->second);
}

bool TrackPersonalFocus()
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !PersonalAdmitted()) return true;
    Entity focus = Entity(hero).Focus.GetFocusEntity();
    personal::Kind kind;
    if (!PersonalKind(focus, kind)) return true;
    const std::string key = EntityKey(focus.GetInstance());
    if (PendingAwaitingDeath(key, focus)) return true;
    return RememberPersonal(focus);
}

bool PersonalPass(bool capture, std::set<std::string>* attempted)
{
    if (destroyedPersonalObservers > 0) ReclaimCompletedObservers();
    if (!capture && personalPending.empty()) return true;
    bool ok = true;
    auto const* every = world::ProcessedEntities();
    for (GEU16 i = 0; every && i < every->GetCount(); ++i) {
        eCEntity* instance = every->GetAt(i);
        if (!instance) continue;
        const std::string key = EntityKey(instance);
        Entity entity(instance);
        if (PersonalTeardown(entity)) continue;
        if (personalPending.count(key)) {
            if (PendingAwaitingDeath(key, entity)) continue;
            if (attempted) attempted->insert(key);
            if (!EnsurePersonal(entity)) { ok = false; continue; }
        }
        if (!capture || !PersonalAdmitted()) continue;
        personal::Kind kind;
        if (!PersonalKind(entity, kind)) continue;
        const bool generated = entity.Inventory.IsGeneratedPlunder() || entity.Inventory.IsGeneratedTrade();
        if (personalLedger.count(key) || generated) ok = RememberPersonal(entity) && ok;
    }
    return ok;
}

bool FlushPersonalObservers()
{
    std::vector<std::string> keys;
    keys.reserve(currentPersonalObservers.size());
    for (auto const& entry : currentPersonalObservers) keys.push_back(entry.first);
    bool allClean = true;
    for (auto const& key : keys) {
        auto it = currentPersonalObservers.find(key);
        if (it == currentPersonalObservers.end()) continue;
        ObserverState* observer = it->second;
        if (!observer->active || !observer->changes.dirty) continue;
        if (observer->depth) { allClean = false; continue; }
        Entity entity = ResolveKey(key);
        eCEntity* instance = entity.GetInstance();
        if (!observer->inventory || !instance ||
            InventoryOf(instance) != observer->inventory) {
            observer->active = false;
            observer->inventory = nullptr;
            continue;
        }
        if (!RememberPersonal(entity)) allClean = false;
    }
    ReclaimCompletedObservers();
    if (allClean) personalUncertain.clear();
    return allClean;
}

void Reset()
{
    RetirePersonalObservers();
    items::OnWorldChange();
    personalLedger.clear();
    personalLedgerBytes = personal::HeaderBytes;
    ClearTrackingState();
}

bool Capture(std::vector<uint8_t>& out, AccessGuardFn installAccessGuards)
{
    if (!PersonalAdmitted() || !installAccessGuards || !installAccessGuards()) {
        Log("FAIL containers: personal capture refused: admission or access guard unavailable");
        return false;
    }
    FlushPersonalObservers();
    TrackPersonalFocus();
    PersonalPass(true);
    std::vector<std::string> keys;
    keys.reserve(personalLedger.size());
    for (auto const& entry : personalLedger) keys.push_back(entry.first);
    for (auto const& key : keys) {
        auto current = personalLedger.find(key);
        if (current == personalLedger.end()) continue;
        Entity entity = ResolveKey(key);
        if (!entity.GetInstance() || PersonalTeardown(entity) || PendingAwaitingDeath(key, entity)) continue;
        if (EnsurePersonal(entity) && personalLedger.count(key)) RememberPersonal(entity);
    }
    personal::Snapshot combined;
    size_t encodedBytes = personal::HeaderBytes;
    size_t inventories = 0;
    for (auto const& entry : personalLedger) {
        if (!personal::Id(entry.first) || !personal::Valid(entry.second)) continue;
        const size_t rowBytes = personal::EncodedRowBytes(entry.second);
        if (inventories >= personal::MaxInventoryRows || combined.size() >= personal::MaxRows ||
            encodedBytes + rowBytes > personal::Limit) break;
        if (combined.emplace(entry).second) {
            ++inventories;
            encodedBytes += rowBytes;
        }
    }
    personal::Snapshot ground;
    items::AppendPersonal(ground);
    for (auto const& entry : ground) {
        if (!personal::Id(entry.first) || entry.second.kind != personal::Kind::GroundTaken ||
            !personal::Valid(entry.second) || combined.count(entry.first)) continue;
        if (combined.size() >= personal::MaxRows || encodedBytes + personal::RowHeaderBytes > personal::Limit) break;
        if (combined.emplace(entry).second) encodedBytes += personal::RowHeaderBytes;
    }
    if (!personal::Encode(combined, out)) {
        Log("FAIL containers: personal snapshot encoding failed");
        return false;
    }
    return true;
}

bool GuardsAvailable(AccessGuardFn installAccessGuards) { return installAccessGuards && installAccessGuards(); }

bool Restore(std::vector<uint8_t> const& bytes, AccessGuardFn installAccessGuards)
{
    personal::Snapshot snapshot;
    if (!bytes.empty() && !personal::Decode(bytes, snapshot)) {
        Log("FAIL containers: invalid personal inventory snapshot");
        return false;
    }
    const bool hasInventories = std::any_of(snapshot.begin(), snapshot.end(), [](auto const& entry) {
        return entry.second.kind != personal::Kind::GroundTaken;
    });
    if (hasInventories && !GuardsAvailable(installAccessGuards)) {
        Log("FAIL containers: personal restore refused: required access guards unavailable");
        return false;
    }
    if (!items::RestorePersonal(snapshot)) {
        Log("FAIL containers: personal pickup restore failed");
        return false;
    }
    items::TickPersonal();
    for (auto it = snapshot.begin(); it != snapshot.end();) {
        if (it->second.kind == personal::Kind::GroundTaken) it = snapshot.erase(it);
        else ++it;
    }
    RetirePersonalObservers();
    personalLedger.swap(snapshot);
    personalLedgerBytes = personal::HeaderBytes;
    for (auto const& entry : personalLedger) personalLedgerBytes += personal::EncodedRowBytes(entry.second);
    ClearTrackingState();
    for (auto const& entry : personalLedger) personalPending.insert(entry.first);
    if (personalLedger.empty()) return true;
    std::set<std::string> attempted;
    PersonalPass(false, &attempted);
    for (auto it = personalPending.begin(); it != personalPending.end();) {
        const std::string key = *it++;
        if (attempted.count(key)) continue;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entity.GetInstance() || PersonalTeardown(entity)) continue;
        auto row = personalLedger.find(key);
        if (row != personalLedger.end() && AwaitingDeath(entity, row->second)) continue;
        EnsurePersonal(entity);
    }
    return true;
}

bool PersonalMutation::Enter(gCInventory_PS* inventory)
{
    if (!inventory) return true;
    for (auto* observer : held_) if (observer->inventory == inventory) return true;
    Entity entity(inventory->GetEntity());
    if (!entity.GetInstance()) return true;
    const std::string key = EntityKey(entity.GetInstance());
    if (personalPending.count(key) != 0 && !EnsurePersonal(entity)) return false;
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return true;
    if (!PersonalAdmitted()) return false;
    personalUncertain.erase(key);
    auto found = currentPersonalObservers.find(key);
    ObserverState* observer = found == currentPersonalObservers.end() ? nullptr : found->second;
    if (observer && observer->depth && (observer->changes.destroyed || observer->inventory != inventory))
        return false;
    if (!observer || observer->inventory != inventory || !observer->depth) {
        RememberPersonal(entity);
        if (!ObservePersonal(entity)) return false;
        observer = currentPersonalObservers[key];
    }
    ++observer->depth;
    held_.push_back(observer);
    return true;
}

bool PersonalMutation::StillAvailable(gCInventory_PS* inventory) const
{
    for (auto* observer : held_) {
        if (observer->inventory != inventory) continue;
        Entity entity = ResolveKey(observer->key);
        return observer->active && !observer->changes.destroyed && entity.GetInstance() &&
               entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory) == inventory;
    }
    return false;
}

void PersonalMutation::Finish()
{
    for (ObserverState* observer : held_) {
        if (!observer->depth || --observer->depth) continue;
        auto found = currentPersonalObservers.find(observer->key);
        const bool orphaned = observer->generation != observerGeneration ||
                              found == currentPersonalObservers.end() ||
                              found->second != observer;
        if (orphaned) {
            if (!observer->active && !observer->inventory) {
                observer->generation = 0;
                reusablePersonalObservers.push_back(observer);
            }
            continue;
        }
        Entity entity = ResolveKey(observer->key);
        eCEntity* instance = entity.GetInstance();
        if (!observer->active || !observer->inventory || !instance ||
            InventoryOf(instance) != observer->inventory) {
            personalUncertain.insert(observer->key);
            Log("WARN containers: personal mutation completed after inventory unload; keeping last committed snapshot");
            continue;
        }
        if (!RememberPersonal(entity))
            Log("WARN containers: personal inventory mutation skipped uncapturable state");
    }
    held_.clear();
}

} // namespace g3t::containers::ledger
