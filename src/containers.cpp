// SPDX-License-Identifier: GPL-3.0-or-later
#include "containers.h"

#include "body.h"
#include "guid.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "personal_store.h"
#include "remote.h"
#include "trade.h"
#include "ai.h"
#include "downed.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace g3t::containers {
namespace {
using Content = std::map<std::string, int>;  // item template ID (16 bytes) -> amount, equipped stacks left out
std::unordered_map<std::string, std::string> names;  // template ID -> item name, for logs and QA
constexpr float InterestRadius = 4000.0f;
constexpr float PreRollRadius = 1500.0f;  // a client this close to an unrolled chest: the host rolls it

bool disabled = false;
bool traderFillOff = false;  // QA negative control: the host does not fill traders before a dialog
bool forgetOff = false;  // QA negative control: container state survives a world change (as before R5)
bool handsOff = false;  // QA negative control: a corpse's hand keeps a held stack of its contents (as before 23.10.)
bool interactLoot = true;  // QA negative control (containersyncoff interact): only chests count as opened by use (before 24.10.)
unsigned contentsSent = 0, contentsReceived = 0, rewrites = 0, txSent = 0, txApplied = 0, txRefused = 0, txMismatch = 0, txDenied = 0,
         reverted = 0, handsReleased = 0;
unsigned ignoredSharedMessages = 0;
DWORD lastLocalPoll = 0;
namespace personal = saving::personal;
personal::Snapshot personalLedger;
size_t personalLedgerBytes = 12;
std::set<std::string> personalPending;
std::set<std::string> personalErrors;
std::set<std::string> personalRestoreErrors;
std::set<std::string> personalUncertain;
bool personalErrorOverflow = false;
std::set<std::string> restoredPersonal, openedPersonalTrade, seenPersonalTradePage;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

bool HasInventory(eCEntity* entity) { return entity && entity->GetPropertySet(eEPropertySetType_Inventory); }

// Chests and corpses: an inventory, not an item, not a living NPC.
bool IsContainer(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!HasInventory(instance) || entity.IsItem() || instance == gCSession::GetInstance().GetHero()) return false;
    return !instance->GetPropertySet(eEPropertySetType_NPC) || entity.IsDead();
}

// A chest: an inventory the game rolls from treasure sets when a player first opens it (PS_Interact_Chest), on
// every machine by itself unless told it already did.
bool IsChest(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return HasInventory(instance) && !entity.IsItem() && !instance->GetPropertySet(eEPropertySetType_NPC) &&
           instance != gCSession::GetInstance().GetHero();
}

std::string TemplateKey(Entity& entity, int stack)
{
    const Template item = entity.Inventory.GetTemplate(stack);
    bCPropertyID const& id = item;
    return std::string(reinterpret_cast<char const*>(&id.GetCore()), 16);
}

Template TemplateOf(std::string const& key)
{
    _GUID raw;
    std::memcpy(&raw, key.data(), 16);
    return Template(guid::Property(raw));
}

char const* NameOf(std::string const& key)
{
    auto it = names.find(key);
    return it == names.end() ? "?" : it->second.c_str();
}

// A trader: a living NPC with a treasure set the game fills its trade inventory from (in ZS_Talk, on the machine where
// the dialog runs; C container-populate-c.md).
bool IsTrader(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!HasInventory(instance) || !instance->GetPropertySet(eEPropertySetType_NPC) || entity.IsDead() ||
        instance == gCSession::GetInstance().GetHero())
        return false;
    Entity const sets[5] = {entity.Inventory.GetTreasureSet1(), entity.Inventory.GetTreasureSet2(), entity.Inventory.GetTreasureSet3(),
                            entity.Inventory.GetTreasureSet4(), entity.Inventory.GetTreasureSet5()};
    for (Entity set : sets) {
        if (!set.GetInstance()) continue;
        const gETreasureDistribution kind = set.TreasureSet.TreasureDistribution;
        if (kind == gETreasureDistribution_Trade_Generate || kind == gETreasureDistribution_Trade_Refresh) return true;
    }
    return false;
}

// World inventories eligible for personal loot/stock. Never the hero, a puppet or a loose item.
// The old shared-inventory routines below are not registered or ticked by the personal inventory runtime.
bool Shared(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return HasInventory(instance) && !entity.IsItem() && instance != gCSession::GetInstance().GetHero() && !remote::IsPuppet(instance);
}

bool Corpse(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    return instance && instance->GetPropertySet(eEPropertySetType_NPC) && entity.IsDead();
}

// A corpse's plunder (an animal's fur and teeth) is rolled once, by the host, like a chest's: the client rolled its own at
// every opening, the host's copy stayed empty and nothing was reported, so a wolf could be plundered for ever (playtest
// 27.10.). false: as before 27.10.
bool corpsePlunder = true;
bool Rolled(Entity& entity) { return IsChest(entity) || (corpsePlunder && Corpse(entity)); }

// A stack that is part of the contents: not in a slot (equipped or held).
bool Counted(Entity& entity, int stack)
{
    if (entity.Inventory.GetSlotFromStack(stack) != gESlot_None) return false;
    Entity item = entity.Inventory.GetTemplateItem(stack);
    // "Fist" is the bare-hand weapon an unarmed NPC dies holding (host only, never plunder): counted, a client
    // that cannot have it drifted every 2 s and a quick loot sent "Fist:-1" every poll (stageQuickLoot 22.10.).
    return item.GetInstance() && std::strcmp(item.GetName().GetText(), "Fist") != 0;
}

// Take `amount` of the item `key` out of the stacks that count as contents (the same ones Read counts), last stack
// first; returns how many went. FindStackIndex(template) did not find a scroll that QA stocked into a corpse: nothing
// was deleted, the host still reported the transaction as applied and the item existed twice (stageU1, 22.10.).
int Remove(Entity& entity, std::string const& key, int amount, bool equippedToo = false)
{
    int removed = 0;
    // A hero's undo may also take a stack the player equipped meanwhile (Codex review R3, 22.10.); unequipped ones go first.
    for (int pass = 0; pass < (equippedToo ? 2 : 1); ++pass)
    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0 && removed < amount; --i) {
        if ((pass == 0 ? !Counted(entity, i) : entity.Inventory.GetSlotFromStack(i) == gESlot_None) || TemplateKey(entity, i) != key) continue;
        const int here = entity.Inventory.GetAmount(i);
        const int take = std::min<int>(amount - removed, here);
        if (take <= 0) continue;
        // A whole stack goes with DeleteStack: DeleteItems left a stocked scroll where it was (stageU1, 22.10.).
        if (take == here ? entity.Inventory.DeleteStack(i) : entity.Inventory.DeleteItems(i, take)) removed += take;
    }
    return removed;
}

Content Read(Entity& entity)
{
    Content content;
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        if (!Counted(entity, i)) continue;
        Entity item = entity.Inventory.GetTemplateItem(i);
        const std::string key = TemplateKey(entity, i);
        names[key] = item.GetName().GetText();
        content[key] += entity.Inventory.GetAmount(i);
    }
    return content;
}

bool merchandise = true;  // a trader's goods are written as merchandise stacks (false: Normal stacks, as before 27.10.)

// A trader's goods as merchandise, as the game fills them: PSInventory::CreateItems makes Normal stacks, which the trade
// screen does not list for a trader (empty for the client), and which lay as plunder on a knocked-out one (Joey,
// playtest 27.10.; the same as for puppets, trade.cpp CreateMerchandise). Everything else as Normal stacks.
void CreateGoods(Entity& entity, std::string const& key, int amount)
{
    if (amount <= 0) return;
    if (merchandise && IsTrader(entity)) {
        auto* inventory = static_cast<gCInventory_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        _GUID raw;
        std::memcpy(&raw, key.data(), 16);
        eCTemplateEntityProxy item;
        item.SetEntity(guid::Property(raw));
        if (eCTemplateEntity* templ = item.GetEntity(); inventory && templ) {
            inventory->CreateItems(templ, 0, static_cast<GEU32>(amount), gEStackType_Merchandise);
            return;
        }
    }
    entity.Inventory.CreateItems(TemplateOf(key), 0, amount);
}

void Write(Entity& entity, Content const& content)
{
    // On the host a dying NPC puts what it held into its inventory (a slave's rice), while its mirror here still holds
    // it: new items of that kind would join the held stack and never count (Slave_08's It_FakeRice drifted every 2 s,
    // stageQuickLoot 22.10.). On a corpse such a held stack goes too, once its hand let go of it (below). A weapon the
    // host's NPC dropped is not in the contents and stays in the mirror's hand.
    const bool corpse = Corpse(entity);
    // DeleteStack leaves a stack held in a hand where it is, and CreateItems then adds to it: the client's dead slave held
    // one more bread every 2 s and its copy drifted for good (fakebreadlivediag 23.10.; It_FakeBread, It_FakeCup_Clay).
    // So the hand lets go of it first; the other hand keeps what it holds.
    if (corpse && !handsOff) {
        GEInt hold[2] = {-1, -1};  // left, right
        bool release = false;
        for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
            const gESlot slot = entity.Inventory.GetSlotFromStack(i);
            if (slot != gESlot_LeftHand && slot != gESlot_RightHand) continue;
            const bool inContents = content.count(TemplateKey(entity, i)) != 0;
            release = release || inContents;
            hold[slot == gESlot_LeftHand ? 0 : 1] = inContents ? -1 : i;
        }
        if (release && entity.Inventory.HoldStacks(hold[0], hold[1])) ++handsReleased;
    }
    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i)
        if (entity.Inventory.GetSlotFromStack(i) == gESlot_None || (corpse && content.count(TemplateKey(entity, i))))
            entity.Inventory.DeleteStack(i);
    for (auto const& [key, amount] : content) CreateGoods(entity, key, amount);
    ++rewrites;
}

std::vector<net::ContentEntry> Entries(Content const& content)
{
    std::vector<net::ContentEntry> out;
    for (auto const& [key, amount] : content) {
        net::ContentEntry entry{{}, {}, amount};
        std::memcpy(entry.id, key.data(), 16);
        strncpy_s(entry.name, NameOf(key), _TRUNCATE);
        out.push_back(entry);
    }
    return out;
}

std::string KeyOf(net::ContentEntry const& entry) { return std::string(reinterpret_cast<char const*>(entry.id), 16); }

Content FromEntries(net::ContentEntry const* entries, int count)
{
    Content content;
    for (int i = 0; i < count; ++i) {
        const std::string key = KeyOf(entries[i]);
        names.emplace(key, entries[i].name);
        content[key] += entries[i].amount;
    }
    return content;
}

std::string Describe(Content const& content)
{
    std::map<std::string, int> byName;
    for (auto const& [key, amount] : content) byName[NameOf(key)] += amount;
    std::string out;
    for (auto const& [name, amount] : byName) out += (out.empty() ? "" : ",") + name + ":" + std::to_string(amount);
    return out.empty() ? "-" : out;
}

// Log lines about containers (PLAN-2026-10-22 1.6: the logs of the first play evening showed nothing about chests,
// corpses and traders). Capped, since a host passes many corpses and chests.
unsigned notes = 0;
constexpr unsigned MaxNotes = 600;

void Note(char const* format, ...)
{
    if (notes > MaxNotes) return;
    if (notes++ == MaxNotes) { Log("containers: log limit reached; no more container lines this session"); return; }
    char line[700];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);  // as Logf: never end the process on a long line
    va_end(args);
    Log(line);
}

std::string NameOf(Entity& entity) { return entity.GetInstance() ? entity.GetName().GetText() : "?"; }

float DistanceTo(eCEntity* entity, float const at[3])
{
    const bCVector& p = entity->GetWorldPosition();
    const float q[3] = {p.GetX(), p.GetY(), p.GetZ()};
    return body::Distance(q, at);
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

// Listener notifications can arrive during an operation or teardown, not necessarily after it.
// They only mark state dirty; snapshots are committed at known completion boundaries.
struct PersonalObserver final : gCInventoryListener {
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
    void OnDestroyInventory() override
    {
        if (!active || ai::ShuttingDown()) return;
        changes.Destroyed();
        retiredInventory = inventory;
        inventory = nullptr;
        if (changes.uncertain || depth) {
            personalUncertain.insert(key);
            Log("FAIL containers: personal inventory unloaded with uncommitted changes; saving refused");
        }
        if (personalLedger.count(key)) personalPending.insert(key);
    }
};

constexpr size_t MaxPersonalObservers = 65536;
std::vector<std::unique_ptr<PersonalObserver>> personalObservers;
std::vector<PersonalObserver*> reusablePersonalObservers;
std::map<std::string, PersonalObserver*> currentPersonalObservers;

void RetirePersonalObservers()
{
    for (auto const& entry : currentPersonalObservers) {
        PersonalObserver* observer = entry.second;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(entry.first.data()));
        eCEntity* instance = entity.GetInstance();
        if (observer->inventory && instance &&
            instance->GetPropertySet(eEPropertySetType_Inventory) == observer->inventory) {
            observer->inventory->RemoveListener(observer);
            observer->active = false;
            observer->inventory = nullptr;
            reusablePersonalObservers.push_back(observer);
        } else {
            // A destroyed or unresolvable inventory may still be unwinding its callbacks.
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
    if (!inventory || personalUncertain.count(key)) return false;
    auto existing = currentPersonalObservers.find(key);
    if (existing != currentPersonalObservers.end()) {
        PersonalObserver* observer = existing->second;
        if (observer->inventory == inventory && observer->changes.CanRefresh()) return true;
        if (observer->changes.dirty && !observer->changes.destroyed) {
            personalUncertain.insert(key);
            return false;
        }
        observer->active = false;
        currentPersonalObservers.erase(existing);
    }
    PersonalObserver* observer = nullptr;
    if (!reusablePersonalObservers.empty()) {
        observer = reusablePersonalObservers.back();
        reusablePersonalObservers.pop_back();
    } else {
        if (personalObservers.size() >= MaxPersonalObservers) {
            personalErrorOverflow = true;
            Log("FAIL containers: personal inventory observer limit reached");
            return false;
        }
        personalObservers.push_back(std::make_unique<PersonalObserver>());
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

bool PersonalTeardown(Entity& entity)
{
    if (!entity.GetInstance()) return false;
    auto observer = currentPersonalObservers.find(EntityKey(entity.GetInstance()));
    return observer != currentPersonalObservers.end() && observer->second->changes.destroyed &&
           observer->second->owner == entity.GetInstance() &&
           entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory) == observer->second->retiredInventory;
}

bool PersonalCounted(Entity& entity, int index)
{
    if (!Counted(entity, index)) return false;
    if (entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) {
        const gEItemCategory category = entity.Inventory.GetCategory(index);
        if ((category == gEItemCategory_Skill || category == gEItemCategory_Spellbook) &&
            !entity.Inventory.IsStackMerchandise(index)) return false;
    }
    return true;
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
        if (entity.Inventory.GetSlotFromStack(i) != gESlot_None) continue;
        Entity item = entity.Inventory.GetTemplateItem(i);
        auto* stack = inventory->GetStack(i);
        if (!item.GetInstance() || !stack) return false;
        if (!std::strcmp(item.GetName().GetText(), "Fist")) continue;
        if (!PersonalCounted(entity, i)) continue;
        if (!item.GetInstance()->GetPropertySet(eEPropertySetType_Item)) return false;
        const gEStackType type = stack->GetType();
        if (type != gEStackType_Normal && type != gEStackType_Treasure && type != gEStackType_Merchandise)
            return false;
        const int amount = entity.Inventory.GetAmount(i);
        if (amount <= 0) return false;
        if (result.stacks.size() == personal::MaxStacks) return false;
        result.stacks.push_back({TemplateKey(entity, i), static_cast<uint32_t>(amount),
                                 entity.Inventory.GetQuality(i),
                                 type == gEStackType_Merchandise ? personal::Stock::Merchandise : personal::Stock::Normal});
    }
    if (!personal::Valid(result)) return false;
    row = std::move(result);
    return true;
}

bool ApplyPersonal(Entity& entity, personal::Inventory const& row)
{
    personal::Kind kind;
    if (!PersonalKind(entity, kind) || kind != row.kind) return false;
    eCEntity* instance = entity.GetInstance();
    auto* inventory = static_cast<gCInventory_PS*>(instance->GetPropertySet(eEPropertySetType_Inventory));
    if (!inventory ||
        ((row.flags & personal::HasDoor) != 0) != (instance->GetPropertySet(eEPropertySetType_Door) != nullptr) ||
        ((row.flags & personal::HasLock) != 0) != (instance->GetPropertySet(eEPropertySetType_Lock) != nullptr)) return false;

    std::vector<eCTemplateEntity*> templates;
    for (auto const& stack : row.stacks) {
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
        for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
            if (!PersonalCounted(entity, i) && TemplateKey(entity, i) == stack.item)
                return false;  // creation could merge personal loot into authoritative equipment
        }
        templates.push_back(item);
    }
    // Validate the existing inventory too, before removing anything.
    personal::Inventory before;
    if (!ReadPersonal(entity, before)) return false;
    for (int i = entity.Inventory.GetStackCount() - 1; i >= 0; --i)
        if (PersonalCounted(entity, i) && !entity.Inventory.DeleteStack(i)) return false;
    for (size_t i = 0; i < row.stacks.size(); ++i) {
        auto const& stack = row.stacks[i];
        const gEStackType type = stack.type == personal::Stock::Merchandise ? gEStackType_Merchandise : gEStackType_Normal;
        if (inventory->CreateItems(templates[i], stack.quality, stack.amount, type) < 0) return false;
    }
    inventory->AccessGeneratedPlunder() = (row.flags & personal::GeneratedPlunder) != 0;
    inventory->AccessGeneratedTrade() = (row.flags & personal::GeneratedTrade) != 0;
    if (row.flags & personal::HasLock)
        entity.Lock.Status = (row.flags & personal::Locked) ? gELockStatus_Locked : gELockStatus_Unlocked;
    if (row.flags & personal::HasDoor) {
        const bool open = entity.Door.IsOpen() != GEFalse;
        if ((row.flags & personal::DoorOpen) && !open) entity.Door.Open(GEFalse);
        else if (!(row.flags & personal::DoorOpen) && open) entity.Door.Close(GEFalse);
    }
    personal::Inventory after;
    if (!ReadPersonal(entity, after)) return false;
    if (row.kind != after.kind || row.flags != after.flags) return false;
    using StackKey = std::tuple<std::string, uint32_t, personal::Stock>;
    std::map<StackKey, uint64_t> totals;
    for (auto const& stack : row.stacks) totals[{stack.item, stack.quality, stack.type}] += stack.amount;
    for (auto const& stack : after.stacks) totals[{stack.item, stack.quality, stack.type}] -= stack.amount;
    for (auto const& entry : totals) if (entry.second != 0) return false;
    return true;
}

// A corpse record for an NPC that has not yet received its mirrored death is intentionally deferred.
bool AwaitingDeath(Entity& entity, personal::Inventory const& row)
{
    return row.kind == personal::Kind::Corpse && entity.GetInstance() &&
           entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) && !entity.IsDead();
}

bool EnsurePersonal(Entity& entity)
{
    eCEntity* instance = entity.GetInstance();
    if (!instance) return true;
    const std::string key = EntityKey(instance);
    if (personalUncertain.count(key)) return false;
    if (PersonalTeardown(entity)) return false;
    auto pending = personalPending.find(key);
    if (pending == personalPending.end()) return true;
    auto row = personalLedger.find(key);
    if (row == personalLedger.end()) return false;
    if (AwaitingDeath(entity, row->second)) return false;
    if (!ApplyPersonal(entity, row->second)) {
        if (personalRestoreErrors.insert(key).second)
            Logf("FAIL containers: personal restore failed for %s; access and save refused", NameOf(entity).c_str());
        return false;
    }
    if (!ObservePersonal(entity)) {
        Logf("FAIL containers: cannot observe restored inventory for %s", NameOf(entity).c_str());
        return false;
    }
    personalRestoreErrors.erase(key);
    restoredPersonal.insert(key);
    personalPending.erase(pending);
    return true;
}

bool RememberPersonal(Entity& entity)
{
    if (!EnsurePersonal(entity)) return false;
    const std::string key = EntityKey(entity.GetInstance());
    if (!ObservePersonal(entity)) return false;
    PersonalObserver* observer = currentPersonalObservers[key];
    if (observer->depth || !observer->changes.CanRefresh()) return false;
    personal::Inventory row;
    if (!personal::Id(key) || !ReadPersonal(entity, row)) {
        if (!personalErrors.count(key)) {
            if (personalErrors.size() < personal::MaxInventoryRows) {
                personalErrors.insert(key);
                Logf("FAIL containers: cannot capture personal inventory for %s", NameOf(entity).c_str());
            } else personalErrorOverflow = true;
        }
        return false;
    }
    auto old = personalLedger.find(key);
    const size_t oldSize = old == personalLedger.end() ? 0 : 20 + 25 * old->second.stacks.size();
    const size_t newSize = 20 + 25 * row.stacks.size();
    if ((old == personalLedger.end() && personalLedger.size() >= personal::MaxInventoryRows) ||
        personalLedgerBytes - oldSize + newSize > personal::Limit) {
        if (!personalErrors.count(key)) {
            if (personalErrors.size() < personal::MaxInventoryRows) {
                personalErrors.insert(key);
                Logf("FAIL containers: personal inventory ledger limit reached for %s", NameOf(entity).c_str());
            } else personalErrorOverflow = true;
        }
        return false;
    }
    personalLedgerBytes = personalLedgerBytes - oldSize + newSize;
    personalErrors.erase(key);
    personalLedger[key] = std::move(row);
    observer->changes.Refreshed();
    return true;
}

bool PersonalAdmitted()
{
    return !net::Connected() || net::IsHost() || net::InWorld(net::LocalId());
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

// Pending records are applied every frame, including before client admission. Ordinary tracking waits for admission.
bool PersonalPass(bool capture, std::set<std::string>* attempted = nullptr)
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
    if (!personalUncertain.empty()) return false;
    for (auto const& entry : currentPersonalObservers) {
        PersonalObserver* observer = entry.second;
        if (!observer->active || !observer->changes.dirty) continue;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(entry.first.data()));
        eCEntity* instance = entity.GetInstance();
        if (observer->depth) return false;
        if (!observer->inventory || !instance ||
            instance->GetPropertySet(eEPropertySetType_Inventory) != observer->inventory) {
            personalUncertain.insert(entry.first);
            Log("FAIL containers: dirty personal inventory is no longer available; saving refused");
            return false;
        }
        if (!RememberPersonal(entity)) return false;
    }
    return personalUncertain.empty();
}

class PersonalMutation {
public:
    bool Enter(gCInventory_PS* inventory)
    {
        if (!inventory) return true;
        for (auto* observer : held_) if (observer->inventory == inventory) return true;
        Entity entity(inventory->GetEntity());
        if (!entity.GetInstance()) return true;
        const std::string key = EntityKey(entity.GetInstance());
        const bool wasPending = personalPending.count(key) != 0;
        if (wasPending && !EnsurePersonal(entity)) return false;
        if (wasPending) return false;  // stack indices and trade selections came from the old inventory
        personal::Kind kind;
        if (!PersonalKind(entity, kind)) return true;
        if (!PersonalAdmitted() || personalUncertain.count(key)) return false;
        auto found = currentPersonalObservers.find(key);
        PersonalObserver* observer = found == currentPersonalObservers.end() ? nullptr : found->second;
        if (!observer || observer->inventory != inventory || !observer->depth) {
            if (!RememberPersonal(entity)) return false;
            observer = currentPersonalObservers[key];
        }
        ++observer->depth;
        held_.push_back(observer);
        return true;
    }

    bool StillAvailable(gCInventory_PS* inventory) const
    {
        for (auto* observer : held_) {
            if (observer->inventory != inventory) continue;
            Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(observer->key.data()));
            return observer->active && !observer->changes.destroyed && entity.GetInstance() &&
                   entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory) == inventory;
        }
        return false;
    }

    void Finish()
    {
        for (PersonalObserver* observer : held_) {
            if (!observer->depth || --observer->depth) continue;
            Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(observer->key.data()));
            eCEntity* instance = entity.GetInstance();
            if (!observer->active || !observer->inventory || !instance ||
                instance->GetPropertySet(eEPropertySetType_Inventory) != observer->inventory) {
                personalUncertain.insert(observer->key);
                Log("FAIL containers: personal mutation completed after inventory unload; saving refused");
                continue;
            }
            if (!RememberPersonal(entity))
                Log("FAIL containers: cannot cache completed personal inventory mutation");
        }
        held_.clear();
    }

    ~PersonalMutation() { Finish(); }
private:
    std::vector<PersonalObserver*> held_;
};

using PersonalGenerateFn = GEBool (__fastcall *)(gCInventory_PS*, void*);
using PersonalBeginTradeFn = GEBool (__fastcall *)(gCGUIManager*, void*, eCEntity*);
using PersonalTransferItemsFn = GEBool (__fastcall *)(gCInventory_PS*, void*, gCInventory_PS*, GEInt, GEInt, gEStackType);
using PersonalTransferStackFn = GEBool (__fastcall *)(gCInventory_PS*, void*, gCInventory_PS*, GEInt, gEStackType);
using PersonalTransferAllFn = GEBool (__fastcall *)(gCInventory_PS*, void*, gCInventory_PS*, gEStackType);
using PersonalTransactionFn = GEBool (__fastcall *)(gCInventory_PS*, void*, gCInventory_PS*, gEStackType, gEStackType);
using PersonalScriptTransferFn = GEBool (__fastcall *)(PSInventory*, void*, GEInt, GEInt, Entity&);
PersonalGenerateFn nativePlunder = nullptr, nativeTrade = nullptr, nativeRefreshTrade = nullptr;
PersonalBeginTradeFn nativeBeginTrade = nullptr;
PersonalTransferItemsFn nativeTransferItems = nullptr;
PersonalTransferStackFn nativeTransferStack = nullptr;
PersonalTransferAllFn nativeTransferAll = nullptr, nativeTransferTransaction = nullptr;
PersonalTransactionFn nativeTransaction = nullptr;
PersonalScriptTransferFn nativeScriptTransfer = nullptr;
std::map<std::string, gFScriptAIState> personalStates;
bool personalStateHooks = false;

GEBool __fastcall GeneratePersonal(gCInventory_PS* inventory, void* edx, bool trade)
{
    PersonalGenerateFn original = trade ? nativeTrade : nativePlunder;
    if (!original) return GEFalse;
    if (ai::ShuttingDown() || !inventory) return original(inventory, edx);
    Entity entity(inventory->GetEntity());
    const bool wasPending = entity.GetInstance() && personalPending.count(EntityKey(entity.GetInstance())) != 0;
    if (wasPending && !EnsurePersonal(entity)) return GEFalse;
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return original(inventory, edx);
    if (!PersonalAdmitted() || !EnsurePersonal(entity)) return GEFalse;
    if ((wasPending || restoredPersonal.count(EntityKey(entity.GetInstance()))) &&
        ((trade && entity.Inventory.IsGeneratedTrade()) || (!trade && entity.Inventory.IsGeneratedPlunder()))) return GETrue;
    PersonalMutation mutation;
    if (!mutation.Enter(inventory)) return GEFalse;
    const GEBool result = original(inventory, edx);
    if (result) {
        if (!mutation.StillAvailable(inventory)) return GEFalse;
        if (trade) inventory->AccessGeneratedTrade() = GETrue;
        else inventory->AccessGeneratedPlunder() = GETrue;
    }
    return result;
}

GEBool __fastcall GeneratePersonalPlunder(gCInventory_PS* inventory, void* edx)
{
    return GeneratePersonal(inventory, edx, false);
}
GEBool __fastcall GeneratePersonalTrade(gCInventory_PS* inventory, void* edx)
{
    return GeneratePersonal(inventory, edx, true);
}

bool PersonalPlayerTransfer(gCInventory_PS* from, gCInventory_PS* to)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return hero && ((from && from->GetEntity() == hero) || (to && to->GetEntity() == hero));
}

GEBool __fastcall RefreshPersonalTrade(gCInventory_PS* inventory, void* edx)
{
    if (!nativeRefreshTrade) return GEFalse;
    if (ai::ShuttingDown() || !inventory) return nativeRefreshTrade(inventory, edx);
    Entity entity(inventory->GetEntity());
    if (entity.GetInstance() && personalPending.count(EntityKey(entity.GetInstance())) && !EnsurePersonal(entity)) return GEFalse;
    if (!IsTrader(entity) || remote::IsPuppet(entity.GetInstance())) return nativeRefreshTrade(inventory, edx);
    if (!EnsurePersonal(entity) || !PersonalAdmitted()) return GEFalse;
    if (restoredPersonal.count(EntityKey(entity.GetInstance())) && entity.Inventory.IsGeneratedTrade()) return GETrue;
    PersonalMutation mutation;
    if (!mutation.Enter(inventory)) return GEFalse;
    return nativeRefreshTrade(inventory, edx);
}

GEBool __fastcall TransferPersonalItems(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                       GEInt stack, GEInt amount, gEStackType type)
{
    if (!nativeTransferItems) return GEFalse;
    if (ai::ShuttingDown()) return nativeTransferItems(from, edx, to, stack, amount, type);
    if (!PersonalPlayerTransfer(from, to)) return nativeTransferItems(from, edx, to, stack, amount, type);
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    return nativeTransferItems(from, edx, to, stack, amount, type);
}

GEBool __fastcall TransferPersonalStack(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                       GEInt stack, gEStackType type)
{
    if (!nativeTransferStack) return GEFalse;
    if (ai::ShuttingDown()) return nativeTransferStack(from, edx, to, stack, type);
    if (!PersonalPlayerTransfer(from, to)) return nativeTransferStack(from, edx, to, stack, type);
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    return nativeTransferStack(from, edx, to, stack, type);
}

GEBool __fastcall TransferPersonalAll(gCInventory_PS* from, void* edx, gCInventory_PS* to, gEStackType type)
{
    if (!nativeTransferAll) return GEFalse;
    if (ai::ShuttingDown()) return nativeTransferAll(from, edx, to, type);
    if (!PersonalPlayerTransfer(from, to)) return nativeTransferAll(from, edx, to, type);
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    return nativeTransferAll(from, edx, to, type);
}

GEBool __fastcall TransferPersonalTransaction(gCInventory_PS* from, void* edx, gCInventory_PS* to, gEStackType type)
{
    if (!nativeTransferTransaction) return GEFalse;
    if (ai::ShuttingDown()) return nativeTransferTransaction(from, edx, to, type);
    if (!PersonalPlayerTransfer(from, to)) return nativeTransferTransaction(from, edx, to, type);
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    return nativeTransferTransaction(from, edx, to, type);
}

GEBool __fastcall PerformPersonalTransaction(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                            gEStackType fromType, gEStackType toType)
{
    if (!nativeTransaction) return GEFalse;
    if (ai::ShuttingDown()) return nativeTransaction(from, edx, to, fromType, toType);
    if (!PersonalPlayerTransfer(from, to)) return nativeTransaction(from, edx, to, fromType, toType);
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    return nativeTransaction(from, edx, to, fromType, toType);
}

GEBool __fastcall TransferPersonalScript(PSInventory* from, void* edx, GEInt stack, GEInt amount, Entity& to)
{
    if (!nativeScriptTransfer) return GEFalse;
    if (ai::ShuttingDown()) return nativeScriptTransfer(from, edx, stack, amount, to);
    auto* property = from ? reinterpret_cast<EntityPropertySet*>(from)->m_pEngineEntityPropertySet : nullptr;
    auto* inventory = property ? static_cast<gCInventory_PS*>(property) : nullptr;
    auto* target = to.GetInstance()
        ? static_cast<gCInventory_PS*>(to.GetInstance()->GetPropertySet(eEPropertySetType_Inventory)) : nullptr;
    if (!PersonalPlayerTransfer(inventory, target)) return nativeScriptTransfer(from, edx, stack, amount, to);
    PersonalMutation mutation;
    if (!mutation.Enter(inventory) || !mutation.Enter(target)) return GEFalse;
    return nativeScriptTransfer(from, edx, stack, amount, to);
}

GEBool __fastcall BeginPersonalTrade(gCGUIManager* manager, void* edx, eCEntity* partner)
{
    if (!nativeBeginTrade) return GEFalse;
    if (ai::ShuttingDown()) return nativeBeginTrade(manager, edx, partner);
    Entity entity(partner);
    if (partner && personalPending.count(EntityKey(partner)) && !EnsurePersonal(entity)) return GEFalse;
    const bool trader = IsTrader(entity) && !remote::IsPuppet(partner);
    const std::string key = trader ? EntityKey(partner) : std::string();
    if (trader) {
        if (!PersonalAdmitted() || !EnsurePersonal(entity) || !RememberPersonal(entity)) return GEFalse;
    }
    const GEBool result = nativeBeginTrade(manager, edx, partner);
    if (result && !key.empty() && restoredPersonal.count(key)) openedPersonalTrade.insert(key);
    return result;
}

GEBool GE_STDCALL PersonalState(bTObjStack<gScriptRunTimeSingleState>& stack, gCScriptProcessingUnit* spu)
{
    const char* name = stack.GetCount() ? stack.Peek().m_strScriptName.GetText() : "";
    auto original = personalStates.find(name);
    if (original == personalStates.end()) return GEFalse;
    if (ai::ShuttingDown()) return original->second(stack, spu);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity focus(hero ? Entity(hero).Focus.GetFocusEntity() : Entity());
    Entity target(spu ? spu->GetTargetEntity() : nullptr);
    const bool local = hero && spu && spu->GetSelfEntity() == hero;
    Entity* chest = local && IsChest(target) ? &target : local && IsChest(focus) ? &focus : nullptr;
    if (chest && (!PersonalAdmitted() || !EnsurePersonal(*chest) || !RememberPersonal(*chest))) return GEFalse;
    const std::string key = chest ? EntityKey(chest->GetInstance()) : std::string();
    const GEBool result = original->second(stack, spu);
    if (!key.empty()) {
        Entity current = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (current.GetInstance() && !PersonalTeardown(current) && !RememberPersonal(current)) return GEFalse;
    }
    return result;
}

void* InstallPersonalNative(char const* symbol, void* detour, char const* module = "Game.dll")
{
    HMODULE game = GetModuleHandleA(module);
    auto const* site = personal::native::Find(module, symbol);
    auto* entry = game ? reinterpret_cast<unsigned char*>(GetProcAddress(game, symbol)) : nullptr;
    const bool expectedAddress = site && entry &&
        reinterpret_cast<uintptr_t>(entry) == reinterpret_cast<uintptr_t>(game) + site->rva;
    const size_t stolen = expectedAddress
        ? personal::native::Match(*site, site->rva, entry, site->headSize) : 0;
    if (!stolen) {
        Logf("FAIL containers: unsupported native personal inventory guard %s", symbol);
        return nullptr;
    }
    void* original = ai::JumpTo(entry, detour, stolen);
    if (!original) Logf("FAIL containers: cannot install personal inventory guard %s", symbol);
    return original;
}

bool InstallPersonalGuards()
{
    if (!nativePlunder)
        nativePlunder = reinterpret_cast<PersonalGenerateFn>(InstallPersonalNative(
            "?GeneratePlunderInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", reinterpret_cast<void*>(&GeneratePersonalPlunder)));
    if (!nativeTrade)
        nativeTrade = reinterpret_cast<PersonalGenerateFn>(InstallPersonalNative(
            "?GenerateTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", reinterpret_cast<void*>(&GeneratePersonalTrade)));
    if (!nativeRefreshTrade)
        nativeRefreshTrade = reinterpret_cast<PersonalGenerateFn>(InstallPersonalNative(
            "?RefreshTradeInvFromTreasureSets@gCInventory_PS@@QAE_NXZ", reinterpret_cast<void*>(&RefreshPersonalTrade)));
    if (!nativeBeginTrade)
        nativeBeginTrade = reinterpret_cast<PersonalBeginTradeFn>(InstallPersonalNative(
            "?BeginTrade@gCGUIManager@@QAE_NPAVeCEntity@@@Z", reinterpret_cast<void*>(&BeginPersonalTrade)));
    if (!nativeTransferItems)
        nativeTransferItems = reinterpret_cast<PersonalTransferItemsFn>(InstallPersonalNative(
            "?TransferItemsTo@gCInventory_PS@@QAE_NPAV1@HHW4gEStackType@@@Z", reinterpret_cast<void*>(&TransferPersonalItems)));
    if (!nativeTransferStack)
        nativeTransferStack = reinterpret_cast<PersonalTransferStackFn>(InstallPersonalNative(
            "?TransferStackTo@gCInventory_PS@@QAE_NPAV1@HW4gEStackType@@@Z", reinterpret_cast<void*>(&TransferPersonalStack)));
    if (!nativeTransferAll)
        nativeTransferAll = reinterpret_cast<PersonalTransferAllFn>(InstallPersonalNative(
            "?TransferAllTo@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@@Z", reinterpret_cast<void*>(&TransferPersonalAll)));
    if (!nativeTransferTransaction)
        nativeTransferTransaction = reinterpret_cast<PersonalTransferAllFn>(InstallPersonalNative(
            "?TransferTransactionItemsTo@gCInventory_PS@@IAE_NPAV1@W4gEStackType@@@Z", reinterpret_cast<void*>(&TransferPersonalTransaction)));
    if (!nativeTransaction)
        nativeTransaction = reinterpret_cast<PersonalTransactionFn>(InstallPersonalNative(
            "?PerformTransaction@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@1@Z", reinterpret_cast<void*>(&PerformPersonalTransaction)));
    if (!nativeScriptTransfer)
        nativeScriptTransfer = reinterpret_cast<PersonalScriptTransferFn>(InstallPersonalNative(
            "?TransferAmountTo@PSInventory@@QAE_NHHAAVEntity@@@Z", reinterpret_cast<void*>(&TransferPersonalScript), "Script.dll"));
    if (!personalStateHooks) {
        bool chest = false, pirate = false, hud = false;
        auto& states = GetScriptAdminExt().GetScriptAIStates();
        GE_MAP_FOR_EACH(name, state, states) {
            const char* text = name.GetText();
            const bool c = !std::strncmp(text, "PS_Interact_Chest", 17);
            const bool p = !std::strncmp(text, "PS_Interact_PirateTreasure", 26);
            const bool h = !std::strcmp(text, "PS_HUD_Loop");
            if (!(c || p || h) || !state || !state->m_funcScriptAIState) continue;
            chest |= c; pirate |= p; hud |= h;
            if (personalStates.count(text)) continue;
            personalStates[text] = state->m_funcScriptAIState;
            state->m_funcScriptAIState = &PersonalState;
            ai::TrackPointer(reinterpret_cast<void*>(&state->m_funcScriptAIState),
                             reinterpret_cast<void*>(personalStates[text]), reinterpret_cast<void*>(&PersonalState),
                             "personal container state");
        }
        personalStateHooks = chest && pirate && hud;
        if (!personalStateHooks) Log("FAIL containers: personal chest interaction guards unavailable");
    }
    return nativePlunder && nativeTrade && nativeRefreshTrade && nativeBeginTrade && personalStateHooks &&
           nativeTransferItems && nativeTransferStack && nativeTransferAll && nativeTransferTransaction &&
           nativeTransaction && nativeScriptTransfer;
}

void TrackPersonalTradeClose()
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

// --- host ---
std::unordered_map<std::string, Content> lastSent;
std::unordered_map<std::string, DWORD> watched;  // traders a client deals with: key -> until
uint32_t lastJoins = 0;
DWORD lastHostPoll = 0;

void Broadcast(std::string const& key, Content const& content)
{
    const std::vector<net::ContentEntry> entries = Entries(content);
    if (!net::SendContents(reinterpret_cast<uint8_t const*>(key.data()), entries.data(), static_cast<int>(entries.size()))) {
        // Not marked as sent, so it goes out again once it fits (Codex review R4: marked anyway, it never went again).
        if (!lastSent.count(key) || lastSent[key] != content) {
            Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
            Logf("containers: contents of %s not sent (%zu entries, at most %d)", NameOf(entity).c_str(), entries.size(), net::MaxContentEntries);
        }
        return;
    }
    auto before = lastSent.find(key);
    if (watched.count(key) || (before != lastSent.end() && before->second != content)) {  // first sends near a player are many
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        Note("containers: contents of %s sent: %s%s", NameOf(entity).c_str(), Describe(content).c_str(), watched.count(key) ? " (trader watched)" : "");
    }
    lastSent[key] = content;
    ++contentsSent;
}

bool hostFill = true;  // false: the host fills a trader only when a client comes close (as before 27.10.)

bool PrepareLocalLoot(Entity& entity)
{
    if (!IsContainer(entity) || remote::IsPuppet(entity.GetInstance())) return true;
    if (!PersonalAdmitted() || !EnsurePersonal(entity)) return false;
    if (entity.Inventory.IsGeneratedPlunder()) return RememberPersonal(entity);
    if (!entity.Inventory.GeneratePlunderInvFromTreasureSets()) {
        Logf("FAIL containers: cannot generate personal loot for %s", NameOf(entity).c_str());
        return false;
    }
    entity.Inventory.SetGeneratedPlunder(GETrue);
    Note("containers: personal loot generated once for %s", NameOf(entity).c_str());
    return RememberPersonal(entity);
}

bool PrepareLocalTrade(Entity& entity)
{
    if (!IsTrader(entity) || remote::IsPuppet(entity.GetInstance())) return true;
    if (!PersonalAdmitted() || !EnsurePersonal(entity)) return false;
    if (entity.Inventory.IsGeneratedTrade()) return RememberPersonal(entity);
    if (!entity.Inventory.GenerateTradeInvFromTreasureSets()) {
        Logf("FAIL containers: cannot generate personal stock for %s", NameOf(entity).c_str());
        return false;
    }
    if (static_cast<GEFloat>(entity.Dialog.EndDialogTimestamp) == 0.0f)
        if (!entity.Inventory.RefreshTradeInvFromTreasureSets()) {
            Logf("FAIL containers: cannot refresh personal stock for %s", NameOf(entity).c_str());
            return false;
        }
    Note("containers: personal merchant stock prepared for %s", NameOf(entity).c_str());
    return RememberPersonal(entity);
}

void LocalInventoryTick()
{
    if (!PersonalAdmitted()) return;
    TrackPersonalFocus();
    const DWORD now = GetTickCount();
    if (now - lastLocalPoll < 1000) return;
    lastLocalPoll = now;
    PersonalPass(true);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    if (infos && infos->AccessIsRunning()) {
        Entity partner(infos->GetDialogPartnerNPC());
        PrepareLocalTrade(partner);
    }
    // Mirrored NPCs skip native death bookkeeping, so prepare their local plunder before quick loot can inspect it.
    const bCVector& p = hero->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        eCEntity* instance = entity.GetInstance();
        if (instance && DistanceTo(instance, here) <= PreRollRadius && Corpse(entity)) PrepareLocalLoot(entity);
    }
}

void HostTick()
{
    const DWORD now = GetTickCount();
    if (now - lastHostPoll < 1000) return;
    lastHostPoll = now;
    if (net::JoinCount() != lastJoins) {
        lastJoins = net::JoinCount();
        lastSent.clear();  // a newcomer needs everything
    }
    std::vector<std::array<float, 3>> players;
    if (eCEntity* hero = gCSession::GetInstance().GetHero()) {
        const bCVector& p = hero->GetWorldPosition();
        players.push_back({p.GetX(), p.GetY(), p.GetZ()});
    }
    net::Remote const* remotes = net::Remotes();
    bool anyClient = false;
    std::vector<std::array<float, 3>> clients;
    for (int id = 2; id < net::MaxPlayers; ++id) {
        if (!remotes[id].connected) continue;
        anyClient = true;
        if (remotes[id].latest.flags & 1) {
            players.push_back({remotes[id].latest.position[0], remotes[id].latest.position[1], remotes[id].latest.position[2]});
            clients.push_back(players.back());
        }
    }
    if (!anyClient) return;
    for (auto it = watched.begin(); it != watched.end();) it = static_cast<int32_t>(it->second - now) < 0 ? watched.erase(it) : ++it;
    // The host's own dialog partner: a trader a client simulates is a mirror here, and a mirror runs no ZS_Talk, which fills
    // a trader's goods; it stayed empty for the host (Jens after a quick load, playtest 27.10.).
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    eCEntity* hostPartner = hostFill && infos && infos->AccessIsRunning() ? infos->GetDialogPartnerNPC() : nullptr;
    const bTObjArray<Entity> all = Entity::GetEntities();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        eCEntity* instance = entity.GetInstance();
        if (!instance) continue;
        // Distance first, one position read: telling chests, corpses and traders apart takes several property
        // lookups per entity, too many for every entity of the world each second (Fable review 22.10.).
        bool nearPlayer = false;
        for (auto const& at : players) nearPlayer = nearPlayer || DistanceTo(instance, at.data()) <= InterestRadius;
        if (!nearPlayer) continue;
        uint8_t guid[16];
        guid::CopyEntity(instance, guid);
        const std::string key = Key(guid);
        // A chest a client comes close to is rolled here before the client can open it: its loot screen then
        // shows the host's contents from the start (the watch on opening stays as a fallback).
        if (Rolled(entity) && !entity.Inventory.IsGeneratedPlunder()) {
            bool close = false;
            for (auto const& at : clients) close = close || DistanceTo(instance, at.data()) <= PreRollRadius;
            if (close) {
                entity.Inventory.GeneratePlunderInvFromTreasureSets();
                entity.Inventory.SetGeneratedPlunder(GETrue);
                Logf("containers: a player came close to %s %s, rolled here: %s", IsChest(entity) ? "chest" : "corpse", entity.GetName().GetText(),
                     Describe(Read(entity)).c_str());
            }
        }
        // A trader likewise: only the host fills it (a client's dialog runs on the host's NPC and fills nothing, so
        // whoever talked first saw goods and the other none, playtest 21.10.). The first dialog also tops it up from the
        // refresh sets, as ZS_Talk does.
        const bool trader = IsTrader(entity);
        bool clientClose = false;
        if (trader)
            for (auto const& at : clients) clientClose = clientClose || DistanceTo(instance, at.data()) <= PreRollRadius;
        if (trader && (clientClose || instance == hostPartner) && !traderFillOff && !entity.Inventory.IsGeneratedTrade()) {
            entity.Inventory.GenerateTradeInvFromTreasureSets();
            if (static_cast<GEFloat>(entity.Dialog.EndDialogTimestamp) == 0.0f) entity.Inventory.RefreshTradeInvFromTreasureSets();
            Logf("containers: %s trader %s, filled here: %s", clientClose ? "a player came close to" : "the host talks with the mirrored",
                 entity.GetName().GetText(), Describe(Read(entity)).c_str());
        }
        bool wanted = (watched.count(key) && HasInventory(instance)) || IsContainer(entity);  // near a player (above)
        // Its goods go out while a client is close, so the trade screen shows them from the start.
        if (!wanted && trader && clientClose && entity.Inventory.IsGeneratedTrade()) wanted = true;
        if (!wanted) continue;
        const Content content = Read(entity);
        auto sent = lastSent.find(key);
        if (sent == lastSent.end() || sent->second != content) Broadcast(key, content);
    }
}

void OnTx(uint8_t from, uint8_t const guid[16], uint32_t seq, net::ContentEntry const* entries, int count)
{
    if (disabled) return;
    Entity container = guid::Resolve(guid);
    bool ok = container.GetInstance() && Shared(container);
    Content have = ok ? Read(container) : Content{};
    constexpr int32_t MaxAmount = 1000000;  // more than any stack in the game; keeps -amount from overflowing
    for (int i = 0; i < count && ok; ++i)
        if (entries[i].amount < -MaxAmount || entries[i].amount > MaxAmount ||
            (entries[i].amount < 0 && have[KeyOf(entries[i])] < -entries[i].amount))
            ok = false;
    if (ok) {
        // All or nothing: every removal is still possible, so apply the whole transaction.
        for (int i = 0; i < count; ++i) {
            if (entries[i].amount < 0) {
                Remove(container, KeyOf(entries[i]), -entries[i].amount);
            } else if (entries[i].amount > 0) {
                CreateGoods(container, KeyOf(entries[i]), entries[i].amount);  // a trader: what it bought is merchandise
            }
        }
        ++txApplied;
        // Applied means applied: the contents are now what the transaction says.
        Content expected = have;
        for (int i = 0; i < count; ++i) expected[KeyOf(entries[i])] += entries[i].amount;
        for (auto it = expected.begin(); it != expected.end();) it = it->second == 0 ? expected.erase(it) : std::next(it);
        if (Read(container) != expected) {
            ++txMismatch;
            Logf("containers: transaction %u at %s did not come out as checked: expected %s", seq, NameOf(container).c_str(),
                 Describe(expected).c_str());
        }
    } else {
        ++txRefused;
    }
    net::SendContainerTxResult(from, guid, seq, ok);
    if (container.GetInstance()) Broadcast(Key(guid), Read(container));
    Logf("containers: transaction %u from player %u %s (%d entries: %s) at %s; now %s", seq, from, ok ? "applied" : "refused", count,
         Describe(FromEntries(entries, count)).c_str(), NameOf(container).c_str(), container.GetInstance() ? Describe(Read(container)).c_str() : "-");
}

void OnWatch(uint8_t from, uint8_t const guid[16])
{
    if (disabled) return;
    Entity chest = guid::Resolve(guid);
    if (chest.GetInstance() && !Shared(chest)) return;
    watched[Key(guid)] = GetTickCount() + 120000;
    Note("containers: player %u opened %s (%s); here %s", from, NameOf(chest).c_str(), IsChest(chest) ? "chest" : "trade",
         chest.GetInstance() && HasInventory(chest.GetInstance()) ? Describe(Read(chest)).c_str() : "(not here)");
    // A client opened a chest: its contents are rolled here, once, as if the host had opened it.
    if (!Rolled(chest) || chest.Inventory.IsGeneratedPlunder()) return;
    chest.Inventory.GeneratePlunderInvFromTreasureSets();
    chest.Inventory.SetGeneratedPlunder(GETrue);
    const Content content = Read(chest);
    Broadcast(Key(guid), content);
    Logf("containers: player %u opened chest %s, rolled here: %s", from, chest.GetName().GetText(), Describe(content).c_str());
}

// --- client ---
bool tradeKeep = true;  // see below (dialogTrade)
bool tradeBase = true;  // QA negative control tradebaseoff: P1 of the code review 28.10. as before (see HostPlusPending)
bool tradeSnap = false;  // QA tradesnapon / tradesnapoff (see SnapshotUnknown); off: not proven (29.10.)
std::unordered_map<std::string, Content> known;  // the host's contents
// With tradeKeep (stageTradeRace 28.10.): what a container here holds without this player's changes not reported yet, the
// host's contents plus the transactions sent and not answered yet. The player's own change is "here minus base", never "here
// minus the host's contents": host goods that changed meanwhile (a trader filled on the host) were reported back as the
// player's taking them (Map_World -1/+1, 23 transactions back and forth) when the host's contents waited for a result.
std::unordered_map<std::string, Content> bases;
std::string active;                              // the container this client's player opened
DWORD activeSince = 0;
// Diagnostic (28.10., a client's purchase lost under lag: the trader was set to the host's goods while not "active"): one
// line whenever the container this client's player has open changes, with why.
void NoteActive(char const* what, std::string const& key, char const* why)
{
    Entity container = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
    Logf("containers: open container %s %s (%s)", key.empty() ? "-" : NameOf(container).c_str(), what, why);
}
struct Pending {
    std::string key;
    Content deltas;  // container side: negative = the hero took, positive = the hero put in
    Content after;   // the container here right after sending; host contents arriving meanwhile do not change it
};
std::map<uint32_t, Pending> pending;
uint32_t nextSeq = 1;
DWORD lastClientPoll = 0, lastResync = 0;

bool PendingFor(std::string const& key)
{
    for (auto const& entry : pending)
        if (entry.second.key == key) return true;
    return false;
}

void AddTo(Content& to, Content const& from, int sign)
{
    for (auto const& [name, amount] : from) to[name] += sign * amount;
    for (auto it = to.begin(); it != to.end();) it = it->second == 0 ? to.erase(it) : ++it;
}

// The host's contents plus what was sent for this container and is not answered yet, never below 0: a container cannot
// hold less (code review 28.10., P1: both players bought the last item; the host's contents had it gone, the pending
// purchase took it once more, -1; the poll then reported the 0 the game holds as the player putting one in, and a refusal
// counted the host's gold as the player's).
Content HostPlusPending(std::string const& key)
{
    Content content = known.count(key) ? known[key] : Content{};
    for (auto const& entry : pending)
        if (entry.second.key == key) AddTo(content, entry.second.deltas, 1);
    if (tradeBase)
        for (auto it = content.begin(); it != content.end();) it = it->second < 0 ? content.erase(it) : ++it;
    return content;
}

Content BaseOf(std::string const& key)
{
    auto it = bases.find(key);
    return it != bases.end() ? it->second : HostPlusPending(key);
}

// What this client's player changed in the open container since the host's last contents (with tradeKeep: since its base):
// one transaction.
void ReportChange(Entity& container, Content& hostContent)
{
    const Content local = Read(container);
    const Content from = tradeKeep ? BaseOf(active) : hostContent;
    if (local == from) return;
    Content deltas;
    for (auto const& [name, amount] : local) deltas[name] += amount;
    for (auto const& [name, amount] : from) deltas[name] -= amount;
    for (auto it = deltas.begin(); it != deltas.end();) it = it->second == 0 ? deltas.erase(it) : ++it;
    const std::vector<net::ContentEntry> entries = Entries(deltas);
    const uint32_t seq = nextSeq;
    // Pending only once it went out: a transaction that could not be sent blocked the container for good (R4).
    if (!net::SendContainerTx(reinterpret_cast<uint8_t const*>(active.data()), seq, entries.data(), static_cast<int>(entries.size()))) {
        Logf("containers: transaction for %s not sent (%zu entries, at most %d)", NameOf(container).c_str(), entries.size(), net::MaxContentEntries);
        return;
    }
    ++nextSeq;
    pending[seq] = Pending{active, deltas, local};
    ++txSent;
    Note("containers: transaction %u sent for %s: %s", seq, NameOf(container).c_str(), Describe(deltas).c_str());
    if (tradeKeep) bases[active] = local;  // the host's contents stay the host's
    else hostContent = local;  // optimistic until the result arrives
}

void OnContents(uint8_t const guid[16], net::ContentEntry const* entries, int count)
{
    if (disabled) return;
    ++contentsReceived;
    const std::string key = Key(guid);
    Entity container = guid::Resolve(guid);
    if (container.GetInstance() && !Shared(container)) return;
    if (tradeKeep) {
        // The open container: the host's contents plus what is sent and not answered, plus the player's change not reported
        // yet (here minus base). Any other: the host's contents plus what is sent and not answered (the game's own drift
        // goes back, as before).
        // A change of the player's not reported yet goes out first, as without tradeKeep: folded into the host's contents a
        // purchase of an item the host's contents no longer hold was lost (the container cannot go below 0), and the hero kept
        // the item the other player bought too (code review P1, model_tradekeep.py scenario B).
        const bool first = !known.count(key);
        const bool based = !first || (tradeSnap && bases.count(key));  // the host's goods, or the snapshot of a trader's first trade
        if (tradeBase && key == active && container.GetInstance() && based && Read(container) != BaseOf(key)) {
            Content unused = first ? Content{} : known[key];
            ReportChange(container, unused);
        }
        Content mine;
        if (key == active && container.GetInstance() && based) {
            AddTo(mine, Read(container), 1);
            AddTo(mine, BaseOf(key), -1);
        }
        known[key] = FromEntries(entries, count);
        if (Rolled(container) && !container.Inventory.IsGeneratedPlunder()) container.Inventory.SetGeneratedPlunder(GETrue);
        bases[key] = HostPlusPending(key);
        if (key == active && first)
            Note("containers: host contents of the open %s: %s; here %s", NameOf(container).c_str(), Describe(known[key]).c_str(),
                 container.GetInstance() ? Describe(Read(container)).c_str() : "(not here)");
        if (!container.GetInstance()) return;
        Content want = bases[key];
        AddTo(want, mine, 1);
        const Content here = Read(container);
        if (here != want) {
            Note("containers: %s%s set to the host's contents%s: %s (was %s)", NameOf(container).c_str(), key == active ? " (open)" : "",
                 mine.empty() ? "" : " and the player's change", Describe(want).c_str(), Describe(here).c_str());
            Write(container, want);
        }
        return;
    }
    if (key == active && container.GetInstance() && !PendingFor(key) && known.count(key)) {
        // A change by this player that the poll has not reported yet must not be overwritten (that lost the
        // take and doubled the item): report it against the contents it was made on; the host decides.
        ReportChange(container, known[key]);
    }
    const bool first = !known.count(key);
    known[key] = FromEntries(entries, count);
    if (Rolled(container) && !container.Inventory.IsGeneratedPlunder()) container.Inventory.SetGeneratedPlunder(GETrue);
    if (key == active && first)
        Note("containers: host contents of the open %s: %s; here %s", NameOf(container).c_str(), Describe(known[key]).c_str(),
             container.GetInstance() ? Describe(Read(container)).c_str() : "(not here)");
    if (key == active && PendingFor(key)) return;  // the transaction result decides first
    if (container.GetInstance()) {
        const Content here = Read(container);
        if (here != known[key]) {
            if (key != active) NoteActive("is not", key, active.empty() ? "nothing open" : "another one is open");
            Note("containers: %s%s set to the host's contents: %s (was %s)", NameOf(container).c_str(), key == active ? " (open)" : "",
                 Describe(known[key]).c_str(), Describe(here).c_str());
            Write(container, known[key]);
        }
    }
}

void OnTxResult(uint8_t const guid[16], uint32_t seq, bool ok)
{
    auto it = pending.find(seq);
    if (it == pending.end()) return;
    const Pending done = it->second;
    // tradeKeep: the container is set to the host's contents (plus the pending, plus the player's change) while it waits, so
    // the player's own change since is the container against its base (still with this transaction), not against done.after.
    const Content base = tradeKeep && tradeBase ? BaseOf(done.key) : done.after;
    pending.erase(it);
    Note("containers: transaction %u %s by the host", seq, ok ? "applied" : "refused");
    if (ok) return;
    ++txDenied;
    // Undo what this hero gained or gave in that transaction, and in anything done since that was not
    // reported yet; the container then follows the host's contents.
    Entity container = guid::Resolve(guid);
    // What the hero changed here since sending counts against the container as it was then, not against host contents that
    // arrived meanwhile (Codex review R2: with those, a refused take could leave the hero its item); with tradeKeep its base.
    Content undo = done.deltas;
    if (container.GetInstance()) {
        for (auto const& [name, amount] : Read(container)) undo[name] += amount;
        for (auto const& [name, amount] : base) undo[name] -= amount;
    }
    if (eCDynamicEntity* hero = gCSession::GetInstance().GetHero()) {
        Entity self(hero);
        for (auto const& [key, delta] : undo) {
            const Template item = TemplateOf(key);
            if (delta < 0) {
                if (Remove(self, key, -delta, true) < -delta)
                    Logf("containers: undo at %s could not take back all of %s", done.key == active ? "the open container" : "a container", NameOf(key));
            } else if (delta > 0) {
                self.Inventory.CreateItems(item, 0, delta);
            }
        }
        ++reverted;
    }
    auto host = known.find(done.key);
    if (tradeKeep) {
        // Undone above (the hero), so the container holds the host's contents plus what is still sent and not answered.
        bases[done.key] = HostPlusPending(done.key);
        if (container.GetInstance()) Write(container, bases[done.key]);
    } else if (container.GetInstance() && host != known.end()) {
        Write(container, host->second);
    }
    Logf("containers: transaction %u refused by the host; undone", seq);
}

bool dialogTrade = true;  // false: a dialog's trade is not reported (as before 27.10.)
// While the trade screen with a trader is open, that trader stays this client's open container, however far its copy here
// stands and however long it is open (stageTradeAgain 28.10. under NETSIM: Hamlar's copy 18 m from the hero, so the 8-m rule
// let him go and the dialog trade set him again in every poll; host goods arriving in between set him back to the host's,
// and a purchase not yet reported was lost: evening run scene 14). On (stageTradeRace 28.10.: tradekeepoff lost 3 of 6 purchases,
// with it 6 of 6 arrived; stageTrade, stageNpcBuy, stageQuickLoot, stageChest green). QA tradekeepon / tradekeepoff (declared above).
// QA (reproducing scene 14): the poll reports a change of the open container only once it is ReportDelay old, so that host
// goods arriving meanwhile meet a purchase not yet reported (the window is otherwise a few ms). Off by default.
bool reportDelay = false;
constexpr DWORD ReportDelay = 600;  // ms
DWORD changeSince = 0;
unsigned dialogTrades = 0;

// The trade screen of a dialog seen every frame, not only in the 250-ms poll (29.10., stageAbend: opened, bought and closed within
// one poll, the trader never became the open container and the purchase stayed on the client). Once it is the open container,
// the poll reports the change, also after the screen closed. QA tradeframeon / tradeframeoff. Off by default: stageTradeQuick
// 29.10. lost the same with and without it; the purchase of stageAbend fell on the tutorial page the game shows at a player's
// first trade (page 15, not the trade page 5), which a player closes before buying.
bool tradeFrame = false;
// A trader whose goods this client does not know yet (its first trade): its copy here as it was when it became the open
// container is the base, so a purchase before the host's goods arrive is reported against it (29.10. stageTradeQuick: the
// host's goods came 0.6 s after the screen opened and overwrote the purchase; the hero kept the item, its gold was gone, the
// host never heard of it; seen once, in run 20261029-060453). Not reproduced on purpose since (the purchase came after the
// host's goods), so off by default: QA tradesnapon (tradeSnap, declared above).

void SnapshotUnknown(Entity& container)
{
    if (tradeSnap && tradeKeep && !known.count(active) && container.GetInstance()) bases[active] = Read(container);
}

void DialogTradeFrame(DWORD now)
{
    gCGUIManager* guiManager = gCSession::GetInstance().GetGUIManager();
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    eCEntity* partner = guiManager && infos && guiManager->GetActivePage() == gEGamePage_Trade ? infos->GetDialogPartnerNPC() : nullptr;
    Entity trader(partner);
    if (!partner || remote::PuppetOwner(partner) || !IsTrader(trader)) return;
    uint8_t guid[16];
    guid::CopyEntity(partner, guid);
    if (Key(guid) == active) return;
    active = Key(guid);
    activeSince = now;
    SnapshotUnknown(trader);
    NoteActive("set", active, "dialog trade, frame");
    net::SendWatch(guid);
    ++dialogTrades;
    Logf("containers: trading with %s (dialog trade seen in its frame, host goods known=%d)", trader.GetName().GetText(), known.count(active) ? 1 : 0);
}

void ClientTick()
{
    const DWORD now = GetTickCount();
    if (dialogTrade && tradeFrame && net::Connected() && gCSession::GetInstance().GetHero()) DialogTradeFrame(now);
    if (now - lastClientPoll < 250) return;
    lastClientPoll = now;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !net::Connected()) return;
    // Chests open through the action key (PS_Interact -> PS_Interact_Chest), not PlayerOpenLoot: the chest in use is
    // this client's open container, and the host rolls its contents. A dug-up pirate treasure the same way
    // (PS_Interact_PirateTreasure, then its loot window, PS_HUD_Loop): before, its takes were never sent, the host's
    // contents came back and the client took them again (treasurediag 24.10.: ore 10 of 5, then the host took all 5).
    Entity self(hero);
    char const* state = self.Routine.GetCurrentState().GetText();
    const bool interact = !std::strncmp(state, "PS_Interact_Chest", 17) ||
                          (interactLoot && (!std::strncmp(state, "PS_Interact_PirateTreasure", 26) || !std::strcmp(state, "PS_HUD_Loop")));
    if (interact) {
        Entity chest = self.Focus.GetFocusEntity();
        if (IsChest(chest) && chest.Interaction.GetUser().GetInstance() == hero) {
            uint8_t guid[16];
            guid::CopyEntity(chest.GetInstance(), guid);
            if (Key(guid) != active) {
                active = Key(guid);
                activeSince = now;
                NoteActive("set", active, "chest interaction");
                net::SendWatch(guid);
                Logf("containers: opened %s (%s, host contents known=%d)", chest.GetName().GetText(), state, known.count(active) ? 1 : 0);
            }
        }
    }
    // A trade from a dialog (info command DoTrade) opens the trade screen without PlayerOpenTrade, so Opened never ran: the
    // client's purchases and sales were never sent, and every 2 s the trader was set back to the host's goods (playtest
    // 27.10.: Gelford 233 times "drifted", an endless stock). While the trade screen is open, the dialog partner is this
    // client's open container, and the poll below reports what changed as a transaction.
    bool trading = false;  // the trade screen with the open container is up (tradeKeep)
    if (dialogTrade) {
        gCGUIManager* guiManager = gCSession::GetInstance().GetGUIManager();
        gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
        eCEntity* partner = guiManager && infos && guiManager->GetActivePage() == gEGamePage_Trade ? infos->GetDialogPartnerNPC() : nullptr;
        Entity trader(partner);
        if (partner && !remote::PuppetOwner(partner) && IsTrader(trader)) {
            uint8_t guid[16];
            guid::CopyEntity(partner, guid);
            if (tradeKeep && Key(guid) == active) {
                trading = true;
                activeSince = now;  // open as long as the screen is
            }
            if (Key(guid) != active) {
                active = Key(guid);
                activeSince = now;
                trading = tradeKeep;
                SnapshotUnknown(trader);
                NoteActive("set", active, "dialog trade");
                net::SendWatch(guid);
                ++dialogTrades;
                static std::string lastLogged;  // once per trader in a row (a trader far from the hero is let go every poll)
                if (lastLogged != active) Logf("containers: trading with %s (dialog trade, host goods known=%d)", trader.GetName().GetText(), known.count(active) ? 1 : 0);
                lastLogged = active;
            }
        }
    }
    if (!active.empty()) {
        Entity container = guid::Resolve(reinterpret_cast<uint8_t const*>(active.data()));
        auto host = known.find(active);
        bool hold = false;
        if (reportDelay && container.GetInstance() && host != known.end()) {
            if (Read(container) == host->second) changeSince = 0;
            else if (!changeSince) changeSince = now ? now : 1;
            hold = changeSince && now - changeSince < ReportDelay;
        }
        if (!hold && container.GetInstance() && host != known.end() && !PendingFor(active)) {
            ReportChange(container, host->second);
            changeSince = 0;
        }
        // Only after reporting what changed: the player walked away, or the container is gone.
        const bCVector& h = hero->GetWorldPosition();
        const float at[3] = {h.GetX(), h.GetY(), h.GetZ()};
        if (!container.GetInstance() || (!trading && (DistanceTo(container.GetInstance(), at) > 800.0f || now - activeSince > 120000))) {
            char why[64];
            if (!container.GetInstance()) Format(why, "gone");
            else if (DistanceTo(container.GetInstance(), at) > 800.0f) Format(why, "%.0f m away", DistanceTo(container.GetInstance(), at) / 100.0f);
            else Format(why, "open %lu s", (now - activeSince) / 1000);
            NoteActive("cleared", active, why);
            active.clear();
        }
    }
    if (now - lastResync < 2000) return;
    lastResync = now;
    // Chests near this player count as rolled here: only the host rolls them (a chest the host has not sent yet too).
    const bCVector& h = hero->GetWorldPosition();
    const float at[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const bTObjArray<Entity> all = Entity::GetEntities();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        if (entity.GetInstance() && DistanceTo(entity.GetInstance(), at) <= InterestRadius && Rolled(entity) && !entity.Inventory.IsGeneratedPlunder())
            entity.Inventory.SetGeneratedPlunder(GETrue);
    }
    // Anything else that drifted from the host's contents (a corpse rolling its own plunder) is put back.
    for (auto const& [key, host] : known) {
        if (key == active) continue;
        Entity container = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        // Far ones are skipped, not forgotten: the host sends a container again only when it changes.
        if (!container.GetInstance() || DistanceTo(container.GetInstance(), at) > InterestRadius) continue;
        const Content here = Read(container);
        const Content content = tradeKeep ? HostPlusPending(key) : host;  // what was sent and not answered stays
        if (here == content) continue;
        Note("containers: %s drifted; set back to the host's contents: %s (was %s)", NameOf(container).c_str(), Describe(content).c_str(),
             Describe(here).c_str());
        Write(container, content);
    }
}

// The game's own entry points for opening loot and trade: remember what this client's player opened.
gFScript originalLoot = nullptr, originalTrade = nullptr;
bool hooked = false, nativeLootGuard = false;
constexpr uintptr_t OpenLootRva = 0x6bfd0;
DWORD lastHookTry = 0;

bool Opened(gCScriptProcessingUnit* spu, GELPVoid other, bool trade)
{
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    // Trade with another player's puppet, on any machine: filled with that player's goods (trade.cpp), never a container.
    if (trade && partner.GetInstance() && trade::OnOpen(partner.GetInstance())) return true;
    if (!partner.GetInstance()) return true;
    if (personalPending.count(EntityKey(partner.GetInstance())) && !EnsurePersonal(partner)) return false;
    return trade ? PrepareLocalTrade(partner) : PrepareLocalLoot(partner);
}

GEInt GE_STDCALL OpenLoot(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalLoot ? originalLoot(spu, self, other, args) : 0;
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    if (remote::PuppetOwner(partner.GetInstance())) {
        eCEntity* actor = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
        if (remote::IsHeldPuppet(partner.GetInstance()) && actor &&
            actor == gCSession::GetInstance().GetHero() && !downed::CombatUnavailable(actor)) {
            // A loot action queued before the owner hold arrived still enters here directly from PS_GonnaLoot.
            const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTalk"), actor, partner.GetInstance(), 0);
            remote::LogPuppetInteraction(partner.GetInstance(), "loot-rerouted-to-talk", result);
            return result;
        }
        remote::LogPuppetInteraction(partner.GetInstance(), "loot-refused", 0);
        Note("containers: loot of %s refused (another player)", NameOf(partner).c_str());
        return 0;
    }
    if (!Opened(spu, other, false)) return 0;
    return originalLoot(spu, self, other, args);
}

GEInt GE_STDCALL OpenTrade(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalTrade ? originalTrade(spu, self, other, args) : 0;
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    if (remote::PuppetOwner(partner.GetInstance()) && !trade::Available()) {
        Log("trade: native player exchange unavailable; opening refused");
        return 0;
    }
    if (!Opened(spu, other, true)) return 0;
    return originalTrade(spu, self, other, args);
}

// Quick loot uses this TransferAmountTo call site without PlayerOpenLoot. Keep the native local transfer and
// the protection against looting another player's puppet; never wait for host inventory contents.
// ZS_RansackBody uses the same import from another call site and is not touched.
constexpr uintptr_t QuickLootCall = 0x81ced;
bool quickHooked = false, quickHookFailed = false;
unsigned quickTakes = 0, quickRefused = 0;

GEBool __fastcall QuickTake(PSInventory* self, void*, GEInt stack, GEInt amount, Entity& to)
{
    if (ai::ShuttingDown()) return self ? self->TransferAmountTo(stack, amount, to) : GEFalse;
    eCEntity* owner = nullptr;
    if (auto* set = reinterpret_cast<EntityPropertySet*>(self)->m_pEngineEntityPropertySet) owner = set->GetEntity();
    if (remote::PuppetOwner(owner)) { ++quickRefused; return GEFalse; }  // another player's puppet (ransack.cpp): never looted
    Entity entity(owner);
    const bool toHero = owner && to.GetInstance() == gCSession::GetInstance().GetHero();
    const bool wasPending = toHero && personalPending.count(EntityKey(owner)) != 0;
    if (wasPending) {
        EnsurePersonal(entity);  // the saved stack index may have changed, even on successful restoration
        ++quickRefused;
        return GEFalse;
    }
    personal::Kind kind;
    if (toHero && PersonalKind(entity, kind)) {
        if (!PersonalAdmitted()) { ++quickRefused; return GEFalse; }
        if (!EnsurePersonal(entity)) { ++quickRefused; return GEFalse; }
        if (!RememberPersonal(entity)) { ++quickRefused; return GEFalse; }
        const std::string key = EntityKey(owner);
        const GEBool moved = self->TransferAmountTo(stack, amount, to);
        if (moved) {
            ++quickTakes;
            Entity current = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
            if (current.GetInstance() && !PersonalTeardown(current)) RememberPersonal(current);
        }
        return moved;
    }
    return self->TransferAmountTo(stack, amount, to);
}

void HookQuickLoot()
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("Script_Game.dll"));
    if (!base) return;
    uint8_t* site = base + QuickLootCall;
    // Expect "call dword ptr [Script_Game.dll+0xe4500]" (ff 15 <address>); anything else: another build, leave it.
    uint32_t slot = 0;
    std::memcpy(&slot, site + 2, 4);
    if (site[0] != 0xFF || site[1] != 0x15 || slot != reinterpret_cast<uintptr_t>(base) + 0xe4500) {
        quickHookFailed = true;
        Log("containers: quick loot call site not as expected; quick loot is not synchronized");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) { quickHookFailed = true; return; }
    unsigned char original[6], installed[6];
    std::memcpy(original, site, sizeof(original));
    site[0] = 0xE8;  // call rel32 QuickTake, then a nop: same length, same stack
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(&QuickTake) - reinterpret_cast<intptr_t>(site + 5));
    std::memcpy(site + 1, &to, 4);
    site[5] = 0x90;
    std::memcpy(installed, site, sizeof(installed));
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    ai::TrackPatch(site, original, installed, sizeof(original), "containers quick loot call");
    quickHooked = true;
    Log("containers: quick loot hook installed");
}

// QA (1.2): the game's own quick loot input. Use2 in PS_Normal with QuickLoot on appends action 0x90 to the
// script's action list (Script_Game.dll+0x118d54: entries, count, capacity); PS_Normal_Loop takes it and loots
// the focused corpse or chest. The list's append helper (+0xcb90) takes the list header in EAX and the entry on
// the stack and cleans it up itself (C quick-loot-input-c.md).
struct QuickAction {
    GEU32 action;
    GEU32 aux;
    Entity target;
};
static_assert(sizeof(QuickAction) == 0xb0, "native action layout");
constexpr uintptr_t ActionList = 0x118d54, ActionAppend = 0xcb90;

void AppendAction(void* list, QuickAction* entry, void* append)
{
    __asm {
        mov eax, list
        push entry
        call append
    }
}
}

void QuickLoot(LogFn log)
{
    char line[300];
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("Script_Game.dll"));
    Entity hero(gCSession::GetInstance().GetHero());
    char const* task = hero.GetInstance() ? hero.Routine.GetCurrentState().GetText() : "-";
    const int count = base ? *reinterpret_cast<int*>(base + ActionList + 4) : -1;
    const bool setting = eCApplication::GetInstance().GetEngineSetup().QuickLoot != GEFalse;
    char const* refused = !base ? "no_script_game" : !hero.GetInstance() ? "no_hero" : !setting ? "quick_loot_off"
                          : std::strcmp(task, "PS_Normal") && std::strcmp(task, "PS_Normal_Loop") ? "not_ps_normal" : count != 0 ? "actions_pending" : nullptr;
    if (refused) {
        Format(line, "QUICKLOOT queued=0 reason=%s task=%s actions=%d quick_loot=%d", refused, task, count, setting);
        log(line);
        return;
    }
    QuickAction entry{0x90, 0, Entity()};
    AppendAction(base + ActionList, &entry, base + ActionAppend);
    Entity focus = hero.Focus.GetFocusEntity();
    Format(line, "QUICKLOOT queued=1 task=%s actions_before=%d actions_after=%d focus=%s", task, count,
              *reinterpret_cast<int*>(base + ActionList + 4), NameOf(focus).c_str());
    log(line);
}

void QuickLootActions(LogFn log)
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("Script_Game.dll"));
    char line[80];
    Format(line, "QUICKACTIONS count=%d", base ? *reinterpret_cast<int*>(base + ActionList + 4) : -1);
    log(line);
}

namespace {
}

namespace {
void IgnoreSharedInventory(char const* kind)
{
    ++ignoredSharedMessages;
    if (ignoredSharedMessages <= 8)
        Logf("containers: ignored remote %s; loot and NPC merchant inventories are personal", kind);
}
void IgnoreContents(uint8_t const*, net::ContentEntry const*, int) { IgnoreSharedInventory("contents"); }
void IgnoreTransaction(uint8_t, uint8_t const*, uint32_t, net::ContentEntry const*, int) { IgnoreSharedInventory("transaction"); }
void IgnoreResult(uint8_t const*, uint32_t, bool) { IgnoreSharedInventory("transaction result"); }
void IgnoreWatch(uint8_t, uint8_t const*) { IgnoreSharedInventory("inventory watch"); }
}

void Start()
{
    net::SetContainerHandlers(&IgnoreContents, &IgnoreTransaction, &IgnoreResult, &IgnoreWatch);
    Log("containers: chest/corpse loot and NPC merchant stock are personal; direct player trades remain transfers");
}

bool IsPersonalInventory(eCEntity* instance)
{
    Entity entity(instance);
    return Shared(entity);
}

void OnWorldChange()
{
    RetirePersonalObservers();
    items::OnWorldChange();
    personalLedger.clear();
    personalLedgerBytes = 12;
    personalPending.clear();
    personalErrors.clear();
    personalRestoreErrors.clear();
    personalUncertain.clear();
    personalErrorOverflow = false;
    restoredPersonal.clear();
    openedPersonalTrade.clear();
    seenPersonalTradePage.clear();
    if (forgetOff) return;
    // A load brings the containers back as saved: what was known, open, sent or pending belongs to the world before.
    // Kept, a client compared the reloaded container with its old copy and reported the difference as a deposit
    // (Codex review R5). Changes count again from the host's first contents in the new world.
    known.clear();
    bases.clear();
    pending.clear();
    if (!active.empty()) NoteActive("cleared", active, "world change");
    active.clear();
    lastSent.clear();
    watched.clear();
    lastLocalPoll = 0;
}

bool InstallAccessGuards()
{
    if (!gCSession::GetInstance().IsGameRunning()) return false;
    if (!hooked) {
        gSScript *loot = nullptr, *trade = nullptr;
        auto& scripts = GetScriptAdminExt().GetScripts();
        if (scripts.GetAt(bCString("PlayerOpenLoot"), loot) && loot && scripts.GetAt(bCString("PlayerOpenTrade"), trade) && trade) {
            const gFScript nativeLoot = loot->m_funcScript;
            originalLoot = nativeLoot;
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleA("Script_Game.dll"));
            static const unsigned char head[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x01, 0x00, 0x00};
            if (base && reinterpret_cast<void*>(nativeLoot) == base + OpenLootRva &&
                !std::memcmp(base + OpenLootRva, head, sizeof(head))) {
                if (void* trampoline = ai::JumpTo(base + OpenLootRva, reinterpret_cast<void*>(&OpenLoot))) {
                    originalLoot = reinterpret_cast<gFScript>(trampoline);
                    nativeLootGuard = true;
                }
            }
            loot->m_funcScript = &OpenLoot;
            ai::TrackPointer(reinterpret_cast<void*>(&loot->m_funcScript),
                             reinterpret_cast<void*>(nativeLoot),
                             reinterpret_cast<void*>(&OpenLoot), "containers PlayerOpenLoot script");
            originalTrade = trade->m_funcScript;
            trade->m_funcScript = &OpenTrade;
            ai::TrackPointer(reinterpret_cast<void*>(&trade->m_funcScript),
                             reinterpret_cast<void*>(originalTrade),
                             reinterpret_cast<void*>(&OpenTrade), "containers PlayerOpenTrade script");
            hooked = true;
            Log("containers: loot and trade hooks installed");
            Log(nativeLootGuard ? "containers: player loot guard installed at native and script entries"
                            : "containers: player loot guard SCRIPT ONLY; native address/prologue or jump unavailable");
        }
    }
    if (!quickHooked && !quickHookFailed) HookQuickLoot();
    InstallPersonalGuards();
    return PersonalHooksReady();
}

bool PersonalHooksReady()
{
    return !ai::ShuttingDown() && hooked && nativeLootGuard && quickHooked && nativePlunder && nativeTrade &&
           nativeRefreshTrade && nativeBeginTrade && personalStateHooks && nativeTransferItems &&
           nativeTransferStack && nativeTransferAll && nativeTransferTransaction && nativeTransaction && nativeScriptTransfer;
}

bool CapturePersonal(std::vector<uint8_t>& out)
{
    if (!PersonalAdmitted() || !InstallAccessGuards() || !FlushPersonalObservers() ||
        !TrackPersonalFocus() || !PersonalPass(true)) {
        Log("FAIL containers: personal capture refused: admission, access guard or pending restore");
        return false;
    }
    // ProcessedEntities may not include every still-loaded entity. Refresh all resolvable ledger entries now.
    for (auto& entry : personalLedger) {
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(entry.first.data()));
        if (!entity.GetInstance()) continue;
        if (PersonalTeardown(entity)) continue;
        if (personalPending.count(entry.first) && AwaitingDeath(entity, entry.second)) continue;
        if (!EnsurePersonal(entity) || !RememberPersonal(entity)) {
            Log("FAIL containers: personal capture refused: available entry is unresolved");
            return false;
        }
    }
    personal::Snapshot ground;
    items::AppendPersonal(ground);
    personal::Snapshot combined = personalLedger;
    for (auto const& entry : ground) {
        if (entry.second.kind != personal::Kind::GroundTaken || !personal::Valid(entry.second) ||
            !combined.emplace(entry).second) {
            Log("FAIL containers: invalid or conflicting personal pickup record");
            return false;
        }
    }
    if (personalErrorOverflow || !personalErrors.empty() || !personal::Encode(combined, out)) {
        Log("FAIL containers: personal capture exceeds limits or contains an invalid entry");
        return false;
    }
    return true;
}

bool RestorePersonal(std::vector<uint8_t> const& bytes)
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
    personalLedgerBytes = 12;
    for (auto const& entry : personalLedger) personalLedgerBytes += 20 + 25 * entry.second.stacks.size();
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
    if (!InstallAccessGuards()) {
        Log("FAIL containers: personal restore refused: required access guards unavailable");
        return false;
    }
    std::set<std::string> attempted;
    bool ok = PersonalPass(false, &attempted);
    for (auto it = personalPending.begin(); it != personalPending.end();) {
        const std::string key = *it++;
        if (attempted.count(key)) continue;
        Entity entity = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (!entity.GetInstance()) continue;
        if (PersonalTeardown(entity)) continue;
        auto row = personalLedger.find(key);
        if (row != personalLedger.end() && AwaitingDeath(entity, row->second)) continue;
        if (!EnsurePersonal(entity)) ok = false;
    }
    return ok;
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    items::TickPersonal();
    if (disabled) return;
    if (GetTickCount() - lastHookTry > 1000) {
        lastHookTry = GetTickCount();
        InstallAccessGuards();
    }
    PersonalPass(false);
    TrackPersonalTradeClose();
    LocalInventoryTick();
}

void Contents(char const* guid, LogFn log)
{
    uint8_t raw[16];
    Entity container = guid::Parse(guid, raw) ? guid::Resolve(raw) : Entity();
    if (!container.GetInstance() || !HasInventory(container.GetInstance())) { log("REFUSED contents: no container"); return; }
    const std::string line = "CONTENTS guid=" + std::string(guid) + " items=" + Describe(Read(container));
    log(line.c_str());
}

void Open(char const* guid, LogFn log)
{
    uint8_t raw[16];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    // "puppet<id>": another player's puppet (stage Ransack: nobody plunders a knocked-out partner)
    Entity container = !std::strncmp(guid, "puppet", 6) ? Entity(remote::PuppetOf(std::atoi(guid + 6))) : guid::Parse(guid, raw) ? guid::Resolve(raw) : Entity();
    if (!hero || !container.GetInstance()) { log("REFUSED lootopen: no hero or container"); return; }
    // Stand next to it first, as a player does.
    const bCVector& c = container.GetInstance()->GetWorldPosition();
    const float position[3] = {c.GetX() + 100.0f, c.GetY(), c.GetZ()};
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, position, rotation));
    const GEInt result = GetScriptAdminExt().CallScript(bCString("PlayerOpenLoot"), hero, container.GetInstance(), 0);
    char line[160];
    Format(line, "LOOTOPEN guid=%s result=%d active=%d", guid, result, !active.empty());
    log(line);
}

void Take(char const* argument, LogFn log)
{
    // "GUID" takes the first stack, "GUID_Template" the stack of that item template.
    uint8_t raw[16];
    char guid[40] = {};
    strncpy_s(guid, argument, 36);
    char const* wanted = std::strlen(argument) > 37 && argument[36] == '_' ? argument + 37 : nullptr;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity container = guid::Parse(guid, raw) ? guid::Resolve(raw) : Entity();
    if (!hero || !container.GetInstance()) { log("REFUSED loottake: no hero or container"); return; }
    if (personalPending.count(EntityKey(container.GetInstance())) && !EnsurePersonal(container)) {
        log("REFUSED loottake: personal restore unavailable"); return;
    }
    if (!PrepareLocalLoot(container)) { log("REFUSED loottake: personal restore unavailable"); return; }
    for (int i = 0; i < container.Inventory.GetStackCount(); ++i) {
        if (!Counted(container, i)) continue;
        Entity item = container.Inventory.GetTemplateItem(i);
        if (wanted && std::strcmp(item.GetName().GetText(), wanted) != 0) continue;
        const GEInt amount = container.Inventory.GetAmount(i);
        Entity self(hero);
        const GEBool ok = container.Inventory.TransferAmountTo(i, amount, self);
        if (ok) RememberPersonal(container);
        char line[200];
        Format(line, "LOOTTAKE guid=%s name=%s amount=%d ok=%d", guid, item.GetInstance() ? item.GetName().GetText() : "-", amount, ok ? 1 : 0);
        log(line);
        return;
    }
    log("REFUSED loottake: empty");
}

void Report(LogFn log)
{
    char line[700];
    Format(line, "CONTAINERSYNC hooked=%d quick_hooked=%d quick_takes=%u quick_refused=%u active=%d known=%zu pending=%zu contents_sent=%u contents_received=%u rewrites=%u tx_sent=%u tx_applied=%u tx_refused=%u tx_mismatch=%u tx_denied=%u reverted=%u hands_released=%u personal=1 ignored_remote=%u",
              hooked, quickHooked, quickTakes, quickRefused, !active.empty(), known.size(), pending.size(), contentsSent, contentsReceived, rewrites, txSent, txApplied,
              txRefused, txMismatch, txDenied, reverted, handsReleased, ignoredSharedMessages);
    log(line);
}

void Disable(char const* what, LogFn log)
{
    if (!std::strcmp(what, "forget")) {
        forgetOff = true;
        log("CONTAINERSYNCOFF ok=1 forget=0");
        return;
    }
    if (!std::strcmp(what, "hands")) {
        handsOff = true;
        log("CONTAINERSYNCOFF ok=1 hands=0");
        return;
    }
    if (!std::strcmp(what, "trader")) {
        traderFillOff = true;
        log("CONTAINERSYNCOFF ok=1 trader_fill=0");
        return;
    }
    if (!std::strcmp(what, "interact")) {
        interactLoot = false;
        log("CONTAINERSYNCOFF ok=1 interact_loot=0");
        return;
    }
    disabled = true;
    log("CONTAINERSYNCOFF ok=1");
}
}

namespace g3t::containers {
// QA (host): the first stack of a container leaves without a contents message, as if someone took it
// unseen; a client still showing it gets its take refused.
void Silent(char const* argument, LogFn log)
{
    uint8_t raw[16];
    char guid[40] = {};
    strncpy_s(guid, argument, 36);
    char const* wanted = std::strlen(argument) > 37 && argument[36] == '_' ? argument + 37 : nullptr;
    Entity container = guid::Parse(guid, raw) ? guid::Resolve(raw) : Entity();
    if (!net::IsHost() || !container.GetInstance()) { log("REFUSED lootsilent: not the host or no container"); return; }
    for (int i = 0; i < container.Inventory.GetStackCount(); ++i) {
        if (!Counted(container, i)) continue;
        Entity item = container.Inventory.GetTemplateItem(i);
        if (wanted && std::strcmp(item.GetName().GetText(), wanted) != 0) continue;
        char line[200];
        Format(line, "LOOTSILENT guid=%s name=%s amount=%d", guid, item.GetInstance() ? item.GetName().GetText() : "-",
                  container.Inventory.GetAmount(i));
        container.Inventory.DeleteStack(i);
        lastSent[Key(raw)] = Read(container);
        log(line);
        return;
    }
    log("REFUSED lootsilent: empty");
}
}

namespace g3t::containers {
namespace {
// "GUID" or "GUID_Template" (the QA argument alphabet has no other separator).
Entity Target(char const* argument, char const** wanted)
{
    if (!std::strncmp(argument, "puppet", 6)) {  // "puppet<id>[_Template]": a player's puppet (player trade, QA)
        char const* sep = std::strchr(argument, '_');
        *wanted = sep ? sep + 1 : nullptr;
        return Entity(remote::PuppetOf(std::atoi(argument + 6)));
    }
    uint8_t raw[16];
    char guid[40] = {};
    strncpy_s(guid, argument, 36);
    *wanted = std::strlen(argument) > 37 && argument[36] == '_' ? argument + 37 : nullptr;
    return guid::Parse(guid, raw) ? guid::Resolve(raw) : Entity();
}

int StackOf(Entity& entity, char const* wanted, bool skipGold, int quality = -1)
{
    for (int i = 0; i < entity.Inventory.GetStackCount(); ++i) {
        if (entity.Inventory.GetSlotFromStack(i) != gESlot_None) continue;
        if (quality >= 0 && entity.Inventory.GetQuality(i) != static_cast<GEU32>(quality)) continue;
        Entity item = entity.Inventory.GetTemplateItem(i);
        if (!item.GetInstance()) continue;
        char const* name = item.GetName().GetText();
        if (wanted ? std::strcmp(name, wanted) == 0 : !(skipGold && std::strcmp(name, "It_Gold") == 0)) return i;
    }
    return -1;
}

// "Template-q<quality>" (player trade with qualities, 24.10.): the name into `name`, the quality returned; -1 without one.
int SplitQuality(char const* wanted, char (&name)[64])
{
    name[0] = '\0';
    if (!wanted) return -1;
    strncpy_s(name, wanted, _TRUNCATE);
    char* q = std::strstr(name, "-q");
    if (!q) return -1;
    *q = '\0';
    return std::atoi(q + 2);
}
}

// QA: open trade with an NPC the way a trade dialog option does (PlayerOpenTrade), standing next to him.
void TradeOpen(char const* argument, LogFn log)
{
    char const* unused = nullptr;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity trader = Target(argument, &unused);
    if (!hero || !trader.GetInstance()) { log("REFUSED tradeopen: no hero or trader"); return; }
    const bCVector& c = trader.GetInstance()->GetWorldPosition();
    const float position[3] = {c.GetX() + 120.0f, c.GetY(), c.GetZ()};
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, position, rotation));
    const GEInt result = GetScriptAdminExt().CallScript(bCString("PlayerOpenTrade"), hero, trader.GetInstance(), 0);
    char line[200];
    Format(line, "TRADEOPEN trader=%s result=%d active=%d", trader.GetName().GetText(), result, !active.empty());
    log(line);
}

// QA (stageNpcBuy): the trade screen the way a dialog's DoTrade opens it, gCGUIManager::BeginTrade, not PlayerOpenTrade
// (whose hook makes the trader the open container by itself): during a dialog with the trader.
void TradeBegin(char const* argument, LogFn log)
{
    char const* unused = nullptr;
    Entity trader = Target(argument, &unused);
    gCGUIManager* guiManager = gCSession::GetInstance().GetGUIManager();
    if (!guiManager || !trader.GetInstance()) { log("REFUSED tradebegin: no GUI or trader"); return; }
    const GEBool ok = guiManager->BeginTrade(trader.GetInstance());
    char line[200];
    Format(line, "TRADEBEGIN trader=%s ok=%d page=%d", trader.GetName().GetText(), ok ? 1 : 0, static_cast<int>(guiManager->GetActivePage()));
    log(line);
}

// QA: buy one piece (the first non-gold stack, or the named template) and pay its gold value, as the trade
// screen moves the item one way and the gold the other.
void TradeBuy(char const* argument, LogFn log)
{
    char const* wanted = nullptr;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity trader = Target(argument, &wanted);
    if (!hero || !trader.GetInstance()) { log("REFUSED tradebuy: no hero or trader"); return; }
    if (remote::PuppetOwner(trader.GetInstance())) {
        const bool ok = wanted && trade::Exchange(trader.GetInstance(), wanted, 1, true);
        log(ok ? "TRADEBUY price=0 native_exchange=1" : "REFUSED tradebuy: player exchange unavailable");
        return;
    }
    if ((personalPending.count(EntityKey(trader.GetInstance())) && !EnsurePersonal(trader)) || !PrepareLocalTrade(trader)) {
        log("REFUSED tradebuy: personal restore unavailable"); return;
    }
    Entity self(hero);
    char only[64];
    const int quality = SplitQuality(wanted, only);
    const int stack = StackOf(trader, wanted ? only : nullptr, true, quality);
    if (stack < 0) { log("REFUSED tradebuy: nothing to buy"); return; }
    Entity item = trader.Inventory.GetTemplateItem(stack);
    const std::string name = item.GetName().GetText();
    const unsigned stackQuality = trader.Inventory.GetQuality(stack);
    const int price = item.Item.GetGoldValue() > 0 ? item.Item.GetGoldValue() : 1;
    const int gold = StackOf(self, "It_Gold", false);
    if (gold < 0 || self.Inventory.GetAmount(gold) < price) { log("REFUSED tradebuy: not enough gold"); return; }
    const GEBool got = trader.Inventory.TransferAmountTo(stack, 1, self);
    const GEBool paid = self.Inventory.TransferAmountTo(StackOf(self, "It_Gold", false), price, trader);
    if (got || paid) RememberPersonal(trader);
    char line[200];
    Format(line, "TRADEBUY trader=%s name=%s price=%d got=%d paid=%d quality=%u", trader.GetName().GetText(), name.c_str(), price, got ? 1 : 0,
              paid ? 1 : 0, stackQuality);
    log(line);
}

// QA: the other way of the trade window's swap: one piece of the hero's goes to the trader, who pays its value.
void TradeSell(char const* argument, LogFn log)
{
    char const* wanted = nullptr;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity trader = Target(argument, &wanted);
    if (!hero || !trader.GetInstance() || !wanted) { log("REFUSED tradesell: no hero or trader, or no item"); return; }
    if (remote::PuppetOwner(trader.GetInstance())) {
        const bool ok = trade::Exchange(trader.GetInstance(), wanted, 1, false);
        log(ok ? "TRADESELL price=0 native_exchange=1" : "REFUSED tradesell: player exchange unavailable");
        return;
    }
    if ((personalPending.count(EntityKey(trader.GetInstance())) && !EnsurePersonal(trader)) || !PrepareLocalTrade(trader)) {
        log("REFUSED tradesell: personal restore unavailable"); return;
    }
    Entity self(hero);
    char only[64];
    const int quality = SplitQuality(wanted, only);
    const int stack = StackOf(self, only, true, quality);
    if (stack < 0) { log("REFUSED tradesell: nothing to sell"); return; }
    Entity item = self.Inventory.GetTemplateItem(stack);
    const std::string name = item.GetName().GetText();
    const unsigned stackQuality = self.Inventory.GetQuality(stack);
    const int price = item.Item.GetGoldValue() > 0 ? item.Item.GetGoldValue() : 1;
    const int gold = StackOf(trader, "It_Gold", false);
    if (gold < 0 || trader.Inventory.GetAmount(gold) < price) { log("REFUSED tradesell: the trader has not enough gold"); return; }
    const GEBool gave = self.Inventory.TransferAmountTo(stack, 1, trader);
    const GEBool paid = trader.Inventory.TransferAmountTo(StackOf(trader, "It_Gold", false), price, self);
    if (gave || paid) RememberPersonal(trader);
    char line[200];
    Format(line, "TRADESELL trader=%s name=%s price=%d gave=%d paid=%d quality=%u", trader.GetName().GetText(), name.c_str(), price, gave ? 1 : 0,
              paid ? 1 : 0, stackQuality);
    log(line);
}

// QA: inventory contents of any entity (living NPCs included), equipped stacks left out.
void Goods(char const* argument, LogFn log)
{
    char const* unused = nullptr;
    Entity entity = Target(argument, &unused);
    if (!entity.GetInstance() || !HasInventory(entity.GetInstance())) { log("REFUSED goods: no inventory"); return; }
    const std::string line = std::string("GOODS guid=") + std::string(argument, 36) + " items=" + Describe(Read(entity));
    log(line.c_str());
}
}

namespace g3t::containers {
// QA (host): put one piece of an item into an NPC's inventory ("GUID_Template"), to set up a trade.
void Stock(char const* argument, LogFn log)
{
    char const* wanted = nullptr;
    Entity entity = Target(argument, &wanted);
    if (!entity.GetInstance() || !wanted || !HasInventory(entity.GetInstance())) { log("REFUSED stock: need GUID_Template"); return; }
    const GEInt stack = entity.Inventory.CreateItems(Template(wanted), 0, 1);
    char line[160];
    Format(line, "STOCK name=%s stack=%d", wanted, stack);
    log(line);
}
}

namespace g3t::containers {
namespace {
std::string InvStateOf(Entity& entity)
{
    std::string sets;
    Entity const all[5] = {entity.Inventory.GetTreasureSet1(), entity.Inventory.GetTreasureSet2(), entity.Inventory.GetTreasureSet3(),
                           entity.Inventory.GetTreasureSet4(), entity.Inventory.GetTreasureSet5()};
    for (Entity const& set : all)
        if (set.GetInstance()) sets += (sets.empty() ? "" : ",") + std::string(set.GetName().GetText());
    char flags[80];
    Format(flags, " gen_plunder=%d gen_trade=%d generated_stacks=%d", entity.Inventory.IsGeneratedPlunder() ? 1 : 0,
              entity.Inventory.IsGeneratedTrade() ? 1 : 0, entity.Inventory.GetGeneratedStackCount());
    const int locked = entity.GetInstance()->GetPropertySet(eEPropertySetType_Lock) ? (entity.Lock.IsLocked() ? 1 : 0) : -1;
    return std::string(" name=") + entity.GetName().GetText() + flags + " locked=" + std::to_string(locked) + " sets=" + (sets.empty() ? "-" : sets) +
           " items=" + Describe(Read(entity));
}
}

// QA: whether the game has rolled an inventory from its treasure sets yet (plunder or trade), and what it holds.
void InvState(char const* argument, LogFn log)
{
    char const* unused = nullptr;
    Entity entity = Target(argument, &unused);
    if (!entity.GetInstance() || !HasInventory(entity.GetInstance())) { log("REFUSED invstate: no inventory"); return; }
    const std::string line = "INVSTATE guid=" + std::string(argument, 36) + InvStateOf(entity);
    log(line.c_str());
}

// QA: every stack of an inventory, held ones too, and whether it counts as contents (It_FakeBread drift 23.10.).
void Stacks(char const* argument, LogFn log)
{
    char const* unused = nullptr;
    Entity entity = Target(argument, &unused);
    if (!entity.GetInstance() || !HasInventory(entity.GetInstance())) { log("REFUSED stacks: no inventory"); return; }
    const int count = entity.Inventory.GetStackCount();
    const bool npc = entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    char line[300];
    Format(line, "STACKS guid=%.36s name=%s corpse=%d hp=%d aimode=%d count=%d", argument, entity.GetName().GetText(), Corpse(entity) ? 1 : 0,
              npc ? static_cast<int>(entity.DamageReceiver.HitPoints) : -1, npc ? static_cast<int>(entity.Routine.AIMode) : -1, count);
    log(line);
    for (int i = 0; i < count; ++i) {
        Entity item = entity.Inventory.GetTemplateItem(i);
        Format(line, "STACK %d name=%s amount=%d slot=%d category=%d quality=%u counted=%d", i,
                  item.GetInstance() ? item.GetName().GetText() : "-", entity.Inventory.GetAmount(i),
                  static_cast<int>(entity.Inventory.GetSlotFromStack(i)), static_cast<int>(entity.Inventory.GetCategory(i)),
                  entity.Inventory.GetQuality(i), Counted(entity, i) ? 1 : 0);
        log(line);
    }
    log("STACKS end");
}

// QA: one piece of an item into an NPC's right hand ("GUID_Template"), as a slave holds the bread it eats (FakeBread 23.10.).
void HoldItem(char const* argument, LogFn log)
{
    char const* wanted = nullptr;
    Entity entity = Target(argument, &wanted);
    if (!entity.GetInstance() || !wanted || !HasInventory(entity.GetInstance())) { log("REFUSED holditem: need GUID_Template"); return; }
    const GEInt stack = entity.Inventory.CreateItems(Template(wanted), 0, 1);
    const bool held = stack >= 0 && entity.Inventory.HoldStacks(-1, stack);
    const Entity right = entity.Inventory.GetItemFromSlot(gESlot_RightHand);
    char line[200];
    Format(line, "HOLDITEM name=%s stack=%d held=%d right=%s", wanted, stack, held ? 1 : 0, right.GetInstance() ? right.GetName().GetText() : "-");
    log(line);
}

// QA: chests near the hero (an inventory, no NPC, no item), nearest first, with their roll state.
void Chests(LogFn log)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("CHESTS end count=0"); return; }
    const bCVector& h = hero->GetWorldPosition();
    const float at[3] = {h.GetX(), h.GetY(), h.GetZ()};
    std::multimap<float, std::string> found;
    const bTObjArray<Entity> all = Entity::GetEntities();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        eCEntity* instance = entity.GetInstance();
        if (!HasInventory(instance) || entity.IsItem() || instance == hero || instance->GetPropertySet(eEPropertySetType_NPC)) continue;
        const float distance = DistanceTo(instance, at);
        if (distance > InterestRadius) continue;
        char guid[48];
        const eCEntityProxy proxy(instance);
        _GUID raw;
        std::memcpy(&raw, &proxy.GetEntityID().GetCore(), 16);
        Format(guid, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", raw.Data1, raw.Data2, raw.Data3, raw.Data4[0], raw.Data4[1],
                  raw.Data4[2], raw.Data4[3], raw.Data4[4], raw.Data4[5], raw.Data4[6], raw.Data4[7]);
        char dist[32];
        Format(dist, " distance_cm=%d", static_cast<int>(distance));
        found.emplace(distance, std::string("CHEST guid=") + guid + dist + InvStateOf(entity));
    }
    for (auto const& [distance, line] : found) log(line.c_str());
    const std::string end = "CHESTS end count=" + std::to_string(found.size());
    log(end.c_str());
}
// QA negative controls of the play-evening fixes (27.10.): each switches its fix off (on: clientfameon).
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "dialogtradeoff")) { dialogTrade = false; log("SWITCH dialogtradeoff ok=1"); return true; }
    if (!std::strcmp(action, "tradekeepoff")) { tradeKeep = false; log("SWITCH tradekeepoff ok=1"); return true; }
    if (!std::strcmp(action, "tradekeepon")) { tradeKeep = true; log("SWITCH tradekeepon ok=1"); return true; }
    if (!std::strcmp(action, "tradebaseoff")) { tradeBase = false; log("SWITCH tradebaseoff ok=1"); return true; }
    if (!std::strcmp(action, "tradeframeoff")) { tradeFrame = false; log("SWITCH tradeframeoff ok=1"); return true; }
    if (!std::strcmp(action, "tradesnapoff")) { tradeSnap = false; log("SWITCH tradesnapoff ok=1"); return true; }
    if (!std::strcmp(action, "tradesnapon")) { tradeSnap = true; log("SWITCH tradesnapon ok=1"); return true; }
    if (!std::strcmp(action, "tradeframeon")) { tradeFrame = true; log("SWITCH tradeframeon ok=1"); return true; }
    if (!std::strcmp(action, "tradereportdelayon")) { reportDelay = true; log("SWITCH tradereportdelayon ok=1"); return true; }
    if (!std::strcmp(action, "corpseplunderoff")) { corpsePlunder = false; log("SWITCH corpseplunderoff ok=1"); return true; }
    if (!std::strcmp(action, "hostfilloff")) { hostFill = false; log("SWITCH hostfilloff ok=1"); return true; }
    if (!std::strcmp(action, "merchandiseoff")) { merchandise = false; log("SWITCH merchandiseoff ok=1"); return true; }
    return false;
}
}
