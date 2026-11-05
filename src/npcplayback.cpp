// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcplayback.h"

#include "ai.h"
#include "body.h"
#include "log.h"
#include "net.h"
#include "hud.h"
#include "i18n.h"
#include "qa.h"
#include "remote.h"
#include "config.h"
#include "npcs.h"
#include "guards.h"
#include "talk.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>

namespace g3t::npcplayback {
namespace {
constexpr int Slots = 8;
// Engine 1.75's actor PlayMotion reads seven 32-bit words. The SDK declares this descriptor without storage.
using MotionOptions = std::array<uint32_t, 7>;
static_assert(sizeof(MotionOptions) == 28, "Native motion options are seven 32-bit words");
static_assert(sizeof(uintptr_t) == 4, "The checked Gothic 3 native ABI is Win32");
struct CapturedOptions {
    MotionOptions words{};
    eCWrapper_emfx2Motion const* motion = nullptr;
    bool available = false;
    bool defaults = false;
    uint32_t instance = 0;
};
std::unordered_map<eCWrapper_emfx2Actor*, std::array<CapturedOptions, Slots>> sourceOptions;
std::mutex sourceOptionsMutex;
uint32_t nextInstance = 0;
struct Motion {
    MotionOptions options{};
    uint32_t instance = 0;
    eCResourceAnimationMotion_PS* resource = nullptr;
    char name[96] = {};
    float time = 0.0f, duration = 0.0f, speed = 0.0f, weight = 0.0f, intro = 0.0f, extro = 0.0f;
    eCWrapper_emfx2Motion::eEMotionBlendMode blend{};
    bool active = false;
    bool defaults = false;
};
struct Frame {
    struct BonePose { std::string name; int index = 0; bCMatrix actorMatrix; float position[3] = {}; };
    float position[3] = {}, rotation[4] = {};
    gEAniState aniState{};
    bool facial = false;
    Motion motions[Slots];
    std::vector<BonePose> bones;
};
struct ProbeState {
    eCEntity* source = nullptr;
    eCEntity* target = nullptr;
    Frame previous;
    bCMatrix originalPose;
    bCString originalTask;
    uint32_t frames = 0, routineBlocked = 0, scriptBlocked = 0, movementBlocked = 0, animationBlocked = 0;
    uint32_t entityBlocked = 0, propertiesBlocked = 0;
    uint32_t animationStopsBlocked = 0;
    uint32_t slotChecks = 0, slotErrors = 0, replays = 0;
    uint32_t waitingForOptions = 0;
    float maximumPositionError = 0.0f, maximumPhaseError = 0.0f;
    float maximumRotationError = 0.0f;
    float maximumBoneError = 0.0f;
    char maximumBoneName[96] = {};
    float maximumImmediateBoneError = 0.0f;
    float maximumBoneLookupError = 0.0f;
    uint32_t boneChecks = 0;
    bool havePrevious = false, failed = false;
    bool exactPose = false;
};
ProbeState probe;
struct NativeBone { int index; bCMatrix matrix; };
struct NativePose {
    std::vector<NativeBone> bones;
    void const* instance = nullptr;
    void const* nodes = nullptr;
    int count = 0;
    bool applied = false, evaluated = false, valid = true;
};
struct ReceivedPose { pose::History history; uint32_t arrived = 0; };
std::unordered_map<std::string, ReceivedPose> receivedPoses;
struct SourceSkeleton {
    void const* instance = nullptr;
    void const* nodes = nullptr;
    std::string actorName;
    std::vector<std::string> names;
    std::vector<uint32_t> hashes;
    uint32_t seen = 0;
    bool valid = false;
};
std::unordered_map<eCWrapper_emfx2Actor*, SourceSkeleton> sourceSkeletons;
uint32_t poseCleanupAt = 0;
thread_local NativePose* presentingPose = nullptr;
bool enabled = false, clientMode = false;
thread_local bool presenting = false;
thread_local bool updatingVisual = false;
thread_local eCEntity* visualEntity = nullptr;
thread_local eCEntity const* verdictEntity = nullptr;
thread_local char verdictTask[64] = {};
thread_local eCEntity* originEntity = nullptr;
thread_local uint8_t originPlayer = 0;
thread_local uint32_t originSequence = 0;
std::atomic<uint32_t> blockedRoutines{0}, blockedScripts{0}, blockedMovement{0}, blockedTasks{0}, blockedAnimations{0};
std::atomic<uint32_t> blockedEntities{0};
std::atomic<uint32_t> blockedProperties{0};
std::atomic<uint32_t> predictedHits{0}, skippedEchoes{0}, presentedFrames{0};
std::atomic<uint32_t> blockedEvents{0};
std::atomic<uint32_t> blockedNpcTicks{0};
std::atomic<uint32_t> posedFrames{0}, poseWaits{0}, stalePoses{0};
float maximumReplayPhaseError = 0.0f;
std::unordered_map<std::string, uint32_t> presentationMissingAt;
uint32_t presentationNoticeAt = 0;
unsigned presentationWarnings = 0;
using ProcessFn = void(__fastcall*)(eCEntityPropertySet*, void*);
using ScriptFn = bEResult(__fastcall*)(gCScriptProcessingUnit*, void*);
using EventScriptFn = GEInt(__fastcall*)(gCScriptAdmin*, void*, bCString const&, eCEntity*, eCEntity*, GEInt);
using PlayFn = void(__fastcall*)(eCVisualAnimation_PS*, void*, eCWrapper_emfx2Actor::eEMotionType,
                                eCWrapper_emfx2Motion::eSMotionDesc*);
using ActorPlayFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, eCWrapper_emfx2Actor::eEMotionType,
                                     eCWrapper_emfx2Motion&, eCWrapper_emfx2Motion::eSMotionDesc*);
using ActorProcessFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, bCMatrix const&, GEBool, GEBool, GEFloat);
using FacialFn = GEBool(__fastcall*)(eCVisualAnimationFactory*, void*);
using StopFn = void(__fastcall*)(eCWrapper_emfx2Actor*, void*, eCWrapper_emfx2Actor::eEMotionType, float);
using TaskFn = void(__fastcall*)(gCScriptRoutine_PS*, void*, bCString const&);
using EntityProcessFn = void(__fastcall*)(eCEntity*, void*, GEBool);
using AddPropertyFn = GEBool(__fastcall*)(eCEntity*, void*, eCEntityPropertySet*, GEBool);
ProcessFn originalRoutine = nullptr, originalMovement = nullptr;
ScriptFn originalScript = nullptr;
EventScriptFn originalEventScript = nullptr;
PlayFn originalPlay = nullptr;
ActorPlayFn originalActorPlay = nullptr;
ActorProcessFn originalActorProcess = nullptr;
FacialFn originalFacialStart = nullptr, originalFacialStop = nullptr;
StopFn originalStop = nullptr;
TaskFn originalSetTask = nullptr, originalAISetTask = nullptr;
EntityProcessFn originalEntityProcess = nullptr, originalEntityPre = nullptr, originalEntityPost = nullptr;
AddPropertyFn originalAddProperty = nullptr;
eCVisualAnimation_PS* Animation(eCEntity* entity);
struct PropertyTable {
    void** table = nullptr;
    ProcessFn original[3] = {};
    void* events[6] = {};
};
std::array<PropertyTable, 128> propertyTables;
std::atomic<unsigned> propertyTableCount{0};
std::mutex propertyTableMutex;
using TriggerFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCEntity*);
using TouchFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCContactIterator&);
using DamageFn = void(__fastcall*)(eCEntityPropertySet*, void*, eCEntity*, eCEntity*, GEInt, GEInt, eCContactIterator&);

struct PresentationScope {
    explicit PresentationScope(bool value = true) : before(presenting) { presenting = value; }
    ~PresentationScope() { presenting = before; }
    bool before;
};

struct NativePoseScope {
    NativePose* before;
    explicit NativePoseScope(NativePose* pose) : before(presentingPose) { presentingPose = pose; }
    ~NativePoseScope() { presentingPose = before; }
};

bool Passive(eCEntity const* entity)
{
    if (!entity) return false;
    if (entity == probe.target) return true;
    if (!enabled || !clientMode || !entity->GetPropertySet(eEPropertySetType_NPC)) return false;
    if (entity->GetPropertySet(eEPropertySetType_PlayerMemory) || entity == gCSession::GetInstance().GetHero() ||
        entity == remote::ControlledBody() || remote::IsPuppet(entity)) return false;
    return true;
}

// The one NPC our hero is talking to: its script routine runs here so the dialog speaks the NPC's lines and waits for them as
// the game does. The host still simulates it; tasks, movement and animation stay blocked and its pose comes from the host.
bool DialogPartner(eCEntity const* entity) { return entity && talk::TalkingWithHero(entity); }

void PropertyDispatch(eCEntityPropertySet* self, void* edx, unsigned phase)
{
    const bool partnerRoutine = self->GetPropertySetType() == eEPropertySetType_ScriptRoutine && DialogPartner(self->GetEntity());
    if (!partnerRoutine && Passive(self->GetEntity()) && !(updatingVisual && self->GetPropertySetType() == eEPropertySetType_Animation)) {
        ++blockedProperties;
        if (self->GetEntity() == probe.target) ++probe.propertiesBlocked;
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

PropertyTable const* PropertyHooks(eCEntityPropertySet* self)
{
    void** table = *reinterpret_cast<void***>(self);
    const unsigned count = propertyTableCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) if (propertyTables[i].table == table) return &propertyTables[i];
    Log("FAIL npcplayback: missing event dispatch table");
    return nullptr;
}

void TriggerEvent(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b, unsigned index)
{
    if (Passive(self->GetEntity())) { ++blockedEvents; return; }
    if (auto* hooks = PropertyHooks(self)) reinterpret_cast<TriggerFn>(hooks->events[index])(self, edx, a, b);
}
void __fastcall PropertyTrigger(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b) { TriggerEvent(self, edx, a, b, 0); }
void __fastcall PropertyUntrigger(eCEntityPropertySet* self, void* edx, eCEntity* a, eCEntity* b) { TriggerEvent(self, edx, a, b, 1); }
void TouchEvent(eCEntityPropertySet* self, void* edx, eCEntity* other, eCContactIterator& contacts, unsigned index)
{
    if (Passive(self->GetEntity())) { ++blockedEvents; return; }
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
    if (Passive(self->GetEntity()) && self->GetPropertySetType() != eEPropertySetType_DamageReceiver) {
        ++blockedEvents;
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
    DWORD ignored = 0;
    if (!VirtualProtect(table + event, bytes, old, &ignored))
        Log("WARN npcplayback: property dispatch page protection could not be restored");
    return true;
}

bool RegisterEntity(eCEntity* entity)
{
    for (int i = 0; i < entity->GetPropertySetCount(); ++i)
        if (!RegisterProperty(entity->GetPropertySetAt(i))) return false;
    return true;
}

GEBool __fastcall AddProperty(eCEntity* self, void* edx, eCEntityPropertySet* property, GEBool flag)
{
    if (enabled && clientMode && property && !RegisterProperty(property)) {
        Log("FAIL npcplayback: refusing an unguarded property during client entity creation");
        return GEFalse;
    }
    return originalAddProperty(self, edx, property, flag);
}

void __fastcall EntityProcess(eCEntity* self, void* edx, GEBool recursive)
{
    if (Passive(self) && !DialogPartner(self)) { ++probe.entityBlocked; ++blockedEntities; return; }
    originalEntityProcess(self, edx, recursive);
}

void __fastcall EntityPre(eCEntity* self, void* edx, GEBool recursive)
{
    if (Passive(self) && !DialogPartner(self)) { ++blockedEntities; return; }
    originalEntityPre(self, edx, recursive);
}

void __fastcall EntityPost(eCEntity* self, void* edx, GEBool recursive)
{
    if (Passive(self) && !DialogPartner(self)) { ++blockedEntities; return; }
    originalEntityPost(self, edx, recursive);
}


void __fastcall Routine(eCEntityPropertySet* self, void* edx)
{
    if (Passive(self->GetEntity()) && !DialogPartner(self->GetEntity())) { ++probe.routineBlocked; ++blockedRoutines; return; }
    const uint8_t player = npcs::ContextPlayer(self->GetEntity());
    guards::Acting context(player > 1 ? remote::PuppetOf(player) : nullptr);
    originalRoutine(self, edx);
}

void __fastcall Movement(eCEntityPropertySet* self, void* edx)
{
    if (Passive(self->GetEntity())) { ++probe.movementBlocked; ++blockedMovement; return; }
    originalMovement(self, edx);
}

ProcessFn originalMovementUpdated = nullptr;
void __fastcall MovementUpdated(eCEntityPropertySet* self, void* edx)
{
    if (Passive(self->GetEntity())) { ++blockedMovement; return; }
    originalMovementUpdated(self, edx);
}

bEResult __fastcall Script(gCScriptProcessingUnit* self, void* edx)
{
    if (Passive(self->GetSelfEntity()) && !DialogPartner(self->GetSelfEntity())) { ++probe.scriptBlocked; ++blockedScripts; return bEResult_Ok; }
    return originalScript(self, edx);
}

GEInt __fastcall EventScript(gCScriptAdmin* self, void* edx, bCString const& name,
                            eCEntity* receiver, eCEntity* other, GEInt args)
{
    const char* event = name.GetText();
    if ((!std::strcmp(event, "OnTick") || !std::strcmp(event, "OnEnterProcessingRange") ||
         !std::strcmp(event, "OnExitProcessingRange") || !std::strcmp(event, "Respawn")) && Passive(receiver)) {
        ++blockedEvents;
        if (!std::strcmp(event, "OnTick")) ++blockedNpcTicks;
        return 0;
    }
    return originalEventScript(self, edx, name, receiver, other, args);
}

void __fastcall Play(eCVisualAnimation_PS* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot,
                     eCWrapper_emfx2Motion::eSMotionDesc* options)
{
    if (!presenting && Passive(self->GetEntity())) { ++probe.animationBlocked; ++blockedAnimations; return; }
    originalPlay(self, edx, slot, options);
}

void __fastcall ActorPlay(eCWrapper_emfx2Actor* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot,
                          eCWrapper_emfx2Motion& motion, eCWrapper_emfx2Motion::eSMotionDesc* options)
{
    if (!presenting && updatingVisual && Passive(visualEntity)) {
        ++probe.animationBlocked;
        ++blockedAnimations;
        return;
    }
    const int index = static_cast<int>(slot);
    CapturedOptions captured;
    if (options) std::memcpy(captured.words.data(), options, sizeof(captured.words));
    captured.motion = &motion;
    captured.defaults = options == nullptr;
    originalActorPlay(self, edx, slot, motion, options);
    if (presenting || index < 0 || index >= Slots || !self->HasMotionInstance(slot)) return;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    if (++nextInstance == 0) ++nextInstance;
    captured.instance = nextInstance;
    captured.available = true;
    sourceOptions[self][index] = captured;
}

bool ApplyPose(eCWrapper_emfx2Actor& actor, NativePose& pose)
{
    auto* skeleton = actor.GetInstance();
    if (!skeleton || skeleton != pose.instance || skeleton->mNodes.mData != pose.nodes ||
        skeleton->mNodes.mLength != pose.count) return pose.valid = false;
    for (auto const& bone : pose.bones) {
        if (bone.index < 0 || bone.index >= skeleton->mNodes.mLength) return pose.valid = false;
        std::memcpy(reinterpret_cast<unsigned char*>(skeleton->mNodes.mData + bone.index) + 0x44,
                    &bone.matrix, sizeof(bCMatrix));
    }
    pose.applied = true;
    return true;
}

void __fastcall ActorProcess(eCWrapper_emfx2Actor* self, void* edx, bCMatrix const& world, GEBool skeleton,
                            GEBool physics, GEFloat scale)
{
    auto* animation = updatingVisual && Passive(visualEntity) ? Animation(visualEntity) : nullptr;
    if (presentingPose && animation && animation->GetActor() == self) {
        // Retire stopped native instances without advancing a mirror's animation or blend clocks.
        originalActorProcess(self, edx, world, GETrue, GEFalse, 0.0f);
        presentingPose->evaluated = true;
        ApplyPose(*self, *presentingPose);
        return;
    }
    originalActorProcess(self, edx, world, skeleton, physics, scale);
}

bool CanonicalFacial(eCVisualAnimationFactory* factory)
{
    auto* animation = updatingVisual && Passive(visualEntity) ? Animation(visualEntity) : nullptr;
    return animation && !animation->GetRagDollEnabled() && &animation->GetAnimationFactory() == factory;
}

GEBool __fastcall FacialStart(eCVisualAnimationFactory* self, void* edx)
{
    if (CanonicalFacial(self) && !self->IsFacialAnimationStarted()) { ++blockedAnimations; return GEFalse; }
    return originalFacialStart(self, edx);
}

GEBool __fastcall FacialStop(eCVisualAnimationFactory* self, void* edx)
{
    if (CanonicalFacial(self) && self->IsFacialAnimationStarted()) { ++blockedAnimations; return GEFalse; }
    return originalFacialStop(self, edx);
}

void __fastcall StopMotion(eCWrapper_emfx2Actor* self, void* edx, eCWrapper_emfx2Actor::eEMotionType slot, float blend)
{
    auto* animation = Animation(probe.target);
    auto* visual = updatingVisual && Passive(visualEntity) ? Animation(visualEntity) : nullptr;
    if (!presenting && ((animation && animation->GetActor() == self) ||
        (visual && !visual->GetRagDollEnabled()))) { ++probe.animationStopsBlocked; ++blockedAnimations; return; }
    originalStop(self, edx, slot, blend);
}

bool TaskBlocked(gCScriptRoutine_PS* self, bCString const& task)
{
    eCEntity* entity = self->GetEntity();
    if (!Passive(entity) || task.GetLength() == 0) return false;
    if (ai::detail::damageReceiver == entity) {
        verdictEntity = entity;
        strncpy_s(verdictTask, task.GetText(), _TRUNCATE);
    }
    ++blockedTasks;
    return true;
}

void __fastcall SetTask(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
    if (!TaskBlocked(self, task)) originalSetTask(self, edx, task);
}

void __fastcall AISetTask(gCScriptRoutine_PS* self, void* edx, bCString const& task)
{
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

bool Install()
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
    const unsigned char entityProcess[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x3c};
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
           Hook(originalEntityProcess, "Engine.dll", "?Process@eCEntity@@QAEX_N@Z", entityProcess,
                reinterpret_cast<void*>(&EntityProcess)) &&
           Hook(originalEntityPre, "Engine.dll", "?PreProcess@eCEntity@@QAEX_N@Z", entityProcess,
                reinterpret_cast<void*>(&EntityPre)) &&
           Hook(originalEntityPost, "Engine.dll", "?PostProcess@eCEntity@@QAEX_N@Z", entityProcess,
                reinterpret_cast<void*>(&EntityPost)) &&
           Hook(originalAddProperty, "Engine.dll", "?AddPropertySet@eCEntity@@UAE_NPAVeCEntityPropertySet@@_N@Z", addProperty,
                reinterpret_cast<void*>(&AddProperty));
}

eCVisualAnimation_PS* Animation(eCEntity* entity)
{
    return entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
}

pose::Transform TransformOf(bCMatrix const& matrix, uint32_t name)
{
    pose::Transform transform;
    transform.name = name;
    const bCVector axes[] = {matrix.GetXAxis(), matrix.GetYAxis(), matrix.GetZAxis(), matrix.GetTranslation()};
    for (int axis = 0; axis < 4; ++axis) {
        transform.values[axis * 3] = axes[axis].GetX();
        transform.values[axis * 3 + 1] = axes[axis].GetY();
        transform.values[axis * 3 + 2] = axes[axis].GetZ();
    }
    return transform;
}

bCMatrix MatrixOf(pose::Transform const& transform)
{
    bCMatrix matrix;
    matrix.SetIdentity();
    matrix.AccessXAxis() = bCVector(transform.values[0], transform.values[1], transform.values[2]);
    matrix.AccessYAxis() = bCVector(transform.values[3], transform.values[4], transform.values[5]);
    matrix.AccessZAxis() = bCVector(transform.values[6], transform.values[7], transform.values[8]);
    matrix.AccessTranslation() = bCVector(transform.values[9], transform.values[10], transform.values[11]);
    return matrix;
}

bool SetFacialRig(eCVisualAnimation_PS& animation, bool facial)
{
    auto& factory = animation.GetAnimationFactory();
    if (factory.IsFacialAnimationStarted() == facial) return true;
    if (!facial) return animation.StopFacialMotion() != GEFalse;
    if (!factory.StartFacialAnimation()) return false;
    // The native facial-motion wrapper relinks these cached node references after assembling facial bones.
    animation.RelinkAttachments();
    animation.RelinkLookAtConstraints();
    return true;
}

float CurrentWeight(eCWrapper_emfx2Actor const& actor, int slot)
{
    unsigned char const* instance = nullptr;
    std::memcpy(&instance, reinterpret_cast<unsigned char const*>(&actor) + 0x10 + slot * sizeof(void*), sizeof(instance));
    float weight = 0.0f;
    // SetTargetWeight sets both fields; +0x24 is the current blend and +0x28 is its destination.
    if (instance) std::memcpy(&weight, instance + 0x24, sizeof(weight));
    return weight;
}

bool Capture(eCEntity* entity, Frame& frame, float offset = 0.0f)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    frame.facial = animation->GetAnimationFactory().IsFacialAnimationStarted() != GEFalse;
    const bCVector& at = entity->GetWorldPosition();
    frame.position[0] = at.GetX(), frame.position[1] = at.GetY(), frame.position[2] = at.GetZ();
    frame.position[0] += offset;
    const bCQuaternion rotation = entity->GetWorldRotation();
    frame.rotation[0] = rotation.GetX(), frame.rotation[1] = rotation.GetY();
    frame.rotation[2] = rotation.GetZ(), frame.rotation[3] = rotation.GetW();
    if (auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)))
        frame.aniState = routine->AccessAniState().GetValue();
    for (int i = 0; i < Slots; ++i) {
        Motion& motion = frame.motions[i];
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        motion.active = animation->HasMotion(slot) && actor->HasMotionInstance(slot);
        if (!motion.active) continue;
        motion.weight = CurrentWeight(*actor, i);
        if (!std::isfinite(motion.weight) || motion.weight < 0.0f || motion.weight > 1.0f) {
            Log("npcplayback: invalid native current blend weight");
            return false;
        }
        motion.active = motion.weight > 0.0001f || actor->GetTargetWeight(slot) > 0.0001f;
        if (!motion.active) continue;
        // The SDK descriptor has no declared storage. Only use the engine-owned instance.
        auto& description = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation->GetMotionDesc(slot));
        motion.resource = description.GetMotion();
        if (!motion.resource) return false;
        if (std::strlen(description.GetMotionFilename().GetText()) >= sizeof(motion.name)) return false;
        strcpy_s(motion.name, description.GetMotionFilename().GetText());
        CapturedOptions options;
        {
            std::lock_guard<std::mutex> lock(sourceOptionsMutex);
            const auto found = sourceOptions.find(actor);
            if (found != sourceOptions.end()) options = found->second[i];
        }
        if (!options.available || options.motion != &animation->GetMotion(slot)) {
            ++probe.waitingForOptions;
            static unsigned missingOptions = 0;
            if (missingOptions++ < 20)
                Logf("npcplayback: %s slot %d lacks matching actor options for %s (captured=%d)",
                     entity->GetName().GetText(), i, motion.name, options.available ? 1 : 0);
            return false;
        }
        motion.options = options.words;
        motion.defaults = options.defaults;
        motion.instance = options.instance;
        const float maximum = static_cast<float>(actor->GetMaxTime(slot));
        if (!(maximum > 0.0f) || !std::isfinite(maximum)) return false;
        motion.duration = maximum;
        motion.time = std::min(static_cast<float>(actor->GetPlayTime(slot)), std::nextafter(maximum, 0.0f));
        motion.speed = actor->GetPlaySpeed(slot);
        motion.intro = actor->GetIntroBlending(slot);
        motion.extro = actor->GetExtroBlending(slot);
        motion.blend = actor->GetBlendMode(slot);
    }
    if (entity == probe.source && offset != 0.0f && actor->GetInstance()) {
        auto const& nodes = actor->GetInstance()->mNodes;
        if (nodes.mLength <= 0 || nodes.mLength > 4096) return false;
        for (int i = 0; i < std::min(nodes.mLength, 128); ++i) {
            bCMatrix pose;
            if (!animation->GetBoneMatrix(i, pose)) return false;
            Frame::BonePose bone;
            bone.index = i;
            std::memcpy(&bone.actorMatrix, reinterpret_cast<unsigned char const*>(nodes.mData + i) + 0x44, sizeof(bCMatrix));
            bone.name = actor->GetName(nodes.mData + i).GetText();
            const auto& at = pose.GetTranslation();
            bone.position[0] = at.GetX() + offset;
            bone.position[1] = at.GetY();
            bone.position[2] = at.GetZ();
            frame.bones.push_back(std::move(bone));
        }
    }
    return true;
}

int BoneIndex(eCWrapper_emfx2Actor& actor, MCore::Array<EMotionFX::Node> const& nodes, Frame::BonePose const& bone)
{
    if (bone.index < nodes.mLength && bone.name == actor.GetName(nodes.mData + bone.index).GetText()) return bone.index;
    int found = -1;
    for (int i = 0; i < nodes.mLength; ++i) {
        if (bone.name != actor.GetName(nodes.mData + i).GetText()) continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

void Measure(Frame const& expected, bool immediate = false)
{
    auto* animation = Animation(probe.target);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) { probe.failed = true; Log("npcplayback: probe lost its target actor"); return; }
    const bCVector& at = probe.target->GetWorldPosition();
    const float here[3] = {at.GetX(), at.GetY(), at.GetZ()};
    probe.maximumPositionError = std::max(probe.maximumPositionError, body::Distance(here, expected.position));
    const bCQuaternion rotation = probe.target->GetWorldRotation();
    const float actual[4] = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()};
    double dot = 0.0, actualLength = 0.0, expectedLength = 0.0;
    for (int i = 0; i < 4; ++i) {
        dot += static_cast<double>(actual[i]) * expected.rotation[i];
        actualLength += static_cast<double>(actual[i]) * actual[i];
        expectedLength += static_cast<double>(expected.rotation[i]) * expected.rotation[i];
    }
    if (!(actualLength > 0.0) || !(expectedLength > 0.0) || !std::isfinite(actualLength) ||
        !std::isfinite(expectedLength) || !std::isfinite(dot)) {
        probe.failed = true;
        Log("npcplayback: probe has an invalid root rotation");
        return;
    }
    const float angle = static_cast<float>(2.0 * std::acos(std::clamp(std::fabs(dot) / std::sqrt(actualLength * expectedLength), 0.0, 1.0))
                                          * 180.0 / 3.14159265358979323846);
    probe.maximumRotationError = std::max(probe.maximumRotationError, angle);
    auto* skeleton = actor->GetInstance();
    if (!skeleton || skeleton->mNodes.mLength <= 0 || skeleton->mNodes.mLength > 4096) {
        probe.failed = true;
        Log("npcplayback: probe has an invalid target skeleton");
        return;
    }
    auto const& nodes = skeleton->mNodes;
    for (auto const& bone : expected.bones) {
        const int index = BoneIndex(*actor, nodes, bone);
        if (index == -2) { probe.failed = true; Log("npcplayback: ambiguous target bone name"); return; }
        if (index < 0) continue;
        bCMatrix pose;
        if (!animation->GetBoneMatrix(index, pose)) { probe.failed = true; Log("npcplayback: target bone unavailable"); return; }
        const auto& at = pose.GetTranslation();
        const float actualBone[3] = {at.GetX(), at.GetY(), at.GetZ()};
        bCMatrix named;
        if (animation->GetBoneMatrix(bCString(bone.name.c_str()), named)) {
            const auto& lookup = named.GetTranslation();
            const float lookupAt[3] = {lookup.GetX(), lookup.GetY(), lookup.GetZ()};
            probe.maximumBoneLookupError = std::max(probe.maximumBoneLookupError, body::Distance(actualBone, lookupAt));
        }
        const float error = body::Distance(actualBone, bone.position);
        if (immediate) probe.maximumImmediateBoneError = std::max(probe.maximumImmediateBoneError, error);
        else if (error > probe.maximumBoneError) {
            probe.maximumBoneError = error;
            strncpy_s(probe.maximumBoneName, bone.name.c_str(), _TRUNCATE);
        }
        if (!immediate) ++probe.boneChecks;
    }
    for (int i = 0; i < Slots; ++i) {
        Motion const& motion = expected.motions[i];
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        if (!motion.active) continue;
        ++probe.slotChecks;
        if (!animation->HasMotion(slot) || !actor->HasMotionInstance(slot) ||
            std::strcmp(animation->GetMotionDesc(slot).GetMotionFilename().GetText(), motion.name)) {
            ++probe.slotErrors;
            if (probe.slotErrors <= 8)
                Logf("npcplayback: slot %d changed between frames: descriptor=%d instance=%d expected=%s actual=%s",
                     i, animation->HasMotion(slot) ? 1 : 0, actor->HasMotionInstance(slot) ? 1 : 0, motion.name,
                     animation->HasMotion(slot) ? animation->GetMotionDesc(slot).GetMotionFilename().GetText() : "-");
            continue;
        }
        probe.maximumPhaseError = std::max(probe.maximumPhaseError, std::fabs(static_cast<float>(actor->GetPlayTime(slot)) - motion.time));
    }
}

bool Apply(Frame const& frame)
{
    PresentationScope scope;
    auto* animation = Animation(probe.target);
    if (!animation) return false;
    if (probe.exactPose && !SetFacialRig(*animation, frame.facial)) return false;
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    const bCMatrix pose = body::Pose(*probe.target, frame.position, frame.rotation);
    probe.target->SetToWorldMatrix(pose);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(probe.target->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(pose);
    if (auto* routine = static_cast<gCScriptRoutine_PS*>(probe.target->GetPropertySet(eEPropertySetType_ScriptRoutine)))
        routine->AccessAniState().AccessValue() = frame.aniState;
    for (int i = 0; i < Slots; ++i) {
        Motion const& motion = frame.motions[i];
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        if (!motion.active) {
            if (actor->HasMotionInstance(slot)) animation->StopMotion(slot, 0.0f);
            continue;
        }
        if (!probe.havePrevious || motion.instance != probe.previous.motions[i].instance ||
            !animation->HasMotion(slot) || !actor->HasMotionInstance(slot) ||
            std::strcmp(animation->GetMotionDesc(slot).GetMotionFilename().GetText(), motion.name)) {
            animation->SetMotion(slot, motion.resource);
            MotionOptions options = motion.options;
            animation->PlayMotion(slot, motion.defaults ? nullptr :
                                  reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(options.data()));
            ++probe.replays;
        }
        if (!actor->HasMotionInstance(slot)) return false;
        actor->SetBlendMode(slot, motion.blend);
        actor->SetIntroExtroBlending(slot, motion.intro, motion.extro);
        actor->SetTargetWeight(slot, motion.weight);
        actor->SetPlaySpeed(slot, 0.0f);
        actor->PauseMotion(GEFalse, slot);
        actor->SetPlayTime(slot, motion.time);
    }
    return true;
}

void Stop()
{
    eCEntity* target = probe.target;
    if (target) {
        auto* animation = Animation(target);
        if (auto* actor = animation ? animation->GetActor() : nullptr)
            for (int i = 0; i < Slots; ++i) {
                const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
                if (probe.havePrevious) actor->SetPlaySpeed(slot, probe.previous.motions[i].speed);
                actor->PauseMotion(GEFalse, slot);
            }
        probe.target = nullptr;
        Entity entity(target);
        entity.MoveTo(probe.originalPose);
        entity.Routine.FullStop();
        entity.Routine.SetTask(probe.originalTask);
    }
    probe = ProbeState{};
}
}

void OnWorldDeactivate()
{
    presentationMissingAt.clear();
    presentationNoticeAt = 0;
    probe = ProbeState{};
    receivedPoses.clear();
    sourceSkeletons.clear();
    poseCleanupAt = 0;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    sourceOptions.clear();
    nextInstance = 0;
}

bool Start(bool client)
{
    clientMode = client;
    enabled = true;
    net::SetNpcPoseHandler([](net::NpcRecord const& record, pose::Bone const* bones, int count, uint8_t flags, bool) {
        if (net::IsHost()) return;
        auto& received = receivedPoses[std::string(reinterpret_cast<char const*>(record.guid), 16)];
        if (received.history.Push(record.incarnation, record.sent, bones, count, flags)) received.arrived = net::Now();
        else ++stalePoses;
    });
    if (!Install()) {
        enabled = false;
        Log("FAIL NPC replication cannot start: required native playback hooks are unavailable");
        return false;
    }
    return true;
}

bool Available() { return enabled; }

bool IsPassive(eCEntity const* entity) { return Passive(entity); }

void Silence(eCEntity* entity)
{
    if (!Passive(entity)) return;
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    if (!routine || (!routine->AccessCurrentTask().GetLength() && !routine->AccessCurrentState().GetLength())) return;
    routine->AIFullStop();
    routine->AccessCurrentTask() = bCString();
    routine->AccessCurrentState() = bCString();
}

uint32_t MotionInstance(eCEntity* entity, int slot)
{
    auto* animation = Animation(entity);
    if (!animation || slot < 0 || slot >= Slots) return 0;
    std::lock_guard<std::mutex> lock(sourceOptionsMutex);
    const auto found = sourceOptions.find(animation->GetActor());
    return found == sourceOptions.end() ? 0 : found->second[slot].instance;
}

HitOrigin::HitOrigin(eCEntity* entity, uint8_t player, uint32_t sequence)
    : before(originEntity), beforePlayer(originPlayer), beforeSequence(originSequence)
{
    originEntity = entity; originPlayer = player; originSequence = sequence;
}
HitOrigin::~HitOrigin() { originEntity = before; originPlayer = beforePlayer; originSequence = beforeSequence; }

void BeginHit(eCEntity const* entity) { verdictEntity = entity; verdictTask[0] = '\0'; }
char const* HitTask(eCEntity const* entity) { return entity == verdictEntity ? verdictTask : ""; }

void Stats(LogFn log)
{
    char line[768];
    Format(line, "PASSIVE enabled=%d client=%d routines_blocked=%u scripts_blocked=%u movement_blocked=%u "
                 "tasks_blocked=%u animations_blocked=%u entities_blocked=%u properties_blocked=%u "
                 "local_hits=%u echo_skipped=%u presented_frames=%u events_blocked=%u ticks_blocked=%u phase_error_ms=%.3f "
                 "posed_frames=%u pose_waits=%u stale_poses=%u pending_npcs=%u readiness_warnings=%u",
           enabled, clientMode, blockedRoutines.load(), blockedScripts.load(), blockedMovement.load(),
           blockedTasks.load(), blockedAnimations.load(), blockedEntities.load(), blockedProperties.load(),
           predictedHits.load(), skippedEchoes.load(), presentedFrames.load(), blockedEvents.load(), blockedNpcTicks.load(),
           maximumReplayPhaseError * 1000.0f, posedFrames.load(), poseWaits.load(), stalePoses.load(),
           static_cast<unsigned>(presentationMissingAt.size()), presentationWarnings);
    log(line);
}

bool CapturePose(eCEntity* entity, std::vector<pose::Bone>& bones, uint8_t& flags, PublishName publish)
{
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!skeleton) return false;
    flags = animation->GetAnimationFactory().IsFacialAnimationStarted() ? pose::Facial : 0;
    auto const& nodes = skeleton->mNodes;
    if (nodes.mLength <= 0 || nodes.mLength > pose::MaxBones) {
        Logf("npcplayback: %s has an unsupported pose size: %d", entity->GetName().GetText(), nodes.mLength);
        return false;
    }
    bones.clear();
    bones.reserve(nodes.mLength);
    auto& cache = sourceSkeletons[actor];
    cache.seen = net::Now();
    if (cache.instance != skeleton || cache.nodes != nodes.mData ||
        cache.names.size() != static_cast<size_t>(nodes.mLength) || cache.actorName != actor->GetActorName().GetText()) {
        cache.instance = skeleton;
        cache.nodes = nodes.mData;
        cache.actorName = actor->GetActorName().GetText();
        cache.names.clear();
        cache.hashes.clear();
        cache.valid = true;
        for (int i = 0; i < nodes.mLength; ++i) {
            cache.names.emplace_back(actor->GetName(nodes.mData + i).GetText());
            const auto hash = net::HashName(cache.names.back().c_str());
            if (std::find(cache.hashes.begin(), cache.hashes.end(), hash) != cache.hashes.end()) cache.valid = false;
            cache.hashes.push_back(hash);
        }
        if (!cache.valid) Logf("npcplayback: %s has ambiguous bone names", entity->GetName().GetText());
    }
    if (!cache.valid) return false;
    for (int i = 0; i < nodes.mLength; ++i) {
        if (publish) publish(cache.hashes[i], cache.names[i].c_str());
        bCMatrix matrix;
        std::memcpy(&matrix, reinterpret_cast<unsigned char const*>(nodes.mData + i) + 0x44, sizeof(matrix));
        const auto transform = TransformOf(matrix, cache.hashes[i]);
        pose::Bone bone;
        if (!pose::Encode(transform, bone)) {
            Logf("npcplayback: %s bone %d is outside the canonical pose encoding", entity->GetName().GetText(), i);
            return false;
        }
        bones.push_back(bone);
    }
    return true;
}

bool Capture(eCEntity* entity, net::NpcRecord& record, PublishName publish)
{
    auto* animation = Animation(entity);
    if (!animation) return false;
    animation->SetForceAlwaysVisible(GETrue);
    if (!animation->GetActor()) return false;
    Frame frame;
    if (!Capture(entity, frame)) return false;
    for (int i = 0; i < Slots; ++i) {
        Motion const& source = frame.motions[i];
        auto& motion = record.motions[i];
        motion = net::NpcMotion{};
        if (!source.active) continue;
        motion.active = source.defaults ? 3 : 1;
        motion.name = net::HashName(source.name);
        motion.instance = source.instance;
        motion.phase = std::clamp(source.time / source.duration, 0.0f, 1.0f);
        motion.speed = source.speed;
        motion.weight = source.weight;
        motion.intro = source.intro;
        motion.extro = source.extro;
        motion.blend = static_cast<int32_t>(source.blend);
        std::memcpy(motion.options, source.options.data(), sizeof(motion.options));
        if (publish) publish(motion.name, source.name);
    }
    return true;
}

namespace {
bool PrepareTransforms(eCEntity* entity, eCVisualAnimation_PS& animation, State& state,
                       std::vector<pose::Transform> const& transforms, uint8_t flags, FindName names, NativePose& result)
{
    auto& factory = animation.GetAnimationFactory();
    const bool facial = (flags & pose::Facial) != 0;
    if (factory.IsFacialAnimationStarted() != facial) {
        if (!SetFacialRig(animation, facial)) {
            Logf("npcplayback: %s cannot apply the host facial-rig state", entity->GetName().GetText());
            return false;
        }
        state.initialized = false;
        state.nodes = nullptr;
    }
    auto* actor = animation.GetActor();
    auto* skeleton = actor ? actor->GetInstance() : nullptr;
    if (!skeleton || skeleton->mNodes.mLength <= 0 || skeleton->mNodes.mLength > 4096) return false;
    auto const& nodes = skeleton->mNodes;
    result.instance = skeleton;
    result.nodes = nodes.mData;
    result.count = nodes.mLength;
    if (state.nodes != nodes.mData || state.nodeCount != nodes.mLength) {
        state.boneIndices.clear();
        for (int i = 0; i < nodes.mLength; ++i) {
            const auto hash = net::HashName(actor->GetName(nodes.mData + i).GetText());
            const auto inserted = state.boneIndices.emplace(hash, i);
            if (!inserted.second) inserted.first->second = -1;
        }
        state.nodes = nodes.mData;
        state.nodeCount = nodes.mLength;
    }
    result.bones.reserve(transforms.size());
    for (auto const& transform : transforms) {
        const auto found = state.boneIndices.find(transform.name);
        if (found == state.boneIndices.end() || found->second < 0) {
            static unsigned missingBones = 0;
            if (missingBones++ < 20)
                Logf("npcplayback: %s canonical bone %08x (%s) is unavailable on the mirror (nodes=%d)",
                     entity->GetName().GetText(), transform.name,
                     names && names(transform.name) ? names(transform.name) : "(cached pose)", nodes.mLength);
            return false;
        }
        NativeBone bone;
        bone.index = found->second;
        bone.matrix = MatrixOf(transform);
        result.bones.push_back(bone);
    }
    return true;
}

bool PreparePose(eCEntity* entity, eCVisualAnimation_PS& animation, State& state,
                 net::NpcRecord const& record, FindName names, NativePose& result)
{
    const auto received = receivedPoses.find(std::string(reinterpret_cast<char const*>(record.guid), 16));
    std::vector<pose::Transform> transforms;
    uint8_t flags = 0;
    if (received == receivedPoses.end() || !received->second.history.Sample(record.incarnation, record.sent, transforms, &flags)) {
        ++poseWaits;
        return false;
    }
    return PrepareTransforms(entity, animation, state, transforms, flags, names, result);
}

bool ReactionMotion(char const* name)
{
    return name && std::strstr(name, "Stumble");
}

bool PresentMotion(eCVisualAnimation_PS& animation, eCWrapper_emfx2Actor& actor, int index, char const* name,
                   net::NpcMotion const& motion, bool restart)
{
    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(index);
    if (restart || !actor.HasMotionInstance(slot)) {
        auto& description = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation.GetMotionDesc(slot));
        description.SetMotion(bCString(name));
        auto* resource = description.GetMotion();
        if (!resource) { Logf("npcplayback: motion resource unavailable: %s", name); return false; }
        animation.SetMotion(slot, resource);
        MotionOptions options;
        std::memcpy(options.data(), motion.options, sizeof(motion.options));
        animation.PlayMotion(slot, (motion.active & 2u) ? nullptr :
                             reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(options.data()));
    }
    if (!actor.HasMotionInstance(slot)) { Logf("npcplayback: motion did not start: %s", name); return false; }
    const float duration = static_cast<float>(actor.GetMaxTime(slot));
    if (!(duration > 0.0f) || !std::isfinite(duration)) { Logf("npcplayback: invalid motion duration: %s", name); return false; }
    actor.SetBlendMode(slot, static_cast<eCWrapper_emfx2Motion::eEMotionBlendMode>(motion.blend));
    actor.SetIntroExtroBlending(slot, motion.intro, motion.extro);
    actor.SetTargetWeight(slot, motion.weight);
    // Keep skeletal/blend evaluation active; only the received timeline advances clip time.
    actor.SetPlaySpeed(slot, 0.0f);
    actor.PauseMotion(GEFalse, slot);
    actor.SetPlayTime(slot, std::min(duration * motion.phase, std::nextafter(duration, 0.0f)));
    return true;
}
}

bool LocalHit(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options, uint32_t hit, uint32_t now)
{
    if (!entity || !motion || !*motion || !hit || !options.active || !IsPassive(entity)) return false;
    auto* animation = Animation(entity);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) { Log("npcplayback: local hit has no presentation actor"); return false; }
    PresentationScope scope;
    net::NpcMotion local = options;
    local.phase = 0.0f;
    local.weight = 1.0f;
    if (!PresentMotion(*animation, *actor, 0, motion, local, true)) return false;
    state.pendingHit = hit;
    state.acknowledgedAt = 0;
    state.localStart = now;
    state.localDuration = static_cast<float>(actor->GetMaxTime(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0)));
    state.localOptions = options;
    strcpy_s(state.localMotion, motion);
    ++predictedHits;
    return true;
}

bool Replay(eCEntity* entity, State& state, net::NpcRecord const& record, FindName names, uint32_t now)
{
    if (!entity || !names) return false;
    Silence(entity);
    PresentationScope scope;
    const bCMatrix pose = body::Pose(*entity, record.position, record.rotation);
    entity->SetToWorldMatrix(pose);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(pose);
    auto* animation = Animation(entity);
    if (animation) animation->SetForceAlwaysVisible(GETrue);
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    if (state.incarnation != record.incarnation) {
        state = State{};
        state.incarnation = record.incarnation;
    }
    if (record.flags & 3) { state.pendingHit = 0; *state.localMotion = '\0'; }
    const bool acknowledged = state.pendingHit && record.hitPlayer == net::LocalId() &&
                              static_cast<int32_t>(record.hitSequence - state.pendingHit) >= 0;
    if (acknowledged) {
        state.acknowledgedHit = record.hitSequence;
        if (record.motions[0].instance != state.localOptions.instance) state.pendingHit = 0;
    }
    if (state.pendingHit && state.acknowledgedAt &&
        static_cast<int32_t>(record.sent - state.acknowledgedAt) >= 0 &&
        record.motions[0].instance != state.localOptions.instance) state.pendingHit = 0;
    const float localAge = static_cast<float>(now - state.localStart) / 1000.0f;
    const bool local = *state.localMotion && (state.pendingHit || localAge < state.localDuration);
    bool echo = false;
    NativePose hostPose;
    const bool havePose = PreparePose(entity, *animation, state, record, names, hostPose);
    actor = animation->GetActor();
    if (!actor) return false;
    animation->ClearLookAtConstraints();
    if (animation->IsAutoAimingEnabled()) {
        bCString bone;
        animation->EnableAutoAiming(GEFalse, nullptr, bone);
    }
    for (int i = 0; i < Slots; ++i) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        auto const& motion = record.motions[i];
        char const* name = motion.active ? names(motion.name) : nullptr;
        if (record.hitPlayer == net::LocalId() && record.hitSequence && record.hitSequence == state.acknowledgedHit &&
            ReactionMotion(name) && state.suppressed[i] != motion.instance) {
            state.suppressed[i] = motion.instance;
            ++skippedEchoes;
        }
        if (local) {
            if (i == 0) {
                net::NpcMotion overlay = state.localOptions;
                overlay.phase = state.localDuration > 0.0f ? std::min(localAge / state.localDuration, 1.0f) : 1.0f;
                overlay.weight = 1.0f;
                if (!PresentMotion(*animation, *actor, i, state.localMotion, overlay, false)) return false;
            } else if (actor->HasMotionInstance(slot)) {
                animation->StopMotion(slot, 0.0f);
            }
            state.instances[i] = 0;
            continue;
        }
        if (motion.active && state.suppressed[i] == motion.instance && i == 0 && *state.localMotion) {
            net::NpcMotion overlay = state.localOptions;
            overlay.phase = 1.0f;
            overlay.weight = 1.0f;
            if (!PresentMotion(*animation, *actor, i, state.localMotion, overlay, false)) return false;
            echo = true;
            continue;
        }
        if (!motion.active || state.suppressed[i] == motion.instance) {
            if (actor->HasMotionInstance(slot)) animation->StopMotion(slot, 0.0f);
            state.instances[i] = 0;
            continue;
        }
        if (!name) {
            if (!echo && havePose) { state.instances[i] = 0; continue; }
            Logf("npcplayback: waiting for motion dictionary entry %08x", motion.name);
            return false;
        }
        if (!PresentMotion(*animation, *actor, i, name, motion, !state.initialized || state.instances[i] != motion.instance)) {
            if (echo || !havePose) return false;
            state.instances[i] = 0;
            continue;
        }
        if (ReactionMotion(name) && state.effects[i] != motion.instance) {
            state.effects[i] = motion.instance;
            const bCString effect(std::strstr(name, "Parade") ? "eff_col_wh_01_me_me" : "eff_col_wh_02_me_fl");
            if (EffectSystem::ExistsEffect(effect)) EffectSystem::StartEffect(effect, Entity(entity));
        }
        state.instances[i] = motion.instance;
    }
    if (!local && !echo) *state.localMotion = '\0';
    state.initialized = true;
    ++presentedFrames;
    if (!local && !echo && !havePose) return false;
    NativePoseScope poseScope(local || echo ? nullptr : &hostPose);
    UpdatePresentation(entity);
    if (!local && !echo) {
        if (!hostPose.evaluated || !hostPose.applied || !hostPose.valid) {
            Log("npcplayback: canonical skeletal evaluation failed; contacts disabled");
            return false;
        }
        ++posedFrames;
        // The authoritative bones are the visible pose. Secondary native clip bookkeeping cannot invalidate them.
        return true;
    }
    animation = Animation(entity);
    actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return false;
    for (int i = 0; i < Slots; ++i) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
        const auto& motion = record.motions[i];
        char const* expected = nullptr;
        if (i == 0 && (local || echo)) expected = state.localMotion;
        else if (!local && motion.active && state.suppressed[i] != motion.instance) expected = names(motion.name);
        const bool visible = actor->HasMotionInstance(slot) && actor->GetTargetWeight(slot) > 0.0001f;
        bool phaseMatches = true;
        if (expected && visible) {
            const float duration = static_cast<float>(actor->GetMaxTime(slot));
            const float phase = i == 0 && local
                ? (state.localDuration > 0.0f ? std::min(localAge / state.localDuration, 1.0f) : 1.0f)
                : i == 0 && echo ? 1.0f : motion.phase;
            const float expectedTime = std::min(duration * phase, std::nextafter(duration, 0.0f));
            const float error = std::fabs(static_cast<float>(actor->GetPlayTime(slot)) - expectedTime);
            maximumReplayPhaseError = std::max(maximumReplayPhaseError, error);
            phaseMatches = std::isfinite(error) && error <= 0.001f;
        }
        const bool expectedVisible = expected && (local || echo || motion.weight > 0.0001f);
        if ((expectedVisible && (!visible || std::strcmp(expected, animation->GetMotionDesc(slot).GetMotionFilename().GetText()))) ||
            (!expected && visible) || !phaseMatches) {
            static unsigned failedPresentations = 0;
            if (failedPresentations++ < 20)
                Logf("npcplayback: %s slot %d changed during visual update; contacts disabled", entity->GetName().GetText(), i);
            return false;
        }
    }
    return true;
}

bool ReplayFrozenPose(eCEntity* entity, State& state, std::vector<pose::Bone> const& bones, uint8_t flags)
{
    auto* animation = Animation(entity);
    if (!animation || bones.empty()) return false;
    PresentationScope scope;
    std::vector<pose::Transform> transforms;
    transforms.reserve(bones.size());
    for (auto const& bone : bones) transforms.push_back(pose::Decode(bone));
    NativePose frozen;
    if (!PrepareTransforms(entity, *animation, state, transforms, flags, nullptr, frozen)) return false;
    NativePoseScope poseScope(&frozen);
    UpdatePresentation(entity);
    return frozen.evaluated && frozen.applied && frozen.valid;
}

void UpdatePresentation(eCEntity* entity)
{
    auto* animation = Animation(entity);
    if (!animation) return;
    PresentationScope scope(false);
    struct VisualScope {
        bool before = updatingVisual;
        eCEntity* beforeEntity = visualEntity;
        explicit VisualScope(eCEntity* entity) { updatingVisual = true; visualEntity = entity; }
        ~VisualScope() { updatingVisual = before; visualEntity = beforeEntity; }
    } visual(entity);
    animation->SetForceAlwaysVisible(GETrue);
    animation->OnPreProcess();
    animation->OnProcess();
    animation->OnPostProcess();
    if (presentingPose) {
        auto* actor = animation->GetActor();
        if (actor && ApplyPose(*actor, *presentingPose)) actor->ProcessGeometry();
    }
}

void Tick()
{
    const uint32_t cleanupNow = net::Now();
    if (!poseCleanupAt || cleanupNow - poseCleanupAt >= 1000) {
        poseCleanupAt = cleanupNow;
        for (auto it = receivedPoses.begin(); it != receivedPoses.end();)
            if (cleanupNow - it->second.arrived > 5000) it = receivedPoses.erase(it);
            else ++it;
        for (auto it = sourceSkeletons.begin(); it != sourceSkeletons.end();)
            if (cleanupNow - it->second.seen > 5000) it = sourceSkeletons.erase(it);
            else ++it;
    }
    if (enabled && clientMode && gCSession::GetInstance().IsGameRunning()) {
        const auto all = Entity::GetNPCs();
        bool waiting = false;
        std::string waitingName;
        std::unordered_map<std::string, uint32_t> pending;
        const uint32_t now = net::Now();
        eCEntity* hero = remote::ControlledBody();
        for (int i = 0; i < all.GetCount(); ++i) {
            Entity npc = all.GetAt(i);
            if (!Passive(npc.GetInstance())) continue;
            if (!RegisterEntity(npc.GetInstance())) {
                Log("FAIL npcplayback: an NPC property class could not be guarded; NPC processing disabled");
                npc.EnableProcessing(GEFalse);
            }
            Silence(npc.GetInstance());
            if (hero && net::Connected() && npcs::PresentationPending(npc.GetInstance())) {
                const bCVector delta = npc.GetInstance()->GetWorldPosition() - hero->GetWorldPosition();
                if (delta.GetSquareMagnitude() < 3000.0f * 3000.0f) {
                    const eCEntityProxy proxy(npc.GetInstance());
                    const std::string key(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
                    const auto previous = presentationMissingAt.find(key);
                    const uint32_t since = previous == presentationMissingAt.end() ? now : previous->second;
                    pending.emplace(key, since);
                    if (now - since >= 2000) {
                        waiting = true;
                        waitingName = npc.GetInstance()->GetName().GetText();
                    }
                }
            }
        }
        presentationMissingAt = std::move(pending);
        if (waiting && (!presentationNoticeAt || now - presentationNoticeAt >= 30000)) {
            hud::Message(hud::Warning, i18n::Text(i18n::NpcStatePending));
            ++presentationWarnings;
            Logf("npcplayback: %s has lacked a usable presentation for at least 2 seconds", waitingName.c_str());
            presentationNoticeAt = now ? now : 1;
        }
    }
    if (!probe.target || probe.failed) return;
    if (probe.havePrevious) Measure(probe.previous);
    if (probe.failed) return;
    Frame frame;
    const uint32_t before = probe.waitingForOptions;
    if (!Capture(probe.source, frame, 500.0f)) {
        if (before != probe.waitingForOptions) return;
        probe.failed = true;
        Log("npcplayback: native source capture failed");
        return;
    }
    if (!Apply(frame)) {
        probe.failed = true;
        Log("npcplayback: native capture/replay failed; probe held, not resumed as local simulation");
        return;
    }
    NativePose hostPose;
    if (probe.exactPose) {
        auto* animation = Animation(probe.target);
        auto* actor = animation ? animation->GetActor() : nullptr;
        auto* skeleton = actor ? actor->GetInstance() : nullptr;
        if (!skeleton) { probe.failed = true; Log("npcplayback: exact pose lost target skeleton"); return; }
        hostPose.instance = skeleton;
        hostPose.nodes = skeleton->mNodes.mData;
        hostPose.count = skeleton->mNodes.mLength;
        for (auto const& bone : frame.bones) {
            const int index = BoneIndex(*actor, skeleton->mNodes, bone);
            if (index < 0) {
                probe.failed = true;
                Logf("npcplayback: exact pose cannot map bone %s (source node %d, target nodes %d)",
                     bone.name.c_str(), bone.index, skeleton->mNodes.mLength);
                return;
            }
            pose::Bone encoded;
            if (!pose::Encode(TransformOf(bone.actorMatrix, net::HashName(bone.name.c_str())), encoded)) {
                probe.failed = true; Log("npcplayback: exact probe pose cannot be encoded"); return;
            }
            hostPose.bones.push_back({index, MatrixOf(pose::Decode(encoded))});
        }
    }
    NativePoseScope poseScope(probe.exactPose ? &hostPose : nullptr);
    UpdatePresentation(probe.target);
    if (probe.exactPose && (!hostPose.evaluated || !hostPose.applied || !hostPose.valid)) {
        probe.failed = true; Log("npcplayback: exact probe pose was not evaluated"); return;
    }
    Measure(frame, true);
    probe.previous = frame;
    probe.havePrevious = true;
    ++probe.frames;
}

void Probe(char const* argument, LogFn log)
{
    if (!qa::Enabled() || !net::IsHost()) {
        log("REFUSED passiveprobe needs=qa_running_host");
        return;
    }
    if (argument && !std::strcmp(argument, "prepare")) {
        log(Install() ? "PASSIVEPROBE prepared=1" : "REFUSED passiveprobe reason=unsupported_native_hooks");
        return;
    }
    if (!gCSession::GetInstance().IsGameRunning()) { log("REFUSED passiveprobe reason=not_in_game"); return; }
    if (argument && !std::strncmp(argument, "clone_", 6)) {
        constexpr char TargetName[] = "G3T_ReplayProbe";
        const Entity source = Entity::GetEntity(bCString(argument + 6));
        const Template shape(argument + 6);
        if (!source.GetInstance() || !shape.IsValid() || Entity::GetEntity(bCString(TargetName)).GetInstance()) {
            log("REFUSED passiveprobe reason=invalid_or_existing_clone");
            return;
        }
        Entity target = Entity::Spawn(shape, source.GetInstance()->GetWorldMatrix());
        if (!target.GetInstance()) { log("REFUSED passiveprobe reason=clone_spawn_failed"); return; }
        target.GetInstance()->SetName(bCString(TargetName));
        target.EnableProcessing(GETrue);
        log("PASSIVEPROBE cloned=1 name=G3T_ReplayProbe");
        return;
    }
    if (argument && !std::strcmp(argument, "off")) { Stop(); log("PASSIVEPROBE active=0"); return; }
    if (argument && *argument && std::strcmp(argument, "report")) {
        const bool exactPose = !std::strncmp(argument, "pose_", 5);
        if (exactPose) argument += 5;
        char sourceName[96] = {}, targetName[96] = {}, tail = '\0';
        if (std::sscanf(argument, "%95[^-]-%95s%c", sourceName, targetName, &tail) != 2) {
            log("REFUSED passiveprobe expected=source-target");
            return;
        }
        eCEntity* source = Entity::GetEntity(bCString(sourceName)).GetInstance();
        eCEntity* target = Entity::GetEntity(bCString(targetName)).GetInstance();
        auto isNpc = [](eCEntity* entity) {
            return entity && entity->GetPropertySet(eEPropertySetType_NPC) &&
                   entity->GetPropertySet(eEPropertySetType_ScriptRoutine) &&
                   entity != remote::ControlledBody() && entity != gCSession::GetInstance().GetHero() && !remote::IsPuppet(entity);
        };
        if (!isNpc(source) || !isNpc(target) || source == target || !Animation(target) || !Animation(source) ||
            !Animation(source)->GetActor()) {
            log("REFUSED passiveprobe reason=invalid_source_or_target");
            return;
        }
        if (!Install()) { log("REFUSED passiveprobe reason=unsupported_native_hooks"); return; }
        if (!RegisterEntity(target)) { log("REFUSED passiveprobe reason=unsupported_property_dispatch"); return; }
        Stop();
        probe.source = source;
        probe.exactPose = exactPose;
        probe.originalPose = target->GetWorldMatrix();
        probe.originalTask = Entity(target).Routine.GetCurrentTask();
        Entity(target).Routine.FullStop();
        Entity(target).CharacterMovement.StopMovement();
        probe.target = target;
        Tick();
    }
    char line[768];
    Format(line, "PASSIVEPROBE active=%d failed=%d frames=%u routine_blocked=%u script_blocked=%u movement_blocked=%u "
                 "animation_blocked=%u stop_blocked=%u entity_blocked=%u properties_blocked=%u slot_checks=%u slot_errors=%u replays=%u waiting_options=%u "
                 "max_position_error=%.3f max_rotation_error=%.6f max_phase_error=%.6f bone_checks=%u max_bone_error=%.3f immediate_bone_error=%.3f bone_lookup_error=%.3f worst_bone=%s",
           probe.target ? 1 : 0, probe.failed ? 1 : 0, probe.frames, probe.routineBlocked, probe.scriptBlocked,
           probe.movementBlocked, probe.animationBlocked, probe.animationStopsBlocked, probe.entityBlocked, probe.propertiesBlocked,
           probe.slotChecks, probe.slotErrors, probe.replays,
           probe.waitingForOptions,
           probe.maximumPositionError, probe.maximumRotationError, probe.maximumPhaseError, probe.boneChecks,
           probe.maximumBoneError, probe.maximumImmediateBoneError, probe.maximumBoneLookupError,
           *probe.maximumBoneName ? probe.maximumBoneName : "-");
    log(line);
}
}
