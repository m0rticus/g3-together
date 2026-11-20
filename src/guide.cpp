// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide.h"
#include "guide_policy.h"

#include "guid.h"
#include "guards.h"
#include "infocmd.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"
#include "telemetry.h"

#include <Game.h>
#include <Script.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::guide {
namespace {
static_assert(guide_policy::QuestStatusOpen == gEQuestStatus_Open);
static_assert(guide_policy::QuestStatusRunning == gEQuestStatus_Running);
static_assert(guide_policy::QuestStatusFailed == gEQuestStatus_Failed);
static_assert(guide_policy::QuestStatusObsolete == gEQuestStatus_Obsolete);
static_assert(guide_policy::QuestStatusCancelled == gEQuestStatus_Cancelled);
static_assert(guide_policy::QuestStatusLost == gEQuestStatus_Lost);
static_assert(guide_policy::QuestTypeFollowNpc == gEQuestType_FollowNpc);

// Static analysis (re/out/p8-check-20261024-c.c, p8-decompiled.c): StartGuidePlayer (Script_Game 0xa9110) sets
// GetPlayer().Party.Leader = the guide; SelectRoutineTask (0xa9440) chooses ZS_GuidePlayer only when the local hero's
// leader is the NPC, ZS_FollowPlayer only when the NPC's leader is the local hero; ZS_GuidePlayer_Wait measures the
// distance to GetPlayer. Taken over, the player's own game guides and follows its hero; it waits only for the
// player who took the quest, as in the single-player game (owner 25.10.).
enum Kind : uint8_t { Guide, Follower };
struct Held { Kind kind; std::string name; std::string questName; };
std::unordered_map<std::string, Held> held;  // GUID -> what this machine took over here
std::unordered_map<std::string, uint32_t> joined;  // GUID -> until when a slave join here is watched
std::unordered_map<std::string, int> observedQuestStatus;  // questName -> last observed status
std::unordered_set<std::string> hostSyncedRunning;         // FollowNpc quests marked Running by host sync
std::unordered_set<std::string> awaitingHostRelease;       // client guides released here, awaiting host guidePlayer clearance
constexpr uint32_t JoinWatch = 5000;  // ms
bool on = true;  // QA negative control: guidesyncoff
unsigned guidesTaken = 0, followersTaken = 0, given = 0, readopted = 0, joinsStarted = 0;

std::string Key(eCEntity const* entity)
{
    uint8_t raw[16] = {};
    guid::CopyEntity(const_cast<eCEntity*>(entity), raw);
    return std::string(reinterpret_cast<char const*>(raw), 16);
}

Entity Resolve(std::string const& key)
{
    return guid::Resolve(reinterpret_cast<uint8_t const*>(key.data()));
}

eCEntity* LeaderOf(eCEntity* entity)
{
    if (!entity || !entity->GetPropertySet(eEPropertySetType_Party)) return nullptr;
    return Entity(entity).Party.GetPartyLeader().GetInstance();
}

gCQuest_PS* FindRunningFollowQuestForNpc(eCEntity* npc)
{
    gCQuestManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
    if (!manager || !npc) return nullptr;
    const bCString name = Entity(npc).GetName();
    auto& quests = manager->AccessQuests();
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        auto* quest = static_cast<gCQuest_PS*>(quests.AccessAt(qi));
        if (!quest || quest->GetType().GetValue() != gEQuestType_FollowNpc || quest->GetStatus().GetValue() != gEQuestStatus_Running) continue;
        auto const& who = quest->GetDeliveryEntities();
        for (GEInt i = 0; i < who.GetCount(); ++i)
            if (who.GetAt(i) == name) return quest;
    }
    return nullptr;
}

eCEntity* FirstResolvedDeliveryNpc(gCQuest_PS* quest)
{
    if (!quest) return nullptr;
    auto const& who = quest->GetDeliveryEntities();
    for (GEInt i = 0; i < who.GetCount(); ++i) {
        Entity candidate = Entity::GetEntity(who.GetAt(i));
        if (candidate.GetInstance()) return candidate.GetInstance();
    }
    return nullptr;
}

// Whether a running follow quest (type FollowNpc) is delivered by this NPC: returns the quest name it guides in.
char const* QuestGuidedBy(eCEntity* npc)
{
    gCQuest_PS* quest = FindRunningFollowQuestForNpc(npc);
    return quest ? quest->AccessName().GetText() : nullptr;
}

bool GuidesInQuest(eCEntity* npc) { return QuestGuidedBy(npc) != nullptr; }

bool IsQuestStillRunning(std::string const& questName)
{
    if (questName.empty() || !gCSession::GetInstance().IsGameRunning()) return false;
    gCQuestManager_PS* manager = gCQuestManager_PS::GetInstance();
    if (!manager) return false;
    gCQuest_PS* quest = manager->GetQuest(bCString(questName.c_str()));
    return quest && quest->GetStatus().GetValue() == gEQuestStatus_Running;
}

eCEntity* QuestDestinationForNpc(eCEntity* npc)
{
    gCQuest_PS* quest = FindRunningFollowQuestForNpc(npc);
    if (!quest) return nullptr;
    bCString const& destName = quest->GetDestinationEntity();
    eCEntity* dest = destName.GetLength() > 0 ? Entity::GetEntity(destName).GetInstance() : nullptr;
    return guide_policy::SelectGuideDestination(destName.GetLength() > 0, dest != nullptr) ==
                   guide_policy::GuideDestinationSource::QuestDestinationEntity
               ? dest
               : nullptr;
}

// A guide taken over: its own routine again, chosen as at a dialog's end; with the local hero's leader on it,
// SelectRoutineTask makes it ZS_GuidePlayer.
void StartGuiding(Entity& npc)
{
    const uint8_t actingPlayer = guide_policy::GuideRoutineActingPlayer(
        net::IsHost(), talk::IsHeld(npc.GetInstance()), npcs::ContextPlayer(npc.GetInstance()));
    guards::Acting context(actingPlayer > 1 ? remote::PuppetOf(actingPlayer) : nullptr);
    npc.Routine.FullStop();
    npc.Routine.ContinueRoutine();
}

void Take(eCEntity* npc, Kind kind, char const* questName = nullptr)
{
    const std::string key = Key(npc);
    if (held.count(key) || npcs::Claimed(npc) || npcs::HostTaken(npc)) return;  // ours already (a recruited companion too)
    if (npclease::LocalAuthority(npc)) npclease::RelinquishToHost(npc, "guide started");
    Entity e(npc);
    char const* resolvedQuest = (questName && *questName) ? questName : (kind == Guide ? QuestGuidedBy(npc) : nullptr);
    if (net::IsHost()) {
        if (!npcs::HostTake(npc)) return;
        if (kind == Follower) {
            e.Routine.FullStop();
            e.Routine.SetTask(bCString("ZS_FollowPlayer"));
        }
    } else if (!infocmd::TakeOver(npc, true, kind == Follower, resolvedQuest)) {
        return;
    }
    awaitingHostRelease.erase(key);
    held[key] = {kind, e.GetName().GetText(), resolvedQuest ? resolvedQuest : ""};
    if (kind == Guide) {
        if (net::IsHost()) StartGuiding(e);
        ++guidesTaken;
    } else {
        ++followersTaken;
    }
    Logf("guide: took over %s (it %s this player, quest=%s), task %s", e.GetName().GetText(), kind == Guide ? "guides" : "follows",
         resolvedQuest ? resolvedQuest : "-", e.Routine.GetCurrentTask().GetText());
    telemetry::OnGuideEvent("take_over", net::LocalId(), npc, reinterpret_cast<uint8_t const*>(key.data()),
                            kind == Follower, resolvedQuest ? resolvedQuest : e.Routine.GetCurrentTask().GetText());
}
void TickJoinedFollowers(eCEntity* hero, uint32_t now)
{
    for (auto it = joined.begin(); it != joined.end();) {
        Entity e = Resolve(it->first);
        eCEntity* npc = e.GetInstance();
        if (npc && LeaderOf(npc) == hero) {
            if (npcs::IsMirrored(npc) || npclease::LocalAuthority(npc)) {
                if (net::Connected()) Take(npc, Follower);  // follows through the companion path
            } else {
                // Simulated here: what JoinPlayer true does after JoinPlayerSlavery (re/findings/party-members.md).
                e.Routine.FullStop();
                e.Routine.SetTask(bCString("ZS_FollowPlayer"));
                ++joinsStarted;
                Logf("guide: %s joined this player and follows", e.GetName().GetText());
            }
            it = joined.erase(it);
        } else if (static_cast<int32_t>(now - it->second) > 0) {
            it = joined.erase(it);
        } else {
            ++it;
        }
    }
}

void TickClientFollowQuests(eCEntity* hero)
{
    if (net::IsHost() || !net::InWorld(net::LocalId())) return;
    for (auto relIt = awaitingHostRelease.begin(); relIt != awaitingHostRelease.end();) {
        Entity e = Resolve(*relIt);
        eCEntity* npc = e.GetInstance();
        net::NpcRecord const* combat = npc ? npcs::CombatRecord(npc) : nullptr;
        const bool hostStillEchoesLocal = combat && combat->guidePlayer == net::LocalId();
        if (!hostStillEchoesLocal && LeaderOf(hero) != npc) relIt = awaitingHostRelease.erase(relIt);
        else ++relIt;
    }
    gCQuestManager_PS* manager = gCQuestManager_PS::GetInstance();
    if (!manager) return;
    auto& quests = manager->AccessQuests();
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        auto* quest = static_cast<gCQuest_PS*>(quests.AccessAt(qi));
        if (!quest || quest->GetType().GetValue() != gEQuestType_FollowNpc) continue;
        char const* qName = quest->AccessName().GetText();
        if (!qName || !*qName) continue;
        const int cur = quest->GetStatus().GetValue();
        auto prevIt = observedQuestStatus.find(qName);
        const int prev = prevIt == observedQuestStatus.end() ? static_cast<int>(gEQuestStatus_Open) : prevIt->second;
        const bool fromHost = hostSyncedRunning.count(qName) != 0;
        if (guide_policy::ShouldClaimLocalFollowQuest(
                false, net::Connected(), quest->GetType().GetValue(), prev, cur, fromHost)) {
            eCEntity* deliveryNpc = FirstResolvedDeliveryNpc(quest);
            if (!deliveryNpc) continue;
            observedQuestStatus[qName] = cur;
            if (hero->GetPropertySet(eEPropertySetType_Party) && LeaderOf(hero) != deliveryNpc)
                Entity(hero).Party.SetPartyLeader(Entity(deliveryNpc));
            Take(deliveryNpc, Guide, qName);
        } else {
            observedQuestStatus[qName] = cur;
            if (cur != gEQuestStatus_Running) hostSyncedRunning.erase(qName);
        }
    }
}

void TickHeldGuides(eCEntity* hero)
{
    if (eCEntity* npc = LeaderOf(hero); npc && npc != hero && (npcs::IsMirrored(npc) || npclease::LocalAuthority(npc))) {
        char const* runningQuest = QuestGuidedBy(npc);
        const std::string key = Key(npc);
        net::NpcRecord const* combat = !net::IsHost() ? npcs::CombatRecord(npc) : nullptr;
        const bool hostEchoesLocal = combat && combat->guidePlayer == net::LocalId();
        const bool awaitingRelease = !net::IsHost() && awaitingHostRelease.count(key) != 0;
        if (guide_policy::ShouldAdoptHeroLeaderAsGuide(net::IsHost(), runningQuest != nullptr, hostEchoesLocal, awaitingRelease)) {
            Take(npc, Guide, runningQuest);
        } else if (awaitingRelease && hero->GetPropertySet(eEPropertySetType_Party)) {
            Entity(hero).Party.SetPartyLeader(Entity());
        }
    }
    // Given back once the relation ends: StopGuidePlayer at the destination, the slave's delivery, a dismissal.
    for (auto it = held.begin(); it != held.end();) {
        Entity e = Resolve(it->first);
        eCEntity* npc = e.GetInstance();
        if (!npc) { ++it; continue; }  // not loaded here now; kept until it is
        const bool ours = net::IsHost() ? npcs::HostTaken(npc) : npcs::Claimed(npc);
        const bool questStillRunning = it->second.questName.empty() || IsQuestStillRunning(it->second.questName);
        const bool holds = it->second.kind == Guide ? (LeaderOf(hero) == npc && questStillRunning) : (LeaderOf(npc) == hero);
        if (holds && ours) { ++it; continue; }
        if (it->second.kind == Guide && !questStillRunning && hero->GetPropertySet(eEPropertySetType_Party) &&
            LeaderOf(hero) == npc)
            Entity(hero).Party.SetPartyLeader(Entity());
        if (ours) {
            if (net::IsHost()) {
                npcs::HostRelease(npc);
            } else {
                if (it->second.kind == Guide) awaitingHostRelease.insert(it->first);
                infocmd::TakeOver(npc, false);
            }
            ++given;
            Logf("guide: %s no longer %s this player; %s", it->second.name.c_str(), it->second.kind == Guide ? "guides" : "follows",
                 net::IsHost() ? "the usual simulation rules again" : "given back to the host");
            telemetry::OnGuideEvent("released", net::LocalId(), npc, reinterpret_cast<uint8_t const*>(it->first.data()),
                                    it->second.kind == Follower, it->second.questName.c_str());
        }
        it = held.erase(it);  // or the host took it back (a revoked claim)
    }
}
}

void Joined(eCEntity* npc)
{
    if (on && npc) joined[Key(npc)] = net::Now() + JoinWatch;
}

void NoteQuestStatusFromHost(char const* questName, int status)
{
    if (!questName || !*questName) return;
    observedQuestStatus[questName] = status;
    if (status == gEQuestStatus_Running) hostSyncedRunning.insert(questName);
    else hostSyncedRunning.erase(questName);
}

void OnClientFollowQuestRunning(unsigned char fromPlayer, char const* questName, int previousStatus, int currentStatus)
{
    if (!on || !net::IsHost() || !questName || !*questName || !gCSession::GetInstance().IsGameRunning()) return;
    gCQuestManager_PS* manager = gCQuestManager_PS::GetInstance();
    if (!manager) return;
    gCQuest_PS* quest = manager->GetQuest(bCString(questName));
    eCEntity* npc = FirstResolvedDeliveryNpc(quest);
    if (!npc) return;
    uint8_t existingGuidePlayer = 0;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (id != net::LocalId() && !net::Remotes()[id].connected) continue;
        eCEntity* actor = id == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(id);
        if (actor && LeaderOf(actor) == npc) { existingGuidePlayer = id; break; }
    }
    const uint8_t existingClaimOwner = npcs::ClaimedBy(npc) ? npcs::ClaimedBy(npc) : (npcs::HostTaken(npc) ? uint8_t(1) : uint8_t(0));
    if (!guide_policy::ShouldHostAutoGuideFromQuestSync(
            true, fromPlayer, false, quest->GetType().GetValue(), previousStatus, currentStatus, existingClaimOwner, existingGuidePlayer))
        return;
    uint8_t npcGuid[16];
    guid::CopyEntity(npc, npcGuid);
    if (npcs::OnClaim(npcGuid, fromPlayer, true)) {
        npcs::Guide(npcGuid, fromPlayer, questName);
        Logf("guide: host auto-started %s guiding player %u from synced quest %s",
             Entity(npc).GetName().GetText(), fromPlayer, questName);
        telemetry::OnGuideEvent("host_auto_guide", fromPlayer, npc, npcGuid, false, questName);
    }
}

void Tick()
{
    if (!on || !gCSession::GetInstance().IsGameRunning()) return;
    const uint32_t now = net::Now();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    TickJoinedFollowers(hero, now);
    if (!net::Connected()) return;
    TickClientFollowQuests(hero);
    TickHeldGuides(hero);
}

bool Readopt(eCEntity* npc)
{
    char const* questName = QuestGuidedBy(npc);
    if (!on || !questName) return false;
    const std::string key = Key(npc);
    awaitingHostRelease.erase(key);
    if (!net::IsHost()) {
        Entity hero(gCSession::GetInstance().GetHero());
        if (!hero.GetInstance() || !hero.GetInstance()->GetPropertySet(eEPropertySetType_Party)) return false;
        hero.Party.SetPartyLeader(Entity(npc));
        held[key] = {Guide, Entity(npc).GetName().GetText(), questName};
        ++readopted;
        return infocmd::TakeOver(npc, true, false, questName);
    }
    // StartGuidePlayer as the quest's start runs it: the local hero's leader becomes the guide.
    GetScriptAdminExt().CallScript(bCString("StartGuidePlayer"), npc, QuestDestinationForNpc(npc), 0);
    Entity e(npc);
    held[key] = {Guide, e.GetName().GetText(), questName};
    StartGuiding(e);
    ++readopted;
    Logf("guide: %s guides this player again (from the host's save), task %s", e.GetName().GetText(), e.Routine.GetCurrentTask().GetText());
    telemetry::OnGuideEvent("readopt", net::LocalId(), npc, reinterpret_cast<uint8_t const*>(key.data()), false, questName);
    return true;
}

bool IsQuestGuide(eCEntity* npc) { return GuidesInQuest(npc); }

bool ClientHolding(eCEntity const* npc)
{
    if (!npc || net::IsHost()) return false;
    const std::string key = Key(npc);
    if (held.count(key) != 0) return true;
    if (awaitingHostRelease.count(key) != 0) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return hero && LeaderOf(hero) == npc;
}

void OnWorldChange()
{
    held.clear();
    joined.clear();
    observedQuestStatus.clear();
    hostSyncedRunning.clear();
    awaitingHostRelease.clear();
}

void Report(char const* name, LogFn log)
{
    char line[400];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* leader = LeaderOf(hero);
    std::string about;
    if (name && *name) {
        Entity npc = Entity::GetEntity(bCString(name));
        eCEntity* instance = npc.GetInstance();
        eCEntity* its = LeaderOf(instance);
        char part[200];
        Format(part, " npc=%s found=%d leader=%s task=%s mirrored=%d claimed=%d", name, instance ? 1 : 0,
                  its ? Entity(its).GetName().GetText() : "-", instance ? npc.Routine.GetCurrentTask().GetText() : "-",
                  npcs::IsMirrored(instance) ? 1 : 0, npcs::Claimed(instance) ? 1 : 0);
        about = part;
    }
    Format(line, "GUIDE on=%d hero_leader=%s held=%zu guides_taken=%u followers_taken=%u given=%u readopted=%u joins_started=%u%s",
              on ? 1 : 0, leader ? Entity(leader).GetName().GetText() : "-", held.size(), guidesTaken, followersTaken, given, readopted,
              joinsStarted, about.c_str());
    log(line);
}

void Off(LogFn log)
{
    on = false;
    log("GUIDESYNCOFF ok=1");
}
}
