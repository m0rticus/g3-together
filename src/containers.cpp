// SPDX-License-Identifier: GPL-3.0-or-later
#include "containers.h"

#include "ai.h"
#include "body.h"
#include "downed.h"
#include "guid.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "personal_ledger.h"
#include "personal_store.h"
#include "remote.h"
#include "telemetry.h"
#include "trade.h"
#include "witness.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace g3t::containers {
namespace {
constexpr float PreRollRadius = 1500.0f;
unsigned ignoredSharedMessages = 0;
DWORD lastLocalPoll = 0;
namespace personal = saving::personal;
using ledger::Corpse;
using ledger::EnsurePersonal;
using ledger::EntityKey;
using ledger::IsChest;
using ledger::IsContainer;
using ledger::IsPending;
using ledger::IsRestored;
using ledger::IsTrader;
using ledger::MarkTradeOpened;
using ledger::NameOf;
using ledger::PersonalAdmitted;
using ledger::PersonalKind;
using ledger::PersonalMutation;
using ledger::PersonalPass;
using ledger::PersonalTeardown;
using ledger::RememberPersonal;
using ledger::Shared;
using ledger::TrackPersonalFocus;
using ledger::TrackTradeClose;

unsigned notes = 0;
constexpr unsigned MaxNotes = 600;

void Note(char const* format, ...)
{
    if (notes > MaxNotes) return;
    if (notes++ == MaxNotes) { Log("containers: log limit reached; no more container lines this session"); return; }
    char line[700];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    Log(line);
}

float DistanceTo(eCEntity* entity, float const at[3])
{
    const bCVector& p = entity->GetWorldPosition();
    const float q[3] = {p.GetX(), p.GetY(), p.GetZ()};
    return body::Distance(q, at);
}

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
    const bool wasPending = entity.GetInstance() && IsPending(EntityKey(entity.GetInstance()));
    if (wasPending && !EnsurePersonal(entity)) return GEFalse;
    personal::Kind kind;
    if (!PersonalKind(entity, kind)) return original(inventory, edx);
    if (!PersonalAdmitted() || !EnsurePersonal(entity)) return GEFalse;
    if ((wasPending || IsRestored(EntityKey(entity.GetInstance()))) &&
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

bool IsLootPageOpen()
{
    gCGUIManager* guiManager = gCSession::GetInstance().GetGUIManager();
    return guiManager && guiManager->IsAnyPageOpen() && guiManager->GetActivePage() == gEGamePage_Loot;
}

bool BlockWitnessedChestLoot(eCEntity* chestEntity)
{
    Entity chest(chestEntity);
    if (!chestEntity || !IsChest(chest) || !witness::PerceiveChestUse(chestEntity)) return false;
    telemetry::OnContainerAccess("chest_witness_blocked", gCSession::GetInstance().GetHero(), chestEntity, false, "blocked");
    if (IsLootPageOpen()) gCSession::GetInstance().GetGUIManager()->ClosePage();
    return true;
}

bool ChestTakeBlockedByWitness(gCInventory_PS* from, gCInventory_PS* to)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return hero && from && to && to->GetEntity() == hero && BlockWitnessedChestLoot(from->GetEntity());
}

GEBool __fastcall RefreshPersonalTrade(gCInventory_PS* inventory, void* edx)
{
    if (!nativeRefreshTrade) return GEFalse;
    if (ai::ShuttingDown() || !inventory) return nativeRefreshTrade(inventory, edx);
    Entity entity(inventory->GetEntity());
    if (entity.GetInstance() && IsPending(EntityKey(entity.GetInstance())) && !EnsurePersonal(entity)) return GEFalse;
    if (!IsTrader(entity) || remote::IsPuppet(entity.GetInstance())) return nativeRefreshTrade(inventory, edx);
    if (!EnsurePersonal(entity) || !PersonalAdmitted()) return GEFalse;
    if (IsRestored(EntityKey(entity.GetInstance())) && entity.Inventory.IsGeneratedTrade()) return GETrue;
    PersonalMutation mutation;
    if (!mutation.Enter(inventory)) return GEFalse;
    return nativeRefreshTrade(inventory, edx);
}

template <typename Call>
GEBool GuardedInventoryTransfer(char const* mode, gCInventory_PS* from, gCInventory_PS* to, Call&& call, int stack = -1, int amount = -1)
{
    if (ai::ShuttingDown() || !PersonalPlayerTransfer(from, to)) return call();
    if (ChestTakeBlockedByWitness(from, to)) return GEFalse;
    PersonalMutation mutation;
    if (!mutation.Enter(from) || !mutation.Enter(to)) return GEFalse;
    telemetry::TransferScope transferScope(mode, from ? from->GetEntity() : nullptr, to ? to->GetEntity() : nullptr, stack, amount);
    return transferScope.Finish(call() != GEFalse) ? GETrue : GEFalse;
}

GEBool __fastcall TransferPersonalItems(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                       GEInt stack, GEInt amount, gEStackType type)
{
    if (!nativeTransferItems) return GEFalse;
    return GuardedInventoryTransfer("transfer_items", from, to, [&] { return nativeTransferItems(from, edx, to, stack, amount, type); }, stack, amount);
}

GEBool __fastcall TransferPersonalStack(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                       GEInt stack, gEStackType type)
{
    if (!nativeTransferStack) return GEFalse;
    return GuardedInventoryTransfer("transfer_stack", from, to, [&] { return nativeTransferStack(from, edx, to, stack, type); }, stack, -1);
}

GEBool __fastcall TransferPersonalAll(gCInventory_PS* from, void* edx, gCInventory_PS* to, gEStackType type)
{
    if (!nativeTransferAll) return GEFalse;
    return GuardedInventoryTransfer("transfer_all", from, to, [&] { return nativeTransferAll(from, edx, to, type); });
}

GEBool __fastcall TransferPersonalTransaction(gCInventory_PS* from, void* edx, gCInventory_PS* to, gEStackType type)
{
    if (!nativeTransferTransaction) return GEFalse;
    return GuardedInventoryTransfer("transfer_tx", from, to, [&] { return nativeTransferTransaction(from, edx, to, type); });
}

GEBool __fastcall PerformPersonalTransaction(gCInventory_PS* from, void* edx, gCInventory_PS* to,
                                            gEStackType fromType, gEStackType toType)
{
    if (!nativeTransaction) return GEFalse;
    return GuardedInventoryTransfer("perform_tx", from, to, [&] { return nativeTransaction(from, edx, to, fromType, toType); });
}

GEBool __fastcall TransferPersonalScript(PSInventory* from, void* edx, GEInt stack, GEInt amount, Entity& to)
{
    if (!nativeScriptTransfer) return GEFalse;
    auto* property = from ? reinterpret_cast<EntityPropertySet*>(from)->m_pEngineEntityPropertySet : nullptr;
    auto* inventory = property ? static_cast<gCInventory_PS*>(property) : nullptr;
    auto* target = to.GetInstance()
        ? static_cast<gCInventory_PS*>(to.GetInstance()->GetPropertySet(eEPropertySetType_Inventory)) : nullptr;
    return GuardedInventoryTransfer("script_transfer", inventory, target, [&] { return nativeScriptTransfer(from, edx, stack, amount, to); }, stack, amount);
}

GEBool __fastcall BeginPersonalTrade(gCGUIManager* manager, void* edx, eCEntity* partner)
{
    if (!nativeBeginTrade) return GEFalse;
    if (ai::ShuttingDown()) return nativeBeginTrade(manager, edx, partner);
    Entity entity(partner);
    if (partner && IsPending(EntityKey(partner)) && !EnsurePersonal(entity)) return GEFalse;
    const bool trader = IsTrader(entity) && !remote::IsPuppet(partner);
    const std::string key = trader ? EntityKey(partner) : std::string();
    if (trader) {
        if (!PersonalAdmitted() || !EnsurePersonal(entity)) return GEFalse;
        RememberPersonal(entity);
    }
    const GEBool result = nativeBeginTrade(manager, edx, partner);
    if (result && !key.empty() && IsRestored(key)) MarkTradeOpened(key);
    telemetry::OnContainerAccess("begin_trade", gCSession::GetInstance().GetHero(), partner, true, result ? "opened" : "failed");
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
    const bool hudLoop = !std::strcmp(name, "PS_HUD_Loop");
    Entity* chest = local && IsChest(target) ? &target : local && IsChest(focus) ? &focus : nullptr;
    if (chest) {
        if (!PersonalAdmitted() || !EnsurePersonal(*chest)) {
            if (hudLoop) return original->second(stack, spu);
            return GEFalse;
        }
        RememberPersonal(*chest);
    }
    if (local && chest && !std::strcmp(name, "PS_Interact_Chest_Loop") && IsLootPageOpen()) {
        BlockWitnessedChestLoot(chest->GetInstance());
    }
    const std::string key = chest ? EntityKey(chest->GetInstance()) : std::string();
    const GEBool result = original->second(stack, spu);
    if (!key.empty()) {
        Entity current = guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
        if (current.GetInstance() && !PersonalTeardown(current)) RememberPersonal(current);
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

template <typename GenerateFn>
bool PreparePersonalInventory(Entity& entity, bool eligible, bool alreadyGenerated, GenerateFn&& generate)
{
    if (!eligible || remote::IsPuppet(entity.GetInstance())) return true;
    if (!PersonalAdmitted() || !EnsurePersonal(entity)) return false;
    if (!alreadyGenerated) generate();
    RememberPersonal(entity);
    return true;
}

bool PrepareLocalLoot(Entity& entity)
{
    return PreparePersonalInventory(entity, IsContainer(entity), entity.Inventory.IsGeneratedPlunder() != GEFalse, [&] {
        if (!entity.Inventory.GeneratePlunderInvFromTreasureSets())
            Note("containers: no generated personal loot sets for %s", NameOf(entity).c_str());
        entity.Inventory.SetGeneratedPlunder(GETrue);
        Note("containers: personal loot generated once for %s", NameOf(entity).c_str());
    });
}

bool PrepareLocalTrade(Entity& entity)
{
    return PreparePersonalInventory(entity, IsTrader(entity), entity.Inventory.IsGeneratedTrade() != GEFalse, [&] {
        if (!entity.Inventory.GenerateTradeInvFromTreasureSets())
            Note("containers: no generated personal stock sets for %s", NameOf(entity).c_str());
        if (static_cast<GEFloat>(entity.Dialog.EndDialogTimestamp) == 0.0f)
            if (!entity.Inventory.RefreshTradeInvFromTreasureSets())
                Note("containers: personal stock refresh skipped for %s", NameOf(entity).c_str());
        if (auto* inv = static_cast<gCInventory_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory)))
            inv->AccessGeneratedTrade() = GETrue;
        Note("containers: personal merchant stock prepared for %s", NameOf(entity).c_str());
    });
}

void LocalInventoryTick()
{
    if (!PersonalAdmitted()) return;
    if (eCEntity* hero = gCSession::GetInstance().GetHero()) {
        Entity focus = Entity(hero).Focus.GetFocusEntity();
        if (focus.GetInstance() && Corpse(focus)) PrepareLocalLoot(focus);
    }
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
    const bCVector& p = hero->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity entity = all.GetAt(i);
        eCEntity* instance = entity.GetInstance();
        if (instance && DistanceTo(instance, here) <= PreRollRadius && Corpse(entity)) PrepareLocalLoot(entity);
    }
}

gFScript originalLoot = nullptr, originalTrade = nullptr;
bool hooked = false, nativeLootGuard = false;
constexpr uintptr_t OpenLootRva = 0x6bfd0;
DWORD lastHookTry = 0;

bool Opened(gCScriptProcessingUnit* spu, GELPVoid other, bool trade)
{
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    if (trade && partner.GetInstance() && trade::OnOpen(partner.GetInstance())) return true;
    if (!partner.GetInstance()) return true;
    if (IsPending(EntityKey(partner.GetInstance())) && !EnsurePersonal(partner)) return false;
    return trade ? PrepareLocalTrade(partner) : PrepareLocalLoot(partner);
}

void ExitGonnaLoot(eCEntity* actor)
{
    if (!actor || actor != gCSession::GetInstance().GetHero() || downed::CombatUnavailable(actor)) return;
    if (!actor->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;
    Entity hero(actor);
    if (std::strcmp(hero.Routine.GetCurrentState().GetText(), "PS_GonnaLoot") != 0) return;
    hero.Routine.FullStop();
    hero.Routine.ContinueRoutine();
}

GEInt GE_STDCALL OpenLoot(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalLoot ? originalLoot(spu, self, other, args) : 0;
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    if (remote::PuppetOwner(partner.GetInstance())) {
        eCEntity* actor = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
        if (remote::IsHeldPuppet(partner.GetInstance()) && actor &&
            actor == gCSession::GetInstance().GetHero() && !downed::CombatUnavailable(actor)) {
            const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTalk"), actor, partner.GetInstance(), 0);
            remote::LogPuppetInteraction(partner.GetInstance(), "loot-rerouted-to-talk", result);
            if (!result) ExitGonnaLoot(actor);
            return result;
        }
        remote::LogPuppetInteraction(partner.GetInstance(), "loot-refused", 0);
        Note("containers: loot of %s refused (another player)", NameOf(partner).c_str());
        ExitGonnaLoot(actor);
        return 0;
    }
    if (!Opened(spu, other, false)) {
        eCEntity* actor = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
        ExitGonnaLoot(actor);
        telemetry::OnContainerAccess("open_loot", gCSession::GetInstance().GetHero(), partner.GetInstance(), false, "refused_pending");
        return 0;
    }
    telemetry::OnContainerAccess("open_loot", gCSession::GetInstance().GetHero(), partner.GetInstance(), false, "opened");
    return originalLoot(spu, self, other, args);
}

GEInt GE_STDCALL OpenTrade(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalTrade ? originalTrade(spu, self, other, args) : 0;
    Entity partner = other ? *static_cast<Entity*>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
    if (remote::PuppetOwner(partner.GetInstance()) && !trade::Available()) {
        Log("trade: native player exchange unavailable; opening refused");
        telemetry::OnContainerAccess("open_player_trade", gCSession::GetInstance().GetHero(), partner.GetInstance(), true, "refused_unavailable");
        return 0;
    }
    if (!Opened(spu, other, true)) {
        telemetry::OnContainerAccess("open_trade", gCSession::GetInstance().GetHero(), partner.GetInstance(), true, "refused_pending");
        return 0;
    }
    telemetry::OnContainerAccess("open_trade", gCSession::GetInstance().GetHero(), partner.GetInstance(), true, "opened");
    return originalTrade(spu, self, other, args);
}

constexpr uintptr_t QuickLootCall = 0x81ced;
bool quickHooked = false, quickHookFailed = false;
unsigned quickTakes = 0, quickRefused = 0;

GEBool __fastcall QuickTake(PSInventory* self, void*, GEInt stack, GEInt amount, Entity& to)
{
    if (ai::ShuttingDown()) return self ? self->TransferAmountTo(stack, amount, to) : GEFalse;
    if (!self) return GEFalse;
    eCEntity* owner = nullptr;
    if (auto* set = reinterpret_cast<EntityPropertySet*>(self)->m_pEngineEntityPropertySet) owner = set->GetEntity();
    if (remote::PuppetOwner(owner)) { ++quickRefused; return GEFalse; }
    Entity entity(owner);
    const bool toHero = owner && to.GetInstance() == gCSession::GetInstance().GetHero();
    const bool wasPending = toHero && IsPending(EntityKey(owner));
    if (wasPending) {
        EnsurePersonal(entity);
        ++quickRefused;
        return GEFalse;
    }
    personal::Kind kind;
    if (toHero && PersonalKind(entity, kind)) {
        if (!PersonalAdmitted()) { ++quickRefused; return GEFalse; }
        if (!EnsurePersonal(entity)) { ++quickRefused; return GEFalse; }
        if (BlockWitnessedChestLoot(owner)) {
            ++quickRefused;
            return GEFalse;
        }
        if (kind == personal::Kind::Corpse && !entity.Inventory.IsGeneratedPlunder() &&
            !PrepareLocalLoot(entity)) {
            ++quickRefused;
            return GEFalse;
        }
        RememberPersonal(entity);
        const std::string key = EntityKey(owner);
        telemetry::TransferScope transferScope("quickloot", owner, to.GetInstance(), stack, amount);
        const GEBool moved = transferScope.Finish(self->TransferAmountTo(stack, amount, to) != GEFalse) ? GETrue : GEFalse;
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
    site[0] = 0xE8;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(&QuickTake) - reinterpret_cast<intptr_t>(site + 5));
    std::memcpy(site + 1, &to, 4);
    site[5] = 0x90;
    std::memcpy(installed, site, sizeof(installed));
    DWORD restoredOld = 0;
    const bool restored = VirtualProtect(site, 6, old, &restoredOld) != 0;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), site, 6) != 0;
    ai::TrackPatch(site, original, installed, sizeof(original), "containers quick loot call");
    if (!restored || !flushed) {
        Logf("WARN containers: quick loot hook finalize warning restored=%d flushed=%d error=%lu",
             restored ? 1 : 0, flushed ? 1 : 0, GetLastError());
    }
    quickHooked = true;
    Log("containers: quick loot hook installed");
}

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

bool AreAllPersonalHooksReady()
{
    return !ai::ShuttingDown() && hooked && nativeLootGuard && quickHooked && nativePlunder && nativeTrade &&
           nativeRefreshTrade && nativeBeginTrade && personalStateHooks && nativeTransferItems &&
           nativeTransferStack && nativeTransferAll && nativeTransferTransaction && nativeTransaction &&
           nativeScriptTransfer;
}
} // namespace

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
    ledger::Reset();
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
    return AreAllPersonalHooksReady();
}

bool PersonalHooksReady()
{
    if (AreAllPersonalHooksReady()) return true;
    return !ai::ShuttingDown() && gCSession::GetInstance().IsGameRunning() && InstallAccessGuards();
}

void PrepareCorpseLoot(eCEntity* entity)
{
    if (!entity || ai::ShuttingDown() || !gCSession::GetInstance().IsGameRunning()) return;
    Entity target(entity);
    if (Corpse(target)) PrepareLocalLoot(target);
}

bool CapturePersonal(std::vector<uint8_t>& out)
{
    return ledger::Capture(out, &PersonalHooksReady);
}

bool RestorePersonal(std::vector<uint8_t> const& bytes)
{
    return ledger::Restore(bytes, &PersonalHooksReady);
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    items::TickPersonal();
    if (GetTickCount() - lastHookTry > 1000) {
        lastHookTry = GetTickCount();
        InstallAccessGuards();
    }
    PersonalPass(false);
    TrackTradeClose();
    LocalInventoryTick();
}
} // namespace g3t::containers
