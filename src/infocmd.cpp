// SPDX-License-Identifier: GPL-3.0-or-later
#include "infocmd_internal.h"

#include "address.h"
#include "arena.h"
#include "arena_policy.h"
#include "body.h"
#include "combat_hostility.h"
#include "doors.h"
#include "downed.h"
#include "enclaves.h"
#include "ending.h"
#include "fightmem.h"
#include "guards.h"
#include "guide.h"
#include "guide_policy.h"
#include "log.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "npcs.h"
#include "occupancy.h"
#include "pickpocket.h"
#include "remote.h"
#include "reputation_policy.h"
#include "story.h"
#include "talk.h"
#include "xpshare.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::infocmd {
namespace detail {

Stats stats;
Flags flags;
std::vector<Pending> pending;
std::vector<Fight> fights;

namespace {
// Commands whose result is a state of their first entity (static analysis of IS_Running_ProcessInfo: dialog
// flags, guard status, class, political alignment, immortality, routine, position, enclave raid/revolution).
char const* const StateCommands[] = {"SetClass", "SetPAL", "SetTeachEnabled", "SetTradeEnabled", "SetPartyEnabled", "SetMobEnabled",
                                     "SetSlaveryEnabled", "SetRevolutionEnabled", "SetRaidEnabled", "SetSleepingPoint", "SetWorkingPoint",
                                     "SetRelaxingPoint", "FullStop", "SetRoutine", "SetGuardStatus", "MoveTo", "StartRevolution", "Humanize",
                                     "JoinNPC", "JoinPlayer"};

bool IsStateCommand(char const* command)
{
    for (char const* c : StateCommands)
        if (!std::strcmp(c, command)) return true;
    return false;
}

void ApplyCompanionWaiting(net::WorldState const& s, uint8_t from)
{
    if (!net::IsHost()) return;
    Entity npc(npcs::NpcByGuid(s.guid));
    if (npc.GetInstance() && npcs::CompanionOwner(npc.GetInstance()) == from)
        npcs::Follow(s.guid, from, static_cast<uint8_t>(static_cast<gEPartyMemberType>(npc.Party.PartyMemberType)), s.immortal != 0);
    else
        Log("infocmd: companion waiting request refused: not that player's follower");
}

void ApplyCompanionRelation(net::WorldState const& s, uint8_t from)
{
    if (!net::IsHost()) return;
    Entity npc(npcs::NpcByGuid(s.guid));
    eCEntity* actor = remote::PuppetOf(from);
    if (!npc.GetInstance() || !actor) { Log("infocmd: NPC relation request refused: missing actor"); return; }
    if (s.kind == 5) {
        npcs::SetContext(npc.GetInstance(), 0);
        if (npcs::OnClaim(s.guid, from, false) && npc.GetInstance()->GetPropertySet(eEPropertySetType_Party))
            npc.Party.SetPartyLeader(Entity());
        if (actor->GetPropertySet(eEPropertySetType_Party)) {
            Entity player(actor);
            if (player.Party.GetPartyLeader().GetInstance() == npc.GetInstance()) player.Party.SetPartyLeader(Entity());
        }
        if (guide_policy::ShouldImmediatelyRestartRoutineOnApply(false, talk::IsHeld(npc.GetInstance()))) {
            npc.Routine.FullStop();
            npc.Routine.ContinueRoutine();
        }
        return;
    }
    if (s.npcClass == 2) {
        if (npcs::OnClaim(s.guid, from, true)) npcs::Guide(s.guid, from, s.routine);
    } else if (npcs::OnClaim(s.guid, from, true)) {
        npcs::Follow(s.guid, from, static_cast<uint8_t>(gEPartyMemberType_Slave));
    }
}

void ApplySectorState(net::WorldState const& s)
{
    if (net::IsHost()) NoteSyncedSector(s.routine, s.enclave != 0);
    if (!gCSession::GetInstance().IsGameRunning()) {
        BufferClientSectorState(s.routine, s.enclave != 0);
        return;
    }
    world.SetSectorStatus(bCString(s.routine), s.enclave != 0);
    ++stats.sectorsApplied;
    Logf("infocmd: sector %s %s", s.routine, s.enclave ? "on" : "off");
}

void ApplyNpcSnapshot(net::WorldState const& s, uint8_t from)
{
    const GuidKey key = GuidKeyOf(s.guid);
    if (net::IsHost()) NoteSyncedNpc(key);
    if (!gCSession::GetInstance().IsGameRunning()) {
        BufferClientNpcState(key, s);
        return;
    }
    Entity e = guid::Resolve(s.guid);
    eCEntity* instance = e.GetInstance();
    if (!instance) {
        if (!net::IsHost()) BufferClientNpcState(key, s);
        return;
    }
    if (npc_progression_policy::HasDialog(s.dialog) && instance->GetPropertySet(eEPropertySetType_Dialog)) {
        const uint8_t effectiveDialog = (net::IsHost() && from > 1)
            ? npc_progression_policy::MergeHostDialog(s.dialog, e.Dialog.TalkedToPlayer != GEFalse)
            : s.dialog;
        e.Dialog.TeachEnabled = (effectiveDialog & npc_progression_policy::DialogTeach) != 0;
        e.Dialog.TradeEnabled = (effectiveDialog & npc_progression_policy::DialogTrade) != 0;
        e.Dialog.PartyEnabled = (effectiveDialog & npc_progression_policy::DialogParty) != 0;
        e.Dialog.MobEnabled = (effectiveDialog & npc_progression_policy::DialogMob) != 0;
        e.Dialog.SlaveryEnabled = (effectiveDialog & npc_progression_policy::DialogSlavery) != 0;
        const bool talked = npc_progression_policy::UnpackTalkedToPlayer(effectiveDialog);
        e.Dialog.TalkedToPlayer = talked ? GETrue : GEFalse;
        if (talked) ApplyFriendMemory(e.GetName().GetText());
    }
    if (npc_progression_policy::HasNpcFlags(s.npc) && instance->GetPropertySet(eEPropertySetType_NPC)) {
        e.NPC.GuardStatus = static_cast<gEGuardStatus>(s.guard);
        e.NPC.Class = static_cast<gEClass>(s.npcClass);
        e.NPC.PoliticalAlignment = static_cast<gEPoliticalAlignment>(s.alignment);
        e.NPC.Immortal = s.immortal != 0;
        if (npc_progression_policy::HasDefeatedFlag(s.npc)) {
            const uint8_t effectiveNpc = (net::IsHost() && from > 1)
                ? npc_progression_policy::MergeHostNpcFlags(s.npc, e.NPC.DefeatedByPlayer != GEFalse)
                : s.npc;
            const bool defeated = npc_progression_policy::UnpackDefeatedByPlayer(effectiveNpc);
            e.NPC.DefeatedByPlayer = defeated ? GETrue : GEFalse;
            npcs::SyncDefeatedByPlayer(s.guid, defeated);
        }
    }
    NoteObservedProgression(key, CaptureObserved(e));
    auto* enclave = static_cast<gCEnclave_PS*>(instance->GetPropertySet(eEPropertySetType_Enclave));
    if (s.enclave && enclave) {
        const int oldStatus = static_cast<int>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus()));
        const int newStatus = static_cast<int>(s.enclaveStatus);
        enclave->AccessRaid() = (s.enclave & 2) != 0;
        enclave->AccessRevolution() = (s.enclave & 4) != 0;
        enclave->AccessStatus() = static_cast<gEEnclaveStatus>(s.enclaveStatus);
        if (net::IsHost() && from != 1 && reputation::ShouldAwardLiberationFame(oldStatus, newStatus))
            enclaves::OnHostEnclaveLiberated(enclave, e.GetName().GetText());
    }
    const bool mirrored = npcs::IsMirrored(instance);
    if (s.moved == 2 && flags.keepCompanions && npcs::Claimed(instance)) {
        ++stats.companionsKept;
        Logf("infocmd: the story moved %s on the host; our companion stays with us", e.GetName().GetText());
        return;
    }
    if (auto* nav = static_cast<gCNavigation_PS*>(instance->GetPropertySet(eEPropertySetType_Navigation))) {
        if (*s.routine && std::strcmp(nav->AccessRoutine().GetText(), s.routine) != 0) {
            if (mirrored) nav->AccessRoutine() = bCString(s.routine);
            else e.Navigation.SetRoutine(bCString(s.routine));
            if (guide_policy::ShouldImmediatelyRestartRoutineOnApply(mirrored, talk::IsHeld(instance))) {
                const uint8_t actingPlayer = guide_policy::GuideRoutineActingPlayer(
                    net::IsHost(), false, npcs::ContextPlayer(instance));
                guards::Acting context(actingPlayer > 1 ? remote::PuppetOf(actingPlayer) : nullptr);
                e.Routine.FullStop();
                e.Routine.ContinueRoutine();
            }
        }
    }
    const bCVector& p = instance->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    if (s.moved == 2 && body::Distance(here, s.position) > 200.0f) npcs::StoryMoved(s.guid, s.position, s.rotation);
    else if (s.moved && !mirrored && body::Distance(here, s.position) > 200.0f) e.MoveTo(body::Pose(*instance, s.position, s.rotation));
    ++stats.snapshotsApplied;
    Logf("infocmd: applied the state of %s (routine %s)", e.GetName().GetText(), s.routine);
}
} // namespace

npc_progression_policy::ObservedProgression Snapshot(Entity& e, net::WorldState& s, bool includeRoutine)
{
    eCEntity* instance = e.GetInstance();
    s.kind = 1;
    guid::CopyEntity(instance, s.guid);
    const auto obs = CaptureObserved(e);
    s.dialog = obs.dialog;
    s.npc = obs.npc;
    s.guard = obs.guard;
    s.npcClass = obs.npcClass;
    s.alignment = obs.alignment;
    s.immortal = obs.immortal;
    if (includeRoutine) {
        if (auto* nav = static_cast<gCNavigation_PS*>(instance->GetPropertySet(eEPropertySetType_Navigation)))
            strncpy_s(s.routine, nav->AccessRoutine().GetText(), _TRUNCATE);
    } else {
        s.routine[0] = '\0';
    }
    if (auto* enclave = static_cast<gCEnclave_PS*>(instance->GetPropertySet(eEPropertySetType_Enclave))) {
        s.enclave = 1 | (enclave->AccessRaid() ? 2 : 0) | (enclave->AccessRevolution() ? 4 : 0);
        s.enclaveStatus = static_cast<uint8_t>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus()));
    }
    const bCVector& p = instance->GetWorldPosition();
    s.position[0] = p.GetX(); s.position[1] = p.GetY(); s.position[2] = p.GetZ();
    const bCQuaternion q = instance->GetWorldRotation();
    s.rotation[0] = q.GetX(); s.rotation[1] = q.GetY(); s.rotation[2] = q.GetZ(); s.rotation[3] = q.GetW();
    return obs;
}

void Apply(net::WorldState const& s, uint8_t from)
{
    if (s.kind == 8 || s.kind == 9) {
        if (s.guard == net::LocalId()) {
            uint32_t sequence = 0;
            std::memcpy(&sequence, s.guid, sizeof(sequence));
            if (s.kind == 8) downed::Revived(s.npcClass, sequence);
            else downed::HelperHasNoPotion(s.npcClass);
        }
        return;
    }
    switch (s.kind) {
    case 1: ApplyNpcSnapshot(s, from); return;
    case 2: ApplySectorState(s); return;
    case 3: {
        if (!net::IsHost()) return;
        const uint8_t player = s.guard ? s.guard : from;
        if (player != from || player >= net::MaxPlayers) return;
        Fight f{{}, player, s.routine, net::Now() + 15000, net::JoinCount()};
        std::memcpy(f.attacker, s.guid, 16);
        fights.push_back(f);
        return;
    }
    case 4:
    case 5: ApplyCompanionRelation(s, from); return;
    case 6: if (!net::IsHost() && s.guard == net::LocalId()) npcs::Revoked(s.guid); return;
    case 7: if (!net::IsHost() && s.guard == net::LocalId()) npcs::Adopt(s.guid); return;
    case 10: doors::OnState(from, s); return;
    case 12: pickpocket::OnState(from, s); return;
    case 13: occupancy::OnState(from, s); return;
    case 16: address::OnState(from, s); return;
    case 17: ending::OnState(from, s); return;
    case 18: fightmem::OnState(from, s); return;
    case 19: xpshare::OnState(s); return;
    case 20: arena::OnState(from, s); return;
    case 21: {
        if (!net::IsHost() && from == 1) {
            uint32_t epoch = 0;
            std::memcpy(&epoch, s.guid, sizeof(epoch));
            downed::HostWipe(epoch);
        }
        return;
    }
    case 22: ApplyCompanionWaiting(s, from); return;
    default: return;
    }
}

namespace {
void OnWorldState(uint8_t from, net::WorldState const& s)
{
    uint32_t helpSequence = 0;
    if (s.kind == 8 || s.kind == 9) {
        std::memcpy(&helpSequence, s.guid, sizeof(helpSequence));
        Logf("downed: help message kind=%u transport=%u helper=%u target=%u sequence=%u source=world-state",
             s.kind, from, s.npcClass, s.guard, helpSequence);
    }
    if (s.kind == 15) {
        if (net::IsHost())
            talk::OnGesture(from, s.guid, static_cast<uint32_t>(std::strtoul(s.routine, nullptr, 16)), s.rotation);
        return;
    }
    switch (s.kind) {
    case 20: if (net::IsHost()) arena::OnState(from, s); return;
    case 23: combat_hostility::OnState(from, s); return;
    case 14: Logf("infocmd: obsolete NPC lifecycle result from player %u refused; host snapshots own NPC state", from); return;
    case 6:
    case 7:
    case 21:
        if (!net::IsHost()) Apply(s, from);
        return;
    case 4:
    case 5:
    case 22:
        if (net::IsHost()) Apply(s, from);
        return;
    default:
        break;
    }
    if (net::IsHost() && s.kind == 1) {
        Entity npc = guid::Resolve(s.guid);
        eCEntity* inst = npc.GetInstance();
        if (!inst || inst == gCSession::GetInstance().GetHero() || remote::IsPuppet(inst)) return;
        if (!inst->GetPropertySet(eEPropertySetType_NPC) && !inst->GetPropertySet(eEPropertySetType_Enclave)) return;
        Apply(s, from);
        net::WorldState canonical{};
        Snapshot(npc, canonical, true);
        canonical.moved = s.moved;
        NoteSyncedNpc(GuidKeyOf(s.guid));
        net::SendWorldState(canonical, 0);
        return;
    }
    if (net::IsHost() && s.kind == 8) {
        if (downed::Terminal()) return;
        downed::NoteReviveSent(s.guard, helpSequence);
    }
    if (net::IsHost() && s.kind != 3 && s.kind != 16) net::SendWorldState(s, from);  // pass it on to everyone else
    Apply(s, from);
}
} // namespace

void Send(net::WorldState const& s)
{
    if (net::IsHost()) {
        if (s.kind == 1) NoteSyncedNpc(GuidKeyOf(s.guid));
        else if (s.kind == 2 && *s.routine) NoteSyncedSector(s.routine, s.enclave != 0);
    }
    net::SendWorldState(s, 0);
}

} // namespace detail

using namespace detail;

void Start() { net::SetWorldStateHandler(&detail::OnWorldState); }

void Observe(gCScriptProcessingUnit* spu)
{
    ++stats.observed;
    eCEntity* self = spu ? spu->GetSelfEntity() : nullptr;
    // The info manager lives on the world entity (World_MCP runs the dialog script); the unit's entity first.
    Entity owner(self);
    PSInfoScriptCommand command;
    bool waitingForScript = false;
    if (owner.GetInstance() && owner.GetInstance()->GetPropertySet(eEPropertySetType_InfoManager)) {
        command = owner.InfoManager.GetCurrentCommand();
        waitingForScript = owner.InfoManager.IsWaitingforScript() != GEFalse;
    }
    if (!command.IsValid()) {
        Entity worldEntity = Entity::GetWorldEntity();
        command = worldEntity.InfoManager.GetCurrentCommand();
        waitingForScript = worldEntity.InfoManager.IsWaitingforScript() != GEFalse;
    }
    if (!command.IsValid()) return;
    const Entity e1 = command.GetEntity1(), e2 = command.GetEntity2();
    if (!waitingForScript && flags.sharing && net::Connected()) {
        reputation::DialogRepMutation repMutation{};
        char const* e2Name = (e2.GetInstance() && e2.GetInstance()->GetPropertySet(eEPropertySetType_Enclave))
                                 ? e2.GetName().GetText()
                                 : nullptr;
        if (reputation::ParseDialogReputationCommand(command.GetCommand().GetText(), command.GetID1().GetText(),
                                                     command.GetID2().GetText(), command.GetTextID().GetText(),
                                                     e2Name, repMutation)) {
            if (net::IsHost()) {
                story::FlushReputation();
                flags.flushHostReputation = true;
            } else {
                enclaves::SendDialogReputationDelta(repMutation.isPolitical, repMutation.alignment,
                                                    repMutation.enclave.c_str(), repMutation.delta);
            }
        }
    }
    if (!std::strcmp(command.GetCommand().GetText(), "Say") && !std::strcmp(command.GetTextID().GetText(), "INFO_SKALVERAM320060309153144"))
        downed::HelpLineSaid();
    char key[400];
    Format(key, "%s|%s|%s|%s|%s|%s", command.GetCommand().GetText(), e1.GetInstance() ? e1.GetName().GetText() : "-",
              e2.GetInstance() ? e2.GetName().GetText() : "-", command.GetID1().GetText(), command.GetID2().GetText(),
              command.GetTextID().GetText());
    if (stats.lastKey == key) return;
    stats.lastKey = key;
    stats.lastText = key;
    ++stats.distinct;
    Logf("infocmd: %s (self=%s)", key, self ? Entity(self).GetName().GetText() : "-");
    if (!std::strcmp(command.GetCommand().GetText(), "SaySVM") && e2.GetInstance() &&
        (!_stricmp(command.GetID1().GetText(), "CmdPartyJoin") || !_stricmp(command.GetID1().GetText(), "CmdMobJoin") ||
         !_stricmp(command.GetID1().GetText(), "CmdSlaveJoin")))
        guide::Joined(e2.GetInstance());
    if (!flags.sharing || !net::Connected()) return;
    char const* name = command.GetCommand().GetText();
    if (!std::strcmp(name, "SetSectorStatus")) {
        Pending p{};
        p.sector = command.GetID1().GetText();
        p.due = net::Now() + ReadAfter;
        pending.push_back(std::move(p));
        return;
    }
    if (!std::strcmp(name, "GiveXP")) return;
    eCEntity* target = e1.GetInstance();
    if (!std::strcmp(name, "Attack") &&
        arena_policy::ShouldForwardClientDialogAttack(
            net::IsHost(), net::Connected(), target != nullptr,
            target && npcs::IsMirrored(target), target && npclease::LocalAuthority(target),
            guards::IsLocalPlayer(e2.GetInstance()))) {
        if (npclease::LocalAuthority(target))
            npclease::RelinquishToHost(target, "dialog fight");
        story::FlushQuests();
        net::WorldState s{};
        s.kind = 3;
        guid::CopyEntity(target, s.guid);
        s.guard = net::LocalId();
        strncpy_s(s.routine, command.GetID1().GetText(), _TRUNCATE);
        Send(s);
        ++stats.fightsSent;
        Logf("infocmd: dialog fight with %s (%s) sent to the host", e1.GetName().GetText(), s.routine);
        return;
    }
    if (!std::strcmp(name, "JoinPlayer") && !net::IsHost() && target) {
        const bool join = !_stricmp(command.GetID1().GetText(), "true") || !std::strcmp(command.GetID1().GetText(), "1");
        if (Join(target, join)) return;
    }
    if (!detail::IsStateCommand(name) || !target || target == gCSession::GetInstance().GetHero() || remote::IsPuppet(target)) return;
    const bool immediateRoutine = !std::strcmp(name, "SetRoutine") && *command.GetID1().GetText();
    Pending p{};
    guid::CopyEntity(target, p.guid);
    p.due = immediateRoutine ? net::Now() : (net::Now() + ReadAfter);
    p.moved = std::strcmp(name, "MoveTo") == 0;
    if (immediateRoutine) p.routine = command.GetID1().GetText();
    pending.push_back(std::move(p));
}

void Changed(eCEntity* entity)
{
    if (!entity || !flags.sharing) return;
    Pending p{};
    guid::CopyEntity(entity, p.guid);
    p.due = net::Now() + ReadAfter;
    pending.push_back(std::move(p));
}

void Tick()
{
    HookSectorRefresh();
    SeedHostWorldState();
    FlushBufferedClientStates();
    if (flags.flushHostReputation) {
        flags.flushHostReputation = false;
        story::FlushReputation();
    }
    if (gCSession::GetInstance().IsGameRunning()) {
        if (auto* mgr = gCInfoManager_PS::GetInstance(); !mgr || !mgr->AccessIsRunning()) stats.lastKey.clear();
    }
    TrackLocalDialogAndProgression();
    if (!fights.empty()) StartFights();
    if (pending.empty()) return;
    const uint32_t now = net::Now();
    for (auto it = pending.begin(); it != pending.end();) {
        if (static_cast<int32_t>(now - it->due) < 0) { ++it; continue; }
        net::WorldState s{};
        if (!it->sector.empty()) {
            s.kind = 2;
            strncpy_s(s.routine, it->sector.c_str(), _TRUNCATE);
            s.enclave = world.GetSectorStatus(bCString(it->sector.c_str())) ? 1 : 0;
            ++stats.sectorsSent;
            Send(s);
        } else {
            Entity e = guid::Resolve(it->guid);
            if (e.GetInstance()) {
                const bool includeRoutine = npc_progression_policy::ShouldIncludeRoutineInSnapshot(
                    net::IsHost(), !it->progressionOnly);
                NoteObservedProgression(GuidKeyOf(it->guid), Snapshot(e, s, includeRoutine));
                if (!it->routine.empty())
                    strncpy_s(s.routine, it->routine.c_str(), _TRUNCATE);
                s.moved = it->story ? 2 : it->moved ? 1 : 0;
                ++stats.snapshotsSent;
                Send(s);
                Logf("infocmd: sent the state of %s after a dialog command", e.GetName().GetText());
            }
        }
        it = pending.erase(it);
    }
}

void OnWorldChange()
{
    fights.clear();
    pending.clear();
    stats.lastKey.clear();
    flags.flushHostReputation = false;
    ResetProgressionState();
}

} // namespace g3t::infocmd
