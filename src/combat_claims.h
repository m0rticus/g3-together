// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "combat.h"
#include "net.h"
#include <cstdint>

class eCEntity;
class Entity;

namespace g3t::combat {
// Native damage callbacks can recursively enter another hit dispatch.
class HitDispatchScope {
public:
    HitDispatchScope(bool& forwarded, bool& heroForward, bool forwardHero = false)
        : forwarded_(forwarded), heroForward_(heroForward), previousForwarded_(forwarded),
          previousHeroForward_(heroForward)
    {
        forwarded_ = true;
        heroForward_ = forwardHero;
    }
    ~HitDispatchScope()
    {
        forwarded_ = previousForwarded_;
        heroForward_ = previousHeroForward_;
    }
    HitDispatchScope(HitDispatchScope const&) = delete;
    HitDispatchScope& operator=(HitDispatchScope const&) = delete;
private:
    bool& forwarded_;
    bool& heroForward_;
    bool previousForwarded_;
    bool previousHeroForward_;
};

namespace killconfirm {
constexpr uint32_t UndoDelayMs = 2000;
constexpr uint32_t DeathTaskExpiryMs = 10000;
constexpr uint32_t PendingRewardExpiryMs = 5000;
constexpr uint32_t PlayerAssistWindowMs = 30000;

struct PlayerDamageStamp {
    uint32_t incarnation = 0;
    uint32_t at = 0;
};

inline bool RecentPlayerAssist(PlayerDamageStamp const* stamp, uint32_t currentIncarnation, uint32_t now)
{
    return stamp && stamp->incarnation == currentIncarnation &&
           static_cast<uint32_t>(now - stamp->at) <= PlayerAssistWindowMs;
}

inline bool ShouldPaySharedDefeat(bool alreadyDefeated, bool pendingClaim, bool killerIsCoopSide,
                                  PlayerDamageStamp const* stamp, uint32_t currentIncarnation, uint32_t now)
{
    if (alreadyDefeated || pendingClaim) return false;
    return killerIsCoopSide || RecentPlayerAssist(stamp, currentIncarnation, now);
}

enum class Action : uint8_t {
    Wait = 0,
    Drop = 1,
    Reward = 2,
    UndoAndDrop = 3,
};

// Check confirmed death on every tick so kill XP is paid as soon as ZS_RagDollDead/ZS_Dead
// commits on the next frame, while keeping the 2s/10s undo window when OnCheatKill did not take.
inline Action Decide(uint32_t now, uint32_t undoDue, uint32_t expires, bool sameIncarnation,
                     bool present, bool dead, bool deathTask, uint8_t rewardPlayer, bool rewardEligible)
{
    if (!sameIncarnation) return Action::Drop;
    if (present && dead) return rewardEligible ? Action::Reward : Action::Drop;
    if (static_cast<int32_t>(now - undoDue) < 0) return Action::Wait;
    if (present && rewardPlayer != 0 && deathTask && static_cast<int32_t>(now - expires) < 0) {
        return Action::Wait;
    }
    if (present && !deathTask) return Action::UndoAndDrop;
    return Action::Drop;
}
}
}

namespace g3t::combat::detail {
enum HostAppliedOriginKind { HostAppliedOriginNone, HostAppliedOriginClaim, HostAppliedOriginNpc };
extern thread_local HostAppliedOriginKind hostAppliedOriginKind;
extern thread_local uint8_t hostAppliedOriginPlayer;
extern thread_local uint32_t hostAppliedOriginSequence;
class HostAppliedOriginScope {
public:
    HostAppliedOriginScope(HostAppliedOriginKind kind, uint8_t player, uint32_t sequence);
    ~HostAppliedOriginScope();
    HostAppliedOriginScope(HostAppliedOriginScope const&) = delete;
    HostAppliedOriginScope& operator=(HostAppliedOriginScope const&) = delete;
private:
    HostAppliedOriginKind previousKind;
    uint8_t previousPlayer;
    uint32_t previousSequence;
};

uint8_t Outcome(Entity& victim);
uint8_t Reaction(Entity& victim);
void OnNpcHit(uint8_t from, Entity& target, int32_t delta, uint8_t const npc[16], bool party);
void QueueHitNpc(uint8_t from, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16],
                 uint8_t const npc[16], uint8_t outcome, uint32_t seen, uint8_t reaction,
                 uint32_t sequence, uint32_t incarnation, net::MeleeContact const& contact);
void OnKilled(uint8_t const guid[16], uint32_t incarnation);
void OnHitPlayer(int32_t delta, uint8_t const npc[16], uint8_t attackerPlayer, bool meansToKill);
void DrainQueuedNpcHits();
void ConfirmKills();
void ClearClaims();
void NotePlayerCombatDamage(eCEntity* victim, eCEntity* attacker);
}
