// SPDX-License-Identifier: GPL-3.0-or-later
#include "combat_internal.h"
#include "combat_hostility.h"
#include "guards.h"
#include "guid.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "remote.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace g3t::combat {
namespace detail {
namespace {
struct Unconfirmed {
    uint8_t guid[16];
    DWORD undoDue, expires;
    uint8_t rewardPlayer;
    bool clearDefeatedOnUndo;
    uint32_t generation, incarnation;
};
struct PendingKillReward {
    uint8_t guid[16];
    uint32_t incarnation;
    DWORD expires;
};
std::unordered_set<std::string> rewarded; // client: victims this player was rewarded for
std::vector<Unconfirmed> unconfirmed;
std::vector<PendingKillReward> pendingKillRewards;
std::unordered_map<std::string, killconfirm::PlayerDamageStamp> playerDamaged;

bool IsCoopPlayerSide(eCEntity* entity)
{
    return combat_hostility::IsCoopParticipant(entity);
}

bool OwnsNpcDefeatXp(eCEntity* victim)
{
    if (!victim || !victim->GetPropertySet(eEPropertySetType_NPC) || HasPlayerMemory(victim) ||
        remote::IsPuppet(victim) || victim == gCSession::GetInstance().GetHero() ||
        victim == remote::ControlledBody()) {
        return false;
    }
    if (net::IsHost()) return IsHostSimulatedNpc(victim);
    return net::Connected() && npclease::LocalAuthority(victim);
}

bool PendingClaimFor(std::string const& key)
{
    if (killedHere.count(key) != 0) return true;
    for (auto const& u : unconfirmed) {
        if (KillKey(u.guid) == key) return true;
    }
    return false;
}

void AwardLocalDefeatXp(eCEntity* victim, char const* reason)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !victim || !gCSession::GetInstance().IsGameRunning()) return;
    guards::LocalPlayer localPlayer;
    Entity self(hero);
    const int before = static_cast<int>(self.PlayerMemory.XP);
    const GEInt result = CallScript("GiveXP", victim, hero, -1);
    ++xpAwarded;
    Logf("combat: rewarded co-op %s of %s: GiveXP=%d xp %d -> %d",
         reason, Entity(victim).GetName().GetText(), result, before, static_cast<int>(self.PlayerMemory.XP));
}

void ConfirmSharedKnockouts(DWORD now)
{
    if (!gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero()) return;
    for (auto it = playerDamaged.begin(); it != playerDamaged.end();) {
        uint8_t const* guid = reinterpret_cast<uint8_t const*>(it->first.data());
        const uint32_t currentIncarnation = npcs::Incarnation(guid);
        if (!killconfirm::RecentPlayerAssist(&it->second, currentIncarnation, now)) {
            it = playerDamaged.erase(it);
            continue;
        }
        eCEntity* rawVictim = npcs::NpcByGuid(guid);
        if (!rawVictim) {
            ++it;
            continue;
        }
        Entity victim(rawVictim);
        if (victim.NPC.DefeatedByPlayer || victim.IsDead()) {
            it = playerDamaged.erase(it);
            continue;
        }
        if (OwnsNpcDefeatXp(rawVictim) && victim.IsDown() && Outcome(victim) == net::HitKnockOut &&
            killconfirm::ShouldPaySharedDefeat(false, PendingClaimFor(it->first),
                                               IsCoopPlayerSide(victim.NPC.GetCurrentAttacker().GetInstance()),
                                               &it->second, currentIncarnation, now)) {
            victim.NPC.DefeatedByPlayer = GETrue;
            it = playerDamaged.erase(it);
            AwardLocalDefeatXp(rawVictim, "knockout");
            continue;
        }
        ++it;
    }
}

bool TryApplyKillReward(uint8_t const guid[16], uint32_t incarnation)
{
    if (npcremoval::Suppressed(guid)) return true;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    _GUID raw;
    std::memcpy(&raw, guid, 16);
    Entity victim = guid::Resolve(raw);
    if (!hero || !victim.GetInstance() || !gCSession::GetInstance().IsGameRunning()) return false;
    std::string identity(reinterpret_cast<char const*>(guid), 16);
    identity.append(reinterpret_cast<char const*>(&incarnation), sizeof(incarnation));
    if (!rewarded.insert(identity).second) {
        ++rewardsRepeated;
        Logf("combat: the kill of %s was rewarded already; not again", victim.GetName().GetText());
        return true;
    }
    guards::LocalPlayer localPlayer;
    Entity self(hero);
    const int before = static_cast<int>(self.PlayerMemory.XP);
    const GEInt result = CallScript("GiveXP", victim.GetInstance(), hero, -1); // like the death script: self = the NPC, other = who earns it
    ++xpAwarded;
    Logf("combat: rewarded the kill of %s: GiveXP=%d xp %d -> %d", victim.GetName().GetText(), result, before,
         static_cast<int>(self.PlayerMemory.XP));
    return true;
}

void DrainPendingKillRewards(DWORD now)
{
    for (auto it = pendingKillRewards.begin(); it != pendingKillRewards.end();) {
        if (TryApplyKillReward(it->guid, it->incarnation) ||
            static_cast<int32_t>(now - it->expires) >= 0) {
            it = pendingKillRewards.erase(it);
        } else {
            ++it;
        }
    }
}
} // namespace

void NotePlayerCombatDamage(eCEntity* victim, eCEntity* attacker)
{
    if (!OwnsNpcDefeatXp(victim) || !attacker || attacker == victim || !IsCoopPlayerSide(attacker)) return;
    uint8_t guid[16];
    Guid(victim, guid);
    playerDamaged[KillKey(guid)] = {npcs::Incarnation(guid), GetTickCount()};
}

void RewardKill(uint8_t player, uint8_t const guid[16])
{
    const uint32_t incarnation = npcs::Incarnation(guid);
    if (player == net::LocalId()) {
        OnKilled(guid, incarnation);
    } else {
        net::SendKilled(player, guid, incarnation);
        net::Flush();
    }
}

void Expect(uint8_t const guid[16], uint8_t rewardPlayer, bool clearDefeatedOnUndo)
{
    const DWORD now = GetTickCount();
    Unconfirmed u{{}, now + killconfirm::UndoDelayMs, now + killconfirm::DeathTaskExpiryMs,
                  rewardPlayer, clearDefeatedOnUndo,
                  rewardPlayer ? net::PeerGeneration(rewardPlayer) : 0, npcs::Incarnation(guid)};
    std::memcpy(u.guid, guid, 16);
    unconfirmed.push_back(u);
}

void ConfirmKills()
{
    const DWORD now = GetTickCount();
    DrainPendingKillRewards(now);
    ConfirmSharedKnockouts(now);
    for (auto it = unconfirmed.begin(); it != unconfirmed.end();) {
        Entity victim(npcs::NpcByGuid(it->guid));
        const bool present = victim.GetInstance() != nullptr;
        const bool dead = present && victim.IsDead();
        const bool deathTask = present && !dead && Outcome(victim) == net::HitDeath;
        const bool sameIncarnation = npcs::Incarnation(it->guid) == it->incarnation;
        const bool rewardEligible = it->rewardPlayer != 0 &&
            (it->rewardPlayer == net::LocalId() ||
             (net::PeerJoined(it->rewardPlayer) && net::PeerGeneration(it->rewardPlayer) == it->generation));
        const killconfirm::Action action = killconfirm::Decide(
            now, it->undoDue, it->expires, sameIncarnation, present, dead, deathTask,
            it->rewardPlayer, rewardEligible);
        if (action == killconfirm::Action::Wait) {
            ++it;
            continue;
        }
        if (action == killconfirm::Action::Reward) {
            RewardKill(it->rewardPlayer, it->guid);
        } else if (action == killconfirm::Action::UndoAndDrop) {
            if (it->clearDefeatedOnUndo) victim.NPC.DefeatedByPlayer = GEFalse;
            killedHere.erase(KillKey(it->guid));
            ++killsUndone;
            Logf("combat: the kill of %s did not take (task %s, %d hit points); its next hit counts again",
                 victim.GetName().GetText(), victim.Routine.GetCurrentTask().GetText(), HitPoints(victim));
        }
        it = unconfirmed.erase(it);
    }
}

// Client: this player's hit killed a host NPC: the game's own reward (GiveXP computes it from the NPC's level).
void OnKilled(uint8_t const guid[16], uint32_t incarnation)
{
    if (TryApplyKillReward(guid, incarnation)) return;
    if (pendingKillRewards.size() >= 64) return;
    PendingKillReward pending{};
    std::memcpy(pending.guid, guid, 16);
    pending.incarnation = incarnation;
    pending.expires = GetTickCount() + killconfirm::PendingRewardExpiryMs;
    pendingKillRewards.push_back(pending);
}

void ClearKillXp()
{
    pendingKillRewards.clear();
    unconfirmed.clear();
    rewarded.clear();
    playerDamaged.clear();
}
} // namespace detail

void ResolveKillXp(eCEntity* victim, eCEntity* killer, bool wasDefeated)
{
    using namespace detail;
    if (!victim || !victim->GetPropertySet(eEPropertySetType_NPC)) return;
    uint8_t guid[16];
    Guid(victim, guid);
    const std::string key = KillKey(guid);
    const auto found = playerDamaged.find(key);
    const killconfirm::PlayerDamageStamp stamp = found != playerDamaged.end() ? found->second : killconfirm::PlayerDamageStamp{};
    const bool hasStamp = found != playerDamaged.end();
    if (hasStamp) playerDamaged.erase(found);
    if (!OwnsNpcDefeatXp(victim)) return;
    Entity victimEntity(victim);
    const bool alreadyDefeated = wasDefeated || victimEntity.NPC.DefeatedByPlayer != GEFalse;
    if (!killconfirm::ShouldPaySharedDefeat(alreadyDefeated, PendingClaimFor(key), IsCoopPlayerSide(killer),
                                            hasStamp ? &stamp : nullptr, npcs::Incarnation(guid), GetTickCount())) {
        return;
    }
    victimEntity.NPC.DefeatedByPlayer = GETrue;
    AwardLocalDefeatXp(victim, "shared kill");
}
} // namespace g3t::combat
