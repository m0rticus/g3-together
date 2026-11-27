// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy decisions for binding remote puppets into NPC AI state scopes.
#pragma once

#include <cstddef>
#include <cstring>

namespace g3t::guards_policy {

enum class StateActorSource { None, ContextLeader, WaitNearest, Target, Attacker };

inline bool IsTalkState(char const* state) noexcept
{
    return state && std::strncmp(state, "ZS_Talk", 7) == 0;
}

inline bool IsWaitDestinationState(char const* state) noexcept
{
    return state && std::strcmp(state, "ZS_WaitForFreeDestination_Loop") == 0;
}

inline bool IsFollowsPlayerState(char const* state) noexcept
{
    if (!state) return false;
    static char const* const names[] = {
        "ZS_ReactToZone", "ZS_GotoPlayerZone", "ZS_ObserveZonePlayer", "ZS_ObserveSuspect",
        "ZS_ObserveWeapon", "ZS_ReactToWeapon", "ZS_Threaten", "ZS_ReactToSneak",
        "ZS_ReactToDamage"
    };
    for (char const* name : names) {
        if (std::strncmp(state, name, std::strlen(name)) == 0) return true;
    }
    return false;
}

inline bool IsAttackState(char const* state) noexcept
{
    return state && std::strncmp(state, "ZS_Attack", 9) == 0 &&
           std::strcmp(state, "ZS_Attack_End") != 0 &&
           std::strcmp(state, "ZS_Attack_Arena_End") != 0;
}

// Decides which remote puppet (if any) stands in as Entity::GetPlayer() during an NPC AI state.
// Gothic 3 never clears CurrentAttacker when a fight or knockdown ends, so a stale puppet in
// CurrentAttacker must never hijack a state targeting the local player or any talk/observe state.
inline StateActorSource SelectGuardStateActor(
    bool installed, bool hasNpcProps, bool selfIsLocalPlayer, char const* state,
    bool hasContextLeader, bool hasTarget, bool targetIsPuppet, bool targetIsLocalPlayer,
    bool attackerIsPuppet) noexcept
{
    if (!installed || !hasNpcProps || selfIsLocalPlayer || !state) return StateActorSource::None;
    if (IsTalkState(state)) {
        if (targetIsPuppet) return StateActorSource::Target;
        if (targetIsLocalPlayer) return StateActorSource::None;
        return (!hasTarget && hasContextLeader) ? StateActorSource::ContextLeader : StateActorSource::None;
    }
    const bool followsPlayer = IsFollowsPlayerState(state);
    if ((followsPlayer || IsAttackState(state)) && targetIsLocalPlayer) return StateActorSource::None;
    if (hasContextLeader) return StateActorSource::ContextLeader;
    if (IsWaitDestinationState(state)) return StateActorSource::WaitNearest;
    if (!followsPlayer) return StateActorSource::None;
    if (targetIsPuppet) return StateActorSource::Target;
    if (hasTarget) return StateActorSource::None;
    if (std::strncmp(state, "ZS_ObserveSuspect", 17) == 0) return StateActorSource::None;
    return attackerIsPuppet ? StateActorSource::Attacker : StateActorSource::None;
}

// Same guard for fightmem::StateActor (ZS_Attack* and Stumble/KnockDown/StandUp reactions):
// prefer CurrentTarget, and never let a stale puppet in CurrentAttacker hijack a state whose
// CurrentTarget is the local player.
inline StateActorSource SelectFightStateActor(
    bool enabled, bool selfIsLocalPlayer, bool isAttackState, bool isStumbleReactionState,
    bool hasTarget, bool targetIsPuppet, bool targetIsLocalPlayer, bool attackerIsPuppet) noexcept
{
    if (!enabled || selfIsLocalPlayer || (!isAttackState && !isStumbleReactionState))
        return StateActorSource::None;
    if (targetIsLocalPlayer) return StateActorSource::None;
    if (targetIsPuppet) return StateActorSource::Target;
    if (isAttackState || hasTarget) return StateActorSource::None;
    return attackerIsPuppet ? StateActorSource::Attacker : StateActorSource::None;
}

} // namespace g3t::guards_policy
