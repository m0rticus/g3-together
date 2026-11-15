// SPDX-License-Identifier: GPL-3.0-or-later
#include "talk.h"
#include "talk_policy.h"

#include "ai.h"
#include "body.h"
#include "guid.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcs.h"
#include "remote.h"

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
// Client: the NPC our hero is talking to, tracked by GUID across frames rather than a raw entity pointer.
bool talkingActive = false;
bool talkHoldSent = false;
uint32_t talkingWelcome = 0;
eCEntity* assessingTalkNpc = nullptr;
uint8_t talkingGuid[16] = {};
unsigned begins = 0, ends = 0;

// Host: NPCs held for a remote player's dialog.
struct Hold {
    Entity entity;
    body::State state;
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
            hold.entity.SetAlignmentTarget(Entity());
            hold.entity.SetLookAtTarget(Entity());
            hold.entity.Routine.FullStop();
            hold.entity.Routine.ContinueRoutine();
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
    hold.entity.StartPlayAni(bCString(stem), 0, GEFalse, 1.0f, GEFalse);
    hold.state.actionUntil = net::Now() + 1200;
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
        if (remote::IsHeldPuppet(puppet)) remote::LogPuppetInteraction(puppet, "talk-attempt", -1);
        eCEntity* prevAssessing = assessingTalkNpc;
        if (!net::IsHost() && addressedNpc) assessingTalkNpc = addressedNpc;
        const GEInt result = originalAssessTalk(spu, self, other, args);
        assessingTalkNpc = prevAssessing;
        if (remote::IsHeldPuppet(puppet)) remote::LogPuppetInteraction(puppet, "talk-assessed", result);
        return result;
    }
    ++busyRefused;
    const DWORD now = GetTickCount();
    if (now - lastBusyMessage > 3000) {
        lastBusyMessage = now;
        wchar_t who[64] = {}, player[64] = {}, text[200];
        MultiByteToWideChar(CP_UTF8, 0, Entity(addressedNpc).GetName().GetText(), -1, who, 64);
        MultiByteToWideChar(CP_UTF8, 0, net::PeerName(hold->player), -1, player, 64);
        swprintf_s(text, i18n::Text(i18n::NpcTalking), who, player[0] ? player : L"?");
        hud::Message(hud::Info, text);
        Logf("talk: %s talks to player %u; this player's address refused", Entity(addressedNpc).GetName().GetText(), hold->player);
    }
    return 0;
}

void EnterStationaryTalk(Hold& hold, eCEntity* speaker)
{
    hold.entity.NPC.SetCurrentTarget(Entity(speaker));
    hold.entity.CharacterMovement.StopMovement();
    hold.entity.SetAlignmentTarget(Entity(speaker));
    hold.entity.SetLookAtTarget(Entity(speaker));
    hold.entity.Routine.FullStop();
    hold.entity.Routine.AIMode = gEAIMode_Talk;
    hold.entity.Routine.SetTask(bCString("ZS_Talk"));
}

bool ClientPartnerCanAnimateTalk(eCEntity* partner)
{
    if (!partner || !npcplayback::IsPassive(partner)) return false;
    Entity npc(partner);
    if (!npc.GetInstance() || npc.IsDead() || npc.IsDown()) return false;
    net::NpcRecord const* combat = npcs::CombatRecord(partner);
    return talk_policy::ShouldYieldMirrorToLocalTalk(true, npcs::DiedOnHost(partner), combat ? combat->flags : 0u);
}

void SyncStationaryClientPartner(Entity& npc, eCEntity* hero)
{
    if (npc.NPC.GetCurrentTarget().GetInstance() != hero) npc.NPC.SetCurrentTarget(Entity(hero));
    if (talk_policy::ShouldStopHeldLocomotion(static_cast<int>(npc.CharacterMovement.GetMovementMode())))
        npc.CharacterMovement.StopMovement();
    npc.SetAlignmentTarget(Entity(hero));
    npc.SetLookAtTarget(Entity(hero));
    if (talk_policy::ShouldRepacifyHeldTalk(npc.IsDead(), npc.IsDown(), static_cast<int>(npc.Routine.AIMode),
                                            npc.Routine.GetCurrentTask().GetText())) {
        npc.Routine.AIMode = gEAIMode_Talk;
        npc.Routine.SetTask(bCString("ZS_Talk"));
    }
}

void UnpausePartnerSlot0(eCEntity* partner, eCEntity* hero, bool stopUpperSlots)
{
    if (!hero || !ClientPartnerCanAnimateTalk(partner)) return;
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
    Entity npc(partner);
    SyncStationaryClientPartner(npc, hero);
}

void PrepareClientPartner(eCEntity* partner, eCEntity* hero)
{
    UnpausePartnerSlot0(partner, hero, true);
}

void MaintainClientPartner(eCEntity* partner, eCEntity* hero)
{
    UnpausePartnerSlot0(partner, hero, false);
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
        Hold& hold = holds.emplace(key, Hold{npc, {}, from}).first->second;
        std::memcpy(hold.guid, guid, sizeof(hold.guid));
        hold.generation = net::PeerGeneration(from);
        hold.incarnation = npcs::Incarnation(guid);
        EnterStationaryTalk(hold, speaker);
        ++holdsStarted;
        Logf("talk: holding %s for player %u", npc.GetName().GetText(), from);
    } else if (it != holds.end() && it->second.player == from &&
               it->second.generation == net::PeerGeneration(from)) {
        Hold ended = std::move(it->second);
        holds.erase(it);
        Release(ended);
        Logf("talk: player %u ended its NPC dialog", from);
    }
}
}

void Start() { net::SetTalkHandler(&OnTalk); }

void OnPartnerMotion(eCEntity* partner, char const* motion)
{
    if (net::IsHost() || !net::Connected() ||
        !talk_policy::ShouldForwardGesture(gestures, TalkingWithHero(partner), motion)) return;
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

void Tick()
{
    if (!assessInstalled && gCSession::GetInstance().IsGameRunning() && GetTickCount() - lastAssessTry >= 1000) {
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
    if (net::IsHost()) {
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
            const int aiMode = static_cast<int>(hold.entity.Routine.AIMode);
            if (!hold.entity.IsDown() && aiMode != talk_policy::AiModeCombat) {
                if (hold.entity.NPC.GetCurrentTarget().GetInstance() != speaker)
                    hold.entity.NPC.SetCurrentTarget(Entity(speaker));
                if (talk_policy::ShouldStopHeldLocomotion(static_cast<int>(hold.entity.CharacterMovement.GetMovementMode())))
                    hold.entity.CharacterMovement.StopMovement();
                hold.entity.SetAlignmentTarget(Entity(speaker));
                hold.entity.SetLookAtTarget(Entity(speaker));
                if (talk_policy::ShouldRepacifyHeldTalk(hold.entity.IsDead(), hold.entity.IsDown(), aiMode,
                                                        hold.entity.Routine.GetCurrentTask().GetText())) {
                    EnterStationaryTalk(hold, speaker);
                    ++repacified;
                }
                if (hold.pendingGesture && PlayHeldGesture(hold, hold.pendingGesture))
                    hold.pendingGesture = 0;
            }
            ++it;
        }
        return;
    }
    gCInfoManager_PS* manager = Infos();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* partner = nullptr;
    if (manager && manager->AccessIsRunning()) {
        gCEntity* npc = manager->GetDialogPartnerNPC();
        if (npc && talk_policy::ShouldTrackClientDialogPartner(
                       true, npc->GetPropertySet(eEPropertySetType_NPC) != nullptr, npc == hero, remote::IsPuppet(npc)))
            partner = npc;
    }
    if (talkingActive && partner && SameGuid(partner, talkingGuid)) {
        MaintainClientPartner(partner, hero);
        if (net::Joined() && (!talkHoldSent || talkingWelcome != net::WelcomeSerial())) {
            net::SendTalk(true, talkingGuid);
            talkingWelcome = net::WelcomeSerial();
            talkHoldSent = true;
        }
        return;
    }
    if (talkingActive) {
        uint8_t previousGuid[16] = {};
        std::memcpy(previousGuid, talkingGuid, sizeof(previousGuid));
        ReleaseClientPartner(npcs::NpcByGuid(previousGuid), previousGuid);
        if (talkHoldSent && net::Joined()) net::SendTalk(false, previousGuid);
        talkingActive = false;
        talkHoldSent = false;
        talkingWelcome = 0;
        std::memset(talkingGuid, 0, sizeof(talkingGuid));
        ++ends;
    }
    if (partner) {
        guid::CopyEntity(partner, talkingGuid);
        talkingActive = true;
        PrepareClientPartner(partner, hero);
        if (net::Joined()) {
            net::SendTalk(true, talkingGuid);
            talkingWelcome = net::WelcomeSerial();
            talkHoldSent = true;
        } else {
            talkHoldSent = false;
        }
        ++begins;
    }
}

bool AllowPassiveTalkTask(eCEntity const* entity, char const* task)
{
    if (!entity || net::IsHost()) return false;
    return talk_policy::ShouldAllowPassiveTalkTask(TalkingWithHero(entity), entity == assessingTalkNpc, task);
}

bool TalkingWithHero(eCEntity const* entity)
{
    if (!entity || net::IsHost()) return false;
    if (talkingActive && SameGuid(entity, talkingGuid)) return true;
    gCInfoManager_PS* manager = Infos();
    if (!manager || !manager->AccessIsRunning()) return false;
    gCEntity* npc = manager->GetDialogPartnerNPC();
    if (npc != entity) return false;
    return talk_policy::ShouldTrackClientDialogPartner(
        true, npc->GetPropertySet(eEPropertySetType_NPC) != nullptr,
        npc == gCSession::GetInstance().GetHero(), remote::IsPuppet(npc));
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
    for (auto const& entry : holds)
        if (entity && entry.second.entity.GetInstance() == entity)
            return static_cast<int32_t>(entry.second.state.actionUntil - net::Now()) > 0;
    return false;
}

void OnWorldChange()
{
    holds.clear();
    talkingActive = false;
    talkHoldSent = false;
    talkingWelcome = 0;
    std::memset(talkingGuid, 0, sizeof(talkingGuid));
    assessingTalkNpc = nullptr;
    gestureNamesSent.clear();
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
    Entity partner(talkingActive ? npcs::NpcByGuid(talkingGuid) : nullptr);
    Format(line, "TALKSYNC talking=%d partner=%s begins=%u ends=%u held=%zu held_names=%s holds_started=%u holds_ended=%u repacified=%u hold_disabled=%d busy_refused=%u assess_wrapped=%d gestures_sent=%u gestures_played=%u gestures_unknown=%u",
              talkingActive ? 1 : 0, partner.GetInstance() ? partner.GetName().GetText() : "-", begins, ends, holds.size(),
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
