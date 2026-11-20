// SPDX-License-Identifier: GPL-3.0-or-later
#include "address.h"
#include "address_policy.h"

#include "body.h"
#include "guid.h"
#include "guid_key.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"
#include "talk.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace g3t::address {
namespace {
constexpr uint8_t Kind = 16;            // net::WorldState kind, client -> host
constexpr float CallFrom = 2000.0f;     // cm: AssessActivePerception asks for important infos within this (Script_Game 0x107480)
constexpr float TalkFrom = 250.0f;      // cm: the client addresses the NPC once it stands this near
constexpr float StandBy = 120.0f;       // cm: where the host's NPC stops in front of the puppet
constexpr uint32_t Wait = 25000;        // ms the client waits for the NPC to come
constexpr uint32_t Rest = 60000;        // ms before the same NPC is asked for again
bool on = true;  // QA negative control: addressoff
uint32_t nextLook = 0;
unsigned requested = 0, addressed = 0, expired = 0, accepted = 0, refused = 0, started = 0, gaveUp = 0;

float Distance(eCEntity* a, eCEntity* b)
{
    const bCVector& p = a->GetWorldPosition();
    const bCVector& q = b->GetWorldPosition();
    const float dx = p.GetX() - q.GetX(), dy = p.GetY() - q.GetY(), dz = p.GetZ() - q.GetZ();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

gCInfoManager_PS* Infos() { return gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr; }

bool InDialog(eCEntity* npc = nullptr)
{
    gCInfoManager_PS* manager = Infos();
    if (!manager || !manager->AccessIsRunning()) return false;
    if (!npc) return true;
    return reinterpret_cast<eCEntity*>(manager->GetDialogPartnerNPC()) == npc ||
           reinterpret_cast<eCEntity*>(manager->GetDialogPartner1()) == npc ||
           reinterpret_cast<eCEntity*>(manager->GetCurrentSelf()) == npc ||
           reinterpret_cast<eCEntity*>(manager->GetCurrentOther()) == npc;
}

address_policy::HostNpcState BuildHostNpcState(Entity& npc, eCEntity* puppet)
{
    eCEntity* inst = npc.GetInstance();
    address_policy::HostNpcState s{};
    s.entityExists = inst != nullptr;
    s.puppetExists = puppet != nullptr;
    if (!inst) return s;
    s.isDead = npc.IsDead() != GEFalse;
    s.isDown = npc.IsDown() != GEFalse;
    s.isMirrored = npcs::IsMirrored(inst);
    s.hasNavigation = inst->GetPropertySet(eEPropertySetType_Navigation) != nullptr;
    s.heldForRemoteTalk = talk::IsHeld(inst);
    s.inHostDialog = InDialog(inst);
    s.aiMode = body::AIModeOf(inst);
    s.tooFar = puppet != nullptr && Distance(inst, puppet) > CallFrom + 500.0f;
    return s;
}

// Client: the NPC asked for and until when it may come; NPCs asked for lately.
struct Asked { uint8_t guid[16]; uint32_t until; };
Asked pending{};
bool waiting = false;
std::unordered_map<GuidKey, uint32_t, GuidHash> rested;

// Host: NPCs on their way to a player's puppet.
struct Approach { Entity npc; uint8_t player; uint32_t until, nextGoto; };
std::unordered_map<GuidKey, Approach, GuidHash> approaches;

void ClientTick(uint32_t now)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || Entity(hero).IsDead() || Entity(hero).IsDown()) return;
    if (waiting) {
        Entity npc(npcs::NpcByGuid(pending.guid));
        if (InDialog()) {  // the dialog runs (with this NPC or another one the player chose)
            waiting = false;
            return;
        }
        address_policy::ClientWaitState waitState{};
        waitState.entityExists = npc.GetInstance() != nullptr;
        waitState.isDead = waitState.entityExists && npc.IsDead() != GEFalse;
        waitState.isDown = waitState.entityExists && npc.IsDown() != GEFalse;
        waitState.expired = static_cast<int32_t>(now - pending.until) > 0;
        waitState.tooFar = waitState.entityExists && Distance(npc.GetInstance(), hero) > CallFrom + 500.0f;
        if (address_policy::ShouldCancelClientWait(waitState)) {
            waiting = false;
            ++expired;
            Logf("address: %s did not come", npc.GetInstance() ? npc.GetName().GetText() : "the NPC");
            return;
        }
        if (Distance(npc.GetInstance(), hero) > TalkFrom) return;
        // As the game's talk action does (and as the NPC's own AssessTalk would on the host for its player).
        GetScriptAdminExt().CallScript(bCString("AssessTalk"), npc.GetInstance(), hero, 0);
        waiting = false;
        ++addressed;
        Logf("address: %s stands by; addressed (dialog %s)", npc.GetName().GetText(), InDialog() ? "runs" : "did not start");
        return;
    }
    if (InDialog()) return;
    // The nearest mirrored NPC within 20 m with an important info for this hero.
    eCEntity* best = nullptr;
    float bestDistance = CallFrom;
    const bTObjArray<Entity> list = Entity::GetNPCs();
    for (int i = 0; i < list.GetCount(); ++i) {
        Entity npc = list.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || npc.IsDead() || npc.IsDown() || !npcs::IsMirrored(instance)) continue;
        const float d = Distance(instance, hero);
        if (d >= bestDistance) continue;
        uint8_t guid[16];
        guid::CopyEntity(instance, guid);
        auto it = rested.find(ToGuidKey(guid));
        if (it != rested.end() && static_cast<int32_t>(now - it->second) < 0) continue;
        if (!npc.HasImportantInfo(Entity(hero))) continue;
        best = instance;
        bestDistance = d;
    }
    if (!best) return;
    net::WorldState s{};
    s.kind = Kind;
    guid::CopyEntity(best, s.guid);
    s.guard = net::LocalId();
    net::SendWorldState(s, 0);
    std::memcpy(pending.guid, s.guid, 16);
    pending.until = now + Wait;
    waiting = true;
    rested[ToGuidKey(s.guid)] = now + Rest;
    ++requested;
    Logf("address: %s has an important info for this hero (%.0f m); asked the host to send it", Entity(best).GetName().GetText(),
         bestDistance / 100.0f);
}

void RestoreRoutine(Approach& approach)
{
    eCEntity* npc = approach.npc.GetInstance();
    if (npc && !approach.npc.IsDead() && !approach.npc.IsDown() &&
        !InDialog(npc) && !talk::IsHeld(npc) && body::AIModeOf(npc) == 0) {
        approach.npc.Routine.FullStop();
        approach.npc.Routine.ContinueRoutine();
    }
}

void HostTick(uint32_t now)
{
    for (auto it = approaches.begin(); it != approaches.end();) {
        Approach& a = it->second;
        eCEntity* npc = a.npc.GetInstance();
        if (!npc) {
            npc = npcs::NpcByGuid(it->first.data());
            if (npc) a.npc = Entity(npc);
        }
        eCEntity* puppet = remote::PuppetOf(a.player);
        const address_policy::HostNpcState state = BuildHostNpcState(a.npc, puppet);
        const bool approachExpired = static_cast<int32_t>(now - a.until) > 0;
        switch (address_policy::DecideHostApproach(state, approachExpired)) {
        case address_policy::HostApproachAction::StartedRemoteTalk:
            ++started;
            Logf("address: %s talks to player %u", a.npc.GetName().GetText(), a.player);
            it = approaches.erase(it);
            continue;
        case address_policy::HostApproachAction::CancelKeepState:
            ++gaveUp;
            Logf("address: %s busy (%s, ai=%d); cancelled approach to player %u",
                 a.npc.GetName().GetText(),
                 state.inHostDialog ? "in host dialog"
                 : state.isDown ? "down"
                 : state.isMirrored ? "mirrored"
                 : !state.hasNavigation ? "no navigation"
                 : "not in routine",
                 state.aiMode, a.player);
            it = approaches.erase(it);
            continue;
        case address_policy::HostApproachAction::EraseOnly:
            ++gaveUp;
            Logf("address: %s back to its routine (no dialog with player %u)",
                 npc ? a.npc.GetName().GetText() : "-", a.player);
            it = approaches.erase(it);
            continue;
        case address_policy::HostApproachAction::GiveUpAndRestore:
            ++gaveUp;
            // The player went away (teleport, a far walk): stop walking after the puppet and restore routine.
            if (state.tooFar)
                Logf("address: player %u went away; %s back to its routine", a.player, a.npc.GetName().GetText());
            else
                Logf("address: %s back to its routine (no dialog with player %u)", a.npc.GetName().GetText(), a.player);
            RestoreRoutine(a);
            it = approaches.erase(it);
            continue;
        case address_policy::HostApproachAction::Continue:
            break;
        }
        if (static_cast<int32_t>(now - a.nextGoto) >= 0) {
            a.nextGoto = now + 1000;
            const bCVector& p = puppet->GetWorldPosition();
            const bCVector& n = npc->GetWorldPosition();
            const float dx = n.GetX() - p.GetX(), dy = n.GetY() - p.GetY(), dz = n.GetZ() - p.GetZ();
            const float length = std::sqrt(dx * dx + dz * dz);
            const float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist3d > StandBy + 60.0f && length > 1.0f) {
                const bCVector goal(p.GetX() + dx / length * StandBy, p.GetY(), p.GetZ() + dz / length * StandBy);
                a.npc.StartGoto(goal, gEWalkMode_Walk);
            } else if (dist3d > StandBy + 60.0f) {
                a.npc.StartGoto(p, gEWalkMode_Walk);
            }
        }
        ++it;
    }
}
}

void Tick()
{
    if (!gCSession::GetInstance().IsGameRunning() || !net::Connected()) return;
    const uint32_t now = net::Now();
    if (net::IsHost()) {
        HostTick(now);
        return;
    }
    if (!on || static_cast<int32_t>(now - nextLook) < 0) return;
    nextLook = now + 500;
    ClientTick(now);
}

void OnState(uint8_t from, net::WorldState const& s)
{
    if (!net::IsHost() || s.kind != Kind) return;
    Entity npc(npcs::NpcByGuid(s.guid));
    eCEntity* puppet = remote::PuppetOf(from);
    const GuidKey key = ToGuidKey(s.guid);
    const address_policy::HostNpcState state = BuildHostNpcState(npc, puppet);
    char const* why = address_policy::RejectHostAddressRequest(state, approaches.find(key) != approaches.end());
    if (why) {
        ++refused;
        Logf("address: player %u asks for %s: refused, %s", from, npc.GetInstance() ? npc.GetName().GetText() : "an NPC", why);
        return;
    }
    npc.Routine.FullStop();
    approaches[key] = Approach{npc, from, net::Now() + Wait, 0};
    ++accepted;
    Logf("address: %s goes to player %u (task was %s)", npc.GetName().GetText(), from, npc.Routine.GetCurrentTask().GetText());
}

void OnWorldChange()
{
    approaches.clear();
    rested.clear();
    waiting = false;
    nextLook = 0;
}

void Report(LogFn log)
{
    char line[300];
    Format(line, "ADDRESS on=%d waiting=%d requested=%u addressed=%u expired=%u accepted=%u refused=%u started=%u gave_up=%u approaches=%zu",
              on ? 1 : 0, waiting ? 1 : 0, requested, addressed, expired, accepted, refused, started, gaveUp, approaches.size());
    log(line);
}

void Off(LogFn log)
{
    on = false;
    waiting = false;
    log("ADDRESSOFF ok=1");
}

void On(LogFn log)
{
    on = true;
    log("ADDRESSON ok=1");
}
}
