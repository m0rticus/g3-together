// SPDX-License-Identifier: GPL-3.0-or-later
#include "personal_ledger.h"

#include "ai.h"
#include "guid.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "remote.h"
#include "world.h"

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

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

std::string TemplateKey(Entity& entity, int stack)
{
    const Template item = entity.Inventory.GetTemplate(stack);
    bCPropertyID const& id = item;
    return std::string(reinterpret_cast<char const*>(&id.GetCore()), 16);
}

bool Counted(Entity& entity, int stack)
{
    if (entity.Inventory.GetSlotFromStack(stack) != gESlot_None) return false;
    auto* inventory = entity.GetInstance()
        ? static_cast<gCInventory_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory))
        : nullptr;
    if (!inventory || !inventory->GetStack(stack) || entity.Inventory.GetAmount(stack) <= 0) return false;
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
    if (!Counted(entity, index)) return false;
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) {
        const gEItemCategory category = entity.Inventory.GetCategory(index);
        auto const* inventory = static_cast<gCInventory_PS*>(
            entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        personal::Kind kind = personal::Kind::Corpse;
        PersonalKind(entity, kind);
        if ((category == gEItemCategory_Skill || category == gEItemCategory_Spellbook) &&
            (!inventory || !StackMerchandise(entity, *inventory, index, kind))) return false;
    }
    return true;
}

void RetirePersonalObservers()
{
    for (auto const& entry : currentPersonalObservers) {
        ObserverState* observer = entry.second;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(entry.first.data()));
        eCEntity* instance = entity.GetInstance();
        if (observer->inventory && instance &&
            instance->GetPropertySet(eEPropertySetType_Inventory) == observer->inventory) {
            observer->inventory->RemoveListener(observer);
            observer->active = false;
            observer->inventory = nullptr;
            reusablePersonalObservers.push_back(observer);
        } else {
            observer->active = false;
        }
    }
    currentPersonalObservers.clear();
}

bool ObservePersonal(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!instance) return false;
    const std::string key = EntityKey(instance);
    auto* inventory = static_cast<gCInventory_PS*>(instance->GetPropertySet(eEPropertySetType_Inventory));
    if (!inventory) return false;
    auto existing = currentPersonalObservers.find(key);
    if (existing != currentPersonalObservers.end()) {
        ObserverState* observer = existing->second;
        if (observer->inventory == inventory && observer->changes.CanRefresh()) return true;
        observer->active = false;
        currentPersonalObservers.erase(existing);
    }
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
                if (!PersonalCounted(entity, i) && entity.Inventory.GetSlotFromStack(i) == gESlot_None &&
                    TemplateKey(entity, i) == stack.item)
                    return false;
            }
        }
        templates.push_back(item);
    }
    return true;
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

void RollbackPersonal(Entity& entity, gCInventory_PS& inventory,
                      personal::Inventory const& before,
                      std::vector<eCTemplateEntity*> const& beforeTemplates)
{
    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i)
        if (PersonalCounted(entity, i)) entity.Inventory.DeleteStack(i);
    for (size_t i = 0; i < before.stacks.size() && i < beforeTemplates.size(); ++i) {
        auto const& stack = before.stacks[i];
        const gEStackType type = stack.type == personal::Stock::Merchandise ? gEStackType_Merchandise : gEStackType_Normal;
        inventory.CreateItems(beforeTemplates[i], stack.quality, stack.amount, type);
    }
    ApplyPersonalMetadata(entity, inventory, before.flags);
}

bool ReadPersonal(Entity& entity, personal::Inventory& row)
{
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return false;
    auto* inventory = static_cast<gCInventory_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
    if (!inventory) return false;
    personal::Inventory result;
    result.kind = kind;
    if (entity.Inventory.IsGeneratedPlunder()) result.flags |= personal::GeneratedPlunder;
    if (entity.Inventory.IsGeneratedTrade()) result.flags |= personal::GeneratedTrade;
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_Door)) {
        result.flags |= personal::HasDoor;
        if (entity.Door.IsOpen()) result.flags |= personal::DoorOpen;
    }
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_Lock)) {
        result.flags |= personal::HasLock;
        if (entity.Lock.IsLocked()) result.flags |= personal::Locked;
    }
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        if (!PersonalCounted(entity, i)) continue;
        const int amount = entity.Inventory.GetAmount(i);
        if (amount <= 0) continue;
        const uint32_t clamped = amount > static_cast<int>(personal::MaxAmount)
            ? personal::MaxAmount
            : static_cast<uint32_t>(amount);
        const personal::Stock stock = StackMerchandise(entity, *inventory, i, kind)
            ? personal::Stock::Merchandise
            : personal::Stock::Normal;
        result.stacks.push_back({TemplateKey(entity, i), clamped,
                                 personal::MaskQuality(entity.Inventory.GetQuality(i)), stock});
    }
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
    auto* inventory = static_cast<gCInventory_PS*>(instance->GetPropertySet(eEPropertySetType_Inventory));
    if (!inventory ||
        ((row.flags & personal::HasDoor) != 0) != (instance->GetPropertySet(eEPropertySetType_Door) != nullptr) ||
        ((row.flags & personal::HasLock) != 0) != (instance->GetPropertySet(eEPropertySetType_Lock) != nullptr)) return false;

    std::vector<eCTemplateEntity*> templates;
    if (!ResolveStackTemplates(entity, instance, row.stacks, templates)) return false;

    personal::Inventory before;
    std::vector<eCTemplateEntity*> beforeTemplates;
    if (!ReadPersonal(entity, before) || !ResolveStackTemplates(entity, instance, before.stacks, beforeTemplates))
        return false;

    auto rollbackAndFail = [&]() {
        RollbackPersonal(entity, *inventory, before, beforeTemplates);
        return false;
    };

    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i)
        if (PersonalCounted(entity, i) && !entity.Inventory.DeleteStack(i)) return rollbackAndFail();
    for (size_t i = 0; i < row.stacks.size(); ++i) {
        auto const& stack = row.stacks[i];
        const gEStackType type = stack.type == personal::Stock::Merchandise ? gEStackType_Merchandise : gEStackType_Normal;
        if (inventory->CreateItems(templates[i], stack.quality, stack.amount, type) < 0) return rollbackAndFail();
    }
    ApplyPersonalMetadata(entity, *inventory, row.flags);

    personal::Inventory after;
    if (!ReadPersonal(entity, after)) return rollbackAndFail();
    if (after.kind != kind || row.flags != after.flags) return rollbackAndFail();
    using StackKey = std::tuple<std::string, uint32_t, personal::Stock>;
    std::map<StackKey, int64_t> totals;
    for (auto const& stack : row.stacks) totals[{stack.item, stack.quality, stack.type}] += stack.amount;
    for (auto const& stack : after.stacks) totals[{stack.item, stack.quality, stack.type}] -= stack.amount;
    for (auto const& entry : totals) if (entry.second != 0) return rollbackAndFail();
    return true;
}

bool AwaitingDeath(Entity& entity, personal::Inventory const& row)
{
    return row.kind == personal::Kind::Corpse && entity.GetInstance() &&
           entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) && !entity.IsDead();
}
} // namespace

void ObserverState::OnDestroyInventory()
{
    if (!active || ai::ShuttingDown()) return;
    changes.Destroyed();
    retiredInventory = inventory;
    inventory = nullptr;
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
    return !instance->GetPropertySet(eEPropertySetType_NPC) || entity.IsDead();
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
    if (!HasInventory(instance) || !instance->GetPropertySet(eEPropertySetType_NPC) || entity.IsDead() ||
        instance == gCSession::GetInstance().GetHero())
        return false;
    Entity const sets[5] = {entity.Inventory.GetTreasureSet1(), entity.Inventory.GetTreasureSet2(),
                            entity.Inventory.GetTreasureSet3(), entity.Inventory.GetTreasureSet4(),
                            entity.Inventory.GetTreasureSet5()};
    for (Entity set : sets) {
        if (!set.GetInstance()) continue;
        const gETreasureDistribution kind = set.TreasureSet.TreasureDistribution;
        if (kind == gETreasureDistribution_Trade_Generate || kind == gETreasureDistribution_Trade_Refresh) return true;
    }
    return false;
}

bool Shared(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return HasInventory(instance) && !entity.IsItem() && instance != gCSession::GetInstance().GetHero() &&
           !remote::IsPuppet(instance);
}

bool Corpse(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return instance && instance->GetPropertySet(eEPropertySetType_NPC) && entity.IsDead();
}

bool PersonalKind(Entity& entity, personal::Kind& kind)
{
    if (!Shared(entity)) return false;
    if (Corpse(entity)) kind = personal::Kind::Corpse;
    else if (IsTrader(entity)) kind = personal::Kind::Trader;
    else if (IsChest(entity)) kind = personal::Kind::Chest;
    else return false;
    return true;
}

std::string EntityKey(eCEntity* entity)
{
    uint8_t raw[16];
    guid::CopyEntity(entity, raw);
    return Key(raw);
}

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
    eCEntity* partner = manager && infos && manager->GetActivePage() == gEGamePage_Trade ? infos->GetDialogPartnerNPC() : nullptr;
    const std::string active = partner ? EntityKey(partner) : std::string();
    for (auto it = openedPersonalTrade.begin(); it != openedPersonalTrade.end();) {
        if (*it == active) { seenPersonalTradePage.insert(*it); ++it; continue; }
        if (!seenPersonalTradePage.count(*it)) { ++it; continue; }
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(it->data()));
        if (!entity.GetInstance() || !RememberPersonal(entity)) { ++it; continue; }
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
        observer->changes.Refreshed();
        if (!personalErrors.count(key)) {
            if (personalErrors.size() < personal::MaxInventoryRows) {
                personalErrors.insert(key);
                Logf("WARN containers: skipping personal inventory capture for %s", NameOf(entity).c_str());
            } else personalErrorOverflow = true;
        }
        return false;
    }
    auto old = personalLedger.find(key);
    const size_t oldSize = old == personalLedger.end() ? 0 : personal::EncodedRowBytes(old->second);
    const size_t newSize = personal::EncodedRowBytes(row);
    if ((old == personalLedger.end() && personalLedger.size() >= personal::MaxInventoryRows) ||
        personalLedgerBytes - oldSize + newSize > personal::Limit) {
        observer->changes.Refreshed();
        if (!personalErrors.count(key)) {
            if (personalErrors.size() < personal::MaxInventoryRows) {
                personalErrors.insert(key);
                Logf("WARN containers: personal inventory ledger limit reached for %s", NameOf(entity).c_str());
            } else personalErrorOverflow = true;
        }
        return false;
    }
    personalLedgerBytes = personalLedgerBytes - oldSize + newSize;
    personalErrors.erase(key);
    personalUncertain.erase(key);
    personalLedger[key] = std::move(row);
    observer->changes.Refreshed();
    return true;
}

bool TrackPersonalFocus()
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !PersonalAdmitted()) return true;
    Entity focus = Entity(hero).Focus.GetFocusEntity();
    personal::Kind kind;
    if (!PersonalKind(focus, kind)) return true;
    const std::string key = EntityKey(focus.GetInstance());
    auto pending = personalLedger.find(key);
    if (personalPending.count(key) && pending != personalLedger.end() && AwaitingDeath(focus, pending->second)) return true;
    return RememberPersonal(focus);
}

bool PersonalPass(bool capture, std::set<std::string>* attempted)
{
    if (!capture && personalPending.empty()) return true;
    bool ok = true;
    auto const* every = world::ProcessedEntities();
    for (GEU16 i = 0; every && i < every->GetCount(); ++i) {
        eCEntity* instance = every->GetAt(i);
        if (!instance) continue;
        const std::string key = EntityKey(instance);
        auto pending = personalPending.find(key);
        Entity entity(instance);
        if (PersonalTeardown(entity)) continue;
        if (pending != personalPending.end()) {
            auto row = personalLedger.find(key);
            if (row != personalLedger.end() && AwaitingDeath(entity, row->second)) continue;
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
    for (auto const& entry : currentPersonalObservers) {
        ObserverState* observer = entry.second;
        if (!observer->active || !observer->changes.dirty) continue;
        if (observer->depth) return false;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(entry.first.data()));
        eCEntity* instance = entity.GetInstance();
        if (!observer->inventory || !instance ||
            instance->GetPropertySet(eEPropertySetType_Inventory) != observer->inventory) {
            observer->active = false;
            observer->inventory = nullptr;
            continue;
        }
        RememberPersonal(entity);
    }
    personalUncertain.clear();
    return true;
}

void Reset()
{
    RetirePersonalObservers();
    items::OnWorldChange();
    personalLedger.clear();
    personalLedgerBytes = personal::HeaderBytes;
    personalPending.clear();
    personalErrors.clear();
    personalRestoreErrors.clear();
    personalUncertain.clear();
    personalErrorOverflow = false;
    restoredPersonal.clear();
    openedPersonalTrade.clear();
    seenPersonalTradePage.clear();
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
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entity.GetInstance() || PersonalTeardown(entity)) continue;
        if (personalPending.count(key) && AwaitingDeath(entity, current->second)) continue;
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

bool Restore(std::vector<uint8_t> const& bytes, AccessGuardFn installAccessGuards)
{
    personal::Snapshot snapshot;
    if (!bytes.empty() && !personal::Decode(bytes, snapshot)) {
        Log("FAIL containers: invalid personal inventory snapshot");
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
    personalPending.clear();
    personalErrors.clear();
    personalRestoreErrors.clear();
    personalUncertain.clear();
    personalErrorOverflow = false;
    restoredPersonal.clear();
    openedPersonalTrade.clear();
    seenPersonalTradePage.clear();
    for (auto const& entry : personalLedger) personalPending.insert(entry.first);
    if (personalLedger.empty()) return true;
    if (!installAccessGuards || !installAccessGuards()) {
        Log("FAIL containers: personal restore refused: required access guards unavailable");
        return false;
    }
    std::set<std::string> attempted;
    PersonalPass(false, &attempted);
    for (auto it = personalPending.begin(); it != personalPending.end();) {
        const std::string key = *it++;
        if (attempted.count(key)) continue;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entity.GetInstance()) continue;
        if (PersonalTeardown(entity)) continue;
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
    const bool wasPending = personalPending.count(key) != 0;
    if (wasPending && !EnsurePersonal(entity)) return false;
    if (wasPending) return false;
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return true;
    if (!PersonalAdmitted()) return false;
    personalUncertain.erase(key);
    auto found = currentPersonalObservers.find(key);
    ObserverState* observer = found == currentPersonalObservers.end() ? nullptr : found->second;
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
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(observer->key.data()));
        return observer->active && !observer->changes.destroyed && entity.GetInstance() &&
               entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory) == inventory;
    }
    return false;
}

void PersonalMutation::Finish()
{
    for (ObserverState* observer : held_) {
        if (!observer->depth || --observer->depth) continue;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(observer->key.data()));
        eCEntity* instance = entity.GetInstance();
        if (!observer->active || !observer->inventory || !instance ||
            instance->GetPropertySet(eEPropertySetType_Inventory) != observer->inventory) {
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
