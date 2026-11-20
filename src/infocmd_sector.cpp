// SPDX-License-Identifier: GPL-3.0-or-later
#include "infocmd_internal.h"

#include "ai.h"
#include "arena.h"
#include "body.h"
#include "guards.h"
#include "log.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"
#include "telemetry.h"
#include "xpshare.h"

#include <cstdio>
#include <cstring>

namespace g3t::infocmd::detail {
namespace {
// The dialog command Attack runs the perception script AssessTarget (attack reason, enclave notice, target,
// attack task; static analysis of IS_Running_ProcessInfo). Its reason names map to gEAttackReason.
int ReasonOf(std::string const& name, eCEntity* attacker, eCEntity* target)
{
    if (!_stricmp(name.c_str(), "Duel")) return gEAttackReason_Duel;
    if (!_stricmp(name.c_str(), "Arena")) return gEAttackReason_Arena;
    if (!_stricmp(name.c_str(), "ReactToWeapon")) return gEAttackReason_ReactToWeapon;
    if (!_stricmp(name.c_str(), "Revolution")) return gEAttackReason_Revolution;
    if (!_stricmp(name.c_str(), "Guard")) {
        guards::Acting guardContext(target, false, attacker);
        return GetScriptAdminExt().CallScript(bCString("GetGuardAttackReason"), attacker, target, 0);
    }
    return gEAttackReason_Enemy;
}

// OnEnclaveSectorRefresh (Script_Game 0x100b16f0, called by the game itself, not by name): the story moves people after a
// town changed hands once this machine's player is far from it (Ardea freed, over 420 m: Diego to Braga, Gorn to
// GothaPrison, Milten to the monastery, Lester; Montera, Ben Erai, the big mine; re/out/basis-rest-20261026-b.c), with
// SetRoutine and MoveTo. The host passes on whom it moved (routine and place, kind 1 with moved 2: a mirror moves too).
char const* const StoryMovers[] = {"Diego", "Gorn", "Milten", "Lester", "Jorn", "Lisk", "Mort", "Thorald", "Otis", "Mason", "Daryl",
                                   "Lukar", "G3_Varant_01_Braga_NPC_01", "G3_Varant_01_Braga_NPC_02"};
gFScript originalRefresh = nullptr;
bool refreshTried = false;
unsigned refreshCalls = 0, refreshSent = 0;

GEInt GE_STDCALL SectorRefresh(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalRefresh ? originalRefresh(spu, self, other, args) : 0;
    if (!net::IsHost()) return 0;
    struct Before { eCEntity* entity; std::string routine; bCVector position; bCQuaternion rotation; uint8_t companionOf; };
    std::vector<Before> before;
    const bool passOn = flags.refreshSync && flags.sharing && net::IsHost() && net::Connected();
    if (passOn) {
        for (char const* name : StoryMovers) {
            Entity e = Entity::GetEntity(bCString(name));
            eCEntity* instance = e.GetInstance();
            auto* nav = instance ? static_cast<gCNavigation_PS*>(instance->GetPropertySet(eEPropertySetType_Navigation)) : nullptr;
            if (nav) before.push_back({instance, nav->AccessRoutine().GetText(), instance->GetWorldPosition(), instance->GetWorldRotation(),
                                       flags.keepCompanions ? npcs::ClaimedBy(instance) : uint8_t(0)});
        }
    }
    const GEInt result = originalRefresh(spu, self, other, args);
    ++refreshCalls;
    for (Before const& b : before) {
        auto* nav = static_cast<gCNavigation_PS*>(b.entity->GetPropertySet(eEPropertySetType_Navigation));
        const bCVector& p = b.entity->GetWorldPosition();
        const float here[3] = {p.GetX(), p.GetY(), p.GetZ()}, was[3] = {b.position.GetX(), b.position.GetY(), b.position.GetZ()};
        const bool moved = body::Distance(here, was) > 200.0f;
        if (moved && b.companionOf) {
            // Review 26.10. (point 8): a client's companion (its copy here mirrors the client's) is not moved by the story:
            // it stays with that player, here as there. Its new routine stays; it goes there once the player lets it go.
            const float rotation[4] = {b.rotation.GetX(), b.rotation.GetY(), b.rotation.GetZ(), b.rotation.GetW()};
            Entity(b.entity).MoveTo(body::Pose(*b.entity, was, rotation));
            ++stats.companionsKept;
            Logf("infocmd: the story moved %s, the companion of player %u; it stays with that player", b.entity->GetName().GetText(),
                 b.companionOf);
            continue;
        }
        if (!moved && (!nav || b.routine == nav->AccessRoutine().GetText())) continue;
        Pending pend{};
        guid::CopyEntity(b.entity, pend.guid);
        pend.due = net::Now() + ReadAfter;
        pend.moved = moved;
        pend.story = moved;
        pending.push_back(std::move(pend));
        ++refreshSent;
        Logf("infocmd: the story moved %s (routine %s, %.0f m); passed on", b.entity->GetName().GetText(),
             nav ? nav->AccessRoutine().GetText() : "-", body::Distance(here, was) / 100.0f);
        telemetry::OnNpcProgression("story_sector_refresh", 1, b.entity, 0, 0, 0, 0, 0, 0, 0, moved,
                                    nav ? nav->AccessRoutine().GetText() : "");
    }
    return result;
}
} // namespace

void StartFights()
{
    const uint32_t now = net::Now();
    for (auto it = fights.begin(); it != fights.end();) {
        Entity attacker = guid::Resolve(it->attacker);
        eCEntity* target = remote::PuppetOf(it->player);
        if (static_cast<int32_t>(now - it->until) > 0 ||
            it->generation != net::PeerGeneration(it->player) || !net::InWorld(it->player)) {
            it = fights.erase(it);
            continue;
        }
        if (!attacker.GetInstance() || !target) { ++it; continue; }  // not loaded here yet, or the puppet not spawned
        if (npclease::Fenced(attacker.GetInstance())) { ++it; continue; }
        if (talk::IsHeld(attacker.GetInstance())) {
            it->until = now + 15000;
            ++it;
            continue;
        }  // still standing in the client's dialog
        int reason = 0;
        {
            guards::Acting context(target);
            reason = ReasonOf(it->reason, attacker.GetInstance(), target);
            if (reason == gEAttackReason_Duel || reason == gEAttackReason_Arena ||
                arena::ChallengerForNpc(attacker.GetInstance()) != 0)
                arena::TrackDuel(attacker.GetInstance(), it->player);
            GetScriptAdminExt().CallScript(bCString("AssessTarget"), attacker.GetInstance(), target, reason);
        }
        remote::NoteContact(attacker.GetInstance(), target);
        ++stats.fightsStarted;
        Logf("infocmd: %s attacks player %u (%s, reason %d) after a dialog", attacker.GetName().GetText(), it->player, it->reason.c_str(), reason);
        telemetry::OnTalkEvent("dialog_attack_started", it->player, attacker.GetInstance(), it->attacker, it->reason.c_str());
        it = fights.erase(it);
    }
}

bool Join(eCEntity* self, bool join, bool follow, char const* questName)
{
    if (join && self && (npcs::IsMirrored(self) || npclease::LocalAuthority(self))) {
        if (npclease::LocalAuthority(self)) npclease::RelinquishToHost(self, "companion joined");
        npcs::Claim(self);
        net::WorldState s{};
        s.kind = 4;
        guid::CopyEntity(self, s.guid);
        eCEntity* hero = gCSession::GetInstance().GetHero();
        const bool heroGuidedBySelf = hero && hero->GetPropertySet(eEPropertySetType_Party) &&
                                      Entity(hero).Party.GetPartyLeader().GetInstance() == self;
        const bool guide = !follow && ((questName && *questName) || heroGuidedBySelf);
        s.npcClass = guide ? 2 : 1;
        if (guide && questName && *questName)
            strncpy_s(s.routine, questName, _TRUNCATE);
        Send(s);
        return true;
    }
    if (!join && npcs::Claimed(self)) {
        npcs::Unclaim(self);
        net::WorldState s{};
        s.kind = 5;
        guid::CopyEntity(self, s.guid);
        if (flags.givebackPlace) {  // where it stands here: the host's copy far away stayed where the host last saw it (escortfardiag)
            const bCVector& p = self->GetWorldPosition();
            s.position[0] = p.GetX(); s.position[1] = p.GetY(); s.position[2] = p.GetZ();
            const bCQuaternion q = self->GetWorldRotation();
            s.rotation[0] = q.GetX(); s.rotation[1] = q.GetY(); s.rotation[2] = q.GetZ(); s.rotation[3] = q.GetW();
            s.moved = 1;
        }
        Send(s);
        return true;
    }
    return false;
}

void HookSectorRefresh()
{
    if (refreshTried || !gCSession::GetInstance().IsGameRunning()) return;
    refreshTried = true;
    gSScript* script = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("OnEnclaveSectorRefresh"), script) || !script || !script->m_funcScript) {
        Logf("infocmd: OnEnclaveSectorRefresh not found; story moves stay local");
        return;
    }
    auto* code = reinterpret_cast<unsigned char*>(script->m_funcScript);
    static const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    if (std::memcmp(code, prologue, sizeof(prologue))) {
        Logf("infocmd: OnEnclaveSectorRefresh has another prologue; story moves stay local");
        return;
    }
    originalRefresh = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&SectorRefresh)));
    Logf("infocmd: OnEnclaveSectorRefresh %s", originalRefresh ? "wrapped (the host passes on story moves)" : "could not be wrapped");
}

RefreshStats GetRefreshStats()
{
    return {originalRefresh != nullptr, refreshCalls, refreshSent};
}

} // namespace g3t::infocmd::detail

namespace g3t::infocmd {
bool TakeOver(eCEntity* target, bool join, bool follow, char const* questName)
{
    return detail::Join(target, join, follow, questName);
}
} // namespace g3t::infocmd
