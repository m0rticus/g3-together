// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk.h"
#include "talk_policy.h"

#include "ai.h"
#include "body.h"
#include "guards.h"
#include "guid.h"
#include "guide_policy.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "remote.h"
#include "story.h"
#include "telemetry.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace g3t::talk {
namespace {
static_assert(talk_policy::AiModeCombat == gEAIMode_Combat);
static_assert(talk_policy::AiModeFlee == gEAIMode_Flee);
static_assert(talk_policy::AiModeTalk == gEAIMode_Talk);
static_assert(talk_policy::MoveModeSneak == gECharMovementMode_Sneak);
static_assert(talk_policy::MoveModeSprint == gECharMovementMode_Sprint);
static_assert(talk_policy::AniStateDummy0 == gEAniState_Dummy0);
static_assert(talk_policy::AniStateDummy1 == gEAniState_Dummy1);
static_assert(talk_policy::AniStateStand == gEAniState_Stand);
static_assert(talk_policy::AniStateKneel == gEAniState_Kneel);
static_assert(talk_policy::AniStateSitGround == gEAniState_SitGround);
static_assert(talk_policy::AniStateSitStool == gEAniState_SitStool);
static_assert(talk_policy::AniStateSitBench == gEAniState_SitBench);
static_assert(talk_policy::AniStateSitThrone == gEAniState_SitThrone);
static_assert(talk_policy::AniStateSleepBed == gEAniState_SleepBed);
static_assert(talk_policy::AniStateSleepGround == gEAniState_SleepGround);
static_assert(talk_policy::AniStateTalkStand == gEAniState_TalkStand);
static_assert(talk_policy::AniStateTalkSitGround == gEAniState_TalkSitGround);
static_assert(talk_policy::AniStateTalkSitStool == gEAniState_TalkSitStool);
static_assert(talk_policy::AniStateTalkSitBench == gEAniState_TalkSitBench);
static_assert(talk_policy::AniStateTalkSitThrone == gEAniState_TalkSitThrone);
static_assert(talk_policy::PoseNone == gEPose_None);
static_assert(talk_policy::PoseP0 == gEPose_P0);
static_assert(talk_policy::UseTypeNone == gEUseType_None);
static_assert(talk_policy::UseTypeTorch == gEUseType_Torch);

// Client: the NPC our hero is talking to, tracked by GUID across frames rather than a raw entity pointer.
struct ClientTalkState {
    bool active = false;
    bool holdSent = false;
    uint32_t welcome = 0;
    uint8_t guid[16] = {};
    bool pendingActive = false;
    uint32_t pendingUntil = 0;
    uint8_t pendingGuid[16] = {};
    bool preparedActive = false;
    uint8_t preparedGuid[16] = {};
    eCEntity* assessingNpc = nullptr;

    void ClearPending()
    {
        pendingActive = false;
        pendingUntil = 0;
        std::memset(pendingGuid, 0, sizeof(pendingGuid));
    }

    void ClearPrepared()
    {
        preparedActive = false;
        std::memset(preparedGuid, 0, sizeof(preparedGuid));
    }

    void Reset()
    {
        active = false;
        holdSent = false;
        welcome = 0;
        std::memset(guid, 0, sizeof(guid));
        ClearPending();
        ClearPrepared();
        assessingNpc = nullptr;
    }
} clientTalk;
unsigned begins = 0, ends = 0;

// Host: NPCs held for a remote player's dialog.
struct Hold {
    Entity entity;
    uint32_t gestureUntil = 0;
    uint8_t player;
    uint8_t guid[16] = {};
    uint32_t generation = 0, incarnation = 0;
    uint32_t pendingGesture = 0;
};
std::unordered_map<std::string, Hold> holds;
bool holdDisabled = false;
unsigned holdsStarted = 0, holdsEnded = 0, repacified = 0;
// Phase 5 (25.10., dialogviewdiag): while a client talked to the host's NPC, the host could start a dialog of its own with it,
// two dialogs with one NPC at once. As in single player an NPC in a dialog is busy: the jump into the game's AssessTalk
// (Script_Game 0x30d50) turns the host's hero away from a held NPC regardless of (self, other) argument order.
constexpr uintptr_t AssessTalkRva = 0x30d50;
gFScript originalAssessTalk = nullptr;
bool assessInstalled = false, assessNative = false;
DWORD lastAssessTry = 0, lastBusyMessage = 0;
unsigned busyRefused = 0;
// Gestures (owner 25.10.): the client's copy of its dialog partner runs Say/gesture scripts; each slot-0 gesture motion is
// forwarded immediately (world state kind 15, motion name once through net::SendMotionName) and played on the host's held NPC.
constexpr uint8_t GestureKind = 15;
bool gestures = true;  // QA negative control: gesturesoff
std::unordered_map<uint32_t, bool> gestureNamesSent;
uint32_t gestureNamesWelcome = 0;
unsigned gesturesSent = 0, gesturesPlayed = 0, gesturesUnknown = 0;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

bool SameGuid(eCEntity const* entity, uint8_t const expected[16])
{
    if (!entity) return false;
    uint8_t actual[16] = {};
    guid::CopyEntity(const_cast<eCEntity*>(entity), actual);
    return std::memcmp(actual, expected, sizeof(actual)) == 0;
}

gCInfoManager_PS* Infos() { return gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr; }

// Must be called AFTER removing the Hold from `holds` so talk::IsHeld(entity) is already false when ContinueRoutine()
// invokes SelectRoutineTask (preserving npcs::ContextPlayer for any newly started guide/companion relationship).
void Release(Hold& hold)
{
    hold.entity = Entity(npcs::NpcByGuid(hold.guid));
    if (hold.entity.GetInstance()) {
        if (!hold.entity.IsDead() && npcs::Incarnation(hold.guid) == hold.incarnation) {
            const uint8_t actingPlayer = guide_policy::GuideRoutineActingPlayer(
                net::IsHost(), false, npcs::ContextPlayer(hold.entity.GetInstance()));
            guards::Acting context(actingPlayer > 1 ? remote::PuppetOf(actingPlayer) : nullptr);
            hold.entity.SetAlignmentTarget(Entity());
            hold.entity.SetLookAtTarget(Entity());
            if (talk_policy::ShouldRestartRoutineOnTalkRelease(
                    hold.entity.IsDead(), hold.entity.IsDown(), static_cast<int>(hold.entity.Routine.AIMode))) {
                hold.entity.Routine.FullStop();
                hold.entity.Routine.ContinueRoutine();
            }
        }
    }
    ++holdsEnded;
}

std::unordered_map<std::string, Hold>::iterator EraseAndReleaseHold(std::unordered_map<std::string, Hold>::iterator it)
{
    Hold ended = std::move(it->second);
    auto next = holds.erase(it);
    Release(ended);
    return next;
}

bool PlayHeldGesture(Hold& hold, uint32_t motionHash)
{
    char const* motion = npcs::MotionNameOf(motionHash);
    if (!motion) return false;
    char stem[96];
    if (!talk_policy::MotionStem(motion, stem, sizeof(stem))) return true;
    if (talk_policy::IsStandUpMotion(motion))
        hold.entity.Routine.AniState = gEAniState_Stand;
    hold.entity.StartPlayAni(bCString(stem), 0, GEFalse, 1.0f, GEFalse);
    hold.gestureUntil = net::Now() + 1200;
    ++gesturesPlayed;
    return true;
}

unsigned char* NativeAssessTalk()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x5c, 0x05, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + AssessTalkRva;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

Hold const* HoldOf(eCEntity const* entity)
{
    if (!entity) return nullptr;
    uint8_t rawGuid[16];
    guid::CopyEntity(const_cast<eCEntity*>(entity), rawGuid);
    const auto found = holds.find(Key(rawGuid));
    return found == holds.end() ? nullptr : &found->second;
}

std::pair<int, int> QueryHandUseTypes(Entity& npc, eCEntity* rawEntity)
{
    if (!rawEntity || !rawEntity->GetPropertySet(eEPropertySetType_Inventory))
        return {talk_policy::UseTypeNone, talk_policy::UseTypeNone};
    const GEInt leftIdx = npc.Inventory.FindStackIndex(gESlot_LeftHand);
    const GEInt rightIdx = npc.Inventory.FindStackIndex(gESlot_RightHand);
    const int leftUse = leftIdx >= 0 ? static_cast<int>(npc.Inventory.GetUseType(leftIdx)) : talk_policy::UseTypeNone;
    const int rightUse = rightIdx >= 0 ? static_cast<int>(npc.Inventory.GetUseType(rightIdx)) : talk_policy::UseTypeNone;
    return {leftUse, rightUse};
}

void NormalizeTalkPartnerAnimationState(Entity& npc, eCEntity* rawEntity, bool inferFromSlot0)
{
    if (!rawEntity || !npc.GetInstance()) return;
    int aniState = static_cast<int>(npc.Routine.AniState);
    if (inferFromSlot0) {
        char const* slot0Motion = nullptr;
        if (auto* animation = static_cast<eCVisualAnimation_PS*>(rawEntity->GetPropertySet(eEPropertySetType_Animation))) {
            auto* actor = animation->GetActor();
            const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
            if (actor && actor->HasMotionInstance(slot0))
                slot0Motion = animation->GetMotionDesc(slot0).GetMotionFilename().GetText();
        }
        aniState = talk_policy::InferDialogPartnerAniState(slot0Motion, aniState);
        npc.Routine.AniState = static_cast<gEAniState>(aniState);
        auto [leftUse, rightUse] = QueryHandUseTypes(npc, rawEntity);
        if (talk_policy::ShouldClearHeldItemsForTalk(aniState, leftUse, rightUse))
            npc.Inventory.HoldStacks(-1, -1);
    } else if (aniState == talk_policy::AniStateDummy0 || aniState == talk_policy::AniStateDummy1 ||
               (aniState >= talk_policy::AniStateTalkStand && aniState <= talk_policy::AniStateTalkSitThrone)) {
        aniState = talk_policy::InferDialogPartnerAniState(nullptr, aniState);
        npc.Routine.AniState = static_cast<gEAniState>(aniState);
    }
    if (auto* npcPs = static_cast<gCNPC_PS*>(rawEntity->GetPropertySet(eEPropertySetType_NPC))) {
        auto [leftUse, rightUse] = QueryHandUseTypes(npc, rawEntity);
        const bool emptyOrTorch = leftUse == talk_policy::UseTypeNone &&
                                  (rightUse == talk_policy::UseTypeNone || rightUse == talk_policy::UseTypeTorch);
        const int desiredPose = talk_policy::NormalizeDialogPrimaryPose(
            static_cast<int>(npcPs->GetPrimaryPose()),
            emptyOrTorch || talk_policy::IsSeatedTalkAniState(aniState));
        if (static_cast<int>(npcPs->GetPrimaryPose()) != desiredPose)
            npcPs->SetPrimaryPose(static_cast<gEPose>(desiredPose));
    }
    if (rawEntity->GetPropertySet(eEPropertySetType_Navigation) && npc.GetCurrentAniDirection() == gEDirection_None)
        npc.SetCurrentAniDirection(gEDirection_Fwd);
}

void StopTalkNavigation(Entity& npc)
{
    npc.StopGoto();
    npc.Routine.StopAIGoto();
    if (talk_policy::ShouldStopHeldLocomotion(static_cast<int>(npc.CharacterMovement.GetMovementMode())))
        npc.CharacterMovement.StopMovement();
}

void ApplyTalkFacing(Entity& npc, eCEntity* speaker)
{
    if (talk_policy::ShouldAlignTalkBodyToSpeaker(static_cast<int>(npc.Routine.AniState)))
        npc.SetAlignmentTarget(Entity(speaker));
    else
        npc.SetAlignmentTarget(Entity());
    npc.SetLookAtTarget(Entity(speaker));
}

bool ClientPartnerCanAnimateTalk(eCEntity* partner);
void PrepareClientTalkPartner(eCEntity* partner, eCEntity* hero);

GEInt GE_STDCALL AssessTalkDetour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalAssessTalk ? originalAssessTalk(spu, self, other, args) : 0;
    eCEntity* npc = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* talker = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    // AssessTalk accepts either (npc, hero) or (hero, npc) order; normalize the addressed NPC symmetrically.
    eCEntity* addressedNpc = (hero && (npc == hero || talker == hero)) ? ((npc == hero) ? talker : npc) : nullptr;
    Hold const* hold = (net::IsHost() && !holdDisabled && addressedNpc && addressedNpc != hero)
                           ? HoldOf(addressedNpc)
                           : nullptr;
    if (!hold) {
        eCEntity* puppet = remote::IsPuppet(npc) ? npc : remote::IsPuppet(talker) ? talker : nullptr;
        const int puppetOwner = puppet ? remote::PuppetOwner(puppet) : 0;
        const bool heldPuppet = puppet && remote::IsHeldPuppet(puppet);
        if (puppetOwner && !heldPuppet) {
            net::Remote const& r = net::Remotes()[puppetOwner];
            Entity puppetEntity(puppet);
            const bool hasRoutine = puppet->GetPropertySet(eEPropertySetType_ScriptRoutine) != nullptr;
            char liveMotion[128] = {};
            body::CurrentMotion(puppet, liveMotion, sizeof(liveMotion));
            if (talk_policy::ShouldRefusePuppetTalk(
                    true, false, r.latest.flags, static_cast<int>(r.latest.aniState), r.latest.animation,
                    hasRoutine ? static_cast<int>(puppetEntity.Routine.AIMode) : -1,
                    hasRoutine && puppetEntity.IsDown(), hasRoutine && puppetEntity.IsDead(),
                    hasRoutine ? static_cast<int>(puppetEntity.Routine.AniState) : -1, liveMotion)) {
                remote::LogPuppetInteraction(puppet, "talk-refused-downed", 0);
                return 0;
            }
        }
        if (heldPuppet) remote::LogPuppetInteraction(puppet, "talk-attempt", -1);
        eCEntity* prevAssessing = clientTalk.assessingNpc;
        const bool clientAddressedNpc = !net::IsHost() && addressedNpc && addressedNpc != hero && !remote::IsPuppet(addressedNpc);
        if (clientAddressedNpc) {
            clientTalk.assessingNpc = addressedNpc;
            PrepareClientTalkPartner(addressedNpc, hero);
        }
        const GEInt result = originalAssessTalk(spu, self, other, args);
        clientTalk.assessingNpc = prevAssessing;
        if (clientAddressedNpc) {
            if (result != 0) {
                guid::CopyEntity(addressedNpc, clientTalk.pendingGuid);
                clientTalk.pendingUntil = net::Now() + talk_policy::PendingClientTalkWindowMs;
                clientTalk.pendingActive = true;
            } else if (!clientTalk.active || !SameGuid(addressedNpc, clientTalk.guid)) {
                if (clientTalk.preparedActive && SameGuid(addressedNpc, clientTalk.preparedGuid) &&
                    npcplayback::IsPassive(addressedNpc)) {
                    Entity refusedNpc(addressedNpc);
                    if (refusedNpc.GetInstance() && !refusedNpc.IsDead()) {
                        refusedNpc.SetAlignmentTarget(Entity());
                        refusedNpc.SetLookAtTarget(Entity());
                    }
                }
                clientTalk.ClearPrepared();
            }
        }
        if (heldPuppet) remote::LogPuppetInteraction(puppet, "talk-assessed", result);
        return result;
    }
    ++busyRefused;
    const DWORD now = GetTickCount();
    if (now - lastBusyMessage > 3000) {
        lastBusyMessage = now;
        wchar_t who[64] = {}, player[64] = {}, text[200] = {};
        MultiByteToWideChar(CP_UTF8, 0, Entity(addressedNpc).GetName().GetText(), -1, who, 63);
        MultiByteToWideChar(CP_UTF8, 0, net::PeerName(hold->player), -1, player, 63);
        _snwprintf_s(text, _TRUNCATE, i18n::Text(i18n::NpcTalking), who, player[0] ? player : L"?");
        hud::Message(hud::Info, text);
        Logf("talk: %s talks to player %u; this player's address refused", Entity(addressedNpc).GetName().GetText(), hold->player);
        telemetry::OnTalkEvent("busy_refused", hold->player, addressedNpc, hold->guid, "host_assess_talk");
    }
    return 0;
}

void SetupTalkStance(Entity& npc, eCEntity* speaker, bool inferFromSlot0)
{
    NormalizeTalkPartnerAnimationState(npc, npc.GetInstance(), inferFromSlot0);
    if (npc.NPC.GetCurrentTarget().GetInstance() != speaker) npc.NPC.SetCurrentTarget(Entity(speaker));
    StopTalkNavigation(npc);
    ApplyTalkFacing(npc, speaker);
}

void ApplyTalkTask(Entity& npc, bool fullStop)
{
    if (fullStop) npc.Routine.FullStop();
    npc.Routine.AIMode = gEAIMode_Talk;
    npc.Routine.SetTask(bCString("ZS_Talk"));
}

bool MaintainStationaryTalk(Entity& npc, eCEntity* speaker, bool fullStopOnRepacify)
{
    const int aiMode = static_cast<int>(npc.Routine.AIMode);
    if (npc.IsDead() || npc.IsDown() || aiMode == talk_policy::AiModeCombat || aiMode == talk_policy::AiModeFlee)
        return false;
    SetupTalkStance(npc, speaker, false);
    if (!talk_policy::ShouldRepacifyHeldTalk(npc.IsDead(), npc.IsDown(), aiMode,
                                             npc.Routine.GetCurrentTask().GetText()))
        return false;
    ApplyTalkTask(npc, fullStopOnRepacify);
    return true;
}

void EnterStationaryTalk(Hold& hold, eCEntity* speaker)
{
    SetupTalkStance(hold.entity, speaker, true);
    ApplyTalkTask(hold.entity, true);
}

eCEntity* CurrentClientPartner(eCEntity* hero)
{
    gCInfoManager_PS* manager = Infos();
    if (!manager || !manager->AccessIsRunning()) return nullptr;
    gCEntity* npc = manager->GetDialogPartnerNPC();
    if (!npc || !talk_policy::ShouldTrackClientDialogPartner(
                    true, npc->GetPropertySet(eEPropertySetType_NPC) != nullptr, npc == hero, remote::IsPuppet(npc)))
        return nullptr;
    return npc;
}

bool ClientPartnerCanAnimateTalk(eCEntity* partner)
{
    if (!partner || !npcplayback::IsPassive(partner)) return false;
    Entity npc(partner);
    if (!npc.GetInstance() || npc.IsDead() || npc.IsDown()) return false;
    net::NpcRecord const* combat = npcs::CombatRecord(partner);
    return talk_policy::ShouldYieldMirrorToLocalTalk(true, npcs::DiedOnHost(partner), combat ? combat->flags : 0u);
}

void UnpausePartnerBaseMotion(eCEntity* partner, bool stopUpperSlots)
{
    if (auto* animation = static_cast<eCVisualAnimation_PS*>(partner->GetPropertySet(eEPropertySetType_Animation))) {
        if (auto* actor = animation->GetActor()) {
            const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
            if (stopUpperSlots) actor->SetPlaySpeed(slot0, 1.0f);
            actor->PauseMotion(GEFalse, slot0);
            if (stopUpperSlots) {
                for (int i = 1; i < net::NpcMotionSlots; ++i) {
                    const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(i);
                    if (actor->HasMotionInstance(slot)) animation->StopMotion(slot, 0.15f);
                }
            }
        }
    }
}

void PrepareClientTalkPartner(eCEntity* partner, eCEntity* hero)
{
    if (!hero || !ClientPartnerCanAnimateTalk(partner)) return;
    UnpausePartnerBaseMotion(partner, true);
    Entity npc(partner);
    NormalizeTalkPartnerAnimationState(npc, partner, true);
    StopTalkNavigation(npc);
    ApplyTalkFacing(npc, hero);
    guid::CopyEntity(partner, clientTalk.preparedGuid);
    clientTalk.preparedActive = true;
}

void SyncClientPartner(eCEntity* partner, eCEntity* hero, bool stopUpperSlots)
{
    if (!hero || !ClientPartnerCanAnimateTalk(partner)) return;
    UnpausePartnerBaseMotion(partner, stopUpperSlots);
    Entity npc(partner);
    MaintainStationaryTalk(npc, hero, true);
}

bool PendingClientTalkMatches(eCEntity const* entity)
{
    if (!clientTalk.pendingActive || !entity || !SameGuid(entity, clientTalk.pendingGuid)) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bool heroAlive = hero && !Entity(hero).IsDead();
    gCInfoManager_PS* manager = Infos();
    const bool dialogRunning = manager && manager->AccessIsRunning();
    return talk_policy::ShouldKeepPendingClientTalk(
        clientTalk.pendingActive, dialogRunning, heroAlive, net::Now(), clientTalk.pendingUntil);
}

void ReleaseClientPartner(eCEntity* partner, uint8_t const guid[16])
{
    npcs::OnClientTalkEnded(guid);
    if (!partner || !npcplayback::IsPassive(partner)) return;
    Entity npc(partner);
    if (npc.GetInstance() && !npc.IsDead()) {
        npc.SetAlignmentTarget(Entity());
        npc.SetLookAtTarget(Entity());
    }
    npcplayback::Silence(partner);
}

void OnTalk(uint8_t from, bool begin, uint8_t const guid[16])
{
    if (!net::IsHost() || holdDisabled) return;
    const std::string key = Key(guid);
    auto it = holds.find(key);
    if (begin) {
        if (it != holds.end()) {
            if (it->second.player == from && it->second.generation == net::PeerGeneration(from) &&
                it->second.incarnation == npcs::Incarnation(guid)) return;
            if (net::PeerJoined(it->second.player) &&
                it->second.generation == net::PeerGeneration(it->second.player) &&
                it->second.incarnation == npcs::Incarnation(guid)) {
                Logf("talk: player %u cannot take player %u's active NPC dialog", from, it->second.player);
                telemetry::OnTalkEvent("hold_conflict", from, npcs::NpcByGuid(guid), guid, "already_held");
                return;
            }
            EraseAndReleaseHold(it);
        }
        Entity npc(npcs::NpcByGuid(guid));  // an NPC only, not the host's hero (Fable 1.7)
        if (!npc.GetInstance() || npc.IsDead()) return;
        // One dialog per player: a new hold ends that player's earlier one (one message per GUID held a whole town).
        for (auto other = holds.begin(); other != holds.end(); ++other)
            if (other->second.player == from) {
                EraseAndReleaseHold(other);
                Logf("talk: player %u ended its previous NPC dialog", from);
                break;
            }
        eCEntity* speaker = remote::PuppetOf(from);
        if (!speaker) { Log("talk: host cannot start NPC conversation without the player's body"); return; }
        Hold& hold = holds.emplace(key, Hold{npc, 0, from}).first->second;
        std::memcpy(hold.guid, guid, sizeof(hold.guid));
        hold.generation = net::PeerGeneration(from);
        hold.incarnation = npcs::Incarnation(guid);
        EnterStationaryTalk(hold, speaker);
        ++holdsStarted;
        Logf("talk: holding %s for player %u", npc.GetName().GetText(), from);
        telemetry::OnTalkEvent("host_hold_start", from, npc.GetInstance(), guid, "remote_dialog");
    } else if (it != holds.end() && it->second.player == from &&
               it->second.generation == net::PeerGeneration(from)) {
        EraseAndReleaseHold(it);
        Logf("talk: player %u ended its NPC dialog", from);
        telemetry::OnTalkEvent("host_hold_end", from, npcs::NpcByGuid(guid), guid, "remote_dialog");
    }
}
}

void Start() { net::SetTalkHandler(&OnTalk); }

void OnPartnerMotion(eCEntity* partner, char const* motion)
{
    if (net::IsHost() || !net::Connected() ||
        !talk_policy::ShouldForwardGesture(gestures, TalkingWithHero(partner), motion)) return;
    const uint32_t welcome = net::WelcomeSerial();
    if (gestureNamesWelcome != welcome) {
        gestureNamesSent.clear();
        gestureNamesWelcome = welcome;
    }
    const uint32_t hash = net::HashName(motion);
    if (!gestureNamesSent[hash]) {
        net::SendMotionName(hash, motion);
        gestureNamesSent[hash] = true;
    }
    net::WorldState s{};
    s.kind = GestureKind;
    guid::CopyEntity(partner, s.guid);
    Format(s.routine, "%08x", hash);
    const bCQuaternion q = partner->GetWorldRotation();
    s.rotation[0] = q.GetX(); s.rotation[1] = q.GetY(); s.rotation[2] = q.GetZ(); s.rotation[3] = q.GetW();
    net::SendWorldState(s, 0);
    ++gesturesSent;
}

void OnGesture(uint8_t from, uint8_t const guid[16], uint32_t motion, float const[4])
{
    if (!net::IsHost() || holdDisabled || !motion) return;
    auto it = holds.find(Key(guid));
    if (it == holds.end()) return;
    Hold& hold = it->second;
    hold.entity = Entity(npcs::NpcByGuid(hold.guid));
    const bool valid = hold.entity.GetInstance() && hold.generation == net::PeerGeneration(from) &&
                       hold.incarnation == npcs::Incarnation(hold.guid);
    if (!valid || !talk_policy::ShouldAcceptGestureOnHost(
                      true, true, hold.player, from, hold.entity.IsDead(), hold.entity.IsDown(),
                      static_cast<int>(hold.entity.Routine.AIMode)))
        return;
    if (!PlayHeldGesture(hold, motion)) {
        hold.pendingGesture = motion;
        ++gesturesUnknown;
    } else {
        hold.pendingGesture = 0;
    }
}

void EnsureAssessTalkHook()
{
    if (assessInstalled || !gCSession::GetInstance().IsGameRunning() || GetTickCount() - lastAssessTry < 1000) return;
    lastAssessTry = GetTickCount();
    gSScript* script = nullptr;
    if (GetScriptAdminExt().GetScripts().GetAt(bCString("AssessTalk"), script) && script && script->m_funcScript) {
        if (unsigned char* code = NativeAssessTalk(); code && reinterpret_cast<void*>(script->m_funcScript) == code) {
            originalAssessTalk = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&AssessTalkDetour)));
            assessNative = originalAssessTalk != nullptr;
        }
        assessInstalled = true;
        Logf("talk: AssessTalk %s", assessNative ? "wrapped (an NPC in another player's dialog is busy)" : "not wrapped (another build)");
    }
}

void TickHostHolds()
{
    net::Remote const* remotes = net::Remotes();
    for (auto it = holds.begin(); it != holds.end();) {
        Hold& hold = it->second;
        hold.entity = Entity(npcs::NpcByGuid(hold.guid));
        eCEntity* speaker = remote::PuppetOf(hold.player);
        if (!hold.entity.GetInstance() || hold.entity.IsDead() || !speaker || !remotes[hold.player].connected ||
            hold.generation != net::PeerGeneration(hold.player) || hold.incarnation != npcs::Incarnation(hold.guid)) {
            it = EraseAndReleaseHold(it);
            continue;
        }
        if (MaintainStationaryTalk(hold.entity, speaker, true)) ++repacified;
        if (!hold.entity.IsDown() && static_cast<int>(hold.entity.Routine.AIMode) != talk_policy::AiModeCombat &&
            hold.pendingGesture && PlayHeldGesture(hold, hold.pendingGesture))
            hold.pendingGesture = 0;
        ++it;
    }
}

void TickClientTalk()
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* partner = CurrentClientPartner(hero);
    const bool connected = net::Connected();
    if (!connected) {
        clientTalk.holdSent = false;
        gestureNamesSent.clear();
        gestureNamesWelcome = 0;
    }
    if (clientTalk.active && partner && SameGuid(partner, clientTalk.guid)) {
        clientTalk.ClearPending();
        SyncClientPartner(partner, hero, false);
        const bool talkHoldSent = clientTalk.holdSent;
        const uint32_t talkingWelcome = clientTalk.welcome;
        if (talk_policy::ShouldSendClientTalkHold(connected, talkHoldSent, talkingWelcome, net::WelcomeSerial())) {
            net::SendTalk(true, clientTalk.guid);
            clientTalk.welcome = net::WelcomeSerial();
            clientTalk.holdSent = true;
        }
        return;
    }
    if (clientTalk.active) {
        uint8_t previousGuid[16] = {};
        std::memcpy(previousGuid, clientTalk.guid, sizeof(previousGuid));
        const bool wasHoldSent = clientTalk.holdSent;
        clientTalk.active = false;
        clientTalk.holdSent = false;
        clientTalk.welcome = 0;
        clientTalk.ClearPrepared();
        std::memset(clientTalk.guid, 0, sizeof(clientTalk.guid));
        ReleaseClientPartner(npcs::NpcByGuid(previousGuid), previousGuid);
        if (talk_policy::ShouldReleaseClientTalkHold(connected, wasHoldSent)) net::SendTalk(false, previousGuid);
        story::FlushQuests();
        ++ends;
        telemetry::OnTalkEvent("client_talk_end", net::LocalId(), npcs::NpcByGuid(previousGuid), previousGuid, "client_dialog");
    }
    if (partner) {
        clientTalk.ClearPending();
        guid::CopyEntity(partner, clientTalk.guid);
        clientTalk.active = true;
        if (!clientTalk.preparedActive || !SameGuid(partner, clientTalk.preparedGuid))
            PrepareClientTalkPartner(partner, hero);
        SyncClientPartner(partner, hero, true);
        if (talk_policy::ShouldSendClientTalkHold(connected, false, clientTalk.welcome, net::WelcomeSerial())) {
            net::SendTalk(true, clientTalk.guid);
            clientTalk.welcome = net::WelcomeSerial();
            clientTalk.holdSent = true;
        } else {
            clientTalk.holdSent = false;
        }
        ++begins;
        telemetry::OnTalkEvent("client_talk_start", net::LocalId(), partner, clientTalk.guid, "client_dialog");
        return;
    }
    if (clientTalk.pendingActive) {
        const bool heroAlive = hero && !Entity(hero).IsDead();
        if (talk_policy::ShouldKeepPendingClientTalk(true, false, heroAlive, net::Now(), clientTalk.pendingUntil)) {
            if (eCEntity* pendingNpc = npcs::NpcByGuid(clientTalk.pendingGuid))
                SyncClientPartner(pendingNpc, hero, false);
        } else {
            uint8_t expiredGuid[16] = {};
            std::memcpy(expiredGuid, clientTalk.pendingGuid, sizeof(expiredGuid));
            clientTalk.ClearPending();
            clientTalk.ClearPrepared();
            ReleaseClientPartner(npcs::NpcByGuid(expiredGuid), expiredGuid);
        }
    }
}

void Tick()
{
    EnsureAssessTalkHook();
    if (net::IsHost()) TickHostHolds();
    else TickClientTalk();
}

bool AllowPassiveTalkTask(eCEntity const* entity, char const* task)
{
    if (!entity || net::IsHost()) return false;
    return talk_policy::ShouldAllowPassiveTalkTask(TalkingWithHero(entity), entity == clientTalk.assessingNpc, task);
}

bool BlockHeldTalkEvent(eCEntity const* entity, char const* eventName)
{
    if (!entity || !net::IsHost() || holdDisabled || !IsHeld(entity)) return false;
    Entity npc(const_cast<eCEntity*>(entity));
    if (!npc.GetInstance()) return false;
    return talk_policy::ShouldBlockHeldTalkEvent(
        true, true, npc.IsDead(), npc.IsDown(), static_cast<int>(npc.Routine.AIMode), eventName);
}

bool BlockHeldTalkTask(eCEntity const* entity, char const* task)
{
    if (!entity || !net::IsHost() || holdDisabled || !IsHeld(entity)) return false;
    Entity npc(const_cast<eCEntity*>(entity));
    if (!npc.GetInstance()) return false;
    return talk_policy::ShouldBlockHeldTalkTask(
        true, true, npc.IsDead(), npc.IsDown(), static_cast<int>(npc.Routine.AIMode), task);
}

bool TalkingWithHero(eCEntity const* entity)
{
    if (!entity || net::IsHost()) return false;
    if (entity == clientTalk.assessingNpc) return true;
    if (clientTalk.active && SameGuid(entity, clientTalk.guid)) return true;
    if (PendingClientTalkMatches(entity)) return true;
    return CurrentClientPartner(gCSession::GetInstance().GetHero()) == entity;
}

bool HoldingForPlayer(uint8_t player)
{
    if (!net::IsHost() || holdDisabled || !player) return false;
    for (auto const& entry : holds)
        if (entry.second.player == player) return true;
    return false;
}

bool InGesture(eCEntity const* entity)
{
    if (!net::IsHost() || holdDisabled) return false;
    Hold const* hold = HoldOf(entity);
    return hold && static_cast<int32_t>(hold->gestureUntil - net::Now()) > 0;
}

void OnWorldChange()
{
    holds.clear();
    clientTalk.Reset();
    gestureNamesSent.clear();
    gestureNamesWelcome = 0;
}

bool IsHeld(eCEntity const* entity)
{
    return HoldOf(entity) != nullptr;
}

void Choose(int index, LogFn log)
{
    char line[200];
    gCInfoManager_PS* manager = Infos();
    gCInfo_PS* info = manager && manager->AccessIsRunning() ? manager->GetAvailableInfo(index) : nullptr;
    if (!info) { log("REFUSED talkchoose: no dialog or no such option"); return; }
    const GEBool ok = manager->ExecuteDialogInfo(info);
    Format(line, "TALKCHOOSE index=%d name=%s ok=%d", index, info->AccessName().GetText(), ok ? 1 : 0);
    log(line);
}

// QA, stage H3: run a named dialog option in the running dialog (its conditions not checked), as if chosen.
void ChooseInfo(char const* name, LogFn log)
{
    char line[200];
    gCInfoManager_PS* manager = Infos();
    gCInfo_PS* info = manager && manager->AccessIsRunning() ? manager->GetInfo(bCString(name)) : nullptr;
    if (!info) { Format(line, "REFUSED talkinfo %s: no dialog or no such info", name); log(line); return; }
    const GEBool ok = manager->ExecuteDialogInfo(info);
    Format(line, "TALKINFO name=%s ok=%d", name, ok ? 1 : 0);
    log(line);
}

void Report(LogFn log)
{
    char line[260];
    std::string held;
    for (auto const& entry : holds) {
        Entity live(npcs::NpcByGuid(entry.second.guid));
        held += std::string(held.empty() ? "" : ",") + (live.GetInstance() ? live.GetName().GetText() : "(unloaded)");
    }
    Entity partner(clientTalk.active ? npcs::NpcByGuid(clientTalk.guid) : nullptr);
    Format(line, "TALKSYNC talking=%d partner=%s begins=%u ends=%u held=%zu held_names=%s holds_started=%u holds_ended=%u repacified=%u hold_disabled=%d busy_refused=%u assess_wrapped=%d gestures_sent=%u gestures_played=%u gestures_unknown=%u",
              clientTalk.active ? 1 : 0, partner.GetInstance() ? partner.GetName().GetText() : "-", begins, ends, holds.size(),
              held.empty() ? "-" : held.c_str(), holdsStarted, holdsEnded, repacified, holdDisabled, busyRefused, assessNative ? 1 : 0, gesturesSent,
              gesturesPlayed, gesturesUnknown);
    log(line);
}

void DisableHold(LogFn log)
{
    holdDisabled = true;
    log("TALKHOLDOFF ok=1");
}

void GesturesOff(LogFn log)
{
    gestures = false;
    log("GESTURESOFF ok=1");
}
}
