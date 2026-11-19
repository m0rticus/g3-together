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
static_assert(arena_policy::QuestTypeArena == gEQuestType_Arena);
static_assert(arena_policy::QuestTypeSparring == gEQuestType_Sparring);
static_assert(arena_policy::QuestStatusOpen == gEQuestStatus_Open);
static_assert(arena_policy::QuestStatusRunning == gEQuestStatus_Running);
static_assert(arena_policy::QuestStatusSuccess == gEQuestStatus_Success);
static_assert(arena_policy::QuestStatusFailed == gEQuestStatus_Failed);
static_assert(arena_policy::QuestStatusObsolete == gEQuestStatus_Obsolete);
static_assert(arena_policy::QuestStatusCancelled == gEQuestStatus_Cancelled);
static_assert(arena_policy::QuestStatusLost == gEQuestStatus_Lost);
static_assert(arena_policy::QuestStatusWon == gEQuestStatus_Won);
static_assert(arena_policy::ArenaStatusNone == gEArenaStatus_None);
static_assert(arena_policy::ArenaStatusRunning == gEArenaStatus_Running);
static_assert(arena_policy::AttackReasonArena == gEAttackReason_Arena);
static_assert(arena_policy::AttackReasonDuel == gEAttackReason_Duel);

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
    uint8_t player = 0;
    bool started = false;
    bool duel = false;
    bool awaitingQuestSync = false;
};

struct BoutQuestState {
    uint8_t challenger = 0;       // active challenger player id (1=host, >=2=client)
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

void FlushArenaAndQuests()
{
    SyncArenaStatuses();
    story::FlushQuests();
}

gCQuestManager_PS* QuestManager()
{
    return gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
}

std::vector<CachedBoutQuest> const& BoutQuests()
{
    gCQuestManager_PS* manager = QuestManager();
    if (!manager) {
        cachedBoutQuests.clear();
        cachedArenas.clear();
        cachedManager = nullptr;
        cachedQuestCount = -1;
        cachedFirstQuest = nullptr;
        return cachedBoutQuests;
    }
    auto& quests = manager->AccessQuests();
    const GEInt count = quests.GetCount();
    void const* first = count > 0 ? static_cast<gCQuest_PS*>(quests.AccessAt(0)) : nullptr;
    if (manager == cachedManager && count == cachedQuestCount && first == cachedFirstQuest && !cachedBoutQuests.empty())
        return cachedBoutQuests;
    cachedBoutQuests.clear();
    cachedArenas.clear();
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
                for (size_t ai = 0; ai < cachedArenas.size(); ++ai) {
                    if (cachedArenas[ai].name == dest) { arenaIndex = static_cast<int>(ai); break; }
                }
                if (arenaIndex < 0) {
                    arenaIndex = static_cast<int>(cachedArenas.size());
                    CachedArena ca;
                    ca.name = dest;
                    cachedArenas.push_back(std::move(ca));
                }
            }
        }
        CachedBoutQuest entry;
        entry.quest = quest;
        entry.name = quest->GetName().GetText();
        entry.arenaIndex = arenaIndex;
        cachedBoutQuests.push_back(std::move(entry));
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

bool Fights(eCEntity const* npc)
{
    return FindRunningBoutQuestForNpc(npc) != nullptr;
}

// Remote puppets do not tick native gCNavigation_PS::Process on the host, so guards::KeepZone leaves their
// CurrentZoneEntityProxy stuck on their initial spawn zone. Refresh it dynamically so OnArena (0x11e60)
// sees GetPlayer().GetZone() == arena immediately when the client steps into the ring.
void RefreshPuppetZone(eCEntity* puppet)
{
    static gCNavigationAdmin* admin = nullptr;
    if (!puppet) return;
    if (!admin) admin = static_cast<gCNavigationAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("gCNavigationAdmin")));
    auto* navigation = static_cast<gCNavigation_PS*>(puppet->GetPropertySet(eEPropertySetType_Navigation));
    if (!admin || !navigation) return;
    eCEntityProxy here;
    GEBool path = GEFalse;
    if (admin->GetZone(*puppet, here, path, GEFalse) != bEResult_Ok || !here.GetEntity()) return;
    if (navigation->GetCurrentZoneEntityProxy().GetEntity() != here.GetEntity())
        navigation->AccessCurrentZoneEntityProxy() = here;
}

uint8_t ChallengerOfQuest(char const* questName)
{
    if (!questName || !*questName) return 0;
    auto it = boutQuests.find(questName);
    return it != boutQuests.end() ? it->second.challenger : 0;
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
    h.duel = false;
    h.awaitingQuestSync = awaitingQuestSync;
    h.awaitUntil = awaitingQuestSync ? (net::Now() + 5000u) : 0u;
    if (!h.quest.empty()) boutQuests[h.quest].challenger = player;
    npcs::SetContext(npc, player);
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
        if (!fighter.GetInstance()) {
            allLoaded = false;
            continue;
        }
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
    if (h) {
        h->awaitingQuestSync = false;
        h->awaitUntil = 0;
    }
    FlushArenaAndQuests();
}

GEInt InvokeNativeSendToArena(gCScriptProcessingUnit* spu, eCEntity* npc, eCEntity* arenaEntity,
                              uint8_t player, GEInt args = 0)
{
    if (!originalSendToArena || !npc || !arenaEntity) return 0;
    eCEntity* puppet = player > 1 ? remote::PuppetOf(player) : nullptr;
    if (puppet) RefreshPuppetZone(puppet);
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
    if (puppet) RefreshPuppetZone(puppet);
    guards::LocalPlayer local(player == 1);
    guards::Acting acting(puppet);
    return originalOnArena ? originalOnArena(spu) : GEFalse;
}

GEInt GE_STDCALL OnLeaveAreaDetour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown() || !originalOnLeaveArea)
        return originalOnLeaveArea ? originalOnLeaveArea(spu, self, other, args) : 0;
    eCEntity* leaving = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* areaEntity = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    char const* areaName = areaEntity ? Entity(areaEntity).GetName().GetText() : "";
    if (on && net::Connected() && guards::IsLocalPlayer(leaving) && areaName && *areaName) {
        bool hasRunningBout = false;
        const uint8_t challenger = ActiveChallengerForArea(areaName, hasRunningBout);
        const uint8_t localId = net::IsHost() ? 1 : net::LocalId();
        if (arena_policy::ShouldSuppressPlayerLeaveArea(true, hasRunningBout, challenger, localId)) {
            ++leaveSuppressed;
            Logf("arena: ignored spectator player %u leaving active arena %s (challenger=%u)",
                 localId, areaName, challenger);
            return 0;
        }
    }
    bool hadRunningBout = false;
    if (areaName && *areaName) ActiveChallengerForArea(areaName, hadRunningBout);
    const GEInt result = originalOnLeaveArea(spu, self, other, args);
    if (hadRunningBout && net::Connected()) FlushArenaAndQuests();
    return result;
}

GEInt GE_STDCALL OnEnterAreaDetour(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown() || !originalOnEnterArea)
        return originalOnEnterArea ? originalOnEnterArea(spu, self, other, args) : 0;
    eCEntity* entering = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    bool challengerLeavingBout = false;
    if (on && net::Connected() && guards::IsLocalPlayer(entering) &&
        entering && entering->GetPropertySet(eEPropertySetType_NPC)) {
        Entity leavingHero(entering);
        Entity altTarget = leavingHero.NPC.GetAlternativeTarget();
        if (altTarget != None) {
            char const* altArea = altTarget.GetName().GetText();
            bool hasRunningBout = false;
            const uint8_t challenger = ActiveChallengerForArea(altArea, hasRunningBout);
            const uint8_t localId = net::IsHost() ? 1 : net::LocalId();
            if (arena_policy::ShouldSuppressPlayerLeaveArea(true, hasRunningBout, challenger, localId)) {
                leavingHero.NPC.SetAlternativeTarget(None);
                ++leaveSuppressed;
                Logf("arena: cleared spectator player %u arena exit target %s (challenger=%u)",
                     localId, altArea, challenger);
            } else if (hasRunningBout) {
                challengerLeavingBout = true;
            }
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
        if (gCQuest_PS* q = FindCandidateBoutQuestForNpc(npc, arenaName))
            boutQuests[q->GetName().GetText()].challenger = net::LocalId();
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
        Logf("arena: requested host bout with %s at %s for player %u",
             Entity(npc).GetName().GetText(), (arenaName && *arenaName) ? arenaName : "-", net::LocalId());
        return GETrue;
    }
    if (on && net::IsHost() && npc && net::Connected()) {
        uint8_t player = 1;
        if (eCEntity* acting = guards::CurrentActor(); acting && remote::IsPuppet(acting)) {
            const int owner = remote::PuppetOwner(acting);
            if (owner >= 2 && owner < net::MaxPlayers) player = static_cast<uint8_t>(owner);
        }
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

void ObserveQuestTransitions()
{
    for (CachedBoutQuest const& entry : BoutQuests()) {
        gCQuest_PS* quest = entry.quest;
        if (!quest) continue;
        const int type = static_cast<int>(quest->GetType().GetValue());
        const int status = static_cast<int>(quest->GetStatus().GetValue());
        BoutQuestState& qs = boutQuests[entry.name];
        const bool firstObservation = (qs.observedStatus < 0);
        const int prev = firstObservation ? status : qs.observedStatus;
        qs.observedStatus = status;
        if (!net::IsHost()) {
            const bool synced = qs.syncedFromHost;
            qs.syncedFromHost = false;
            if (!firstObservation && arena_policy::ShouldClaimLocalClientBout(
                                         false, net::Connected(), type, prev, status, synced)) {
                qs.challenger = net::LocalId();
                FlushArenaAndQuests();
                Logf("arena: client player %u started %s locally (%d -> %d)",
                     net::LocalId(), entry.name.c_str(), prev, status);
            } else if (status != gEQuestStatus_Running) {
                qs.challenger = 0;
                if (!firstObservation && prev == gEQuestStatus_Running) {
                    SyncArenaStatuses();
                    if (!synced) story::FlushQuests();
                }
            }
            continue;
        }
        if (!firstObservation && arena_policy::IsBoutStartTransition(type, prev, status)) {
            if (qs.challenger == 0) qs.challenger = 1;
            FlushArenaAndQuests();
        } else if (!firstObservation && status != gEQuestStatus_Running && prev == gEQuestStatus_Running) {
            qs.challenger = 0;
            qs.awaitingNpcLoad = false;
            FlushArenaAndQuests();
        }
    }
}

void ResolvePendingHostBoutQuests()
{
    if (!net::IsHost()) return;
    gCQuestManager_PS* manager = nullptr;
    for (auto& [questName, qs] : boutQuests) {
        if (!qs.awaitingNpcLoad) continue;
        if (!manager) {
            manager = QuestManager();
            if (!manager) return;
        }
        gCQuest_PS* quest = manager->GetQuest(bCString(questName.c_str()));
        if (!quest || quest->GetStatus().GetValue() != gEQuestStatus_Running || qs.challenger == 0) {
            qs.awaitingNpcLoad = false;
            continue;
        }
        if (arena_policy::ShouldForfeitAbsentChallengerOnHost(
                true, qs.challenger, true, net::InWorld(qs.challenger))) {
            const uint8_t forfeitedPlayer = qs.challenger;
            ForfeitBoutQuestOnHost(quest, questName, nullptr);
            Logf("arena: forfeited pending %s because challenger player %u left world",
                 questName.c_str(), forfeitedPlayer);
            continue;
        }
        if (BindQuestFightersOnHost(quest, questName.c_str(), qs.challenger))
            qs.awaitingNpcLoad = false;
    }
}

void InstallHooks()
{
    const DWORD tick = GetTickCount();
    if (tick - lastTry < 1000) return;
    lastTry = tick;
    gSScript* sendScript = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("SendToArena"), sendScript) || !sendScript || !sendScript->m_funcScript)
        return;
    if (unsigned char* code = NativeSendToArena(); code && reinterpret_cast<void*>(sendScript->m_funcScript) == code) {
        originalSendToArena = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Detour)));
        native = originalSendToArena != nullptr;
    } else if (sendScript->m_funcScript != &Detour) {
        originalSendToArena = sendScript->m_funcScript;
        sendScript->m_funcScript = &Detour;
        ai::TrackPointer(reinterpret_cast<void*>(&sendScript->m_funcScript),
                         reinterpret_cast<void*>(originalSendToArena),
                         reinterpret_cast<void*>(&Detour), "arena SendToArena script");
    }
    gSScript* leaveScript = nullptr;
    if (GetScriptAdminExt().GetScripts().GetAt(bCString("OnLeaveArea"), leaveScript) &&
        leaveScript && leaveScript->m_funcScript && leaveScript->m_funcScript != &OnLeaveAreaDetour) {
        originalOnLeaveArea = leaveScript->m_funcScript;
        leaveScript->m_funcScript = &OnLeaveAreaDetour;
        ai::TrackPointer(reinterpret_cast<void*>(&leaveScript->m_funcScript),
                         reinterpret_cast<void*>(originalOnLeaveArea),
                         reinterpret_cast<void*>(&OnLeaveAreaDetour), "arena OnLeaveArea script");
    }
    gSScript* enterScript = nullptr;
    if (GetScriptAdminExt().GetScripts().GetAt(bCString("OnEnterArea"), enterScript) &&
        enterScript && enterScript->m_funcScript && enterScript->m_funcScript != &OnEnterAreaDetour) {
        originalOnEnterArea = enterScript->m_funcScript;
        enterScript->m_funcScript = &OnEnterAreaDetour;
        ai::TrackPointer(reinterpret_cast<void*>(&enterScript->m_funcScript),
                         reinterpret_cast<void*>(originalOnEnterArea),
                         reinterpret_cast<void*>(&OnEnterAreaDetour), "arena OnEnterArea script");
    }
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
        if (!inst && !a.proxy.GetEntityID().IsValid() &&
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
        qs.awaitingNpcLoad = !BindQuestFightersOnHost(quest, questName, from);
        SyncArenaStatuses();
        char const* arenaName = quest->GetDestinationEntity().GetText();
        Logf("arena: host registered client player %u bout %s (%d -> %d, arena=%s)",
             from, questName, prevStatus, newStatus, (arenaName && *arenaName) ? arenaName : "-");
    } else if (newStatus != gEQuestStatus_Running) {
        qs.challenger = 0;
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
    if (npcs::IsMirrored(npc)) npcs::HostTake(npc);
    Entity e(npc);
    Held& h = held[EntityKey(npc)];
    h.name = e.GetName().GetText();
    h.player = player;
    h.duel = true;
    h.started = true;
    h.awaitingQuestSync = false;
    h.awaitUntil = 0;
    if (gCQuest_PS* q = FindRunningBoutQuestForNpc(npc)) {
        h.quest = q->GetName().GetText();
        boutQuests[h.quest].challenger = player;
    }
    npcs::SetContext(npc, player);
    if (player > 1) RefreshPuppetZone(remote::PuppetOf(player));
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
    const bool questAlreadyRunning = Fights(npcInst);
    BindFighterToPlayerOnHost(npcInst, arenaEntity, state.routine, nullptr, from, !questAlreadyRunning);
    SyncArenaStatuses();
    ++taken;
    Held& h = held[EntityKey(npcInst)];
    if (!arenaEntity && !h.arena.empty())
        arenaEntity = Entity::GetEntity(bCString(h.arena.c_str())).GetInstance();
    char const* task = npc.Routine.GetCurrentTask().GetText();
    if (arenaEntity && remote::PuppetOf(from) &&
        arena_policy::ShouldDispatchSendToArenaOnHost(
            Fights(npcInst), talk::IsHeld(npcInst), npclease::Fenced(npc.GetInstance()), task)) {
        InvokeNativeSendToArena(nullptr, npcInst, arenaEntity, from);
        if (arena_policy::HasFighterEnteredBoutTask(npc.Routine.GetCurrentTask().GetText())) {
            h.started = true;
            ++started;
        }
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
    ObserveQuestTransitions();
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
        if (questRunning) {
            h.awaitingQuestSync = false;
            h.awaitUntil = 0;
        } else if (h.awaitingQuestSync &&
                   (!net::InWorld(h.player) || static_cast<int32_t>(now - h.awaitUntil) >= 0)) {
            h.awaitingQuestSync = false;
            h.awaitUntil = 0;
        }

        if (quest && arena_policy::ShouldForfeitAbsentChallengerOnHost(
                         true, h.player, questRunning, net::InWorld(h.player))) {
            ForfeitBoutQuestOnHost(quest, h.quest, &h);
            questRunning = false;
            Logf("arena: forfeited %s because challenger player %u left world", h.quest.c_str(), h.player);
        }

        if (!npc) {
            if (questRunning || h.awaitingQuestSync) { ++it; continue; }
            npcs::ClearHostBinding(it->first.data());
            ++given;
            Logf("arena: released unloaded %s after bout ended for player %u", h.name.c_str(), h.player);
            it = held.erase(it);
            continue;
        }

        npcs::SetContext(npc, h.player);
        eCEntity* puppet = h.player > 1 ? remote::PuppetOf(h.player) : nullptr;
        if (puppet) RefreshPuppetZone(puppet);

        if (quest && puppet &&
            arena_policy::ShouldMarkClientChallengerDefeatedOnHost(
                true, h.player, h.started, questRunning,
                downed::CombatUnavailable(puppet), e.IsDead(), e.IsDown())) {
            ForfeitBoutQuestOnHost(quest, h.quest, &h);
            questRunning = false;
            Logf("arena: marked %s Lost after challenger player %u was defeated", h.quest.c_str(), h.player);
        }

        const bool stillRunning = Fights(npc);
        char const* task = e.Routine.GetCurrentTask().GetText();
        if (!h.duel && !h.started &&
            arena_policy::ShouldDispatchSendToArenaOnHost(
                stillRunning, talk::IsHeld(npc), npclease::Fenced(npc), task)) {
            if (h.arena.empty() && quest) h.arena = quest->GetDestinationEntity().GetText();
            eCEntity* arenaEntity = !h.arena.empty() ? Entity::GetEntity(bCString(h.arena.c_str())).GetInstance() : nullptr;
            if (arenaEntity && (h.player == 1 || puppet != nullptr)) {
                InvokeNativeSendToArena(nullptr, npc, arenaEntity, h.player);
                task = e.Routine.GetCurrentTask().GetText();
                Logf("arena: dispatched %s to %s for player %u (task %s)",
                     h.name.c_str(), h.arena.c_str(), h.player, task);
            }
        }
        if (arena_policy::HasFighterEnteredBoutTask(task)) {
            if (!h.started) {
                h.started = true;
                ++started;
                Logf("arena: %s active in bout for player %u (task %s)", h.name.c_str(), h.player, task);
            }
        }
        if (h.awaitingQuestSync && net::InWorld(h.player)) { ++it; continue; }
        if (stillRunning || arena_policy::IsArenaFighterBusyTask(task)) { ++it; continue; }

        npcs::SetContext(npc, 0);
        if (npcs::HostTaken(npc)) npcs::HostRelease(npc);
        ++given;
        story::FlushQuests();
        Logf("arena: %s finished bout for player %u (task %s)", h.name.c_str(), h.player, task);
        it = held.erase(it);
    }
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
    cachedBoutQuests.clear();
    cachedArenas.clear();
    cachedManager = nullptr;
    cachedQuestCount = -1;
    cachedFirstQuest = nullptr;
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
