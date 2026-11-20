// SPDX-License-Identifier: GPL-3.0-or-later
#include "arena.h"
#include "arena_policy.h"

#include "ai.h"
#include "downed.h"
#include "guid.h"
#include "guid_key.h"
#include "guards.h"
#include "infocmd.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"
#include "story.h"
#include "talk.h"
#include "telemetry.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace g3t::arena {
namespace {
static_assert(arena_policy::QuestTypeArena == gEQuestType_Arena && arena_policy::QuestTypeSparring == gEQuestType_Sparring);
static_assert(arena_policy::QuestStatusOpen == gEQuestStatus_Open && arena_policy::QuestStatusRunning == gEQuestStatus_Running &&
              arena_policy::QuestStatusSuccess == gEQuestStatus_Success && arena_policy::QuestStatusFailed == gEQuestStatus_Failed &&
              arena_policy::QuestStatusObsolete == gEQuestStatus_Obsolete && arena_policy::QuestStatusCancelled == gEQuestStatus_Cancelled &&
              arena_policy::QuestStatusLost == gEQuestStatus_Lost && arena_policy::QuestStatusWon == gEQuestStatus_Won);
static_assert(arena_policy::ArenaStatusNone == gEArenaStatus_None && arena_policy::ArenaStatusRunning == gEArenaStatus_Running);
static_assert(arena_policy::AttackReasonArena == gEAttackReason_Arena && arena_policy::AttackReasonDuel == gEAttackReason_Duel);

constexpr uintptr_t SendToArenaRva = 0xa7f80;
gFScript originalSendToArena = nullptr;
gFScript originalOnLeaveArea = nullptr;
gFScript originalOnEnterArea = nullptr;
gFScriptAICallback originalOnArena = nullptr;
bool installed = false, native = false;
DWORD lastTry = 0;
bool on = true;  // QA negative control: arenasyncoff

struct Held {
    std::string name;
    std::string arena;
    std::string quest;
    uint32_t awaitUntil = 0;
    uint32_t generation = 0;
    uint8_t player = 0;
    bool started = false;
    bool duel = false;
    bool awaitingQuestSync = false;
};

struct BoutQuestState {
    uint8_t challenger = 0;       // active challenger player id (1=host, >=2=client)
    uint32_t generation = 0;      // PeerGeneration of challenger (when >= 2)
    int observedStatus = -1;      // last quest status observed locally (-1 = unobserved)
    bool syncedFromHost = false;  // client: Running arrived via host story sync/baseline
    bool awaitingNpcLoad = false; // host: delivery NPCs not yet loaded in sector
};

struct CachedArena {
    std::string name;
    eCEntityProxy proxy;
    uint32_t nextLookupAt = 0;
    int runningCount = 0;
};

struct CachedBoutQuest {
    gCQuest_PS* quest = nullptr;
    std::string name;
    int arenaIndex = -1;
};

std::unordered_map<GuidKey, Held, GuidHash> held;           // NPC GUID -> active host/client bout or duel
std::unordered_map<std::string, BoutQuestState> boutQuests; // questName -> unified bout lifecycle state
std::vector<CachedBoutQuest> cachedBoutQuests;
std::vector<CachedArena> cachedArenas;
gCQuestManager_PS* cachedManager = nullptr;
GEInt cachedQuestCount = -1;
void const* cachedFirstQuest = nullptr;

unsigned sent = 0, taken = 0, started = 0, given = 0, readopted = 0, leaveSuppressed = 0, statusRefused = 0;

unsigned char* NativeSendToArena()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x01, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + SendToArenaRva;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

GuidKey EntityKey(eCEntity const* entity)
{
    GuidKey key{};
    if (entity) guid::CopyEntity(const_cast<eCEntity*>(entity), key.data());
    return key;
}

void FlushArenaAndQuests() { SyncArenaStatuses(); story::FlushQuests(); }
gCQuestManager_PS* QuestManager() { return gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr; }

void ResetBoutCache()
{
    cachedBoutQuests.clear();
    cachedArenas.clear();
    cachedManager = nullptr;
    cachedQuestCount = -1;
    cachedFirstQuest = nullptr;
}

std::vector<CachedBoutQuest> const& BoutQuests()
{
    gCQuestManager_PS* manager = QuestManager();
    if (!manager) { ResetBoutCache(); return cachedBoutQuests; }
    auto& quests = manager->AccessQuests();
    const GEInt count = quests.GetCount();
    void const* first = count > 0 ? static_cast<gCQuest_PS*>(quests.AccessAt(0)) : nullptr;
    if (manager == cachedManager && count == cachedQuestCount && first == cachedFirstQuest && !cachedBoutQuests.empty())
        return cachedBoutQuests;
    ResetBoutCache();
    cachedManager = manager;
    cachedQuestCount = count;
    cachedFirstQuest = first;
    for (GEInt qi = 0; qi < count; ++qi) {
        auto* quest = static_cast<gCQuest_PS*>(quests.AccessAt(qi));
        if (!quest) continue;
        const int type = static_cast<int>(quest->GetType().GetValue());
        if (!arena_policy::IsArenaOrSparringQuestType(type)) continue;
        int arenaIndex = -1;
        if (type == gEQuestType_Arena) {
            char const* dest = quest->GetDestinationEntity().GetText();
            if (dest && *dest) {
                for (size_t ai = 0; ai < cachedArenas.size(); ++ai)
                    if (cachedArenas[ai].name == dest) { arenaIndex = static_cast<int>(ai); break; }
                if (arenaIndex < 0) {
                    arenaIndex = static_cast<int>(cachedArenas.size());
                    cachedArenas.push_back({dest, {}, 0, 0});
                }
            }
        }
        cachedBoutQuests.push_back({quest, quest->GetName().GetText(), arenaIndex});
    }
    return cachedBoutQuests;
}

bool QuestNamesNpc(gCQuest_PS const* quest, bCString const& npcName)
{
    if (!quest || npcName.GetLength() == 0) return false;
    auto const& who = quest->GetDeliveryEntities();
    for (GEInt i = 0; i < who.GetCount(); ++i)
        if (who.GetAt(i) == npcName) return true;
    return false;
}

gCQuest_PS* FindRunningBoutQuestForNpc(eCEntity const* npc)
{
    if (!npc) return nullptr;
    const bCString name = Entity(const_cast<eCEntity*>(npc)).GetName();
    for (CachedBoutQuest const& entry : BoutQuests()) {
        gCQuest_PS* quest = entry.quest;
        if (quest && quest->GetStatus().GetValue() == gEQuestStatus_Running && QuestNamesNpc(quest, name))
            return quest;
    }
    return nullptr;
}

gCQuest_PS* FindCandidateBoutQuestForNpc(eCEntity const* npc, char const* arenaName)
{
    if (gCQuest_PS* running = FindRunningBoutQuestForNpc(npc)) return running;
    if (!npc) return nullptr;
    const bCString name = Entity(const_cast<eCEntity*>(npc)).GetName();
    for (CachedBoutQuest const& entry : BoutQuests()) {
        gCQuest_PS* quest = entry.quest;
        if (!quest || !QuestNamesNpc(quest, name)) continue;
        const int st = static_cast<int>(quest->GetStatus().GetValue());
        if (!arena_policy::IsCandidateBoutQuestStatus(st)) continue;
        if (arenaName && *arenaName && quest->GetDestinationEntity() != bCString(arenaName)) continue;
        return quest;
    }
    return nullptr;
}

bool Fights(eCEntity const* npc) { return FindRunningBoutQuestForNpc(npc) != nullptr; }
bool IsActiveChallenger(uint8_t owner, uint32_t generation)
{
    return owner == 1 || (net::InWorld(owner) && generation == net::PeerGeneration(owner));
}
void ClearAwaitSync(Held& h) { h.awaitingQuestSync = false; h.awaitUntil = 0; }

uint8_t ChallengerOfQuest(char const* questName)
{
    if (!questName || !*questName) return 0;
    auto it = boutQuests.find(questName);
    return it != boutQuests.end() ? it->second.challenger : 0;
}

uint8_t ConflictingOwner(eCEntity const* npc, GuidKey const& key, uint8_t player, char const*& kind)
{
    if (auto it = held.find(key); it != held.end() && it->second.player != 0 && it->second.player != player &&
        IsActiveChallenger(it->second.player, it->second.generation)) {
        kind = "held by active player";
        return it->second.player;
    }
    if (gCQuest_PS* running = FindRunningBoutQuestForNpc(npc)) {
        char const* qName = running->GetName().GetText();
        const uint8_t owner = ChallengerOfQuest(qName);
        if (owner != 0 && owner != player && IsActiveChallenger(owner, boutQuests[qName].generation)) {
            kind = "running quest owned by player";
            return owner;
        }
    }
    return 0;
}

void ReleaseHostNpc(eCEntity* npc, GuidKey const& key)
{
    if (npc) {
        npcs::SetContext(npc, 0);
        if (npcs::HostTaken(npc)) npcs::HostRelease(npc);
    } else {
        npcs::ClearHostBinding(key.data());
    }
}

uint8_t ActiveChallengerForArea(char const* areaName, bool& hasRunningBout)
{
    hasRunningBout = false;
    if (!areaName || !*areaName) return 0;
    const bCString target(areaName);
    uint8_t challenger = 0;
    for (CachedBoutQuest const& entry : BoutQuests()) {
        gCQuest_PS* quest = entry.quest;
        if (!quest || quest->GetStatus().GetValue() != gEQuestStatus_Running) continue;
        if (quest->GetDestinationEntity() != target) continue;
        hasRunningBout = true;
        if (const uint8_t qPlayer = ChallengerOfQuest(entry.name.c_str()))
            challenger = qPlayer;
    }
    if (!challenger) {
        for (auto const& [key, h] : held)
            if (h.arena == areaName && h.player != 0) { challenger = h.player; break; }
    }
    return challenger;
}

void BindFighterToPlayerOnHost(eCEntity* npc, eCEntity* arenaEntity, char const* arenaName,
                               char const* questName, uint8_t player, bool awaitingQuestSync)
{
    if (!npc || player == 0) return;
    if (npcs::IsMirrored(npc)) npcs::HostTake(npc);
    Entity e(npc);
    Held& h = held[EntityKey(npc)];
    h.name = e.GetName().GetText();
    if (arenaEntity) h.arena = Entity(arenaEntity).GetName().GetText();
    else if (arenaName && *arenaName) h.arena = arenaName;
    if (questName && *questName) h.quest = questName;
    else if (gCQuest_PS* q = FindCandidateBoutQuestForNpc(npc, h.arena.c_str())) h.quest = q->GetName().GetText();
    if (h.arena.empty() && !h.quest.empty()) {
        if (gCQuestManager_PS* mgr = QuestManager())
            if (gCQuest_PS* q = mgr->GetQuest(bCString(h.quest.c_str())))
                h.arena = q->GetDestinationEntity().GetText();
    }
    if (h.player != player || !arena_policy::HasFighterEnteredBoutTask(e.Routine.GetCurrentTask().GetText()))
        h.started = false;
    h.player = player;
    h.generation = player >= 2 ? net::PeerGeneration(player) : 0u;
    h.duel = false;
    h.awaitingQuestSync = awaitingQuestSync;
    h.awaitUntil = awaitingQuestSync ? (net::Now() + 5000u) : 0u;
    if (!h.quest.empty()) {
        BoutQuestState& qs = boutQuests[h.quest];
        qs.challenger = player;
        qs.generation = h.generation;
    }
    npcs::SetContext(npc, player);
    telemetry::OnArenaBind(npc, arenaEntity, h.arena.c_str(), h.quest.c_str(), player, awaitingQuestSync);
}

bool BindQuestFightersOnHost(gCQuest_PS* quest, char const* questName, uint8_t player)
{
    if (!quest || !questName || !*questName || player == 0) return true;
    char const* arenaName = quest->GetDestinationEntity().GetText();
    eCEntity* arenaEntity = (arenaName && *arenaName) ? Entity::GetEntity(bCString(arenaName)).GetInstance() : nullptr;
    bool allLoaded = true;
    auto const& who = quest->GetDeliveryEntities();
    for (GEInt i = 0; i < who.GetCount(); ++i) {
        Entity fighter = Entity::GetEntity(who.GetAt(i));
        if (!fighter.GetInstance()) { allLoaded = false; continue; }
        BindFighterToPlayerOnHost(fighter.GetInstance(), arenaEntity, arenaName, questName, player, false);
    }
    return allLoaded;
}

void ForfeitBoutQuestOnHost(gCQuest_PS* quest, std::string const& questName, Held* h)
{
    if (!quest) return;
    quest->SetStatus(gEQuestStatus_Lost);
    if (!questName.empty()) {
        BoutQuestState& qs = boutQuests[questName];
        qs.observedStatus = gEQuestStatus_Lost;
        qs.challenger = 0;
        qs.awaitingNpcLoad = false;
    }
    if (h) ClearAwaitSync(*h);
    FlushArenaAndQuests();
    telemetry::OnArenaEvent("forfeit", h ? h->arena.c_str() : "", questName.c_str(), nullptr, h ? h->player : 0, gEQuestStatus_Running, gEQuestStatus_Lost, "forfeited");
}

GEInt InvokeNativeSendToArena(gCScriptProcessingUnit* spu, eCEntity* npc, eCEntity* arenaEntity,
                              uint8_t player, GEInt args = 0)
{
    if (!originalSendToArena || !npc || !arenaEntity) return 0;
    eCEntity* puppet = player > 1 ? remote::PuppetOf(player) : nullptr;
    if (puppet) guards::KeepZone(puppet);
    Entity selfEntity(npc);
    Entity placeEntity(arenaEntity);
    guards::LocalPlayer local(player == 1);
    guards::Acting acting(puppet);
    return originalSendToArena(spu, &selfEntity, &placeEntity, args);
}

GEBool GE_STDCALL OnArenaCallback(gCScriptProcessingUnit* spu)
{
    if (ai::ShuttingDown()) return originalOnArena ? originalOnArena(spu) : GEFalse;
    eCEntity* self = spu ? spu->GetSelfEntity() : nullptr;
    uint8_t player = self ? ChallengerForNpc(self) : 0;
    if (!player && self) player = npcs::ContextPlayer(self);
    eCEntity* puppet = player > 1 ? remote::PuppetOf(player) : nullptr;
    if (player > 1 && !puppet) return GEFalse;
    if (puppet) guards::KeepZone(puppet);
    guards::LocalPlayer local(player == 1);
    guards::Acting acting(puppet);
    return originalOnArena ? originalOnArena(spu) : GEFalse;
}

uint8_t LocalPlayerId() { return net::IsHost() ? 1 : net::LocalId(); }

bool ShouldSuppressNonLocalActor(eCEntity* actor)
{
    return on && actor &&
           arena_policy::ShouldSuppressNonLocalAreaActor(
               net::IsHost(), net::Connected(), guards::IsLocalPlayer(actor), npclease::LocalAuthority(actor));
}

bool SuppressSpectatorLeave(char const* areaName, eCEntity* actor, bool& hasRunningBout, char const* action)
{
    hasRunningBout = false;
    if (!on || !net::Connected() || !guards::IsLocalPlayer(actor) || !areaName || !*areaName) return false;
    const uint8_t challenger = ActiveChallengerForArea(areaName, hasRunningBout);
    const uint8_t localId = LocalPlayerId();
    if (!arena_policy::ShouldSuppressPlayerLeaveArea(true, hasRunningBout, challenger, localId)) return false;
    ++leaveSuppressed;
    telemetry::OnArenaEvent("spectator_leave_suppressed", areaName, "", actor, challenger, -1, -1, action);
    Logf("arena: suppressed spectator player %u %s for %s (challenger=%u)", localId, action, areaName, challenger);
    return true;
}

GEInt GE_STDCALL OnLeaveAreaDetour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown() || !originalOnLeaveArea)
        return originalOnLeaveArea ? originalOnLeaveArea(spu, self, other, args) : 0;
    eCEntity* leaving = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* areaEntity = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    char const* areaName = areaEntity ? Entity(areaEntity).GetName().GetText() : "";
    if (ShouldSuppressNonLocalActor(leaving)) return 0;
    bool hasRunningBout = false;
    if (SuppressSpectatorLeave(areaName, leaving, hasRunningBout, "on_leave_area")) return 0;
    if (!hasRunningBout && areaName && *areaName) ActiveChallengerForArea(areaName, hasRunningBout);
    const GEInt result = originalOnLeaveArea(spu, self, other, args);
    if (hasRunningBout && net::Connected()) FlushArenaAndQuests();
    return result;
}

GEInt GE_STDCALL OnEnterAreaDetour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown() || !originalOnEnterArea)
        return originalOnEnterArea ? originalOnEnterArea(spu, self, other, args) : 0;
    eCEntity* entering = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    if (ShouldSuppressNonLocalActor(entering)) return 0;
    bool challengerLeavingBout = false;
    if (entering && entering->GetPropertySet(eEPropertySetType_NPC)) {
        Entity leavingHero(entering);
        Entity altTarget = leavingHero.NPC.GetAlternativeTarget();
        if (altTarget != None) {
            bool hasRunningBout = false;
            if (SuppressSpectatorLeave(altTarget.GetName().GetText(), entering, hasRunningBout, "exit_target"))
                leavingHero.NPC.SetAlternativeTarget(None);
            else if (hasRunningBout)
                challengerLeavingBout = true;
        }
    }
    const GEInt result = originalOnEnterArea(spu, self, other, args);
    if (challengerLeavingBout && net::Connected()) FlushArenaAndQuests();
    return result;
}

GEInt GE_STDCALL Detour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalSendToArena ? originalSendToArena(spu, self, other, args) : 0;
    eCEntity* npc = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* arenaEntity = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    ++sent;
    if (on && !net::IsHost() && npc && net::Connected()) {
        char const* arenaName = arenaEntity ? Entity(arenaEntity).GetName().GetText() : "";
        if (gCQuest_PS* q = FindCandidateBoutQuestForNpc(npc, arenaName)) {
            BoutQuestState& qs = boutQuests[q->GetName().GetText()];
            qs.challenger = net::LocalId();
            qs.syncedFromHost = false;
            qs.observedStatus = static_cast<int>(q->GetStatus().GetValue());
        }
        if (npclease::LocalAuthority(npc))
            npclease::RelinquishToHost(npc, "arena bout");
        if (npcs::Claimed(npc))
            infocmd::TakeOver(npc, false);
        FlushArenaAndQuests();
        net::WorldState request{};
        request.kind = 20;
        guid::CopyEntity(npc, request.guid);
        if (arenaName && *arenaName) strncpy_s(request.routine, arenaName, _TRUNCATE);
        net::SendWorldState(request, 0);
        ++taken;
        telemetry::OnArenaEvent("client_request", arenaName, "", npc, net::LocalId(), -1, -1, "send_to_arena");
        Logf("arena: requested host bout with %s at %s for player %u",
             Entity(npc).GetName().GetText(), (arenaName && *arenaName) ? arenaName : "-", net::LocalId());
        return GETrue;
    }
    if (on && net::IsHost() && npc && net::Connected()) {
        // Client bouts dispatch through InvokeNativeSendToArena; Detour on host only runs from host-initiated scripts/dialogs.
        constexpr uint8_t player = 1;
        char const* arenaName = arenaEntity ? Entity(arenaEntity).GetName().GetText() : "";
        BindFighterToPlayerOnHost(npc, arenaEntity, arenaName, nullptr, player, false);
        SyncArenaStatuses();
        ++taken;
        const GEInt res = arenaEntity
            ? InvokeNativeSendToArena(spu, npc, arenaEntity, player, args)
            : originalSendToArena(spu, self, other, args);
        story::FlushQuests();
        return res;
    }
    return originalSendToArena(spu, self, other, args);
}

void ObserveQuestTransitions(bool allowFlush)
{
    bool syncArenas = false;
    bool flushQuests = false;
    for (CachedBoutQuest const& entry : BoutQuests()) {
        gCQuest_PS* quest = entry.quest;
        if (!quest) continue;
        const int type = static_cast<int>(quest->GetType().GetValue());
        const int status = static_cast<int>(quest->GetStatus().GetValue());
        BoutQuestState& qs = boutQuests[entry.name];
        const bool firstObservation = (qs.observedStatus < 0);
        const int prev = firstObservation ? status : qs.observedStatus;
        qs.observedStatus = status;
        const uint8_t prevChallenger = qs.challenger;
        if (!net::IsHost()) {
            const bool synced = qs.syncedFromHost;
            qs.syncedFromHost = false;
            if (!firstObservation && arena_policy::ShouldClaimLocalClientBout(false, net::Connected(), type, prev, status, synced)) {
                qs.challenger = net::LocalId();
                syncArenas = flushQuests = true;
                telemetry::OnArenaEvent("client_bout_start", "", entry.name.c_str(), nullptr, qs.challenger, prev, status);
                Logf("arena: client player %u started %s locally (%d -> %d)", net::LocalId(), entry.name.c_str(), prev, status);
            } else if (status != gEQuestStatus_Running) {
                if (!firstObservation && prev == gEQuestStatus_Running) {
                    qs.challenger = 0; qs.awaitingNpcLoad = false; syncArenas = true;
                    if (!synced) flushQuests = true;
                    telemetry::OnArenaEvent("bout_end", "", entry.name.c_str(), nullptr, prevChallenger, prev, status);
                } else {
                    qs.challenger = 0;
                }
            }
            continue;
        }
        if (!firstObservation && arena_policy::IsBoutStartTransition(type, prev, status)) {
            if (qs.challenger == 0) qs.challenger = 1;
            syncArenas = flushQuests = true;
            telemetry::OnArenaEvent("host_bout_start", "", entry.name.c_str(), nullptr, qs.challenger, prev, status);
        } else if (!firstObservation && status != gEQuestStatus_Running && prev == gEQuestStatus_Running) {
            qs.challenger = 0; qs.awaitingNpcLoad = false; syncArenas = flushQuests = true;
            telemetry::OnArenaEvent("bout_end", "", entry.name.c_str(), nullptr, prevChallenger, prev, status);
        }
    }
    if (syncArenas) SyncArenaStatuses();
    if (allowFlush && flushQuests) story::FlushQuests();
}

void ResolvePendingHostBoutQuests()
{
    if (!net::IsHost()) return;
    gCQuestManager_PS* manager = QuestManager();
    if (!manager) return;
    for (auto& [questName, qs] : boutQuests) {
        gCQuest_PS* quest = manager->GetQuest(bCString(questName.c_str()));
        const bool running = quest && quest->GetStatus().GetValue() == gEQuestStatus_Running;
        if (!running || qs.challenger == 0) {
            qs.awaitingNpcLoad = false;
            continue;
        }
        const bool peerActive = IsActiveChallenger(qs.challenger, qs.generation);
        if (arena_policy::ShouldForfeitAbsentChallengerOnHost(true, qs.challenger, true, peerActive)) {
            const uint8_t forfeitedPlayer = qs.challenger;
            ForfeitBoutQuestOnHost(quest, questName, nullptr);
            Logf("arena: forfeited %s because challenger player %u left world",
                 questName.c_str(), forfeitedPlayer);
            continue;
        }
        if (qs.awaitingNpcLoad && BindQuestFightersOnHost(quest, questName.c_str(), qs.challenger))
            qs.awaitingNpcLoad = false;
    }
}

void InstallHooks()
{
    const DWORD tick = GetTickCount();
    if (tick - lastTry < 1000) return;
    lastTry = tick;
    auto hookScript = [&](char const* scriptName, gFScript& original, gFScript detour, char const* label) {
        gSScript* script = nullptr;
        if (!GetScriptAdminExt().GetScripts().GetAt(bCString(scriptName), script) ||
            !script || !script->m_funcScript || script->m_funcScript == detour)
            return;
        original = script->m_funcScript;
        script->m_funcScript = detour;
        ai::TrackPointer(reinterpret_cast<void*>(&script->m_funcScript),
                         reinterpret_cast<void*>(original), reinterpret_cast<void*>(detour), label);
    };
    gSScript* sendScript = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("SendToArena"), sendScript) || !sendScript || !sendScript->m_funcScript)
        return;
    if (unsigned char* code = NativeSendToArena(); code && reinterpret_cast<void*>(sendScript->m_funcScript) == code) {
        originalSendToArena = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Detour)));
        native = originalSendToArena != nullptr;
    } else {
        hookScript("SendToArena", originalSendToArena, &Detour, "arena SendToArena script");
    }
    hookScript("OnLeaveArea", originalOnLeaveArea, &OnLeaveAreaDetour, "arena OnLeaveArea script");
    hookScript("OnEnterArea", originalOnEnterArea, &OnEnterAreaDetour, "arena OnEnterArea script");
    GE_MAP_FOR_EACH (name, callback, GetScriptAdminExt().GetScriptAICallbacks())
    {
        if (!callback || !callback->m_funcScriptAICallback) continue;
        if (std::strcmp(name.GetText(), "OnArena") != 0 || callback->m_funcScriptAICallback == &OnArenaCallback)
            continue;
        originalOnArena = callback->m_funcScriptAICallback;
        callback->m_funcScriptAICallback = &OnArenaCallback;
        ai::TrackPointer(reinterpret_cast<void*>(&callback->m_funcScriptAICallback),
                         reinterpret_cast<void*>(originalOnArena),
                         reinterpret_cast<void*>(&OnArenaCallback), "arena OnArena callback");
        break;
    }
    installed = true;
    Logf("arena: hooks installed SendToArena=%s OnLeaveArea=%d OnArena=%d",
         native ? "jump" : (originalSendToArena ? "table" : "none"),
         originalOnLeaveArea ? 1 : 0, originalOnArena ? 1 : 0);
}
} // namespace

void ObserveLocalQuests()
{
    if (!gCSession::GetInstance().IsGameRunning() || !on) return;
    ObserveQuestTransitions(false);
}

void SyncArenaStatuses()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    auto const& bouts = BoutQuests();
    if (cachedArenas.empty()) return;
    for (CachedArena& a : cachedArenas) a.runningCount = 0;
    for (CachedBoutQuest const& entry : bouts) {
        if (entry.arenaIndex >= 0 && entry.quest && entry.quest->GetStatus().GetValue() == gEQuestStatus_Running)
            ++cachedArenas[static_cast<size_t>(entry.arenaIndex)].runningCount;
    }
    const uint32_t now = net::Now();
    for (CachedArena& a : cachedArenas) {
        eCEntity* inst = a.proxy.GetEntity();
        if (!inst &&
            (a.runningCount > 0 || a.nextLookupAt == 0 || static_cast<int32_t>(now - a.nextLookupAt) >= 0)) {
            a.nextLookupAt = now + 1000u;
            Entity resolved = Entity::GetEntity(bCString(a.name.c_str()));
            inst = resolved.GetInstance();
            if (inst) a.proxy = eCEntityProxy(inst);
        }
        if (!inst || !inst->GetPropertySet(eEPropertySetType_Arena)) continue;
        Entity arenaEntity(inst);
        const auto desired = static_cast<gEArenaStatus>(arena_policy::DesiredArenaStatus(a.runningCount));
        if (static_cast<gEArenaStatus>(arenaEntity.Arena.Status) != desired)
            arenaEntity.Arena.Status = desired;
    }
}

bool AllowClientQuestStatusOnHost(uint8_t from, char const* questName, int clientNewStatus)
{
    if (!on || !net::IsHost() || !questName || !*questName) return true;
    gCQuestManager_PS* manager = QuestManager();
    gCQuest_PS* quest = manager ? manager->GetQuest(bCString(questName)) : nullptr;
    if (!quest) return true;
    const int type = static_cast<int>(quest->GetType().GetValue());
    const int hostStatus = static_cast<int>(quest->GetStatus().GetValue());
    uint8_t challenger = ChallengerOfQuest(questName);
    if (!challenger && hostStatus == gEQuestStatus_Running) {
        for (auto const& [key, h] : held)
            if (h.quest == questName && h.player != 0) { challenger = h.player; break; }
        if (!challenger) challenger = 1;
    }
    if (!arena_policy::ShouldAcceptClientQuestStatusOnHost(type, hostStatus, challenger, from, clientNewStatus)) {
        ++statusRefused;
        telemetry::OnArenaEvent("status_refused", "", questName, nullptr, challenger, hostStatus, clientNewStatus, "client_quest_status");
        Logf("arena: refused client player %u status=%d for %s (hostStatus=%d challenger=%u)",
             from, clientNewStatus, questName, hostStatus, challenger);
        return false;
    }
    return true;
}

void OnClientArenaQuestStatus(uint8_t from, char const* questName, int prevStatus, int newStatus)
{
    if (!on || !net::IsHost() || from < 2 || from >= net::MaxPlayers || !questName || !*questName) return;
    gCQuestManager_PS* manager = QuestManager();
    gCQuest_PS* quest = manager ? manager->GetQuest(bCString(questName)) : nullptr;
    if (!quest) return;
    const int type = static_cast<int>(quest->GetType().GetValue());
    if (!arena_policy::IsArenaOrSparringQuestType(type)) return;
    BoutQuestState& qs = boutQuests[questName];
    qs.observedStatus = newStatus;
    if (arena_policy::IsBoutStartTransition(type, prevStatus, newStatus)) {
        qs.challenger = from;
        qs.generation = net::PeerGeneration(from);
        for (auto& [key, h] : held)
            if (h.quest == questName) h.started = false;
        qs.awaitingNpcLoad = !BindQuestFightersOnHost(quest, questName, from);
        SyncArenaStatuses();
        char const* arenaName = quest->GetDestinationEntity().GetText();
        Logf("arena: host registered client player %u bout %s (%d -> %d, arena=%s)",
             from, questName, prevStatus, newStatus, (arenaName && *arenaName) ? arenaName : "-");
    } else if (newStatus != gEQuestStatus_Running) {
        qs.challenger = 0;
        qs.generation = 0;
        qs.awaitingNpcLoad = false;
        for (auto& [key, h] : held)
            if (h.quest == questName) h.awaitingQuestSync = false;
        SyncArenaStatuses();
    }
}

void NoteQuestStatusFromHost(char const* questName, int newStatus)
{
    if (net::IsHost() || !questName || !*questName) return;
    gCQuestManager_PS* manager = QuestManager();
    gCQuest_PS* quest = manager ? manager->GetQuest(bCString(questName)) : nullptr;
    if (!quest || !arena_policy::IsArenaOrSparringQuestType(static_cast<int>(quest->GetType().GetValue())))
        return;
    BoutQuestState& qs = boutQuests[questName];
    qs.observedStatus = newStatus;
    if (newStatus == gEQuestStatus_Running) {
        if (qs.challenger != net::LocalId()) {
            qs.syncedFromHost = true;
            qs.challenger = 1;
        }
    } else {
        qs.syncedFromHost = false;
        qs.challenger = 0;
    }
    SyncArenaStatuses();
}

void TrackDuel(eCEntity* npc, uint8_t player)
{
    if (!on || !net::IsHost() || !npc || player == 0 || player >= net::MaxPlayers) return;
    const GuidKey key = EntityKey(npc);
    char const* conflictKind = nullptr;
    if (ConflictingOwner(npc, key, player, conflictKind)) return;
    if (npcs::IsMirrored(npc)) npcs::HostTake(npc);
    Entity e(npc);
    Held& h = held[key];
    h.name = e.GetName().GetText();
    h.player = player;
    h.generation = player >= 2 ? net::PeerGeneration(player) : 0u;
    h.duel = true;
    h.started = true;
    h.awaitingQuestSync = false;
    h.awaitUntil = 0;
    if (gCQuest_PS* q = FindRunningBoutQuestForNpc(npc)) {
        h.quest = q->GetName().GetText();
        BoutQuestState& qs = boutQuests[h.quest];
        qs.challenger = player;
        qs.generation = h.generation;
    }
    npcs::SetContext(npc, player);
    if (player > 1) guards::KeepZone(remote::PuppetOf(player));
    telemetry::OnArenaEvent("duel_track", "", h.quest.c_str(), npc, player, -1, -1, "duel");
}

uint8_t ChallengerForNpc(eCEntity const* npc)
{
    if (!npc) return 0;
    if (auto it = held.find(EntityKey(npc)); it != held.end() && it->second.player != 0)
        return it->second.player;
    if (gCQuest_PS* q = FindRunningBoutQuestForNpc(npc))
        return ChallengerOfQuest(q->GetName().GetText());
    return 0;
}

void OnState(uint8_t from, net::WorldState const& state)
{
    if (!on || !net::IsHost() || from < 2 || from >= net::MaxPlayers) return;
    Entity npc = guid::Resolve(state.guid);
    if (!npc.GetInstance()) npc = Entity(npcs::NpcByGuid(state.guid));
    eCEntity* arenaEntity = *state.routine ? Entity::GetEntity(bCString(state.routine)).GetInstance() : nullptr;
    if (!npc.GetInstance()) {
        Log("arena: opponent entity not loaded yet for client SendToArena; awaiting quest sync");
        return;
    }
    eCEntity* npcInst = npc.GetInstance();
    const GuidKey key = EntityKey(npcInst);
    char const* conflictKind = nullptr;
    if (const uint8_t owner = ConflictingOwner(npcInst, key, from, conflictKind)) {
        Logf("arena: refused player %u SendToArena for %s (%s %u)", from, npc.GetName().GetText(), conflictKind, owner);
        return;
    }
    const bool questAlreadyRunning = Fights(npcInst);
    BindFighterToPlayerOnHost(npcInst, arenaEntity, state.routine, nullptr, from, !questAlreadyRunning);
    SyncArenaStatuses();
    ++taken;
    Held& h = held[key];
    if (!arenaEntity && !h.arena.empty())
        arenaEntity = Entity::GetEntity(bCString(h.arena.c_str())).GetInstance();
    char const* task = npc.Routine.GetCurrentTask().GetText();
    if (arenaEntity && remote::PuppetOf(from) &&
        arena_policy::ShouldDispatchSendToArenaOnHost(
            Fights(npcInst), talk::IsHeld(npcInst), npclease::Fenced(npc.GetInstance()), task)) {
        InvokeNativeSendToArena(nullptr, npcInst, arenaEntity, from);
        char const* postTask = npc.Routine.GetCurrentTask().GetText();
        if (arena_policy::HasFighterEnteredBoutTask(postTask)) {
            h.started = true;
            ++started;
            telemetry::OnArenaEvent("fighter_active", h.arena.c_str(), h.quest.c_str(), npcInst, h.player, -1, -1, postTask);
        }
    }
}

void TickHostHeld()
{
    if (held.empty()) return;
    gCQuestManager_PS* manager = QuestManager();
    const uint32_t now = net::Now();
    for (auto it = held.begin(); it != held.end();) {
        Held& h = it->second;
        if (h.player == 0) h.player = 1;
        Entity e = guid::Resolve(it->first.data());
        eCEntity* npc = e.GetInstance();

        gCQuest_PS* quest = (!h.quest.empty() && manager) ? manager->GetQuest(bCString(h.quest.c_str())) : nullptr;
        if (!quest && npc) {
            quest = FindRunningBoutQuestForNpc(npc);
            if (quest) h.quest = quest->GetName().GetText();
        }
        bool questRunning = quest ? (quest->GetStatus().GetValue() == gEQuestStatus_Running)
                                  : (npc ? Fights(npc) : false);
        const bool peerActive = IsActiveChallenger(h.player, h.generation);
        if (questRunning || (!peerActive || static_cast<int32_t>(now - h.awaitUntil) >= 0)) {
            if (questRunning || h.awaitingQuestSync) ClearAwaitSync(h);
        }

        if (quest && arena_policy::ShouldForfeitAbsentChallengerOnHost(true, h.player, questRunning, peerActive)) {
            ForfeitBoutQuestOnHost(quest, h.quest, &h);
            questRunning = false;
            Logf("arena: forfeited %s because challenger player %u left world", h.quest.c_str(), h.player);
        }

        if (h.duel && !peerActive) {
            ReleaseHostNpc(npc, it->first);
            ++given;
            Logf("arena: released duel %s after challenger player %u left world", h.name.c_str(), h.player);
            it = held.erase(it);
            continue;
        }

        if (!npc) {
            if (questRunning || h.awaitingQuestSync) { ++it; continue; }
            npcs::ClearHostBinding(it->first.data());
            ++given;
            Logf("arena: released unloaded %s after bout ended for player %u", h.name.c_str(), h.player);
            it = held.erase(it);
            continue;
        }

        npcs::SetContext(npc, peerActive ? h.player : uint8_t(0));
        eCEntity* puppet = (peerActive && h.player > 1) ? remote::PuppetOf(h.player) : nullptr;
        if (puppet) guards::KeepZone(puppet);

        if (quest && puppet &&
            arena_policy::ShouldMarkClientChallengerDefeatedOnHost(
                true, h.player, h.started, questRunning, downed::CombatUnavailable(puppet), e.IsDead(), e.IsDown())) {
            ForfeitBoutQuestOnHost(quest, h.quest, &h);
            questRunning = false;
            Logf("arena: marked %s Lost after challenger player %u was defeated", h.quest.c_str(), h.player);
        }

        const bool stillRunning = Fights(npc);
        char const* task = e.Routine.GetCurrentTask().GetText();
        if (!h.duel && !h.started &&
            arena_policy::ShouldDispatchSendToArenaOnHost(stillRunning, talk::IsHeld(npc), npclease::Fenced(npc), task)) {
            if (h.arena.empty() && quest) h.arena = quest->GetDestinationEntity().GetText();
            eCEntity* arenaEntity = !h.arena.empty() ? Entity::GetEntity(bCString(h.arena.c_str())).GetInstance() : nullptr;
            if (arenaEntity && (h.player == 1 || puppet != nullptr)) {
                InvokeNativeSendToArena(nullptr, npc, arenaEntity, h.player);
                task = e.Routine.GetCurrentTask().GetText();
                Logf("arena: dispatched %s to %s for player %u (task %s)", h.name.c_str(), h.arena.c_str(), h.player, task);
            }
        }
        if (arena_policy::HasFighterEnteredBoutTask(task) && !h.started) {
            h.started = true;
            ++started;
            telemetry::OnArenaEvent("fighter_active", h.arena.c_str(), h.quest.c_str(), npc, h.player, -1, -1, task);
            Logf("arena: %s active in bout for player %u (task %s)", h.name.c_str(), h.player, task);
        }
        if ((h.awaitingQuestSync && peerActive) || stillRunning || arena_policy::IsArenaFighterBusyTask(task)) {
            ++it;
            continue;
        }

        ReleaseHostNpc(npc, it->first);
        ++given;
        story::FlushQuests();
        telemetry::OnArenaEvent("fighter_finished", h.arena.c_str(), h.quest.c_str(), npc, h.player, -1, -1, task);
        Logf("arena: %s finished bout for player %u (task %s)", h.name.c_str(), h.player, task);
        it = held.erase(it);
    }
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning()) return;
    if (!installed) {
        InstallHooks();
        if (!installed) return;
    }
    if (!on) return;
    ObserveQuestTransitions(true);
    SyncArenaStatuses();
    if (!net::IsHost()) {
        if (!net::Connected()) {
            for (auto& [name, qs] : boutQuests) {
                qs.challenger = 0;
                qs.syncedFromHost = false;
            }
        }
        return;
    }
    ResolvePendingHostBoutQuests();
    TickHostHeld();
}

bool Readopt(eCEntity* npc)
{
    if (!Fights(npc)) return false;
    infocmd::TakeOver(npc, false);
    ++readopted;
    Logf("arena: %s from the host's save is an arena opponent, not a companion; given back to the host",
         Entity(npc).GetName().GetText());
    return true;
}

void OnWorldChange()
{
    held.clear();
    boutQuests.clear();
    ResetBoutCache();
}

void Report(LogFn log)
{
    char line[240];
    Format(line,
           "ARENASYNC on=%d installed=%d native=%d sent=%u taken=%u started=%u held=%zu given=%u readopted=%u leave_suppressed=%u refused=%u",
           on ? 1 : 0, installed ? 1 : 0, native ? 1 : 0, sent, taken, started, held.size(), given, readopted,
           leaveSuppressed, statusRefused);
    log(line);
}

void Off(LogFn log)
{
    on = false;
    log("ARENASYNCOFF ok=1");
}
} // namespace g3t::arena
