// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_targets.h"
#include "downed.h"
#include "downed_policy.h"
#include "npc_retarget_policy.h"
#include "guards.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"
#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace g3t::npc_targets {
namespace {
bool HostNpc(eCEntity* entity)
{
    return entity && (net::IsHost() || npclease::LocalAuthority(entity)) &&
        entity->GetPropertySet(eEPropertySetType_NPC) && !guards::IsLocalPlayer(entity) &&
        !remote::IsPuppet(entity) && (!npcs::IsMirrored(entity) || npclease::LocalAuthority(entity)) &&
        (net::IsHost() || !talk::IsHeld(entity));
}
using NextEnemy = Entity (GE_STDCALL *)(Entity);
static_assert(sizeof(Entity) == 0xA8, "Native next-enemy ABI requires an SDK Entity by value");

NextEnemy NativeNextEnemy()
{
    static bool checked = false;
    static NextEnemy nextEnemy = nullptr;
    if (checked) return nextEnemy;
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    checked = true;
    auto* base = reinterpret_cast<unsigned char*>(module);
    static const unsigned char head[] = {0x81, 0xec, 0xe0, 0x08, 0x00, 0x00, 0x53, 0x55, 0x56,
                                         0x8b, 0xb4, 0x24, 0xf0, 0x08, 0x00, 0x00, 0x57};
    static const unsigned char tail[] = {0xc2, 0xac, 0x00};
    // Script_Game+0xA070: hidden Entity return + by-value Entity, RET 0xAC.
    if (!std::memcmp(base + 0xA070, head, sizeof(head)) && !std::memcmp(base + 0xAF1B, tail, sizeof(tail))) {
        nextEnemy = reinterpret_cast<NextEnemy>(base + 0xA070);
        Log("ai: native defeated-opponent selector verified");
    } else {
        Log("ai: native defeated-opponent selector UNAVAILABLE; signature/ABI mismatch, using visible fallback only");
    }
    return nextEnemy;
}

bool EligibleOpponent(eCEntity* npc, Entity const& target)
{
    eCEntity* instance = target.GetInstance();
    const bool properties = instance && instance->GetPropertySet(eEPropertySetType_NPC) &&
                            instance->GetPropertySet(eEPropertySetType_ScriptRoutine);
    return retarget::EligibleTarget(properties, instance == npc, downed::CombatUnavailable(instance),
                                    properties ? static_cast<int>(target.Routine.AIMode) : -1) &&
           npclease::ContactAllowed(npc, instance);
}

bool ReplacementEnemy(eCEntity* npc, Entity const& target, bool visibleFallback)
{
    if (!EligibleOpponent(npc, target)) return false;
    Entity self(npc);
    if (visibleFallback && (!retarget::VisibleFallback(self.GetDistanceTo(target)) ||
                            !self.IsFreeLOS(target, GETrue))) return false;
    const int attitude = GetScriptAdminExt().CallScript(bCString("GetAttitude"), npc, target.GetInstance(), 0);
    return retarget::EnemyAttitude(attitude);
}

Entity SelectReplacement(Entity const& npc, Entity const& current, Entity const& alternative,
                         uint32_t bindings, retarget::Source& source)
{
    if (!(bindings & downed::Current) && EligibleOpponent(npc.GetInstance(), current)) {
        source = retarget::Source::Current;
        return current;
    }
    Entity native;
    if (NextEnemy select = NativeNextEnemy()) native = select(npc);
    const bool nativeValid = ReplacementEnemy(npc.GetInstance(), native, false);
    const bool alternativeValid = !nativeValid && ReplacementEnemy(npc.GetInstance(), alternative, true);
    std::array<Entity, net::MaxPlayers> actors;
    std::array<retarget::PlayerCandidate, net::MaxPlayers> players{};
    if (!nativeValid && !alternativeValid) {
        for (unsigned id = 1; id < net::MaxPlayers; ++id) {
            eCEntity* actor = id == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(id);
            if (!actor) continue;
            actors[id] = Entity(actor);
            players[id].eligible = ReplacementEnemy(npc.GetInstance(), actors[id], true);
            players[id].distanceCm = npc.GetDistanceTo(actors[id]);
        }
    }
    const retarget::Selection selected = retarget::Select(bindings, false, nativeValid, alternativeValid,
                                                         players.data(), players.size());
    source = selected.source;
    switch (source) {
    case retarget::Source::Native: return native;
    case retarget::Source::Alternative: return alternative;
    case retarget::Source::Player: return actors[selected.player];
    default: return Entity();
    }
}

thread_local eCEntity* releasingDownedNpc = nullptr;

struct DownedReleaseScope {
    eCEntity* before;
    explicit DownedReleaseScope(eCEntity* npc) : before(releasingDownedNpc) { releasingDownedNpc = npc; }
    ~DownedReleaseScope() { releasingDownedNpc = before; }
};

} // namespace

bool ReleaseDownedTargets(eCEntity* instance, char const* boundary, gCScriptProcessingUnit* spu)
{
    if (instance && instance == releasingDownedNpc) return true;
    if (!HostNpc(instance) || !downed::UnavailablePlayers() ||
        !instance->GetPropertySet(eEPropertySetType_ScriptRoutine)) return false;
    Entity npc(instance);
    if (npc.IsDead() || npc.IsDown()) return false;
    const bool hasMovement = instance->GetPropertySet(eEPropertySetType_CharacterMovement) != nullptr;
    const Entity current = npc.NPC.GetCurrentTarget(), alternative = npc.NPC.GetAlternativeTarget(),
                 attacker = npc.NPC.GetCurrentAttacker(), last = npc.NPC.GetLastAttacker(),
                 spell = npc.NPC.GetSpellTarget(),
                 alignment = hasMovement ? npc.GetAlignmentTarget() : Entity();
    const Entity inflictor = instance->GetPropertySet(eEPropertySetType_DamageReceiver)
        ? npc.DamageReceiver.GetLastInflictor() : Entity();
    uint32_t bindings = 0;
    eCEntity* down = nullptr;
    auto check = [&](Entity const& target, downed::TargetBinding binding) {
        if (downed::CombatUnavailable(target.GetInstance())) {
            bindings |= binding;
            if (!down) down = target.GetInstance();
        }
    };
    check(current, downed::Current);
    check(alternative, downed::Alternative);
    check(attacker, downed::Attacker);
    check(last, downed::LastAttacker);
    check(spell, downed::Spell);
    check(alignment, downed::Alignment);
    check(inflictor, downed::Inflictor);
    if (!bindings) return false;
    DownedReleaseScope scope(instance);
    downed::LogCombatDecision(instance, down, boundary, true);
    const bool stop = downed::StopTargetInstruction(bindings);
    const bCString taskBefore = npc.Routine.GetCurrentTask(), stateBefore = npc.Routine.GetCurrentState();
    const int modeBefore = npc.Routine.AIMode, weaponBefore = gCEntity::GetCombatMode(instance);
    const unsigned combatBefore = npc.NPC.CombatState;
    const bool fighting = modeBefore == gEAIMode_Combat || combatBefore == 1 ||
        !std::strncmp(taskBefore.GetText(), "ZS_Attack", 9) ||
        !std::strncmp(taskBefore.GetText(), "ZS_RevolutionAttack", 19);
    // A combat instruction owns saved target arguments. Retire it before changing its target proxies.
    if (stop) {
        npc.Routine.FullStop();
        npc.Routine.StopAICombatMove();
        npc.Routine.StopAIGoto();
        if (hasMovement) npc.CharacterMovement.StopMovement();
    }
    Entity none;
    if (bindings & downed::Current) npc.NPC.SetCurrentTarget(none);
    if (bindings & downed::Alternative) npc.NPC.SetAlternativeTarget(none);
    if (bindings & downed::Attacker) npc.NPC.SetCurrentAttacker(none);
    if (bindings & downed::LastAttacker) npc.NPC.SetLastAttacker(none);
    if (bindings & downed::Spell) npc.NPC.SetSpellTarget(none);
    if (bindings & downed::Alignment) { if (hasMovement) npc.SetAlignmentTarget(none); }
    if (bindings & downed::Inflictor) npc.DamageReceiver.SetLastInflictor(none);
    if (stop) {
        if (!spu) spu = &static_cast<gCScriptRoutine_PS*>(instance->GetPropertySet(eEPropertySetType_ScriptRoutine))->GetSPU();
        if (downed::CombatUnavailable(spu->GetOtherEntity())) spu->SetOtherEntity(nullptr);
        retarget::Source source = retarget::Source::None;
        Entity next = fighting ? SelectReplacement(npc, current, alternative, bindings, source) : Entity();
        char const* reason = retarget::Reason(source);
        if (next.GetInstance()) {
            if (hasMovement) npc.SetAlignmentTarget(next);
            if (source == retarget::Source::Current) {
                npc.Routine.SetTask(bCString("ZS_Attack"));
            } else {
                // ZS_Attack_End+0x1A811 uses this same setter/assessment path before any weapon stow.
                const int attitude = GetScriptAdminExt().CallScript(bCString("GetAttitude"), instance, next.GetInstance(), 0);
                const gEAttackReason attackReason = npc.NPC.AttackReason == gEAttackReason_Revolution
                    ? gEAttackReason_Revolution : attitude == gEAttitude_Hostile
                    ? gEAttackReason_Enemy : attitude == gEAttitude_Predator ? gEAttackReason_Prey : gEAttackReason_ReactToDamage;
                const int assessed = GetScriptAdminExt().CallScript(bCString("AssessTarget"), instance, next.GetInstance(), attackReason);
                if (!assessed || npc.NPC.GetCurrentTarget().GetInstance() != next.GetInstance()) {
                    Logf("ai: downed retarget assessment failed npc=%s target=%s result=%d",
                         npc.GetName().GetText(), next.GetName().GetText(), assessed);
                    npc.NPC.SetCurrentTarget(none);
                    if (hasMovement) npc.SetAlignmentTarget(none);
                    next = none;
                    reason = "assessment-failed";
                }
            }
        }
        if (!next.GetInstance()) {
            if (fighting) {
                // Empty target skips finishing/looting a revivable player; native End owns stow/wait/relax.
                if (std::strcmp(npc.Routine.GetCurrentTask().GetText(), "ZS_Attack"))
                    npc.Routine.SetTask(bCString("ZS_Attack"));
                npc.Routine.SetState(bCString("ZS_Attack_End"));
            } else {
                npc.NPC.CombatState = 0;
                npc.Routine.ContinueRoutine();
            }
        }
        Logf("DOWNEDRETARGET npc=%.64s old=%.48s new=%.48s boundary=%s reason=%s task=%.40s->%.40s state=%.40s->%.40s ai=%d->%d combat=%u->%u weapon=%d->%d",
             npc.GetName().GetText(), down ? Entity(down).GetName().GetText() : "-",
             next.GetInstance() ? next.GetName().GetText() : "-", boundary, reason,
             taskBefore.GetText(), npc.Routine.GetCurrentTask().GetText(),
             stateBefore.GetText(), npc.Routine.GetCurrentState().GetText(), modeBefore, static_cast<int>(npc.Routine.AIMode),
             combatBefore, static_cast<unsigned>(npc.NPC.CombatState), weaponBefore, static_cast<int>(gCEntity::GetCombatMode(instance)));
    }
    downed::LogCombatDecision(instance, down, "released", true);
    return stop;
}

bool RetargetDownedBeforeCancel(eCEntity* npc, gCScriptProcessingUnit* spu)
{
    return ReleaseDownedTargets(npc, "cancel-release", spu);
}

ScrubStats ScrubEntityReferences(eCEntity* target, bool removeInstructions, bool cancelCombat)
{
    ScrubStats stats;
    if (!target || !gCSession::GetInstance().IsGameRunning()) return stats;
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* instance = all.GetAt(i).GetInstance();
        if (!instance || instance == target || !instance->GetPropertySet(eEPropertySetType_NPC)) continue;
        Entity npc(instance), none;
        const bool current = npc.NPC.GetCurrentTarget().GetInstance() == target;
        const bool alternative = npc.NPC.GetAlternativeTarget().GetInstance() == target;
        const bool attacker = npc.NPC.GetCurrentAttacker().GetInstance() == target;
        const bool last = npc.NPC.GetLastAttacker().GetInstance() == target;
        const bool spell = npc.NPC.GetSpellTarget().GetInstance() == target;
        const bool movement = instance->GetPropertySet(eEPropertySetType_CharacterMovement) != nullptr;
        const bool alignment = movement && npc.GetAlignmentTarget().GetInstance() == target;
        auto* routine = static_cast<gCScriptRoutine_PS*>(instance->GetPropertySet(eEPropertySetType_ScriptRoutine));
        auto* spu = routine ? &routine->GetSPU() : nullptr;
        const bool other = spu && spu->GetOtherEntity() == target;
        const bool authority = net::IsHost() || npclease::LocalAuthority(instance);
        const bool passive = !authority && npcs::IsMirrored(instance);
        auto* properties = static_cast<gCNPC_PS*>(instance->GetPropertySet(eEPropertySetType_NPC));
        const bool stop = routine && authority && !guards::IsLocalPlayer(instance) && !remote::IsPuppet(instance) &&
            !npc.IsDead() && !npc.IsDown() && (current || spell || other);
        if (removeInstructions && stop && routine) {
            npc.Routine.FullStop();
            npc.Routine.StopAICombatMove();
            npc.Routine.StopAIGoto();
            if (movement) npc.CharacterMovement.StopMovement();
            ++stats.instructionsRemoved;
        }
        using NpcScript = std::remove_reference_t<decltype(npc.NPC)>;
        auto clearSlot = [&](bool matched, eCEntityProxy& (gCNPC_PS::*access)(),
                             void (NpcScript::*set)(Entity const&), unsigned& counter) {
            if (!matched) return;
            if (passive) (properties->*access)().SetEntity(static_cast<eCEntity*>(nullptr));
            else (npc.NPC.*set)(none);
            ++counter;
        };
        clearSlot(current, &gCNPC_PS::AccessCurrentTargetEntity, &NpcScript::SetCurrentTarget, stats.targetCleared);
        clearSlot(alternative, &gCNPC_PS::AccessAlternativeTargetEntity, &NpcScript::SetAlternativeTarget, stats.alternativeCleared);
        clearSlot(attacker, &gCNPC_PS::AccessCurrentAttackerEntity, &NpcScript::SetCurrentAttacker, stats.attackerCleared);
        clearSlot(last, &gCNPC_PS::AccessLastAttackerEntity, &NpcScript::SetLastAttacker, stats.lastAttackerCleared);
        clearSlot(spell, &gCNPC_PS::AccessSpellTarget, &NpcScript::SetSpellTarget, stats.spellTargetCleared);
        if (alignment) npc.SetAlignmentTarget(none);
        if (movement && (current || spell || alignment)) npc.SetLookAtTarget(none);
        if (other) spu->SetOtherEntity(nullptr);
        if (instance->GetPropertySet(eEPropertySetType_DamageReceiver) &&
            npc.DamageReceiver.GetLastInflictor().GetInstance() == target)
            npc.DamageReceiver.SetLastInflictor(none);
        auto* focus = static_cast<gCFocus_PS*>(instance->GetPropertySet(eEPropertySetType_Focus));
        if (focus && npc.Focus.GetFocusEntity().GetInstance() == target) {
            focus->SetFocusLocked(GEFalse);
            focus->SetFocusEntity(nullptr);
        }
        if (cancelCombat && stop && routine && (current || !npc.NPC.GetCurrentTarget().GetInstance())) {
            GetScriptAdminExt().CallScript(bCString("CancelAttack"), instance, nullptr, 0);
            ++stats.combatCancelled;
        }
    }
    return stats;
}

void ScrubDownedProxies(eCEntity* instance)
{
    auto* properties = static_cast<gCNPC_PS*>(instance->GetPropertySet(eEPropertySetType_NPC));
    if (!properties) return;
    auto clear = [](eCEntityProxy& proxy) {
        if (downed::CombatUnavailable(proxy.GetEntity())) proxy.SetEntity(static_cast<eCEntity*>(nullptr));
    };
    clear(properties->AccessCurrentTargetEntity());
    clear(properties->AccessAlternativeTargetEntity());
    clear(properties->AccessCurrentAttackerEntity());
    clear(properties->AccessLastAttackerEntity());
    clear(properties->AccessSpellTarget());
    Entity npc(instance), none;
    if (instance->GetPropertySet(eEPropertySetType_CharacterMovement) &&
        downed::CombatUnavailable(npc.GetAlignmentTarget().GetInstance()))
        npc.SetAlignmentTarget(none);
    if (instance->GetPropertySet(eEPropertySetType_DamageReceiver) &&
        downed::CombatUnavailable(npc.DamageReceiver.GetLastInflictor().GetInstance()))
        npc.DamageReceiver.SetLastInflictor(none);
    if (auto* routine = static_cast<gCScriptRoutine_PS*>(instance->GetPropertySet(eEPropertySetType_ScriptRoutine))) {
        auto& spu = routine->GetSPU();
        if (downed::CombatUnavailable(spu.GetOtherEntity())) spu.SetOtherEntity(nullptr);
    }
}

void SyncDownedTargets()
{
    if (!gCSession::GetInstance().IsGameRunning() || !downed::UnavailablePlayers()) return;
    const auto all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* instance = all.GetAt(i).GetInstance();
        if (!instance) continue;
        if (!net::IsHost() && !npclease::LocalAuthority(instance)) {
            if (npcs::IsMirrored(instance)) ScrubDownedProxies(instance);
            continue;
        }
        if (!ReleaseDownedTargets(instance, "frame-release", nullptr) &&
            !net::IsHost() && npclease::LocalAuthority(instance) && talk::IsHeld(instance)) {
            ScrubDownedProxies(instance);
        }
    }
}
} // namespace g3t::npc_targets
