// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef G3T_NPCLEASE_NATIVE_RUNTIME
#include "guid.h"
#include "npclease.h"
#include "npcs.h"
#include "world.h"

#include <Game.h>
#include <Script.h>
#include <cstring>
#endif

namespace g3t::npclease {
// Label only (diagnostics, creature-vs-human statistics). It no longer gates ownership: every class that
// npcown::Exclusion accepts may live on the client (see npcown_policy.h).
enum class NativeClass { Unsupported, Creature, Ordinary };

#ifdef G3T_NPCLEASE_NATIVE_RUNTIME
inline NativeClass SpeciesClass(gESpecies species)
{
    switch (species) {
    case gESpecies_Golem: case gESpecies_Demon: case gESpecies_Troll: case gESpecies_Minecrawler:
    case gESpecies_Scavenger: case gESpecies_Wolf: case gESpecies_Boar: case gESpecies_Sabertooth:
    case gESpecies_Shadowbeast: case gESpecies_Bison: case gESpecies_Rhino: case gESpecies_Ripper:
    case gESpecies_Lurker: case gESpecies_Varan: case gESpecies_Snapper: case gESpecies_Alligator:
    case gESpecies_Trex: case gESpecies_FireVaran: case gESpecies_Bloodfly: case gESpecies_Lizard:
    case gESpecies_Meatbug: case gESpecies_Rabbit: case gESpecies_Deer: case gESpecies_Chicken:
    case gESpecies_Molerat: case gESpecies_Snake: case gESpecies_Pig: case gESpecies_Cow:
    case gESpecies_Spider: case gESpecies_Turtle: case gESpecies_Gargoyle: case gESpecies_SwampLurker:
    case gESpecies_Ogre: case gESpecies_Scorpion: case gESpecies_FireGolem: case gESpecies_IceGolem:
    case gESpecies_Stalker:
        return NativeClass::Creature;
    default:
        return NativeClass::Ordinary;
    }
}

inline NativeClass CheckpointClass(Checkpoint const& state)
{
    _GUID raw;
    std::memcpy(&raw, state.templateId, sizeof(raw));
    eCTemplateEntityProxy proxy;
    proxy.SetEntity(guid::Property(raw));
    auto* prototype = proxy.GetEntity();
    if (!prototype || !prototype->GetPropertySet(eEPropertySetType_NPC)) return NativeClass::Unsupported;
    return SpeciesClass(static_cast<gESpecies>(Entity(prototype).NPC.Species));
}

inline void CaptureNativeFields(eCEntity* entity, Entity& npc, Checkpoint& state)
{
    const auto& id = entity->GetTemplateID().GetCore();
    std::memcpy(state.templateId, &id, 16);
    const auto& p = entity->GetWorldPosition();
    state.position[0] = p.GetX(); state.position[1] = p.GetY(); state.position[2] = p.GetZ();
    const auto q = entity->GetWorldRotation();
    state.rotation[0] = q.GetX(); state.rotation[1] = q.GetY(); state.rotation[2] = q.GetZ(); state.rotation[3] = q.GetW();
    state.hp = npc.DamageReceiver.HitPoints; state.maxHp = npc.DamageReceiver.HitPointsMax;
    state.mana = npc.DamageReceiver.ManaPoints; state.maxMana = npc.DamageReceiver.ManaPointsMax;
    state.stamina = npc.DamageReceiver.StaminaPoints; state.maxStamina = npc.DamageReceiver.StaminaPointsMax;
    state.level = npc.NPC.Level;
    state.dead = npc.IsDead() ? 1 : 0;
    state.defeated = npc.NPC.DefeatedByPlayer ? 1 : 0;
    state.attitude = static_cast<uint8_t>(static_cast<gEAttitude>(npc.NPC.AttitudeToPlayer2));
    state.aiMode = static_cast<uint8_t>(static_cast<gEAIMode>(npc.Routine.AIMode));
    state.action = static_cast<uint8_t>(static_cast<gEAction>(npc.Routine.Action));
    state.attackReason = static_cast<uint8_t>(static_cast<gEAttackReason>(npc.NPC.AttackReason));
    auto* routine = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine));
    strncpy_s(state.name, npc.GetName().GetText(), _TRUNCATE);
    strncpy_s(state.routine, static_cast<bCString const&>(routine->GetRoutine()).GetText(), _TRUNCATE);
    npcs::LeaseBaseline(entity, state.routine, sizeof(state.routine), state.defeated);
    auto* properties = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
    state.status = properties->GetStatusEffects();
    const uint32_t seconds = Entity::GetTimeStampInSeconds();
    const uint32_t stamps[] = {properties->GetTimeStampFrozen(), properties->GetTimeStampBurning(),
        properties->GetTimeStampPoisoned(), properties->GetTimeStampDiseased(), properties->GetTimeStampTransformed()};
    for (size_t i = 0; i < 5; ++i) state.statusAge[i] = seconds - stamps[i];
}

inline void ImportNativeFields(eCEntity* entity, Checkpoint const& state, bool wasDead)
{
    Entity npc(entity);
    npc.DamageReceiver.HitPointsMax = state.maxHp; npc.DamageReceiver.HitPoints = state.hp;
    npc.DamageReceiver.ManaPointsMax = state.maxMana; npc.DamageReceiver.ManaPoints = state.mana;
    npc.DamageReceiver.StaminaPointsMax = state.maxStamina; npc.DamageReceiver.StaminaPoints = state.stamina;
    npc.NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(state.attitude);
    npc.NPC.Level = state.level;
    npc.NPC.AttackReason = static_cast<gEAttackReason>(state.attackReason);
    npc.NPC.DefeatedByPlayer = state.defeated != 0;
    npc.NPC.CombatState = 0;
    npc.Routine.AIMode = state.dead ? gEAIMode_Dead : gEAIMode_Abandoned;
    npc.Routine.Action = static_cast<gEAction>(state.action);
    auto* properties = static_cast<gCNPC_PS*>(entity->GetPropertySet(eEPropertySetType_NPC));
    properties->AccessStatusEffects() = state.status;
    const uint32_t seconds = Entity::GetTimeStampInSeconds();
    properties->AccessTimeStampFrozen() = seconds - state.statusAge[0];
    properties->AccessTimeStampBurning() = seconds - state.statusAge[1];
    properties->AccessTimeStampPoisoned() = seconds - state.statusAge[2];
    properties->AccessTimeStampDiseased() = seconds - state.statusAge[3];
    properties->AccessTimeStampTransformed() = seconds - state.statusAge[4];
    if (state.dead && !wasDead) npcs::LayDownCorpse(entity);
    world::RequestProcessingUpdate();
}
#endif
}
