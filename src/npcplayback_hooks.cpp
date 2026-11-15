// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcplayback_hooks.h"

#include "ai.h"
#include "downed.h"
#include "guards.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"
#include "talk_policy.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace g3t::npcplayback::detail {
namespace {

std::unordered_map<eCWrapper_emfx2Actor*, std::array<CapturedOptions, Slots>> sourceOptions;
std::mutex sourceOptionsMutex;
uint32_t nextInstance = 0;

HookCounters counters;
thread_local NativePose* presentingPose = nullptr;
thread_local bool presenting = false;
thread_local bool updatingVisual = false;
thread_local eCEntity* visualEntity = nullptr;
thread_local eCEntity const* verdictEntity = nullptr;
thread_local char verdictTask[64] = {};
thread_local eCEntity* originEntity = nullptr;
thread_local uint8_t originPlayer = 0;
thread_local uint32_t originSequence = 0;

using ProcessFn = void(__fastcall*)(eCEntityPropertySet*, void*);
using ScriptFn = bEResult(__fastcall*)(gCScriptProcessingUnit*, void*);
using EventScriptFn = GEInt(__fastcall*)(gCScriptAdmin*, void*, bCString const&, eCEntity*, eCEntity*, GEInt);
using PlayFn = void(__fastcall*)(eCVisualAnimation_PS*, void*, eCWrapper_emfx2Actor::eEMotionType,
                                 eCWrapper_emfx2Motion::eSMotionDesc*);
using ActorPlayFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, eCWrapper_emfx2Actor::eEMotionType,
                                      eCWrapper_emfx2Motion&, eCWrapper_emfx2Motion::eSMotionDesc*);
using FacialFn = GEBool(__fastcall*)(eCVisualAnimationFactory*, void*);
using StopFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, eCWrapper_emfx2Actor::eEMotionType, float);
using TaskFn = void(__fastcall*)(gCScriptRoutine_PS*, void*, bCString const&);
using AddPropertyFn = GEBool(__fastcall*)(eCEntity*, void*, eCEntityPropertySet*, GEBool);
using TriggerFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCEntity*);
using TouchFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCContactIterator&);
using DamageFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCEntity*, GEInt, GEInt, eCContactIterator&);

ProcessFn originalRoutine = nullptr, originalMovement = nullptr, originalMovementUpdated = nullptr;
ScriptFn originalScript = nullptr;
EventScriptFn originalEventScript = nullptr;
PlayFn originalPlay = nullptr;
ActorPlayFn originalActorPlay = nullptr;
ActorProcessFn originalActorProcess = nullptr;
FacialFn originalFacialStart = nullptr, originalFacialStop = nullptr;
StopFn originalStop = nullptr;
TaskFn originalSetTask = nullptr, originalAISetTask = nullptr;
AddPropertyFn originalAddProperty = nullptr;

struct PropertyTable {
    void** table = nullptr;
    ProcessFn original[3] = {};
    void* events[6] = {};
};
std::array<PropertyTable, 128> propertyTables;
std::atomic<unsigned> propertyTableCount{0};
std::mutex propertyTableMutex;

// The one NPC our hero is talking to: while in stationary talk on the client, its local script routine, animation, facial
// lip-sync, and in-place facing run smoothly for gCInfoManager_PS while slot-0 gestures are also forwarded to the host.
bool DialogPartner(eCEntity const* entity) { return entity && talk::TalkingWithHero(entity); }

bool SafeNativeProperty(eEPropertySetType type)
{
    switch (type) {
    case eEPropertySetType_Base:
    case eEPropertySetType_MeshVisual:
    case eEPropertySetType_DummyVisual:
    case eEPropertySetType_Advanced:
    case eEPropertySetType_DynamicCollisionCircle:
    case eEPropertySetType_CollisionCircle:
    case eEPropertySetType_RigidBody:
    case eEPropertySetType_CollisionShape:
    case eEPropertySetType_NPC:
    case eEPropertySetType_Inventory:
    case eEPropertySetType_Damage:
    case eEPropertySetType_DamageReceiver:
    case eEPropertySetType_Interaction:
    case eEPropertySetType_Focus:
    case eEPropertySetType_Party:
    case eEPropertySetType_ArmorSet:
    case eEPropertySetType_Item:
    case eEPropertySetType_EditorVisual:
        return true;
    default:
        return false;
    }
}

PropertyTable const* PropertyHooks(eCEntityPropertySet* self)
{
    void** table = *reinterpret_cast<void***>(self);
    const unsigned count = propertyTableCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) if (propertyTables[i].table == table) return &propertyTables[i];
    Log("FAIL npcplayback: missing event dispatch table");
    return nullptr;
}

void PropertyDispatch(eCEntityPropertySet* self, void* edx, unsigned phase)
{
    if (ai::ShuttingDown()) {
        if (self) {
            void** table = *reinterpret_cast<void***>(self);
            const unsigned count = propertyTableCount.load(std::memory_order_acquire);
            for (unsigned i = 0; i < count; ++i)
                if (propertyTables[i].table == table) { propertyTables[i].original[phase](self, edx); return; }
        }
        return;
    }
    const eEPropertySetType type = self->GetPropertySetType();
    const bool partnerProperty = talk_policy::ShouldAllowDialogPartnerProperty(
        DialogPartner(self->GetEntity()),
        type == eEPropertySetType_ScriptRoutine,
        type == eEPropertySetType_Animation,
        type == eEPropertySetType_CharacterMovement,
        type == eEPropertySetType_Dialog);
    if (!partnerProperty && IsPassive(self->GetEntity()) &&
        !SafeNativeProperty(type) && !(updatingVisual && type == eEPropertySetType_Animation)) {
        ++counters.blockedProperties;
        return;
    }
    void** table = *reinterpret_cast<void***>(self);
    const unsigned count = propertyTableCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i)
        if (propertyTables[i].table == table) { propertyTables[i].original[phase](self, edx); return; }
    Log("FAIL npcplayback: unregistered property dispatch table");
}

void __fastcall PropertyProcess(eCEntityPropertySet* self, void* edx) { PropertyDispatch(self, edx, 0); }
void __fastcall PropertyPre(eCEntityPropertySet* self, void* edx) { PropertyDispatch(self, edx, 1); }
void __fastcall PropertyPost(eCEntityPropertySet* self, void* edx) { PropertyDispatch(self, edx, 2); }

void TriggerEvent(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b, unsigned index)
{
    if (ai::ShuttingDown()) {
        if (auto* hooks = PropertyHooks(self))
            reinterpret_cast<TriggerFn>(hooks->events[index])(self, edx, a, b);
        return;
    }
    if (IsPassive(self->GetEntity())) { ++counters.blockedEvents; return; }
    if (auto* hooks = PropertyHooks(self)) reinterpret_cast<TriggerFn>(hooks->events[index])(self, edx, a, b);
}
void __fastcall PropertyTrigger(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b) { TriggerEvent(self, edx, a, b, 0); }
void __fastcall PropertyUntrigger(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b) { TriggerEvent(self, edx, a, b, 1); }

void TouchEvent(eCEntityPropertySet* self, void* edx, eCEntity* other, eCContactIterator& contacts, unsigned index)
{
    if (ai::ShuttingDown()) {
        if (auto* hooks = PropertyHooks(self))
            reinterpret_cast<TouchFn>(hooks->events[index])(self, edx, other, contacts);
        return;
    }
    if (IsPassive(self->GetEntity())) { ++counters.blockedEvents; return; }
    if (auto* hooks = PropertyHooks(self)) reinterpret_cast<TouchFn>(hooks->events[index])(self, edx, other, contacts);
}
void __fastcall PropertyTouch(eCEntityPropertySet* self, void* edx, eCEntity* other, eCContactIterator& contacts)
{ TouchEvent(self, edx, other, contacts, 2); }
void __fastcall PropertyIntersect(eCEntityPropertySet* self, void* edx, eCEntity* other, eCContactIterator& contacts)
{ TouchEvent(self, edx, other, contacts, 3); }
void __fastcall PropertyUntouch(eCEntityPropertySet* self, void* edx, eCEntity* other, eCContactIterator& contacts)
{ TouchEvent(self, edx, other, contacts, 4); }

void __fastcall PropertyDamage(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b, GEInt c, GEInt d,
                               eCContactIterator& contacts)
{
    if (ai::ShuttingDown()) {
        if (auto* hooks = PropertyHooks(self))
            reinterpret_cast<DamageFn>(hooks->events[5])(self, edx, a, b, c, d, contacts);
        return;
    }
    if (IsPassive(self->GetEntity()) && self->GetPropertySetType() != eEPropertySetType_DamageReceiver) {
        ++counters.blockedEvents;
        return;
    }
    if (auto* hooks = PropertyHooks(self)) reinterpret_cast<DamageFn>(hooks->events[5])(self, edx, a, b, c, d, contacts);
}

bool RegisterProperty(eCEntityPropertySet* property)
{
    if (!property) return false;
    void** table = *reinterpret_cast<void***>(property);
    std::lock_guard<std::mutex> lock(propertyTableMutex);
    const unsigned count = propertyTableCount.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) if (propertyTables[i].table == table) return true;
    if (count == propertyTables.size()) { Log("FAIL npcplayback: too many native property classes"); return false; }
    // CP 1.75's property dispatch loop calls OnProcess at byte 0x124; pre/post follow it.
    constexpr unsigned first = 0x124 / sizeof(void*);
    constexpr unsigned event = 0xc8 / sizeof(void*);
    constexpr size_t bytes = (first + 3 - event) * sizeof(void*);
    DWORD old = 0;
    if (!VirtualProtect(table + event, bytes, PAGE_READWRITE, &old)) {
        Log("FAIL npcplayback: cannot guard native NPC property dispatch");
        return false;
    }
    std::vector<unsigned char> originalTable(reinterpret_cast<unsigned char*>(table + event),
                                             reinterpret_cast<unsigned char*>(table + event) + bytes);
    PropertyTable& hook = propertyTables[count];
    hook.table = table;
    for (unsigned i = 0; i < 3; ++i) hook.original[i] = reinterpret_cast<ProcessFn>(table[first + i]);
    for (unsigned i = 0; i < 6; ++i) hook.events[i] = table[event + i];
    propertyTableCount.store(count + 1, std::memory_order_release);
    table[first] = reinterpret_cast<void*>(&PropertyProcess);
    table[first + 1] = reinterpret_cast<void*>(&PropertyPre);
    table[first + 2] = reinterpret_cast<void*>(&PropertyPost);
    table[event] = reinterpret_cast<void*>(&PropertyTrigger);
    table[event + 1] = reinterpret_cast<void*>(&PropertyUntrigger);
    table[event + 2] = reinterpret_cast<void*>(&PropertyTouch);
    table[event + 3] = reinterpret_cast<void*>(&PropertyIntersect);
    table[event + 4] = reinterpret_cast<void*>(&PropertyUntouch);
    table[event + 5] = reinterpret_cast<void*>(&PropertyDamage);
    std::vector<unsigned char> installedTable(reinterpret_cast<unsigned char*>(table + event),
                                               reinterpret_cast<unsigned char*>(table + event) + bytes);
    DWORD ignored = 0;
    if (!VirtualProtect(table + event, bytes, old, &ignored))
        Log("WARN npcplayback: property dispatch page protection could not be restored");
    ai::TrackPatch(table + event, originalTable.data(), installedTable.data(), bytes, "npcplayback property dispatch vtable");
    return true;
}

GEBool __fastcall AddProperty(eCEntity* self, void* edx, eCEntityPropertySet* property, GEBool flag)
{
    if (ai::ShuttingDown()) return originalAddProperty ? originalAddProperty(self, edx, property, flag) : GEFalse;
    if (Available() && (!net::IsHost() || npclease::Fenced(self)) && property && !RegisterProperty(property)) {
        Log("FAIL npcplayback: refusing an unguarded property during client entity creation");
        return GEFalse;
    }
    return originalAddProperty(self, edx, property, flag);
}

void __fastcall Routine(eCEntityPropertySet* self, void* edx)
{
    if (ai::ShuttingDown()) { if (originalRoutine) originalRoutine(self, edx); return; }
    eCEntity* entity = self->GetEntity();
    if (IsPassive(entity) && !DialogPartner(entity)) { ++counters.blockedRoutines; return; }
    const uint8_t player = talk_policy::EffectiveActingPlayer(net::IsHost(), talk::IsHeld(entity), npcs::ContextPlayer(entity));
    guards::Acting context(player > 1 ? remote::PuppetOf(player) : nullptr);
    originalRoutine(self, edx);
}

void __fastcall Movement(eCEntityPropertySet* self, void* edx)
{
    if (ai::ShuttingDown()) { if (originalMovement) originalMovement(self, edx); return; }
    if (IsPassive(self->GetEntity()) && !DialogPartner(self->GetEntity())) { ++counters.blockedMovement; return; }
    originalMovement(self, edx);
}

void __fastcall MovementUpdated(eCEntityPropertySet* self, void* edx)
{
    if (ai::ShuttingDown()) { if (originalMovementUpdated) originalMovementUpdated(self, edx); return; }
    if (IsPassive(self->GetEntity()) && !DialogPartner(self->GetEntity())) { ++counters.blockedMovement; return; }
    originalMovementUpdated(self, edx);
}

bEResult __fastcall Script(gCScriptProcessingUnit* self, void* edx)
{
    if (ai::ShuttingDown()) return originalScript ? originalScript(self, edx) : bEResult_Ok;
    eCEntity* entity = self->GetSelfEntity();
    if (IsPassive(entity) && !DialogPartner(entity)) { ++counters.blockedScripts; return bEResult_Ok; }
    const uint8_t player = talk_policy::EffectiveActingPlayer(net::IsHost(), talk::IsHeld(entity), npcs::ContextPlayer(entity));
    guards::Acting context(player > 1 ? remote::PuppetOf(player) : nullptr);
    return originalScript(self, edx);
}

GEInt __fastcall EventScript(gCScriptAdmin* self, void* edx, bCString const& name,
                             eCEntity* receiver, eCEntity* other, GEInt args)
{
    if (ai::ShuttingDown()) return originalEventScript
        ? originalEventScript(self, edx, name, receiver, other, args) : 0;
    const char* event = name.GetText();
    if ((!std::strcmp(event, "OnTick") || !std::strcmp(event, "OnEnterProcessingRange") ||
         !std::strcmp(event, "OnExitProcessingRange") || !std::strcmp(event, "Respawn")) && IsPassive(receiver)) {
        ++counters.blockedEvents;
        if (!std::strcmp(event, "OnTick")) ++counters.blockedNpcTicks;
        return 0;
    }
    return originalEventScript(self, edx, name, receiver, other, args);
}

void __fastcall Play(eCVisualAnimation_PS* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot,
                     eCWrapper_emfx2Motion::eSMotionDesc* options)
{
    if (ai::ShuttingDown()) { if (originalPlay) originalPlay(self, edx, slot, options); return; }
    eCEntity* entity = self->GetEntity();
    if (!presenting && IsPassive(entity)) {
        if (DialogPartner(entity)) {
            if (static_cast<int>(slot) == 0)
                talk::OnPartnerMotion(entity, self->GetMotionDesc(slot).GetMotionFilename().GetText());
        } else {
            ++counters.blockedAnimations;
            return;
        }
    }
    originalPlay(self, edx, slot, options);
}

void __fastcall ActorPlay(eCWrapper_emfx2Actor* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot,
                          eCWrapper_emfx2Motion& motion, eCWrapper_emfx2Motion::eSMotionDesc* options)
{
    if (ai::ShuttingDown()) { if (originalActorPlay) originalActorPlay(self, edx, slot, motion, options); return; }
    if (!presenting && updatingVisual && IsPassive(visualEntity) && !DialogPartner(visualEntity)) {
        ++counters.blockedAnimations;
        return;
    }
    const int index = static_cast<int>(slot);
    CapturedOptions captured;
    if (options) std::memcpy(captured.words.data(), options, sizeof(captured.words));
    captured.motion = &motion;
    captured.defaults = options == nullptr;
    captured.started = net::Now();
    originalActorPlay(self, edx, slot, motion, options);
    if (presenting || index < 0 || index >= Slots || !self->HasMotionInstance(slot)) return;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    if (++nextInstance == 0) ++nextInstance;
    captured.instance = nextInstance;
    captured.available = true;
    sourceOptions[self][index] = captured;
}

bool CanonicalFacial(eCVisualAnimationFactory* factory)
{
    auto* animation = updatingVisual && IsPassive(visualEntity) && !DialogPartner(visualEntity) ? Animation(visualEntity) : nullptr;
    return animation && !animation->GetRagDollEnabled() && &animation->GetAnimationFactory() == factory;
}

GEBool __fastcall FacialStart(eCVisualAnimationFactory* self, void* edx)
{
    if (ai::ShuttingDown()) return originalFacialStart ? originalFacialStart(self, edx) : GEFalse;
    if (CanonicalFacial(self) && !self->IsFacialAnimationStarted()) { ++counters.blockedAnimations; return GEFalse; }
    return originalFacialStart(self, edx);
}

GEBool __fastcall FacialStop(eCVisualAnimationFactory* self, void* edx)
{
    if (ai::ShuttingDown()) return originalFacialStop ? originalFacialStop(self, edx) : GEFalse;
    if (CanonicalFacial(self) && self->IsFacialAnimationStarted()) { ++counters.blockedAnimations; return GEFalse; }
    return originalFacialStop(self, edx);
}

void __fastcall StopMotion(eCWrapper_emfx2Actor* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot, float blend)
{
    if (ai::ShuttingDown()) { if (originalStop) originalStop(self, edx, slot, blend); return; }
    auto* visual = updatingVisual && IsPassive(visualEntity) && !DialogPartner(visualEntity) ? Animation(visualEntity) : nullptr;
    if (!presenting && visual && !visual->GetRagDollEnabled()) { ++counters.blockedAnimations; return; }
    originalStop(self, edx, slot, blend);
}

void __fastcall SetTask(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
    if (ai::ShuttingDown()) { if (originalSetTask) originalSetTask(self, edx, task); return; }
    if (!TaskBlocked(self, task)) originalSetTask(self, edx, task);
}

void __fastcall AISetTask(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
    if (ai::ShuttingDown()) { if (originalAISetTask) originalAISetTask(self, edx, task); return; }
    if (net::IsHost() && std::strstr(task.GetText(), "Stumble")) {
        eCEntity* entity = self->GetEntity();
        npcs::HostHit(entity, entity == originEntity ? originPlayer : 0, entity == originEntity ? originSequence : 0);
    }
    if (!TaskBlocked(self, task)) originalAISetTask(self, edx, task);
}

template <typename Fn, size_t N>
bool Hook(Fn& original, char const* moduleName, char const* exportName, unsigned char const (&expected)[N], void* replacement)
{
    if (original) return true;
    HMODULE module = GetModuleHandleA(moduleName);
    auto* entry = module ? reinterpret_cast<unsigned char*>(GetProcAddress(module, exportName)) : nullptr;
    if (!entry || std::memcmp(entry, expected, sizeof(expected))) {
        Logf("npcplayback: %s is unavailable or has an unsupported entry; passive replay refused", exportName);
        return false;
    }
    original = reinterpret_cast<Fn>(ai::JumpTo(entry, replacement, N));
    if (!original) Logf("npcplayback: could not install %s; passive replay refused", exportName);
    return original != nullptr;
}

} // namespace

PresentationScope::PresentationScope(bool value) : before(presenting) { presenting = value; }
PresentationScope::~PresentationScope() { presenting = before; }

NativePoseScope::NativePoseScope(NativePose* pose) : before(presentingPose) { presentingPose = pose; }
NativePoseScope::~NativePoseScope() { presentingPose = before; }

VisualScope::VisualScope(eCEntity* entity) : before(updatingVisual), beforeEntity(visualEntity)
{
    updatingVisual = true;
    visualEntity = entity;
}
VisualScope::~VisualScope()
{
    updatingVisual = before;
    visualEntity = beforeEntity;
}

HookCounters& Counters() { return counters; }
NativePose* CurrentPresentingPose() { return presentingPose; }
ActorProcessFn OriginalActorProcess() { return originalActorProcess; }

void __fastcall ActorProcess(eCWrapper_emfx2Actor* self, void* edx, bCMatrix const& world,
                             GEBool skeleton, GEBool physics, GEFloat scale)
{
    if (ai::ShuttingDown()) { if (originalActorProcess) originalActorProcess(self, edx, world, skeleton, physics, scale); return; }
    auto* animation = updatingVisual && IsPassive(visualEntity) ? Animation(visualEntity) : nullptr;
    if (presentingPose && animation && animation->GetActor() == self) {
        // Retire stopped native instances without advancing a mirror's animation or blend clocks.
        originalActorProcess(self, edx, world, GETrue, GEFalse, 0.0f);
        presentingPose->evaluated = true;
        if (!presentingPose->evaluateOnly) ApplyPose(*self, *presentingPose);
        return;
    }
    originalActorProcess(self, edx, world, skeleton, physics, scale);
}

bool InstallHooks()
{
    HMODULE engine = GetModuleHandleA("Engine.dll");
    auto* weightGetter = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?GetTargetWeight@eCWrapper_emfx2Actor@@QBEMW4eEMotionType@1@@Z")) : nullptr;
    const unsigned char weightLayout[] = {0x8b, 0x44, 0xb1, 0x10, 0xd9, 0x40, 0x28, 0x5e, 0xc2, 0x04, 0x00};
    if (!weightGetter || std::memcmp(weightGetter + 21, weightLayout, sizeof(weightLayout))) {
        Log("npcplayback: unsupported native motion-weight layout; passive replay refused");
        return false;
    }
    auto* actorPlay = engine ? reinterpret_cast<unsigned char*>(GetProcAddress(engine,
        "?PlayMotion@eCWrapper_emfx2Actor@@QAEXW4eEMotionType@1@AAVeCWrapper_emfx2Motion@@PAUeSMotionDesc@3@@Z")) : nullptr;
    const unsigned char actorOptions[] = {0x83, 0xec, 0x24, 0x53, 0x8b, 0xd9};
    const unsigned char actorProcess[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0};
    if (!originalActorPlay && (!actorPlay || std::memcmp(actorPlay, actorOptions, sizeof(actorOptions)))) {
        Log("npcplayback: unsupported native playback options layout; passive replay refused");
        return false;
    }
    const unsigned char routine[] = {0x53, 0x55, 0x56, 0x57, 0x8b, 0xf1};
    const unsigned char script[] = {0x83, 0xec, 0x08, 0x53, 0x55, 0x56};
    const unsigned char aligned[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    const unsigned char stop[] = {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0x8b, 0x50, 0x04};
    const unsigned char setTask[] = {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0x8b, 0x50, 0x58};
    const unsigned char aiSetTask[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00};
    const unsigned char addProperty[] = {0x51, 0x56, 0x8b, 0xf1, 0x8b, 0x4c, 0x24, 0x0c};
    // The complete eight-byte prefix pushes the relocated gCGameApp RTTI address, not a relative call.
    unsigned char eventScript[] = {0x56, 0x6a, 0x01, 0x68, 0, 0, 0, 0};
    const uintptr_t gameAppType = reinterpret_cast<uintptr_t>(GetModuleHandleA("Game.dll")) + 0x3d8bb4;
    std::memcpy(eventScript + 4, &gameAppType, sizeof(gameAppType));
    return Hook(originalRoutine, "Game.dll", "?OnProcess@gCScriptRoutine_PS@@UAEXXZ", routine, reinterpret_cast<void*>(&Routine)) &&
           Hook(originalEventScript, "Game.dll", "?CallScript@gCScriptAdmin@@UAEHABVbCString@@PAVeCEntity@@1H@Z",
                eventScript, reinterpret_cast<void*>(&EventScript)) &&
           Hook(originalScript, "Game.dll", "?ProcessScript@gCScriptProcessingUnit@@QAE?AW4bEResult@@XZ", script,
                reinterpret_cast<void*>(&Script)) &&
           Hook(originalMovement, "Game.dll", "?OnProcess@gCCharacterMovement_PS@@MAEXXZ", aligned, reinterpret_cast<void*>(&Movement)) &&
           Hook(originalMovementUpdated, "Game.dll", "?OnUpdatedWorldMatrix@gCCharacterMovement_PS@@MAEXXZ", aligned,
                reinterpret_cast<void*>(&MovementUpdated)) &&
           Hook(originalPlay, "Engine.dll",
                "?PlayMotion@eCVisualAnimation_PS@@QAEXW4eEMotionType@eCWrapper_emfx2Actor@@PAUeSMotionDesc@eCWrapper_emfx2Motion@@@Z",
                aligned, reinterpret_cast<void*>(&Play)) &&
           Hook(originalActorPlay, "Engine.dll",
                "?PlayMotion@eCWrapper_emfx2Actor@@QAEXW4eEMotionType@1@AAVeCWrapper_emfx2Motion@@PAUeSMotionDesc@3@@Z",
                actorOptions, reinterpret_cast<void*>(&ActorPlay)) &&
           Hook(originalActorProcess, "Engine.dll",
                "?Process@eCWrapper_emfx2Actor@@QAEXABVbCMatrix@@_N1M@Z",
                actorProcess, reinterpret_cast<void*>(&ActorProcess)) &&
           Hook(originalFacialStart, "Engine.dll", "?StartFacialAnimation@eCVisualAnimationFactory@@QAE_NXZ",
                aligned, reinterpret_cast<void*>(&FacialStart)) &&
           Hook(originalFacialStop, "Engine.dll", "?StopFacialAnimation@eCVisualAnimationFactory@@QAE_NXZ",
                aligned, reinterpret_cast<void*>(&FacialStop)) &&
           Hook(originalStop, "Engine.dll", "?StopMotion@eCWrapper_emfx2Actor@@QAEXW4eEMotionType@1@M@Z", stop,
                reinterpret_cast<void*>(&StopMotion)) &&
           Hook(originalSetTask, "Game.dll", "?SetCurrentTask@gCScriptRoutine_PS@@QAEXABVbCString@@@Z", setTask,
                reinterpret_cast<void*>(&SetTask)) &&
           Hook(originalAISetTask, "Game.dll", "?AISetTask@gCScriptRoutine_PS@@QAEXABVbCString@@@Z", aiSetTask,
                reinterpret_cast<void*>(&AISetTask)) &&
           Hook(originalAddProperty, "Engine.dll", "?AddPropertySet@eCEntity@@UAE_NPAVeCEntityPropertySet@@_N@Z", addProperty,
                reinterpret_cast<void*>(&AddProperty));
}

bool RegisterEntity(eCEntity* entity)
{
    for (int i = 0; i < entity->GetPropertySetCount(); ++i)
        if (!RegisterProperty(entity->GetPropertySetAt(i))) return false;
    return true;
}

bool LookupCapturedOptions(eCWrapper_emfx2Actor* actor, int slot, CapturedOptions& out)
{
    if (!actor || slot < 0 || slot >= Slots) return false;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    const auto found = sourceOptions.find(actor);
    if (found == sourceOptions.end()) return false;
    out = found->second[slot];
    return true;
}

bool MarkTimedCombatPlayer(eCWrapper_emfx2Actor* actor, uint32_t instance, uint8_t player)
{
    if (!actor) return false;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    auto found = sourceOptions.find(actor);
    if (found == sourceOptions.end() || found->second[0].instance != instance ||
        found->second[0].timedPlayer == player) return false;
    found->second[0].timedPlayer = player;
    return true;
}

uint32_t ActorMotionInstance(eCEntity* entity, int slot)
{
    auto* animation = Animation(entity);
    if (!animation || slot < 0 || slot >= Slots) return 0;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    const auto found = sourceOptions.find(animation->GetActor());
    return found == sourceOptions.end() ? 0 : found->second[slot].instance;
}

void ResetHooksState()
{
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    sourceOptions.clear();
    nextInstance = 0;
}

void RecordBlockedTaskVerdict(eCEntity const* entity, char const* task)
{
    if (ai::detail::damageReceiver == entity) {
        verdictEntity = entity;
        strncpy_s(verdictTask, task, _TRUNCATE);
    }
    ++counters.blockedTasks;
}

} // namespace g3t::npcplayback::detail

namespace g3t::npcplayback {

HitOrigin::HitOrigin(eCEntity* entity, uint8_t player, uint32_t sequence)
    : before(detail::originEntity), beforePlayer(detail::originPlayer), beforeSequence(detail::originSequence)
{
    detail::originEntity = entity;
    detail::originPlayer = player;
    detail::originSequence = sequence;
}

HitOrigin::~HitOrigin()
{
    detail::originEntity = before;
    detail::originPlayer = beforePlayer;
    detail::originSequence = beforeSequence;
}

void BeginHit(eCEntity const* entity)
{
    detail::verdictEntity = entity;
    detail::verdictTask[0] = '\0';
}

char const* HitTask(eCEntity const* entity)
{
    return entity == detail::verdictEntity ? detail::verdictTask : "";
}

uint32_t MotionInstance(eCEntity* entity, int slot)
{
    return detail::ActorMotionInstance(entity, slot);
}

} // namespace g3t::npcplayback
