// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_companions.h"
#include "npc_contact.h"
#include "npc_corpse.h"

#include "arena.h"
#include "combat_timing.h"
#include "guards.h"
#include "guide.h"
#include "guide_policy.h"
#include "hud.h"
#include "i18n.h"
#include "journal.h"
#include "log.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "steam.h"
#include "talk.h"
#include "talk_policy.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3t::npcs::detail {

void NoteTakeover(GuidKey const& key, net::NpcRecord const& record, uint8_t owner)
{
    static unsigned noted = 0;
    if (noted >= 60) return;
    ++noted;
    Entity npc = Resolve(key);
    eCEntity* body = remote::ControlledBody();
    if (!npc.GetInstance() || !body) return;
    const bCVector& h = body->GetWorldPosition();
    const bCVector& p = npc.GetInstance()->GetWorldPosition();
    const float hero[3] = {h.GetX(), h.GetY(), h.GetZ()}, here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    const auto sent = sendNotes.find(key);
    Logf("npcs: host mirrors %s of player %u: here %.1f m from the host's hero, there %.1f m; last left out here: %s, %d ms ago",
         npc.GetName().GetText(), owner, body::Distance(here, hero) / 100.0f, body::Distance(record.position, hero) / 100.0f,
         sent == sendNotes.end() ? "never seen" : sent->second.reason,
         sent == sendNotes.end() ? -1 : static_cast<int>(net::Now() - sent->second.at));
}

bool Contested(GuidKey const& key, uint8_t owner)
{
    const auto sent = sendNotes.find(key);
    if (sent == sendNotes.end() || std::strcmp(sent->second.reason, "range") || static_cast<int32_t>(net::Now() - sent->second.at) > 1000) return false;
    Entity npc = Resolve(key);
    if (!npc.GetInstance() || npc.IsDead() || !lastPlayerCount) return false;
    const bCVector& p = npc.GetInstance()->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    float nearest = 1e30f;
    for (int k = 0; k < lastPlayerCount; ++k) nearest = std::min(nearest, body::Distance(here, lastPlayers[k]));
    if (nearest > KeepRadius) return false;
    if (contested.insert(key).second && contested.size() < 200)
        Logf("npcs: %s of player %u refused: here %.1f m from the nearest player, in this machine's band; sent from here now",
             npc.GetName().GetText(), owner, nearest / 100.0f);
    return true;
}

void DropMirror(GuidKey const& key)
{
    auto it = mirrors.find(key);
    if (it == mirrors.end()) return;
    if (net::IsHost() && !it->second.killed && it->second.entity.GetInstance()) {
        body::Release(it->second.entity, it->second.body);
        if (it->second.guarded) it->second.entity.NPC.DefeatedByPlayer = it->second.defeatedBefore;
    }
    mirrors.erase(it);
}

void SendRevoke(GuidKey const& key, uint8_t player)
{
    net::WorldState s{};
    s.kind = 6;
    std::memcpy(s.guid, key.data(), 16);
    s.guard = player;
    net::SendWorldState(s, 0);
}

void RefreshCompanionIdentity()
{
    if (!net::IsHost()) return;
    companionIdentity.host = steam::PlayerId();
    auto observe = [](uint64_t account, char const* name) {
        if (!account || !identity::Label(name)) return;
        companionIdentity.candidates.insert(account);
        auto found = companionIdentity.labels.find(account);
        if (found != companionIdentity.labels.end() && found->second != name)
            Logf("identity: name changed steamid=%llu old=%s new=%s", account, found->second.c_str(), name);
        companionIdentity.labels[account] = name;
    };
    observe(steam::PlayerId(), steam::PlayerName());
    for (uint8_t id = 2; id < net::MaxPlayers; ++id)
        if (net::PeerJoined(id)) observe(net::PeerAccount(id), net::PeerName(id));
    for (auto& [key, owner] : companionIdentity.owners) {
        if (owner.account) continue;
        const std::string oldName = owner.legacyName;
        const auto decision = companionIdentity.Migrate(key, GetConfig().legacyCompanionOwners);
        if (decision == identity::Decision::Bound || decision == identity::Decision::Explicit) {
            Logf("identity: migrated companion=%s old=%s steamid=%llu source=%s", identity::Hex(key).c_str(),
                 oldName.c_str(), owner.account, decision == identity::Decision::Explicit ? "host-config" : "unique-Steam-name");
        } else if (identityWarnings.insert(owner.legacyName).second) {
            Logf("identity: migration pending old=%s reason=%s; retained; host config: legacy_companion_owner = <SteamID64> %s",
                 owner.legacyName.c_str(), decision == identity::Decision::Ambiguous ? "ambiguous" : "unknown-or-offline",
                 owner.legacyName.c_str());
            hud::Message(hud::Warning, i18n::Text(i18n::IdentityMigrationPending));
        }
    }
}

void OfferCompanions(uint32_t now)
{
    RefreshCompanionIdentity();
    if (!offering) return;
    for (auto const& [rawKey, owner] : companionIdentity.owners) {
        if (!owner.account || owner.summoned) continue;
        const GuidKey key = FromCompanionKey(rawKey);
        if (claimedBy.count(key)) continue;
        auto last = offered.find(rawKey);
        if (last != offered.end() && static_cast<int32_t>(now - last->second) < static_cast<int32_t>(OfferEvery)) continue;
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            net::Remote const& r = net::Remotes()[id];
            if (!r.connected || !(r.latest.flags & 1) || r.latest.world != remote::CurrentWorld() ||
                owner.account != net::PeerAccount(id)) continue;
            Entity npc = Resolve(key);
            if (!npc.GetInstance()) break;
            if (companionNear) {
                const bCVector& p = npc.GetInstance()->GetWorldPosition();
                const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
                if (body::Distance(at, r.latest.position) > InterestRadius) { ++companionsFar; break; }
            }
            net::WorldState s{};
            s.kind = 7;
            std::memcpy(s.guid, key.data(), 16);
            s.guard = id;
            net::SendWorldState(s, 0);
            claimedBy[key] = id;
            claimSeen[key] = now;
            if (guide::IsQuestGuide(npc.GetInstance()))
                npcs::Guide(key.data(), id);
            else {
                const bool party = npc.GetInstance()->GetPropertySet(eEPropertySetType_Party) != nullptr;
                const auto type = party ? static_cast<gEPartyMemberType>(npc.Party.PartyMemberType) : gEPartyMemberType_Slave;
                npcs::Follow(key.data(), id, static_cast<uint8_t>(type), party && npc.Party.Waiting);
            }
            offered[rawKey] = now;
            ++companionsOffered;
            Logf("npcs: %s is %s's companion (saved steamid=%llu); handed over",
                 Resolve(key).GetName().GetText(), net::PeerName(id), owner.account);
            break;
        }
    }
}

void ClaimTick(uint32_t now)
{
    OfferCompanions(now);
    for (auto it = claimedBy.begin(); it != claimedBy.end();) {
        const uint8_t player = it->second;
        const std::string rawKey = CompanionKey(it->first);
        const auto savedOwner = companionIdentity.owners.find(rawKey);
        const bool sameAccount = savedOwner == companionIdentity.owners.end() ||
            (savedOwner->second.account && savedOwner->second.account == net::PeerAccount(player));
        const bool gone = !sameAccount || !net::Remotes()[player].connected || !net::InWorld(player);
        Entity npc = Resolve(it->first);
        if (!gone) {
            eCEntity* actor = remote::PuppetOf(player);
            const auto joined = claimSeen.find(it->first);
            const bool pending = joined != claimSeen.end() && now - joined->second < 2000;
            if (!npc.GetInstance() || !actor || pending) { ++it; continue; }
            const bool follower = npc.GetInstance()->GetPropertySet(eEPropertySetType_Party) &&
                                  npc.Party.GetPartyLeader().GetInstance() == actor;
            const bool guide = actor->GetPropertySet(eEPropertySetType_Party) &&
                               Entity(actor).Party.GetPartyLeader().GetInstance() == npc.GetInstance();
            if (follower || guide) { ++it; continue; }
            Logf("npcs: %s's canonical relationship with player %u ended", npc.GetName().GetText(), player);
            SendRevoke(it->first, player);
            companionIdentity.owners.erase(rawKey);
            claimSeen.erase(it->first);
            directedBy.erase(it->first);
            it = claimedBy.erase(it);
            if (!talk::IsHeld(npc.GetInstance())) {
                const bCString currentTask = npc.Routine.GetCurrentTask();
                if (!std::strcmp(currentTask.GetText(), "ZS_GuidePlayer") ||
                    !std::strcmp(currentTask.GetText(), "ZS_FollowPlayer")) {
                    npc.Routine.FullStop();
                    npc.Routine.ContinueRoutine();
                }
            }
            continue;
        }
        ++claimsRevoked;
        Logf("npcs: claim of player %u on %s ended (%s)", it->second,
             npc.GetInstance() ? npc.GetName().GetText() : "?", "player left this world");
        if (npc.GetInstance() && npc.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
            npc.Party.SetPartyLeader(Entity());
            npc.NPC.SetCurrentTarget(Entity());
            npc.Routine.FullStop();
            npc.Routine.SetTask(bCString("ZS_HangAround_DoNothing"));
        }
        claimSeen.erase(it->first);
        directedBy.erase(it->first);
        it = claimedBy.erase(it);
    }
}

void FinishClientTalkMirror(Mirror& m, uint32_t now)
{
    if (!m.inClientTalk) return;
    m.inClientTalk = false;
    std::memset(m.replay.instances, 0, sizeof(m.replay.instances));
    if (m.entity.GetInstance() && !m.killed)
        npcplayback::detail::CaptureEntityPoseBlend(m.entity.GetInstance(), m.replay, now ? now : 1u, 0.18f);
}

bool YieldMirrorForClientTalk(Mirror& m, uint8_t flags, uint32_t now)
{
    if (talk_policy::ShouldYieldMirrorToLocalTalk(talk::TalkingWithHero(m.entity.GetInstance()), m.killed, flags)) {
        m.inClientTalk = true;
        m.replayReady = false;
        return true;
    }
    if (m.inClientTalk) FinishClientTalkMirror(m, now);
    return false;
}

} // namespace g3t::npcs::detail

namespace g3t::npcs {
using namespace detail;

void OnClientTalkEnded(uint8_t const guid[16])
{
    if (!guid || net::IsHost()) return;
    const auto it = mirrors.find(Key(guid));
    if (it == mirrors.end()) return;
    FinishClientTalkMirror(it->second, net::Now());
}

void Claim(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    myClaims.insert(key);
    if (auto mirror = mirrors.find(key); mirror != mirrors.end())
        mirror->second.relationAcknowledged = mirror->second.guideAcknowledged = false;
    givenBack.erase(key);
    Logf("npcs: requested player relation for %s; NPC remains host simulated", Entity(entity).GetName().GetText());
}

void Unclaim(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    myClaims.erase(Key(guid));
}

void Foreign(uint8_t const guid[16])
{
    if (net::IsHost()) return;
    Rebind(guid);
}

void Rebind(uint8_t const guid[16])
{
    const auto it = mirrors.find(Key(guid));
    if (it == mirrors.end()) return;
    it->second.resolved = false;
    it->second.missing = false;
}

void Revoked(uint8_t const guid[16])
{
    Entity e = Resolve(guid);
    Logf("npcs: the host took back %s", e.GetInstance() ? e.GetName().GetText() : "?");
    if (e.GetInstance()) {
        Unclaim(e.GetInstance());
        eCEntity* hero = remote::ControlledBody();
        if (hero && hero->GetPropertySet(eEPropertySetType_Party)) {
            Entity player(hero);
            if (player.Party.GetPartyLeader().GetInstance() == e.GetInstance()) player.Party.SetPartyLeader(Entity());
            if (e.GetInstance()->GetPropertySet(eEPropertySetType_Party) && e.Party.GetPartyLeader().GetInstance() == hero)
                e.Party.SetPartyLeader(Entity());
        }
    } else myClaims.erase(Key(guid));
}

bool OnClaim(uint8_t const guid[16], uint8_t player, bool take)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers || !net::PeerJoined(player)) return false;
    const GuidKey key = Key(guid);
    const std::string rawKey = CompanionKey(guid);
    const uint64_t account = net::PeerAccount(player);
    if (!account) { Log("identity: companion claim refused without authenticated SteamID"); return false; }
    RefreshCompanionIdentity();
    auto saved = companionIdentity.owners.find(rawKey);
    if (take && saved != companionIdentity.owners.end() && saved->second.account != account) {
        SendRevoke(key, player);
        Logf("identity: companion claim refused steamid=%llu saved=%llu legacy=%s", account,
             saved->second.account, saved->second.legacyName.c_str());
        return false;
    }
    const auto current = claimedBy.find(key);
    if (take && current != claimedBy.end() && current->second != player && net::Remotes()[current->second].connected) {
        ++claimsDenied;
        SendRevoke(key, player);
        Logf("npcs: player %u cannot take over %s (player %u has it)", player,
             Resolve(guid).GetInstance() ? Resolve(guid).GetName().GetText() : "?", current->second);
        return false;
    }
    if (!take && (current == claimedBy.end() || current->second != player)) return false;
    if (take) {
        claimedBy[key] = player;
        claimSeen[key] = net::Now();
        companionIdentity.owners[rawKey] = {account, {}, saved != companionIdentity.owners.end() && saved->second.summoned};
        DropMirror(key);
    } else {
        claimedBy.erase(key);
        companionIdentity.owners.erase(rawKey);
        claimSeen.erase(key);
        DropMirror(key);
    }
    Logf("npcs: player %u %s %s", player, take ? "took over" : "gave back", Resolve(guid).GetInstance() ? Resolve(guid).GetName().GetText() : "?");
    return true;
}

void PlaceGivenBack(uint8_t const[16], float const[3], float const[4]) {}

void StoryMoved(uint8_t const guid[16], float const position[3], float const rotation[4])
{
    DropMirror(Key(guid));
    Entity e = Resolve(guid);
    eCEntity* instance = e.GetInstance();
    if (!IsNpc(instance) || e.IsDead()) return;
    e.MoveTo(body::Pose(*instance, position, rotation));
    Logf("npcs: the story moved %s on the host; moved here too", e.GetName().GetText());
}

bool HostTake(eCEntity* entity)
{
    if (!entity || !net::IsHost()) return false;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    if (companionWins)
        if (const auto c = claimedBy.find(key); c != claimedBy.end() && net::Remotes()[c->second].connected) return false;
    auto it = mirrors.find(key);
    if (it == mirrors.end()) return false;
    const unsigned owner = it->second.owner;
    hostTaken.insert(key);
    if (!it->second.killed) body::Release(it->second.entity, it->second.body);
    if (it->second.guarded) it->second.entity.NPC.DefeatedByPlayer = it->second.defeatedBefore;
    mirrors.erase(it);
    Logf("npcs: took %s from player %u (it guides or follows the host)", Entity(entity).GetName().GetText(), owner);
    return true;
}

void HostRelease(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    hostTaken.erase(Key(guid));
}

bool HostTaken(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return hostTaken.count(Key(guid)) != 0;
}

uint8_t CompanionOwner(eCEntity* entity)
{
    if (!entity) return 0;
    uint8_t guid[16];
    Guid(entity, guid);
    const GuidKey key = Key(guid);
    if (!net::IsHost()) {
        if (auto mirror = mirrors.find(key); mirror != mirrors.end())
            return mirror->second.latest.partyPlayer ? mirror->second.latest.partyPlayer
                                                     : mirror->second.latest.guidePlayer;
        if (entity->GetPropertySet(eEPropertySetType_Party))
            return PlayerOf(Entity(entity).Party.GetPartyLeader().GetInstance());
        return 0;
    }
    uint8_t activeClaim = 0;
    if (const auto it = claimedBy.find(key); it != claimedBy.end() && net::Remotes()[it->second].connected)
        activeClaim = it->second;
    uint8_t activeGuide = 0;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (id != net::LocalId() && !net::Remotes()[id].connected) continue;
        eCEntity* actor = id == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(id);
        if (actor && actor->GetPropertySet(eEPropertySetType_Party) &&
            Entity(actor).Party.GetPartyLeader().GetInstance() == entity) {
            activeGuide = id;
            break;
        }
    }
    uint8_t partyLeader = 0;
    if (entity->GetPropertySet(eEPropertySetType_Party))
        partyLeader = PlayerOf(Entity(entity).Party.GetPartyLeader().GetInstance());
    uint8_t savedOwner = 0;
    if (const auto saved = companionIdentity.owners.find(CompanionKey(guid)); saved != companionIdentity.owners.end()) {
        if (saved->second.account == steam::PlayerId()) savedOwner = 1;
        else {
            for (uint8_t id = 2; id < net::MaxPlayers; ++id)
                if (saved->second.account && saved->second.account == net::PeerAccount(id)) { savedOwner = id; break; }
        }
    }
    return guide_policy::ResolveCompanionOwner(activeClaim, activeGuide, partyLeader, savedOwner);
}

void SetContext(eCEntity* entity, uint8_t player)
{
    if (!net::IsHost() || !entity || player >= net::MaxPlayers) return;
    uint8_t guid[16];
    Guid(entity, guid);
    if (player) directedBy[Key(guid)] = player;
    else directedBy.erase(Key(guid));
}

uint8_t ContextPlayer(eCEntity* entity)
{
    if (!net::IsHost() || !entity || entity == gCSession::GetInstance().GetHero() ||
        entity == remote::ControlledBody() || remote::IsPuppet(entity) || !entity->GetPropertySet(eEPropertySetType_NPC)) return 0;
    uint8_t guid[16];
    Guid(entity, guid);
    if (auto found = directedBy.find(Key(guid)); found != directedBy.end()) return found->second;
    if (const uint8_t owner = CompanionOwner(entity)) return owner;
    Entity npc(entity);
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    if (const uint8_t player = PlayerOf(target)) return player;
    if (target) if (const uint8_t owner = CompanionOwner(target)) return owner;
    eCEntity* hero = remote::ControlledBody();
    float best = 1e20f;
    if (hero) {
        const bCVector delta = entity->GetWorldPosition() - hero->GetWorldPosition();
        best = std::sqrt(delta.GetX() * delta.GetX() + delta.GetY() * delta.GetY() + delta.GetZ() * delta.GetZ());
    }
    uint8_t player = 1;
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        eCEntity* other = remote::PuppetOf(id);
        if (!other || !net::Remotes()[id].connected || !net::InWorld(id)) continue;
        const bCVector delta = other->GetWorldPosition() - entity->GetWorldPosition();
        const float distance = std::sqrt(delta.GetX() * delta.GetX() + delta.GetY() * delta.GetY() + delta.GetZ() * delta.GetZ());
        if (distance < best) { best = distance; player = id; }
    }
    return player;
}

eCEntity* ResolveQuestGuideDestination(Entity const& npc, char const* questName)
{
    gCQuestManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCQuestManager_PS::GetInstance() : nullptr;
    if (!manager || !npc.GetInstance()) return nullptr;
    auto resolveFromQuest = [&](gCQuest_PS* quest) -> eCEntity* {
        if (!quest || quest->GetType().GetValue() != gEQuestType_FollowNpc) return nullptr;
        bCString const& destName = quest->GetDestinationEntity();
        eCEntity* dest = destName.GetLength() > 0 ? Entity::GetEntity(destName).GetInstance() : nullptr;
        return guide_policy::SelectGuideDestination(destName.GetLength() > 0, dest != nullptr) ==
                       guide_policy::GuideDestinationSource::QuestDestinationEntity
                   ? dest
                   : nullptr;
    };
    if (questName && *questName) {
        if (gCQuest_PS* quest = manager->GetQuest(bCString(questName))) {
            if (quest->GetType().GetValue() == gEQuestType_FollowNpc &&
                quest->GetStatus().GetValue() == gEQuestStatus_Open)
                manager->RunQuest(bCString(questName));
            if (quest->GetStatus().GetValue() == gEQuestStatus_Running)
                return resolveFromQuest(quest);
        }
    }
    const bCString npcName = npc.GetName();
    auto& quests = manager->AccessQuests();
    for (GEInt qi = 0; qi < quests.GetCount(); ++qi) {
        auto* quest = static_cast<gCQuest_PS*>(quests.AccessAt(qi));
        if (!quest || quest->GetType().GetValue() != gEQuestType_FollowNpc ||
            quest->GetStatus().GetValue() != gEQuestStatus_Running)
            continue;
        auto const& delivery = quest->GetDeliveryEntities();
        for (GEInt i = 0; i < delivery.GetCount(); ++i)
            if (delivery.GetAt(i) == npcName) return resolveFromQuest(quest);
    }
    return nullptr;
}

bool Guide(uint8_t const guid[16], uint8_t player, char const* questName)
{
    if (!net::IsHost() || !player || player >= net::MaxPlayers) return false;
    Entity npc(NpcByGuid(guid));
    eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
    if (!npc.GetInstance() || !actor) { Log("npcs: guide request refused: missing NPC or player"); return false; }
    SetContext(npc.GetInstance(), player);
    eCEntity* destination = ResolveQuestGuideDestination(npc, questName);
    {
        guards::Acting context(player == net::LocalId() ? nullptr : actor);
        GetScriptAdminExt().CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), destination, 0);
    }
    if (actor->GetPropertySet(eEPropertySetType_Party))
        Entity(actor).Party.SetPartyLeader(npc);
    if (npc.GetInstance()->GetPropertySet(eEPropertySetType_Navigation)) {
        npc.Navigation.IsOnDestination = GEFalse;
        if (destination) {
            Entity destEntity(destination);
            npc.Navigation.SetCurrentDestinationPoint(destEntity);
        }
    }
    if (guide_policy::ShouldImmediatelyRestartRoutineOnApply(false, talk::IsHeld(npc.GetInstance()))) {
        npc.Routine.FullStop();
        npc.Routine.ContinueRoutine();
    }
    Logf("npcs: %s guides player %u toward %s (held_in_talk=%d)", npc.GetName().GetText(), player,
         destination ? Entity(destination).GetName().GetText() : "(quest-routine)",
         talk::IsHeld(npc.GetInstance()) ? 1 : 0);
    return true;
}

bool Follow(uint8_t const guid[16], uint8_t player, uint8_t memberType, bool waiting)
{
    Entity npc(NpcByGuid(guid));
    eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
    if (!net::IsHost() || !npc.GetInstance() || !actor || !npc.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
        Logf("npcs: follower relation for player %u could not be applied", player);
        return false;
    }
    guards::Acting context(actor);
    npc.Party.SetPartyLeader(Entity(actor));
    npc.Party.PartyMemberType = static_cast<gEPartyMemberType>(memberType);
    npc.Party.Waiting = waiting ? GETrue : GEFalse;
    npc.NPC.SetCurrentTarget(Entity(actor));
    npc.Routine.FullStop();
    if (waiting) npc.Routine.ContinueRoutine();
    else npc.Routine.SetTask(bCString("ZS_FollowPlayer"));
    return true;
}

bool Claimed(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return myClaims.count(Key(guid)) != 0;
}

uint8_t ClaimedBy(eCEntity const* entity)
{
    if (!entity || !net::IsHost()) return 0;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    const auto it = claimedBy.find(Key(guid));
    return it == claimedBy.end() ? 0 : it->second;
}

void Adopt(uint8_t const guid[16])
{
    Entity e = Resolve(guid);
    if (!IsNpc(e.GetInstance())) return;
    Claim(e.GetInstance());
    if (guide::Readopt(e.GetInstance())) { ++companionsAdopted; return; }
    if (arena::Readopt(e.GetInstance())) return;
    e.Routine.FullStop();
    e.Dialog.SlaveryEnabled = GETrue;
    GetScriptAdminExt().CallScript(bCString("JoinPlayerSlavery"), e.GetInstance(), nullptr, 0);
    e.Routine.SetTask(bCString("ZS_FollowPlayer"));
    ++companionsAdopted;
    Logf("npcs: %s is our companion again (from the host's save)", e.GetName().GetText());
}

unsigned WriteCompanions(FILE* file)
{
    RefreshCompanionIdentity();
    const std::string text = companionIdentity.Write();
    if (fwrite(text.data(), 1, text.size(), file) != text.size()) Log("identity: companion journal write failed");
    return static_cast<unsigned>(companionIdentity.owners.size());
}

bool ReadCompanions(FILE* file)
{
    identity::Companions restored;
    offered.clear();
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (restored.Read(line) == identity::Row::Invalid) {
            Log("identity: invalid/duplicate companion journal row; load refused");
            return false;
        }
    }
    if (ferror(file)) { Log("identity: companion journal read failed"); return false; }
    companionIdentity = std::move(restored);
    identityWarnings.clear();
    return true;
}

void ClearCompanions()
{
    companionIdentity = {};
    identityWarnings.clear();
    offered.clear();
}

void SyncDefeatedByPlayer(uint8_t const guid[16], bool defeated)
{
    if (!guid) return;
    const auto it = detail::mirrors.find(detail::Key(guid));
    if (it == detail::mirrors.end()) return;
    it->second.defeatedBefore = defeated;
    if (!net::IsHost() && it->second.entity.GetInstance() &&
        it->second.entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC))
        it->second.entity.NPC.DefeatedByPlayer = defeated ? GETrue : GEFalse;
}

void CompanionAccounts(std::vector<uint64_t> const& accounts)
{
    companionIdentity.candidates.insert(accounts.begin(), accounts.end());
    RefreshCompanionIdentity();
}

void RememberSummon(uint8_t const guid[16], uint8_t player)
{
    if (!net::IsHost()) return;
    const uint64_t account = player == 1 ? steam::PlayerId() : net::PeerAccount(player);
    if (!account) { Log("identity: summon owner unresolved; retained without reassignment"); return; }
    const std::string rawKey = CompanionKey(guid);
    auto existing = companionIdentity.owners.find(rawKey);
    if (existing == companionIdentity.owners.end()) companionIdentity.owners[rawKey] = {account, {}, true};
}

namespace detail {

bool AdmitIncomingNpc(net::NpcRecord const& record, GuidKey const& key, uint32_t captured,
                      uint8_t owner, uint32_t now)
{
    if (npcremoval::Suppressed(record.guid)) return false;
    if (npclease::IgnoreHostSnapshot(record.guid, captured)) return false;
    const auto existing = mirrors.find(key);
    if (existing != mirrors.end() &&
        static_cast<int32_t>(record.incarnation - existing->second.latest.incarnation) < 0) return false;
    const auto absent = absences.find(key);
    if (absent != absences.end() &&
        static_cast<int32_t>(record.incarnation - absent->second.incarnation) <= 0) return false;
    if (absent != absences.end()) {
        absences.erase(absent);
        world::RequestProcessingUpdate();
    }
    if (!net::IsHost() && (record.flags & 1) != 0 && npcpresence::IsVoidDespawnPosition(record.position)) {
        LeaseReleaseMirror(record.guid);
        absences[key] = record;
        pendingForces.erase(key);
        if (eCEntity* live = Resolve(record.guid).GetInstance(); IsNpc(live)) {
            npclease::RelinquishToHost(live, "host despawned NPC");
            const bCMatrix pose = body::Pose(*live, record.position, record.rotation);
            live->SetToWorldMatrix(pose);
            if (auto* rigid = static_cast<eCRigidBody_PS*>(live->GetPropertySet(eEPropertySetType_RigidBody)))
                rigid->SetPose(pose);
            ClearLocalReferencesToDeadMirror(live, nullptr, false, true);
        }
        absenceCheckAt = 0;
        return false;
    }
    if (!(record.flags & 1)) journal::NewLife(record.guid);
    if (net::IsHost() && existing != mirrors.end()) {
        Mirror const& current = existing->second;
        const bool ownerConnected = current.owner > 1 && current.owner < net::MaxPlayers &&
                                    net::PeerJoined(current.owner) && net::InWorld(current.owner) &&
                                    current.ownerGeneration == net::PeerGeneration(current.owner);
        if (current.owner != owner && !current.released && ownerConnected) {
            ++claimsRefused;
            return false;
        }
    }
    if (net::IsHost() && hostTaken.count(key)) { ++claimsRefused; return false; }
    if (net::IsHost() && reclaimed.count(key)) { ++reclaimRefused; return false; }
    if (deadStays && net::IsHost() && owner > 1 && !(record.flags & 1)) {
        Entity here = Resolve(record.guid);
        if (here.GetInstance() && here.IsDead()) { ++deadRefused; return false; }
    }
    if (owned.count(key) && !claimedBy.count(key)) {
        if (net::IsHost() || !hostWinsRule) { ++claimsRefused; return false; }
        owned.erase(key);
        ++hostWins;
    }
    const auto claim = claimedBy.find(key);
    if (claim != claimedBy.end() && claim->second != owner) { ++claimsRefused; return false; }
    if (net::IsHost() && owner > 1 && owner < net::MaxPlayers && claim == claimedBy.end() && joinGraceRule) {
        if (!joinedAt[owner]) joinedAt[owner] = now ? now : 1;
        if (static_cast<int32_t>(now - joinedAt[owner]) < static_cast<int32_t>(JoinGrace) && !mirrors.count(key)) {
            eCEntity* body = remote::ControlledBody();
            const bCVector& h = body ? body->GetWorldPosition() : bCVector();
            const float heroAt[3] = {h.GetX(), h.GetY(), h.GetZ()};
            if (body && body::Distance(record.position, heroAt) <= InterestRadius) { ++joinRefused; return false; }
        }
    }
    if (claim != claimedBy.end()) claimSeen[key] = now;
    givenBack.erase(key);
    if (net::IsHost() && owner > 1 && claim == claimedBy.end() && !mirrors.count(key)) {
        if (contestRule && Contested(key, owner)) { ++contestedRefused; return false; }
        NoteTakeover(key, record, owner);
    }
    return true;
}

void ResetMirrorForNewStream(Mirror& m, GuidKey const& key, net::NpcRecord const& record, bool sameOwner)
{
    if (!sameOwner && m.latest.incarnation != record.incarnation) {
        m.deathLife = replication::TerminalLife{};
        m.ragdollStarted = m.ragdollInitialized = m.corpsePlaced = m.corpseSimulating = false;
        m.ragdollPaused = m.corpseSettled = m.haveLastGroundPos = false;
        m.lastGroundIncarnation = 0;
        m.corpsePose.clear();
        m.corpseActor = nullptr;
        m.corpsePoseAt = m.ragdollTryAt = m.lieCheckAt = 0;
        m.ragdollStarts = m.ragdollWakes = 0;
        m.ragdollFails = m.lieTries = m.lieNoted = 0;
        m.laid = m.killed = m.deathLogged = m.localHitValid = m.focusWatchLogged = false;
        m.deathReferencesCleared = nullptr;
        m.deathClearCount = m.deathCombatStopCount = 0;
        m.deathImpulse = {};
        m.ragdollTraceAt = m.ragdollTraceSampleAt = m.ragdollTraceStage = 0;
        m.localHitAt = m.fallbackGraceUntil = m.focusWatchUntil = 0;
        std::strcpy(m.presentationSource, "gravity");
        std::strcpy(m.killerAttribution, "host_or_npc_unknown");
        if (auto forces = pendingForces.find(key); forces != pendingForces.end()) {
            auto& queue = forces->second;
            queue.erase(std::remove_if(queue.begin(), queue.end(), [&](net::RagdollNote const& force) {
                return static_cast<int32_t>(force.incarnation - record.incarnation) < 0;
            }), queue.end());
            if (queue.empty()) pendingForces.erase(forces);
        }
        forceSequences.erase(ForceSeqKey{key, m.latest.incarnation});
        ForgetCorpseGroundAnchor(key);
    }
    m.replay = npcplayback::State{};
    ResetMirrorIncomingContact(m);
    m.body.track = body::Track{};
    m.body.presentationSerial = 0;
    m.body.actionSeq = -1;
    m.latestTicks = 0;
    m.havePresentation = false;
}

void PackMotionFromRecord(body::MotionState& motion, net::NpcRecord const& r)
{
    motion.motion = r.motion;
    motion.heldRight = r.heldRight;
    motion.heldLeft = r.heldLeft;
    motion.aniState = r.aniState ? r.aniState : -1;
    motion.actionSeq = static_cast<int>(r.motions[0].instance);
    motion.motionFrac = r.motions[0].active ? static_cast<int>(r.motions[0].phase * 65535.0f) : 65535;
    motion.flags = r.flags;
    motion.hitPoints = r.hitPoints;
    motion.hitPointsMax = r.hitPointsMax;
    motion.incarnation = r.incarnation;
    motion.hitPlayer = r.hitPlayer;
    motion.hitSequence = r.hitSequence;
    motion.partyPlayer = r.partyPlayer;
    motion.partyType = r.partyType;
    motion.waiting = r.waiting;
    motion.guidePlayer = r.guidePlayer;
    motion.aiMode = r.aiMode;
    motion.action = r.action;
    motion.attackReason = r.attackReason;
    motion.damageType = r.damageType;
    motion.damageAmount = r.damageAmount;
    motion.damageMultiplier = r.damageMultiplier;
    motion.contact = r.contact;
    motion.attitude = r.attitude;
    char const* firstName = nullptr;
    if (r.motions[0].active) {
        if (auto const it = motionNames.find(r.motions[0].name); it != motionNames.end())
            firstName = it->second.c_str();
    }
    motion.nonAttackCombat = r.motions[0].active && !combat_timing::IncomingAttackMotion(firstName, r.contact);
    std::memcpy(motion.motions, r.motions, sizeof(motion.motions));
}

void CopyMotionToRecord(net::NpcRecord& shown, body::Presentation const& presentation)
{
    shown.flags = presentation.motion.flags;
    shown.hitPoints = presentation.motion.hitPoints;
    shown.hitPointsMax = presentation.motion.hitPointsMax;
    if (presentation.movementMode >= 0) shown.movementMode = static_cast<uint8_t>(presentation.movementMode);
    shown.aniState = presentation.motion.aniState < 0 ? 0 : static_cast<uint8_t>(presentation.motion.aniState);
    shown.heldRight = presentation.motion.heldRight;
    shown.heldLeft = presentation.motion.heldLeft;
    shown.partyPlayer = presentation.motion.partyPlayer;
    shown.partyType = presentation.motion.partyType;
    shown.waiting = presentation.motion.waiting;
    shown.guidePlayer = presentation.motion.guidePlayer;
    shown.aiMode = presentation.motion.aiMode;
    shown.action = presentation.motion.action;
    shown.attackReason = presentation.motion.attackReason;
    shown.damageType = presentation.motion.damageType;
    shown.damageAmount = presentation.motion.damageAmount;
    shown.damageMultiplier = presentation.motion.damageMultiplier;
    shown.incarnation = presentation.motion.incarnation;
    shown.hitPlayer = presentation.motion.hitPlayer;
    shown.hitSequence = presentation.motion.hitSequence;
    shown.contact = presentation.motion.contact;
    shown.attitude = presentation.motion.attitude;
    std::memcpy(shown.motions, presentation.motion.motions, sizeof(shown.motions));
}

void SyncMirrorPartyAndGuide(GuidKey const& key, Mirror& m, net::NpcRecord const& shown)
{
    if (m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
        const bool waiting = m.entity.Party.Waiting != GEFalse;
        const uint8_t actualOwner = PlayerOf(m.entity.Party.GetPartyLeader().GetInstance());
        if (m.partyApplied && m.appliedOwner != net::LocalId() && actualOwner == net::LocalId() && !myClaims.count(key)) {
            Claim(m.entity.GetInstance());
            net::WorldState request{};
            request.kind = 4;
            request.npcClass = 1;
            std::memcpy(request.guid, shown.guid, 16);
            net::SendWorldState(request, 0);
        } else if (m.partyApplied && m.appliedOwner == net::LocalId() && actualOwner != net::LocalId() &&
                   myClaims.count(key) && m.relationAcknowledged) {
            Unclaim(m.entity.GetInstance());
            net::WorldState request{};
            request.kind = 5;
            std::memcpy(request.guid, shown.guid, 16);
            net::SendWorldState(request, 0);
        }
        if (shown.partyPlayer == net::LocalId()) m.relationAcknowledged = true;
        if (m.partyApplied && m.appliedOwner == net::LocalId() && waiting != m.appliedWaiting) {
            net::WorldState request{};
            request.kind = 22;
            std::memcpy(request.guid, shown.guid, 16);
            request.immortal = waiting ? 1 : 0;
            net::SendWorldState(request, 0);
        }
        const auto decodedWaiting = guide_policy::DecodeWaitingField(
            shown.partyPlayer, shown.guidePlayer, shown.waiting, m.guideDestinationApplied);
        eCEntity* leader = shown.partyPlayer == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(shown.partyPlayer);
        const bool pending = myClaims.count(key) && !m.relationAcknowledged && actualOwner == net::LocalId();
        if (!pending) {
            if (m.entity.Party.GetPartyLeader().GetInstance() != leader) m.entity.Party.SetPartyLeader(Entity(leader));
            m.entity.Party.PartyMemberType = static_cast<gEPartyMemberType>(shown.partyType);
            m.entity.Party.Waiting = decodedWaiting.partyWaiting ? GETrue : GEFalse;
        }
        m.partyApplied = true;
        m.appliedOwner = shown.partyPlayer;
        m.appliedWaiting = decodedWaiting.partyWaiting;
    }
    const auto decodedGuide = guide_policy::DecodeWaitingField(
        shown.partyPlayer, shown.guidePlayer, shown.waiting, m.guideDestinationApplied);
    if (decodedGuide.writeGuideDestination && m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Navigation)) {
        m.entity.Navigation.IsOnDestination = decodedGuide.guideIsOnDestination ? GETrue : GEFalse;
        m.guideDestinationApplied = shown.partyPlayer == 0 && shown.guidePlayer != 0;
    }
    eCEntity* hero = remote::ControlledBody();
    if (hero && hero->GetPropertySet(eEPropertySetType_Party)) {
        Entity player(hero);
        if (shown.guidePlayer == net::LocalId()) {
            m.guideAcknowledged = true;
            player.Party.SetPartyLeader(m.entity);
        } else if (m.guideAcknowledged &&
                   player.Party.GetPartyLeader().GetInstance() == m.entity.GetInstance()) {
            m.guideAcknowledged = false;
            player.Party.SetPartyLeader(Entity());
        }
    }
}

} // namespace detail

} // namespace g3t::npcs
