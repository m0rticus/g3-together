// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide.h"
#include "guide_policy.h"

#include "infocmd.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"

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
// Static analysis (re/out/p8-check-20261024-c.c, p8-decompiled.c): StartGuidePlayer (Script_Game 0xa9110) sets
// GetPlayer().Party.Leader = the guide; SelectRoutineTask (0xa9440) chooses ZS_GuidePlayer only when the local hero's
// leader is the NPC, ZS_FollowPlayer only when the NPC's leader is the local hero; ZS_GuidePlayer_Wait measures the
// distance to GetPlayer. A quest the client takes sets these here, on a mirror, while the host simulates the NPC: Mitch
// never guided, the escaped slave never followed (followdiag G3_VIA=dialog, 25.10.). The same for the host with an NPC
// the client simulates (its region). Taken over, the player's own game guides and follows its hero; it waits only for the
// player who took the quest, as in the single-player game (owner 25.10.). A client takes the NPC like a recruited
// companion (claim), the host takes it back from the client (npcs::HostTake).
// A guide is read from the hero's leader every half second (the quest's start reaches StartGuidePlayer by a path not
// traced). Followers only from a join in a dialog here (CmdPartyJoin, CmdMobJoin, CmdSlaveJoin: info conditions 14, 28,
// 29, none a JoinPlayer line): a companion handed back by JoinPlayer false still has the hero as leader on the mirror
// for a moment and was taken again (stageE3, 25.10.).
enum Kind : uint8_t { Guide, Follower };
struct Held { Kind kind; std::string name; std::string questName; };
std::unordered_map<std::string, Held> held;  // GUID -> what this machine took over here
std::unordered_map<std::string, uint32_t> joined;  // GUID -> until when a slave join here is watched
std::unordered_map<std::string, int> observedQuestStatus;  // questName -> last observed status
std::unordered_set<std::string> hostSyncedRunning;         // FollowNpc quests marked Running by host sync
constexpr uint32_t JoinWatch = 5000;  // ms
bool on = true;  // QA negative control: guidesyncoff
unsigned guidesTaken = 0, followersTaken = 0, given = 0, readopted = 0, joinsStarted = 0;

std::string Key(eCEntity* entity)
{
    const eCEntityProxy proxy(entity);
    return std::string(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
}

Entity Resolve(std::string const& key)
{
    _GUID raw;
    std::memcpy(&raw, key.data(), 16);
    return Entity(bCPropertyID(bCGuid(raw)));
}

eCEntity* LeaderOf(eCEntity* entity)
{
    if (!entity || !entity->GetPropertySet(eEPropertySetType_Party)) return nullptr;
    return Entity(entity).Party.GetPartyLeader().GetInstance();
}

// Whether a running follow quest (type FollowNpc) is delivered by this NPC: returns the quest name it guides in.
char const* QuestGuidedBy(eCEntity* npc)
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
            if (who.GetAt(i) == name) return quest->AccessName().GetText();
    }
    return nullptr;
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
    gCQuestManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
    if (!manager || !npc) return nullptr;
    const bCString name = Entity(npc).GetName();
    auto& quests = manager->AccessQuests();
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        auto* quest = static_cast<gCQuest_PS*>(quests.AccessAt(qi));
        if (!quest || quest->GetType().GetValue() != gEQuestType_FollowNpc || quest->GetStatus().GetValue() != gEQuestStatus_Running) continue;
        auto const& who = quest->GetDeliveryEntities();
        for (GEInt i = 0; i < who.GetCount(); ++i) {
            if (who.GetAt(i) != name) continue;
            bCString const& destName = quest->GetDestinationEntity();
            eCEntity* dest = destName.GetLength() > 0 ? Entity::GetEntity(destName).GetInstance() : nullptr;
            return guide_policy::SelectGuideDestination(destName.GetLength() > 0, dest != nullptr) ==
                           guide_policy::GuideDestinationSource::QuestDestinationEntity
                       ? dest
                       : nullptr;
        }
    }
    return nullptr;
}

// A guide taken over: its own routine again, chosen as at a dialog's end; with the local hero's leader on it,
// SelectRoutineTask makes it ZS_GuidePlayer.
void StartGuiding(Entity& npc)
{
    npc.Routine.FullStop();
    npc.Routine.ContinueRoutine();
}

void Take(eCEntity* npc, Kind kind, char const* questName = nullptr)
{
    const std::string key = Key(npc);
    if (held.count(key) || npcs::Claimed(npc) || npcs::HostTaken(npc)) return;  // ours already (a recruited companion too)
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
    held[key] = {kind, e.GetName().GetText(), resolvedQuest ? resolvedQuest : ""};
    if (kind == Guide) {
        if (net::IsHost()) StartGuiding(e);
        ++guidesTaken;
    } else {
        ++followersTaken;
    }
    Logf("guide: took over %s (it %s this player, quest=%s), task %s", e.GetName().GetText(), kind == Guide ? "guides" : "follows",
         resolvedQuest ? resolvedQuest : "-", e.Routine.GetCurrentTask().GetText());
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
    if (!quest) return;
    eCEntity* npc = nullptr;
    auto const& who = quest->GetDeliveryEntities();
    for (GEInt i = 0; i < who.GetCount(); ++i) {
        Entity candidate = Entity::GetEntity(who.GetAt(i));
        if (candidate.GetInstance()) { npc = candidate.GetInstance(); break; }
    }
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
    uint8_t guid[16];
    const eCEntityProxy proxy(npc);
    std::memcpy(guid, &proxy.GetEntityID().GetCore(), 16);
    if (npcs::OnClaim(guid, fromPlayer, true)) {
        npcs::Guide(guid, fromPlayer, questName);
        Logf("guide: host auto-started %s guiding player %u from synced quest %s",
             Entity(npc).GetName().GetText(), fromPlayer, questName);
    }
}

void Tick()
{
    if (!on || !gCSession::GetInstance().IsGameRunning()) return;
    const uint32_t now = net::Now();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    for (auto it = joined.begin(); it != joined.end();) {
        Entity e = Resolve(it->first);
        eCEntity* npc = e.GetInstance();
        if (npc && LeaderOf(npc) == hero) {
            if (npcs::IsMirrored(npc) || npclease::LocalAuthority(npc)) {
                if (net::Connected()) {
                    if (!held.count(Key(npc)) && !npcs::Claimed(npc) && !npcs::HostTaken(npc) &&
                        npclease::LocalAuthority(npc))
                        npclease::RelinquishToHost(npc, "guide started");
                    Take(npc, Follower);  // follows through the companion path
                }
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
    if (!net::Connected()) return;
    if (!net::IsHost() && net::InWorld(net::LocalId())) {
        if (gCQuestManager_PS* manager = gCQuestManager_PS::GetInstance()) {
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
                    eCEntity* deliveryNpc = nullptr;
                    auto const& who = quest->GetDeliveryEntities();
                    for (GEInt i = 0; i < who.GetCount(); ++i) {
                        Entity candidate = Entity::GetEntity(who.GetAt(i));
                        if (candidate.GetInstance()) { deliveryNpc = candidate.GetInstance(); break; }
                    }
                    if (!deliveryNpc) continue;
                    observedQuestStatus[qName] = cur;
                    if (hero->GetPropertySet(eEPropertySetType_Party) && LeaderOf(hero) != deliveryNpc)
                        Entity(hero).Party.SetPartyLeader(Entity(deliveryNpc));
                    if (!held.count(Key(deliveryNpc)) && !npcs::Claimed(deliveryNpc) && !npcs::HostTaken(deliveryNpc) &&
                        npclease::LocalAuthority(deliveryNpc))
                        npclease::RelinquishToHost(deliveryNpc, "guide started");
                    Take(deliveryNpc, Guide, qName);
                } else {
                    observedQuestStatus[qName] = cur;
                    if (cur != gEQuestStatus_Running) hostSyncedRunning.erase(qName);
                }
            }
        }
    }
    if (eCEntity* npc = LeaderOf(hero); npc && npc != hero && (npcs::IsMirrored(npc) || npclease::LocalAuthority(npc))) {
        if (!held.count(Key(npc)) && !npcs::Claimed(npc) && !npcs::HostTaken(npc) && npclease::LocalAuthority(npc))
            npclease::RelinquishToHost(npc, "guide started");
        Take(npc, Guide, QuestGuidedBy(npc));
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
            if (net::IsHost()) npcs::HostRelease(npc);
            else infocmd::TakeOver(npc, false);
            ++given;
            Logf("guide: %s no longer %s this player; %s", it->second.name.c_str(), it->second.kind == Guide ? "guides" : "follows",
                 net::IsHost() ? "the usual simulation rules again" : "given back to the host");
        }
        it = held.erase(it);  // or the host took it back (a revoked claim)
    }
}

bool Readopt(eCEntity* npc)
{
    char const* questName = QuestGuidedBy(npc);
    if (!on || !questName) return false;
    if (!net::IsHost()) {
        Entity hero(gCSession::GetInstance().GetHero());
        if (!hero.GetInstance() || !hero.GetInstance()->GetPropertySet(eEPropertySetType_Party)) return false;
        hero.Party.SetPartyLeader(Entity(npc));
        held[Key(npc)] = {Guide, Entity(npc).GetName().GetText(), questName};
        ++readopted;
        return infocmd::TakeOver(npc, true, false, questName);
    }
    // StartGuidePlayer as the quest's start runs it: the local hero's leader becomes the guide.
    GetScriptAdminExt().CallScript(bCString("StartGuidePlayer"), npc, QuestDestinationForNpc(npc), 0);
    Entity e(npc);
    held[Key(npc)] = {Guide, e.GetName().GetText(), questName};
    StartGuiding(e);
    ++readopted;
    Logf("guide: %s guides this player again (from the host's save), task %s", e.GetName().GetText(), e.Routine.GetCurrentTask().GetText());
    return true;
}

bool IsQuestGuide(eCEntity* npc) { return GuidesInQuest(npc); }

void OnWorldChange()
{
    held.clear();
    joined.clear();
    observedQuestStatus.clear();
    hostSyncedRunning.clear();
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
