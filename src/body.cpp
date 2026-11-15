// SPDX-License-Identifier: GPL-3.0-or-later
#include "body.h"
#include "body_internal.h"

#include "config.h"
#include "guards.h"
#include "log.h"
#include "movement_policy.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3t::body {
unsigned actionsStarted = 0, actionsShown = 0, actionsPlayed = 0, pacifies = 0, ownerReleases = 0;
unsigned combatMoves = 0, combatMoveFaults = 0;
bool staggerEffects = true;
unsigned staggerEffectsPlayed = 0, staggerEffectsSkipped = 0;
int motionOwner = 0;
bool combatReplay = true, stanceSync = true;
unsigned kindStarted[3] = {}, kindPlayed[3] = {}, kindShown[3] = {};
unsigned scriptAnis = 0, scriptAniFaults = 0, loopRestarts = 0;
bool scriptAniReplay = true, loopRestart = true, swimStrokes = true;
unsigned swimStrokesPlayed = 0;

// The timeline now owns replica transforms without resetting navigation, and steers ahead of that same timeline.
// The old controller/buffer coupling made bodies stop between snapshots.
bool interpolate = true;
bool smoothWindow = true;
bool fallMove = false;  // off: no gain (stageWalk 28.10.: fall still frames 37/19/0 with it, 10/5 without)
bool actionSeq = true;  // on by default (stageActionSeq 28.10.: without it 68-83 starts of the same name lost, with it all replayed; stageV2 3/3 green)
unsigned sameNameStarts = 0, sameNameReplayed = 0;

int motionSeek = -1;

bool MotionSeekEnabled()
{
    return motionSeek < 0 ? GetConfig().motionSeek : motionSeek > 0;
}

uint8_t ActionSeq(eCEntity* entity, ActionClock& clock, char const* shown, uint16_t* elapsedMs)
{
    if (elapsedMs) *elapsedMs = 0;
    if (!movement::IsTrackedActionMotion(shown)) {
        *clock.name = '\0';
        return clock.seq;
    }
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    const double rawTime = actor ? actor->GetPlayTime(slot0) : 0.0;
    const double time = std::isfinite(rawTime) ? rawTime : 0.0;
    const double max = actor ? actor->GetMaxTime(slot0) : 0.0;
    const bool wrapped = !movement::IsLoopingPuppetMotion(shown) && time + 0.05 < clock.time;
    if (strcmp(shown, clock.name) != 0 || wrapped) {
        ++clock.seq;
        strncpy_s(clock.name, shown, _TRUNCATE);
    }
    clock.time = time;
    const double unitMs = (max > 0.0 && std::isfinite(max)) ? MotionUnitMs(max) : 1000.0;
    if (elapsedMs) *elapsedMs = static_cast<uint16_t>(std::clamp(time * unitMs, 0.0, 65535.0));
    return clock.seq;
}

uint16_t MotionFraction(eCEntity* entity)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    if (!actor || !actor->IsMotionRunning(slot0)) return movement::NoMotionFrac;
    const double max = actor->GetMaxTime(slot0);
    if (!(max > 0.0) || !std::isfinite(max)) return movement::NoMotionFrac;
    const double part = actor->GetPlayTime(slot0) / max;
    return movement::EncodeRunningMotionFrac(part);
}

// QA: the sender's travel over its states of the last 200 ms or more (cm/s, by its own clock).
float TravelSpeed(Track const& track)
{
    if (track.count < 2) return 0.0f;
    Track::Snap const& newest = track.snaps[track.count - 1];
    for (int i = track.count - 2; i >= 0; --i) {
        const uint32_t span = newest.sent - track.snaps[i].sent;
        if (span >= 200) return Distance(newest.position, track.snaps[i].position) * 1000.0f / span;
    }
    return 0.0f;
}

bool goalAhead = false;
bool leadOld = false;
constexpr float SteadyTurn = 30.0f;     // degrees (45 and 0.5-2x speed, stageGlide 28.10.: not steadier, a puppet stood 18 %)
constexpr uint32_t SteadyRise = 200;    // ms
constexpr uint32_t OvershootFor = 500;  // ms after an unsteady moment, for the glide causes
constexpr float DeadZone = 50.0f;       // cm
constexpr float FarGoal = 400.0f;       // cm: how far ahead of the shown pose a buffered body's movement controller is aimed

float Steadiness(Track const& track, State& state, uint32_t now)
{
    bool steady = track.count >= 3;
    if (steady) {
        Track::Snap const& a = track.snaps[track.count - 3];
        Track::Snap const& b = track.snaps[track.count - 2];
        Track::Snap const& c = track.snaps[track.count - 1];
        const int32_t t0 = static_cast<int32_t>(b.sent - a.sent), t1 = static_cast<int32_t>(c.sent - b.sent);
        float v0[3], v1[3], s0 = 0.0f, s1 = 0.0f, dot = 0.0f;
        for (int k = 0; k < 3; ++k) {
            v0[k] = t0 > 0 ? (b.position[k] - a.position[k]) * 1000.0f / t0 : 0.0f;
            v1[k] = t1 > 0 ? (c.position[k] - b.position[k]) * 1000.0f / t1 : 0.0f;
            s0 += v0[k] * v0[k], s1 += v1[k] * v1[k], dot += v0[k] * v1[k];
        }
        s0 = std::sqrt(s0), s1 = std::sqrt(s1);
        steady = t0 > 0 && t1 > 0 && t0 < 500 && t1 < 500 && s0 > 100.0f && s1 > 100.0f && s1 > s0 * 0.67f && s1 < s0 * 1.5f &&
                 dot / (s0 * s1) > std::cos(SteadyTurn * 3.14159265f / 180.0f);
    }
    if (!steady) {
        state.steadySince = 0;
        state.unsteadyAt = now;
        return 0.0f;
    }
    if (!state.steadySince) state.steadySince = now ? now : 1;
    const float rise = static_cast<float>(now - state.steadySince) / SteadyRise;
    return rise < 1.0f ? rise : 1.0f;
}

bool mirrorLead = false;

float const* SteerGoal(Track& track, uint32_t now, float ahead[3], float const* fallback, bool force)
{
    if (!goalAhead && !force) return fallback;
    float turn[4];
    const uint8_t lastCase = track.lastCase;
    const bool got = Sample(track, now, ahead, turn, GoalAhead, false);
    track.lastCase = lastCase;
    return got ? ahead : fallback;
}

int ReplayKind(char const* name)
{
    return movement::ReplayKind(name);
}

namespace {
char const* const Idle = "ZS_HangAround_DoNothing";

gCScriptRoutine_PS* RoutineOf(eCEntity* entity)
{
    return entity ? static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) : nullptr;
}

bool ParseCombatMove(char const* name, int& action, char* phase, size_t size)
{
    return movement::ParseCombatMove(name, action, phase, size);
}

GEBool CallCombatMove(gCScriptProcessingUnit::sAICombatMoveInstr_Args* args, gCScriptProcessingUnit* spu, bool* fault)
{
    __try {
        return gCScriptProcessingUnit::sAICombatMoveInstr(args, spu, GEFalse);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return GETrue;
    }
}

GEBool CallPlayAni(gCScriptProcessingUnit::sAIPlayAniInstr_Args* args, gCScriptProcessingUnit* spu, bool* fault)
{
    __try {
        return gCScriptProcessingUnit::sAIPlayAniInstr(args, spu, GEFalse);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return GETrue;
    }
}

bool PlayScriptAni(eCEntity* instance, State& state, char const* name)
{
    if (!scriptAniReplay || state.scriptAniFaulted) return false;
    gCScriptRoutine_PS* routine = RoutineOf(instance);
    if (!routine) return false;
    auto* args = GE_NEW(gCScriptProcessingUnit::sAIPlayAniInstr_Args)(instance, nullptr, bCString(name), 1.0f);
    bool fault = false;
    CallPlayAni(args, &routine->GetSPU(), &fault);
    if (fault) {
        ++scriptAniFaults;
        state.scriptAniFaulted = true;
        Logf("body: play-ani %s faulted; this body replays by StartPlayAni from now on", name);
        return false;
    }
    ++scriptAnis;
    return true;
}

bool PlayCombatMove(eCEntity* instance, State& state, char const* name, uint32_t now)
{
    int action = 0;
    char phase[24];
    if (!combatReplay || !state.combatMoves || state.combatFaulted || !ParseCombatMove(name, action, phase, sizeof(phase))) return false;
    if (!std::strcmp(phase, "Recover")) return static_cast<int32_t>(now - state.combatMoveAt) < 1500;
    gCScriptRoutine_PS* routine = RoutineOf(instance);
    if (!routine) return false;
    auto* args = GE_NEW(gCScriptProcessingUnit::sAICombatMoveInstr_Args)(instance, nullptr, static_cast<gEAction>(action),
                                                                          bCString(phase), 1.0f);
    bool fault = false;
    CallCombatMove(args, &routine->GetSPU(), &fault);
    if (fault) {
        ++combatMoveFaults;
        state.combatFaulted = true;
        Logf("body: combat move %s faulted; this body replays by motion name from now on", name);
        return false;
    }
    ++combatMoves;
    if (staggerEffects && !state.puppet && !std::strcmp(phase, "Hit") && ((action >= 20 && action <= 28) || action == 34)) {
        if ((state.localHitAt && now - state.localHitAt < 1500) || (state.staggerEffectAt && now - state.staggerEffectAt < 400)) {
            ++staggerEffectsSkipped;
        } else {
            state.staggerEffectAt = now ? now : 1;
            const bCString effect(action <= 24 ? "eff_col_wh_01_me_me" : "eff_col_wh_02_me_fl");
            if (EffectSystem::ExistsEffect(effect)) {
                EffectSystem::StartEffect(effect, Entity(instance));
                ++staggerEffectsPlayed;
            }
        }
    }
    state.combatMoveAt = now;
    return true;
}
}

char const* IdleTask() { return Idle; }

bool LocallyAttacking(eCEntity* entity)
{
    char name[128], phase[24];
    int action = 0;
    return entity && CurrentMotion(entity, name, sizeof(name)) && MotionRunning(entity) &&
           ParseCombatMove(name, action, phase, sizeof(phase)) &&
           ((action >= 1 && action <= 15) || action == 30) && !std::strcmp(phase, "Hit");
}

float SwingProgress(eCEntity* entity)
{
    if (!LocallyAttacking(entity)) return -1.0f;
    const uint16_t part = MotionFraction(entity);
    return part >= movement::NoMotionFrac ? -1.0f : static_cast<float>(part) / static_cast<float>(movement::MaxRunningMotionFrac);
}


bool MotionName(eCVisualAnimation_PS* animation, int type, char* out, size_t size)
{
    __try {
        bCString const& file = animation->GetMotionDesc(static_cast<eCWrapper_emfx2Actor::eEMotionType>(type)).GetMotionFilename();
        strncpy_s(out, size, file.GetText(), _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy_s(out, size, "<fault>", _TRUNCATE);
        return false;
    }
}

bool CurrentMotion(eCEntity* entity, char* out, size_t size)
{
    *out = '\0';
    eCVisualAnimation_PS* animation =
        entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    return animation && MotionName(animation, 0, out, size);
}

bool MotionRunning(eCEntity* entity)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    return actor && actor->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0));
}

float BoneTop(eCVisualAnimation_PS* animation, bCVector const& base, int* bones)
{
    float top = -1e9f;
    *bones = 0;
    __try {
        bCMatrix m;
        for (int i = 0; i < 128 && animation->GetBoneMatrix(i, m); ++i) {
            bCVector const& at = m.GetTranslation();
            const float dx = at.GetX() - base.GetX(), dz = at.GetZ() - base.GetZ(), dy = at.GetY() - base.GetY();
            if (dx * dx + dz * dz > 400.0f * 400.0f || dy < -200.0f || dy > 400.0f) continue;  // not part of this body's skeleton
            if (dy > top) top = dy;
            ++*bones;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2.0f;
    }
    return *bones ? top : -1.0f;
}

// Height of the posed body: its highest bone above its position (a lying body is flat). The bounds of the actor do not
// follow the pose (deaddiag: about 195 cm for standing and lying orcs alike).
float BoundaryHeight(eCEntity* entity)
{
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    if (!animation) return -1.0f;
    int bones = 0;
    return BoneTop(animation, entity->GetWorldMatrix().GetTranslation(), &bones);
}

bCMatrix Pose(eCEntity const& base, float const position[3], float const rotation[4])
{
    bCMatrix m = base.GetWorldMatrix();
    bCQuaternion q;
    q.AccessX() = rotation[0];
    q.AccessY() = rotation[1];
    q.AccessZ() = rotation[2];
    q.AccessW() = rotation[3];
    m.ModifyRotation(q);
    m.AccessTranslation() = bCVector(position[0], position[1], position[2]);
    return m;
}

bool PlayAniInstruction(eCEntity* instance, State& state, char const* name)
{
    if (guards::RejectRemoteWrite(instance, "body::PlayAniInstruction")) return false;
    return PlayScriptAni(instance, state, name);
}

int AIModeOf(eCEntity* entity)
{
    gCScriptRoutine_PS* routine = RoutineOf(entity);
    return routine ? static_cast<int>(static_cast<gEAIMode&>(routine->AccessAIMode())) : -1;
}

void Pacify(Entity& entity, State& state)
{
    if (!entity.GetInstance() || guards::RejectRemoteWrite(entity.GetInstance(), "body::Pacify")) return;
    // Never touch combat targets here: clearing them while a scene script ran a combat state crashed the
    // game (gCScriptProcessingUnit::sAICombatMoveInstr). AI states of driven bodies are skipped in ai.cpp.
    entity.Routine.FullStop();
    if (gCScriptRoutine_PS* routine = RoutineOf(entity.GetInstance())) {
        bCString& name = static_cast<bCString&>(routine->AccessRoutine());
        gEAIMode& mode = static_cast<gEAIMode&>(routine->AccessAIMode());
        if (!state.pacified) {
            strncpy_s(state.routine, name.GetText(), _TRUNCATE);
            state.aiMode = static_cast<int>(mode);
        }
        name = bCString("");
        mode = gEAIMode_Abandoned;
    }
    entity.Routine.SetTask(bCString(Idle));
    state.pacified = true;
}

void Release(Entity& entity, State& state)
{
    if (guards::RejectRemoteWrite(entity.GetInstance(), "body::Release")) { state = State{}; return; }
    if (!state.pacified || !entity.GetInstance()) { state = State{}; return; }
    entity.CharacterMovement.StopMovement();
    if (gCScriptRoutine_PS* routine = RoutineOf(entity.GetInstance())) {
        static_cast<bCString&>(routine->AccessRoutine()) = bCString(state.routine);
        static_cast<gEAIMode&>(routine->AccessAIMode()) = static_cast<gEAIMode>(state.aiMode);
    }
    entity.Routine.FullStop();
    entity.Routine.ContinueRoutine();
    state = State{};
}

bool SlotRunning(eCEntity* instance)
{
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    return actor && actor->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0));
}

void ReleaseFinished(eCEntity* instance, State& state)
{
    if (!state.combatOwned) return;
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    if (actor && actor->IsMotionRunning(slot0)) return;
    if (animation) animation->SetMotionOwner(slot0, static_cast<eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner>(2));
    state.combatOwned = false;
    state.actionUntil = 0;
    *state.playing = '\0';
    ++ownerReleases;
}

void FollowSenderOnce(eCEntity* instance, State& state, Target const& target, uint32_t now)
{
    const int kind = state.puppet ? 1 : 0;
    if (!target.motion || !*target.motion || strstr(target.motion, "_Move_") || strstr(target.motion, "_Ambient_") ||
        ReplayKind(target.motion) != 0) { ++followSkips[kind][1]; return; }
    if (target.motionFrac < 0 || target.motionFrac >= movement::NoMotionFrac) { ++followSkips[kind][0]; return; }
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    if (!actor || !actor->IsMotionRunning(slot0)) { ++followSkips[kind][2]; return; }
    char shows[128];
    if (!MotionName(animation, 0, shows, sizeof(shows)) || strcmp(shows, target.motion) != 0) {
        ++followSkips[kind][3];
        if (otherNamesLogged[kind] < 8 && ++otherNamesLogged[kind])
            Logf("body: seek %s: the sender plays %.100s, here %.100s", kind ? "puppet" : "mirror", target.motion, shows);
        return;
    }
    const double max = actor->GetMaxTime(slot0);
    if (!(max > 0.0)) { ++followSkips[kind][2]; return; }
    const double unitMs = MotionUnitMs(max);
    const double expected = target.motionFrac / static_cast<double>(movement::MaxRunningMotionFrac) * max + target.motionAgeMs / unitMs;
    if (expected >= max) { ++followSkips[kind][4]; return; }
    ++followSkips[kind][5];
    float lag = static_cast<float>((expected - actor->GetPlayTime(slot0)) * unitMs);
    const bool on = MotionSeekEnabled();
    if (on && std::fabs(lag) > SeekOver && static_cast<int32_t>(now - state.seekAt) >= 0) {
        actor->SetPlayTime(slot0, static_cast<float>(expected));
        state.seekAt = now + SeekEvery;
        ++seeks[kind];
        lag = static_cast<float>((expected - actor->GetPlayTime(slot0)) * unitMs);
    }
    if (on && std::fabs(lag) > PaceDeadZone) {
        actor->SetPlaySpeed(slot0, std::clamp(1.0f + lag / PaceOver, 0.7f, 1.4f));
        state.paced = state.paceNow = true;
        ++paceSteps[kind];
    }
    std::vector<float>& lags = motionLags[kind];
    if (lags.size() < 50000) {
        lagMsUnit = max > 30.0;
        lags.push_back(lag);
    }
}

// The pace belongs to one followed motion: anything else on slot 0 plays at its own speed.
void FollowSender(eCEntity* instance, State& state, Target const& target, uint32_t now)
{
    state.paceNow = false;
    FollowSenderOnce(instance, state, target, now);
    if (!state.paced || state.paceNow) return;
    state.paced = false;
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    if (eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr)
        actor->SetPlaySpeed(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0), 1.0f);
}

void DrivePuppetTimeline(Entity& entity, eCEntity* instance, State& state, Target const& target, bCMatrix const& pose,
                         bool discontinuity, uint32_t now)
{
    if (!instance || guards::RejectRemoteWrite(instance, "body::Drive")) return;
    state.steered = false;
    if (state.walking) {
        entity.CharacterMovement.StopMovement();
        state.walking = false;
    }
    if (discontinuity) {
        ++glide[1].hardSnaps;
        entity.CharacterMovement.StopMovement();
        entity.MoveTo(pose);
        state.lastBranch = 1;
        state.presentationSerial = target.presentationSerial;
        *state.playing = '\0';
    } else {
        instance->SetToWorldMatrix(pose);
        state.lastBranch = 0;
    }
    if (stanceSync && target.aniState >= 0 && static_cast<int>(entity.Routine.AniState) != target.aniState)
        entity.Routine.AniState = static_cast<gEAniState>(target.aniState);
    auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation));
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    using Owner = eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner;
    if (!animation || !actor || !target.motion || !*target.motion) {
        if (animation && (*state.playing || animation->GetMotionOwner(slot0) == static_cast<Owner>(4))) {
            animation->SetMotionOwner(slot0, static_cast<Owner>(2));
            if (actor && actor->IsMotionRunning(slot0)) animation->StopMotion(slot0, 0.1f);
            if (actor) actor->SetPlaySpeed(slot0, 1.0f);
        }
        *state.playing = '\0';
        state.combatOwned = false;
        state.paced = false;
        state.actionUntil = 0;
        return;
    }
    char fileName[128];
    if (std::strstr(target.motion, ".xmot")) strncpy_s(fileName, target.motion, _TRUNCATE);
    else std::snprintf(fileName, sizeof(fileName), "%s.xmot", target.motion);
    const bool looping = movement::IsLoopingPuppetMotion(fileName);
    const bool trackedAction = movement::IsTrackedActionMotion(fileName);
    bool seqRestart = false;
    if (target.actionSeq >= 0) {
        if (!looping && state.actionSeq >= 0 && target.actionSeq != state.actionSeq && std::strcmp(target.motion, state.playing) == 0) {
            ++sameNameStarts;
            if (actionSeq) { seqRestart = true; ++sameNameReplayed; }
        }
        state.actionSeq = target.actionSeq;
    }
    char currentSlot[128] = {};
    MotionName(animation, 0, currentSlot, sizeof(currentSlot));
    const bool sameClip = std::strcmp(target.motion, state.playing) == 0 && std::strcmp(currentSlot, fileName) == 0;
    const bool needStart = !sameClip || seqRestart || (looping && !actor->IsMotionRunning(slot0));
    if (needStart) {
        if (!target.keepOverlays) entity.StopPlayAni();
        if (gCScriptRoutine_PS* routine = RoutineOf(instance)) {
            routine->AIStopPlayAnimation();
            routine->AIStopCombatMove();
        }
        auto& desc = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation->GetMotionDesc(slot0));
        desc.SetMotion(bCString(fileName));
        if (auto* resource = desc.GetMotion()) {
            const float blendSec = discontinuity ? 0.0f : (trackedAction ? 0.10f : 0.14f);
            if (blendSec > 0.0f) actor->SwitchFadeOut(slot0, GEFalse, blendSec);
            if (!target.keepOverlays) {
                for (int extra = 1; extra <= 3; ++extra) {
                    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(extra);
                    if (actor->IsMotionRunning(slot)) animation->StopMotion(slot, blendSec);
                }
            }
            struct MotionPlayDesc {
                float blendInTime;
                int32_t playMode;
                float playSpeed;
                int32_t maxLoops;
                float targetWeight;
                float blendOutTime;
                int32_t blendMode;
            } play{blendSec, 0, 1.0f, looping ? -1 : 1, 1.0f, 0.0f, 1};
            static_assert(sizeof(MotionPlayDesc) == 28);
            animation->SetMotion(slot0, resource);
            animation->PlayMotion(slot0, reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(&play));
            animation->SetMotionOwner(slot0, static_cast<Owner>(4));
            actor->PauseMotion(GEFalse, slot0);
            actor->SetTargetWeight(slot0, 1.0f);
            state.combatOwned = false;
            state.paced = false;
        } else {
            char stem[96];
            strncpy_s(stem, fileName, _TRUNCATE);
            if (char* ext = std::strstr(stem, ".xmot")) *ext = '\0';
            if (!PlayScriptAni(instance, state, stem)) entity.StartPlayAni(bCString(stem), 0, GEFalse, 1.0f, GEFalse);
        }
        if (swimStrokes && static_cast<gECharMovementMode>(target.movementMode) == gECharMovementMode_Swim &&
            std::strstr(fileName, "_Swim_") && std::strstr(fileName, "_Move_"))
            ++swimStrokesPlayed;
        if (trackedAction) {
            char stem[96];
            strncpy_s(stem, fileName, _TRUNCATE);
            if (char* ext = std::strstr(stem, ".xmot")) *ext = '\0';
            const int kind = ReplayKind(stem);
            ++kindStarted[kind];
            ++actionsStarted;
            if (actor->IsMotionRunning(slot0)) { ++actionsPlayed; ++kindPlayed[kind]; }
            state.checkAt = now + 150;
            strncpy_s(state.started, stem, _TRUNCATE);
            state.startedByMove = false;
            state.actionUntil = now + 1200;
        } else {
            state.actionUntil = 0;
        }
        strncpy_s(state.playing, target.motion, _TRUNCATE);
    }
    if (!actor->IsMotionRunning(slot0)) return;
    animation->SetMotionOwner(slot0, static_cast<Owner>(4));
    const double max = actor->GetMaxTime(slot0);
    if (!(max > 0.0) || !std::isfinite(max)) return;
    if (state.lastCase == 3 && target.motionAgeMs > 180 && movement::IsTranslatingLocomotion(fileName)) {
        actor->SetPlaySpeed(slot0, 0.0f);
        state.paced = true;
        return;
    }
    if (target.motionFrac < 0 || target.motionFrac >= movement::NoMotionFrac) {
        if (state.paced) { actor->SetPlaySpeed(slot0, 1.0f); state.paced = false; }
        return;
    }
    const double unitMs = MotionUnitMs(max);
    const double expected = movement::ExpectedPuppetPlayTime(max, target.motionFrac, target.motionAgeMs, unitMs, looping);
    const double maxPlay = std::nextafter(max, 0.0);
    if (needStart) {
        actor->SetPlayTime(slot0, static_cast<float>(std::clamp(expected, 0.0, maxPlay)));
        actor->SetPlaySpeed(slot0, 1.0f);
        return;
    }
    if (!looping && expected >= max) {
        if (state.paced) { actor->SetPlaySpeed(slot0, 1.0f); state.paced = false; }
        ++followSkips[1][4];
        return;
    }
    float lag = movement::PuppetPhaseLagMs(actor->GetPlayTime(slot0), expected, max, unitMs, looping);
    const bool seekOn = MotionSeekEnabled();
    const float seekLimit = looping ? 160.0f : SeekOver;
    if (seekOn && std::fabs(lag) > seekLimit && static_cast<int32_t>(now - state.seekAt) >= 0) {
        actor->SetPlayTime(slot0, static_cast<float>(std::clamp(expected, 0.0, maxPlay)));
        state.seekAt = now + SeekEvery;
        ++seeks[1];
        lag = movement::PuppetPhaseLagMs(actor->GetPlayTime(slot0), expected, max, unitMs, looping);
    }
    if (seekOn) {
        const float speed = movement::PuppetPaceSpeed(lag);
        actor->SetPlaySpeed(slot0, speed);
        state.paced = speed != 1.0f;
        if (state.paced) ++paceSteps[1];
    } else if (state.paced) {
        actor->SetPlaySpeed(slot0, 1.0f);
        state.paced = false;
    }
    if (!looping && ReplayKind(fileName) == 0 && !std::strstr(fileName, "_Move_")) {
        ++followSkips[1][5];
        if (motionLags[1].size() < 50000) {
            lagMsUnit = max > 30.0;
            motionLags[1].push_back(lag);
        }
    }
}

void Drive(Entity& entity, State& state, Target const& target, uint32_t now)
{
    eCEntity* instance = entity.GetInstance();
    if (!instance || guards::RejectRemoteWrite(instance, "body::Drive")) return;
    if (state.localHitUntil && static_cast<int32_t>(state.localHitUntil - now) > 0 &&
        (!target.timeline || state.presentationSerial == target.presentationSerial)) {
        entity.CharacterMovement.StopMovement();
        state.walking = state.steered = false;
        const bCVector& at = instance->GetWorldPosition();
        state.lastPos[0] = at.GetX(); state.lastPos[1] = at.GetY(); state.lastPos[2] = at.GetZ();
        state.lastAt = now;
        return;  // do not pacify or replace a local hit reaction with a pre-hit buffered motion
    }
    state.localHitUntil = 0;
    if (target.motion && *target.motion && !strstr(target.motion, "_Move_") && ReplayKind(target.motion) == 0) ++combatTargets[state.puppet ? 1 : 0];
    ReleaseFinished(instance, state);
    const bCString task = entity.Routine.GetCurrentTask();
    // Undo any task the AI picked in the same frame: a delay let combat AI start attacks.
    if (!state.pacified || (task.GetLength() && strcmp(task.GetText(), Idle) != 0)) {
        if (state.pacified) {
            ++state.repacified;
            strncpy_s(state.lastForeign, task.GetText(), _TRUNCATE);
        }
        Pacify(entity, state);
        ++pacifies;
        state.lastPacify = now;
        *state.playing = '\0';
    }
    if (state.checkAt && static_cast<int32_t>(now - state.checkAt) >= 0) {
        char shows[128];
        CurrentMotion(instance, shows, sizeof(shows));
        // QA: did the action really show (stage V2)?
        const bool shown = !strncmp(shows, state.started, strlen(state.started));
        if (shown) ++actionsShown, kindShown[ReplayKind(state.started)] += !state.startedByMove;
        else if (actionsStarted - actionsShown < 40)
            Logf("body: started %s, 150 ms later shows %s", state.started, shows);
        state.checkAt = 0;
    }
    const bCVector& at = instance->GetWorldPosition();
    const float current[3] = {at.GetX(), at.GetY(), at.GetZ()};
    const float error = Distance(current, target.position);
    // QA (glide): how the body travelled since the last Drive. Faster than 15 m/s is a jump (snap), not travel.
    const uint32_t dt = now - state.lastAt;
    const float traveled = Distance(current, state.lastPos);
    if (state.lastAt && dt > 0 && dt < 500) {
        const float moved = Distance(current, state.lastPos), speed = moved * 1000.0f / dt;
        Glide& g = glide[state.puppet ? 1 : 0];
        if (moved > 150.0f && speed > 1500.0f) ++g.snaps;
        else if (speed > 100.0f) {
            g.movingMs += dt;
            const bool timelineDriven = state.puppet && target.timeline;
            ((state.steered || timelineDriven) ? g.steerMs : g.placeMs) += dt;
            char shows[128];
            CurrentMotion(instance, shows, sizeof(shows));
            if (!strstr(shows, "_Move_")) {
                g.glideMs += dt;
                if (!state.steered && !timelineDriven) g.glidePushMs += dt;
                else if (state.unsteadyAt && now - state.unsteadyAt < OvershootFor) g.glideOvershootMs += dt;
                else g.glideOtherMs += dt;
                auto& seen = glideMotions[state.puppet ? 1 : 0];
                if (auto it = seen.find(shows); it != seen.end()) it->second += dt;
                else if (seen.size() < 200) seen[shows] = dt;
            }
        }
    }
    const bool senderMoves = smoothWindow ? TravelSpeed(state.track) > 100.0f : target.speed > 100.0f;
    if (state.lastAt && dt > 0 && dt < 500 && senderMoves) {
        Glide& g = glide[state.puppet ? 1 : 0];
        const float step = Distance(current, state.lastPos);
        ++g.frames;
        if (step > 30.0f) ++g.jumps;
        if (step > g.maxStep) g.maxStep = step;
        if (step < 0.5f) {
            ++g.stillFrames;
            if (stillMotions[state.puppet ? 1 : 0].size() < 200 || stillMotions[state.puppet ? 1 : 0].count(target.motion ? target.motion : "-"))
                ++stillMotions[state.puppet ? 1 : 0][target.motion && *target.motion ? target.motion : "-"];
            ++g.stillBy[state.lastBranch & 3];
            ++g.stillCase[state.lastCase < 5 ? state.lastCase : 0];
        }
        if (state.track.count) g.lagSum += Distance(current, state.track.snaps[state.track.count - 1].position);
    }
    std::memcpy(state.lastPos, current, sizeof(current));
    state.lastAt = now;
    std::memcpy(state.shownPos[state.shownNext], current, sizeof(current));
    state.shownAt[state.shownNext] = now;
    state.shownNext = (state.shownNext + 1) % 16;
    state.lastCase = state.track.lastCase;
    state.track.lastCase = 0;
    const bCMatrix pose = Pose(*instance, target.position, target.rotation);
    const gECharMovementMode mode = static_cast<gECharMovementMode>(target.movementMode);
    const bool falling = fallMove && target.motion && strstr(target.motion, "_Fall_");
    const bool action = target.motion && *target.motion && !strstr(target.motion, "_Move_") && !strstr(target.motion, "_Ambient_") && !falling;
    const bool moving = !action && ((mode >= gECharMovementMode_Sneak && mode <= gECharMovementMode_Sprint) || falling) &&
                        (target.speed > 40.0f || error > 60.0f);
    const bool actingNow = static_cast<int32_t>(state.actionUntil - now) > 0;
    const bool catchUp = target.smooth && !action && !actingNow && error >= DeadZone && error <= target.snapAt;
    const bool swimStroke = swimStrokes && mode == gECharMovementMode_Swim && target.motion && strstr(target.motion, "_Swim_") &&
                            strstr(target.motion, "_Move_");
    if (!action && mode >= gECharMovementMode_Sneak && mode <= gECharMovementMode_Sprint &&
        target.speed > 40.0f && dt > 0 && dt < 500) {
        state.walkExpectedMs += dt;
        if (traveled > 0.5f) {
            state.walkTravelMs += dt;
            char shown[128] = {};
            CurrentMotion(instance, shown, sizeof(shown));
            if (!IsLocomotionMotion(shown) || !MotionRunning(instance)) state.walkSlideMs += dt;
        } else {
            state.walkStillMs += dt;
        }
    }
    const bool discontinuity = target.timeline && state.presentationSerial != target.presentationSerial;
    if (state.puppet && target.timeline) {
        DrivePuppetTimeline(entity, instance, state, target, pose, discontinuity, now);
        return;
    }
    if (!action) state.actionUntil = 0;
    // The timeline owns the position of a buffered body every frame. Its movement controller only supplies the walk motion (it
    // is given a goal far ahead, below), so it can never "arrive" and drop the clip while the body keeps travelling.
    if (target.timeline && !discontinuity)
        instance->SetToWorldMatrix(pose);
    if (discontinuity || (!target.timeline && error > target.snapAt)) {
        ++glide[state.puppet ? 1 : 0].hardSnaps;
        entity.CharacterMovement.StopMovement();
        entity.MoveTo(pose);
        state.walking = false;
        state.steered = false;
        state.lastBranch = 1;
        state.presentationSerial = target.presentationSerial;
    } else if (moving || catchUp || (!action && state.walking && error > 60.0f && !(target.smooth && error < DeadZone))) {
        if (stanceSync && static_cast<int32_t>(state.actionUntil - now) <= 0 && target.aniState >= 0 &&
            static_cast<int>(entity.Routine.AniState) != target.aniState)
            entity.Routine.AniState = static_cast<gEAniState>(target.aniState);
        if (settleStance && (!state.walking || static_cast<int32_t>(now - state.settleAt) >= 0)) {
            state.settleAt = now + 250;
            char shows[128];
            CurrentMotion(instance, shows, sizeof(shows));
            if (*shows && !strstr(shows, "_Move_")) {
                entity.StopPlayAni();
                if (gCScriptRoutine_PS* routine = RoutineOf(instance)) {
                    routine->AIStopPlayAnimation();
                    routine->AIStopCombatMove();
                }
                if (auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation))) {
                    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
                    ++stopOwners[static_cast<unsigned>(animation->GetMotionOwner(slot0)) & 7];
                    if (stopMotion) animation->StopMotion(slot0, 0.2f);
                }
                ++stanceStops;
            }
        }
        entity.CharacterMovement.SetMovementMode(moving ? (falling ? gECharMovementMode_Run : mode) : gECharMovementMode_Walk);
        float const* goal = target.goal ? target.goal : target.position;
        float farGoal[3];
        if (target.timeline && moving) {
            float dx = goal[0] - target.position[0], dy = goal[1] - target.position[1], dz = goal[2] - target.position[2];
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length > 5.0f) {
                const float scale = FarGoal / length;
                farGoal[0] = target.position[0] + dx * scale, farGoal[1] = target.position[1] + dy * scale, farGoal[2] = target.position[2] + dz * scale;
                goal = farGoal;
            }
        }
        entity.CharacterMovement.SetMovementGoal(bCVector(goal[0], goal[1], goal[2]));
        state.walking = true;
        state.steered = true;
        state.lastBranch = 0;
        *state.playing = '\0';
    } else {
        state.steered = false;
        if (state.walking) {
            entity.CharacterMovement.StopMovement();
            state.walking = false;
        }
        const bool acting = static_cast<int32_t>(state.actionUntil - now) > 0;
        if (target.timeline) {
            state.lastBranch = 3;
        } else if (target.smooth && !acting && error < DeadZone) {
            const bCVector& here = instance->GetWorldPosition();
            const float stay[3] = {here.GetX(), here.GetY(), here.GetZ()};
            entity.MoveTo(Pose(*instance, stay, target.rotation));
            state.lastBranch = 3;
        } else if (!acting || error > (swimStroke ? 40.0f : 100.0f)) {
            if (acting) ++glide[state.puppet ? 1 : 0].actionSnaps;
            entity.MoveTo(pose), state.lastBranch = 2;
        } else {
            state.lastBranch = 3;
        }
        const bool loopLock = acting && state.playing[0] && strstr(state.playing, "_Loop_") != nullptr;
        if (stanceSync && (!acting || loopLock) && target.aniState >= 0 &&
            static_cast<int>(entity.Routine.AniState) != target.aniState) {
            entity.Routine.AniState = static_cast<gEAniState>(target.aniState);
            *state.playing = '\0';
        }
        if (loopRestart && target.motion && (strstr(target.motion, "_Loop_") || strstr(target.motion, "_Ambient_") || swimStroke) &&
            (!strstr(target.motion, "_Move_") || swimStroke) &&
            !strcmp(target.motion, state.playing) && static_cast<int32_t>(now - state.loopCheckAt) >= 0) {
            const bool idle = strstr(target.motion, "_Ambient_") != nullptr;
            state.loopCheckAt = now + (idle ? 1000 : 300);
            char shows[128];
            CurrentMotion(instance, shows, sizeof(shows));
            if (!SlotRunning(instance) || (!idle && strcmp(shows, target.motion) != 0)) {
                *state.playing = '\0';
                state.idleAsScriptAni = idle;
                ++loopRestarts;
            } else if (!idle) {
                state.actionUntil = now + 1200;
            }
        }
        if (target.actionSeq >= 0) {
            if (state.actionSeq >= 0 && target.actionSeq != state.actionSeq && target.motion && *target.motion && !strcmp(target.motion, state.playing)) {
                ++sameNameStarts;
                if (actionSeq) {
                    *state.playing = '\0';
                    ++sameNameReplayed;
                }
            }
            state.actionSeq = target.actionSeq;
        }
        if (target.motion && *target.motion && (!strstr(target.motion, "_Move_") || swimStroke) && strcmp(target.motion, state.playing) != 0) {
            if (swimStroke) ++swimStrokesPlayed;
            char name[96];
            strncpy_s(name, target.motion, _TRUNCATE);
            if (char* extension = strstr(name, ".xmot")) *extension = '\0';
            const bool combatMove = PlayCombatMove(instance, state, name, now);
            const bool scriptAni = !combatMove && (!strstr(name, "_Ambient_") || state.idleAsScriptAni) && PlayScriptAni(instance, state, name);
            state.idleAsScriptAni = false;
            if (!combatMove && !scriptAni) entity.StartPlayAni(bCString(name), 0, GEFalse, 1.0f, GEFalse);
            if (!strstr(name, "_Ambient_")) {
                char now0[128];
                CurrentMotion(instance, now0, sizeof(now0));
                const int kind = ReplayKind(name);
                if (!combatMove) ++kindStarted[kind];
                if (!strncmp(now0, name, strlen(name))) ++actionsPlayed, kindPlayed[kind] += !combatMove;
                else if (actionsStarted - actionsPlayed < 20) Logf("body: StartPlayAni %s did not play, slot 0 shows %s", name, now0);
                if (motionOwner > 0) {
                    if (auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation)))
                        animation->SetMotionOwner(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0),
                                                  static_cast<eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner>(motionOwner));
                    state.combatOwned = true;
                }
                state.actionUntil = now + 1200;
                ++actionsStarted;
                state.checkAt = now + 150;
                strncpy_s(state.started, name, _TRUNCATE);
                state.startedByMove = combatMove;
            }
            strncpy_s(state.playing, target.motion, _TRUNCATE);
        }
        FollowSender(instance, state, target, now);
    }
}
}
