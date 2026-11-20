// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcremoval.h"
#include "npcremoval_policy.h"

#include "config.h"
#include "crashlog.h"
#include "guid.h"
#include "log.h"
#include "net.h"
#include "npc_targets.h"
#include "npcplayback.h"
#include "npcs.h"
#include "remote.h"
#include "telemetry.h"

#include <Game.h>
#include <Script.h>

namespace g3t::npcremoval {
namespace {
struct Removal {
    policy::Fence fence;
    eCEntity const* applied = nullptr;
    bool noted = false;
    bool needsScrub = false;
    policy::Publication publications[net::MaxPlayers];
};
Removal removals[policy::Count];

bool MainWorld() { return remote::CurrentWorld() == net::HashWorld(policy::MainWorld); }

bool Player(eCEntity const* entity)
{
    return entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody() ||
        remote::IsPuppet(entity);
}

void Position(eCEntity const* entity, float out[3])
{
    const auto& at = entity->GetWorldPosition();
    out[0] = at.GetX(); out[1] = at.GetY(); out[2] = at.GetZ();
}

void ClearReferences(eCEntity* instance)
{
    if (!instance || Suppressed(instance) || !instance->GetPropertySet(eEPropertySetType_NPC)) return;
    Entity npc(instance), none;
    const bool current = Suppressed(npc.NPC.GetCurrentTarget().GetInstance());
    const bool alternative = Suppressed(npc.NPC.GetAlternativeTarget().GetInstance());
    const bool attacker = Suppressed(npc.NPC.GetCurrentAttacker().GetInstance());
    const bool last = Suppressed(npc.NPC.GetLastAttacker().GetInstance());
    const bool spell = Suppressed(npc.NPC.GetSpellTarget().GetInstance());
    const bool movement = instance->GetPropertySet(eEPropertySetType_CharacterMovement) != nullptr;
    const bool alignment = movement && Suppressed(npc.GetAlignmentTarget().GetInstance());
    auto* routine = static_cast<gCScriptRoutine_PS*>(instance->GetPropertySet(eEPropertySetType_ScriptRoutine));
    auto* spu = routine ? &routine->GetSPU() : nullptr;
    const bool other = spu && Suppressed(spu->GetOtherEntity());
    const bool stop = policy::StopTargetInstruction(Player(instance), npc.IsDead() || npc.IsDown(), current, spell, other);
    // Instructions retain their own target arguments: retire them before clearing the proxies.
    if (stop && routine) {
        npc.Routine.FullStop();
        npc.Routine.StopAICombatMove();
        if (movement) npc.CharacterMovement.StopMovement();
    }
    if (current) npc.NPC.SetCurrentTarget(none);
    if (alternative) npc.NPC.SetAlternativeTarget(none);
    if (attacker) npc.NPC.SetCurrentAttacker(none);
    if (last) npc.NPC.SetLastAttacker(none);
    if (spell) npc.NPC.SetSpellTarget(none);
    if (alignment) npc.SetAlignmentTarget(none);
    if (current || spell || alignment) npc.SetLookAtTarget(none);
    if (other) spu->SetOtherEntity(nullptr);
    if (instance->GetPropertySet(eEPropertySetType_DamageReceiver) &&
        Suppressed(npc.DamageReceiver.GetLastInflictor().GetInstance()))
        npc.DamageReceiver.SetLastInflictor(none);
    auto* focus = static_cast<gCFocus_PS*>(instance->GetPropertySet(eEPropertySetType_Focus));
    if (focus && Suppressed(npc.Focus.GetFocusEntity().GetInstance())) {
        focus->SetFocusLocked(GEFalse);
        focus->SetFocusEntity(nullptr);
    }
    if (stop && routine && net::IsHost()) {
        if (current || !npc.NPC.GetCurrentTarget().GetInstance()) {
            npc.NPC.CombatState = 0;
            npc.Routine.AIMode = gEAIMode_Reaction;
            npc.Routine.ContinueRoutine();
        } else {
            npc.Routine.SetTask(bCString("ZS_Attack"));
        }
    }
}

void DisableTree(eCEntity* entity)
{
    entity->DisableProcessing(GETrue);
    entity->EnableRendering(GEFalse);
    entity->EnablePicking(GEFalse, GEFalse);
    entity->EnableCollision(GEFalse);
    entity->Enable(GEFalse);
    for (GEInt i = 0; i < entity->GetChildrenCount(); ++i)
        if (auto* child = entity->GetChildAt(i)) DisableTree(child);
}

bool TreeActive(eCEntity const* entity)
{
    if (entity->IsEnabled() || entity->IsRenderingEnabled() || !entity->IsProcessingDisabled() || entity->IsPickable())
        return true;
    for (GEInt i = 0; i < entity->GetChildrenCount(); ++i)
        if (auto const* child = entity->GetChildAt(i); child && TreeActive(child)) return true;
    return false;
}

void Apply(size_t index)
{
    auto& removal = removals[index];
    Entity npc = guid::Resolve(policy::Entries[index].guid.data());
    auto* instance = npc.GetInstance();
    if (!instance || Player(instance) || !instance->GetPropertySet(eEPropertySetType_NPC) ||
        npc.GetName() != policy::Entries[index].name) return;
    if (removal.applied != instance || TreeActive(instance)) {
        if (instance->GetPropertySet(eEPropertySetType_ScriptRoutine)) {
            if (net::IsHost()) {
                npc.Routine.FullStop();
                npc.Routine.StopAICombatMove();
            } else {
                npcplayback::Silence(instance);
            }
        }
        if (instance->GetPropertySet(eEPropertySetType_CharacterMovement))
            npc.CharacterMovement.StopMovement();
        DisableTree(instance);
        removal.applied = instance;
        removal.needsScrub = true;
    }
    npcs::ReleaseRemovedAim(instance);
    if (!removal.noted) {
        float at[3]; Position(instance, at);
        char id[40]; guid::Format(policy::Entries[index].guid.data(), id, sizeof(id));
        const auto* creator = instance->GetCreator();
        Logf("npcremoval: name=%s template=%s guid=%s at=%.1f,%.1f,%.1f reason=ardea-authored-cluster role=%s",
             npc.GetName().GetText(), creator ? creator->GetName().GetText() : "(unavailable)", id,
             at[0], at[1], at[2], net::IsHost() ? "host" : "client");
        telemetry::OnNpcRemoval("suppressed", npc.GetName().GetText(), policy::Entries[index].guid.data(), instance, "ardea_authored_cluster");
        removal.noted = true;
    }
}

void OnRemoval(net::NpcRemovalNote const& note)
{
    const int index = policy::Index(note.guid);
    if (net::IsHost() || !MainWorld() || note.world != remote::CurrentWorld() || index < 0 ||
        !removals[index].fence.Accept(note.epoch, net::WorldEpoch(), note.incarnation, npcs::Incarnation(note.guid))) {
        Log("npcremoval: stale or out-of-scope host removal refused");
        telemetry::OnNpcRemoval("refused_stale", "-", note.guid, nullptr, "stale_or_out_of_scope");
        return;
    }
    npcs::ForgetRemoved(note.guid);
    npcplayback::ForgetRemoved(note.guid);
    Apply(static_cast<size_t>(index));
    ClearReferences(gCSession::GetInstance().GetHero());
    if (remote::ControlledBody() != gCSession::GetInstance().GetHero())
        ClearReferences(remote::ControlledBody());
}
}

bool Suppressed(uint8_t const bytes[16])
{
    const int index = policy::Index(bytes);
    return index >= 0 && removals[index].fence.Removed();
}

bool SuppressedUnloaded(uint8_t const bytes[16], char const* entityName, float const position[3])
{
    return Suppressed(bytes) || (entityName && position &&
        policy::Matches(GetConfig().ardeaRemoveNpcs, MainWorld(), bytes, entityName, position, false));
}

bool Suppressed(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t bytes[16]; guid::CopyEntity(const_cast<eCEntity*>(entity), bytes);
    return Suppressed(bytes);
}

bool ExcludeFromProcessing(eCEntity const* entity)
{
    if (!net::IsHost() || !entity || !entity->GetPropertySet(eEPropertySetType_NPC)) return false;
    uint8_t bytes[16]; guid::CopyEntity(const_cast<eCEntity*>(entity), bytes);
    float at[3]; Position(entity, at);
    return Suppressed(bytes) || policy::Matches(GetConfig().ardeaRemoveNpcs, MainWorld(), bytes,
                           entity->GetName().GetText(), at, Player(entity));
}

void Start() { net::SetNpcRemovalHandler(&OnRemoval); }

void OnWorldDeactivate()
{
    for (auto& removal : removals) removal = Removal{};
}

void CaptureCacheInventory()
{
    unsigned count = 0;
    eCEntity const* first = nullptr;
    for (auto const& removal : removals)
        if (removal.applied) {
            ++count;
            if (!first) first = removal.applied;
        }
    crashlog::CacheObjects(crashlog::CacheKind::Disabled, count, first);
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning() || !MainWorld()) return;
    bool any = false;
    for (size_t i = 0; i != policy::Count; ++i) {
        auto& removal = removals[i];
        if (net::IsHost() && !removal.fence.Removed()) {
            Entity npc = guid::Resolve(policy::Entries[i].guid.data());
            if (ExcludeFromProcessing(npc.GetInstance())) {
                removal.fence.incarnation = npcs::Incarnation(policy::Entries[i].guid.data());
                npcs::ForgetRemoved(policy::Entries[i].guid.data());
                npcplayback::ForgetRemoved(policy::Entries[i].guid.data());
            }
        }
        if (!removal.fence.Removed()) continue;
        any = true;
        Apply(i);
        if (!net::IsHost() || !net::WorldEpoch()) continue;
        for (uint8_t peer = 2; peer < net::MaxPlayers; ++peer) {
            if (!net::PeerJoined(peer) || !net::InWorld(peer)) {
                removal.publications[peer] = policy::Publication{};
                continue;
            }
            const uint32_t generation = net::PeerGeneration(peer), epoch = net::WorldEpoch();
            const uint32_t entry = net::JoinCount();
            if (!removal.publications[peer].Needs(epoch, generation, entry)) continue;
            net::NpcRemovalNote note{};
            std::memcpy(note.guid, policy::Entries[i].guid.data(), 16);
            note.epoch = epoch; note.world = remote::CurrentWorld(); note.incarnation = removal.fence.incarnation;
            if (net::SendNpcRemovalTo(peer, note)) {
                removal.publications[peer].Confirm(epoch, generation, entry);
            }
        }
    }
    if (!any) return;
    const auto all = Entity::GetNPCs();
    for (GEInt i = 0; i < all.GetCount(); ++i) ClearReferences(all.GetAt(i).GetInstance());
    ClearReferences(gCSession::GetInstance().GetHero());
    if (remote::ControlledBody() != gCSession::GetInstance().GetHero())
        ClearReferences(remote::ControlledBody());
    for (auto& removal : removals) {
        if (!removal.needsScrub || !removal.applied) continue;
        npc_targets::ScrubEntityReferences(const_cast<eCEntity*>(removal.applied), true, false);
        removal.needsScrub = false;
    }
}
}
