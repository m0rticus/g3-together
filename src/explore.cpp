// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore.h"
#include "explore_internal.h"
#include "log.h"

#include "ai.h"
#include "body.h"
#include "npcs.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::explore {
using detail::ParseXyz;
using detail::PositionOf;

void Summary(LogFn log)
{
    char line[220];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("REFUSED npcsummary: no hero"); return; }
    float at[3];
    PositionOf(hero, at);
    struct Row { float distance; Entity npc; };
    std::vector<Row> rows;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == hero || remote::IsPuppet(instance)) continue;
        float p[3];
        PositionOf(instance, p);
        rows.push_back({body::Distance(p, at), npc});
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.distance < b.distance; });
    Format(line, "NPCSUMMARY npcs=%zu entities=%d farthest_m=%.0f", rows.size(), Entity::GetEntities().GetCount(),
              rows.empty() ? 0.0f : rows.back().distance / 100.0f);
    log(line);
    // About 200 samples spread over all distances: enough to pick targets at any range.
    for (size_t i = 0; i < rows.size(); i += (rows.size() > 200 ? rows.size() / 200 : 1)) {
        float p[3];
        PositionOf(rows[i].npc.GetInstance(), p);
        Format(line, "NPCAT name=%s distance_m=%.0f pos=%.0f_%.0f_%.0f dead=%d", rows[i].npc.GetName().GetText(),
                  rows[i].distance / 100.0f, p[0], p[1], p[2], rows[i].npc.IsDead() ? 1 : 0);
        log(line);
    }
    log("NPCSUMMARY end");
}

void Teleport(char const* xyz, LogFn log)
{
    char line[200];
    float target[3];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !ParseXyz(xyz, target)) { log("REFUSED teleport: no hero or bad X_Y_Z"); return; }
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, target, rotation));
    float now[3];
    PositionOf(hero, now);
    Format(line, "TELEPORT target=%s pos=%.0f_%.0f_%.0f", xyz, now[0], now[1], now[2]);
    log(line);
}

void HeroPos(LogFn log)
{
    char line[200];
    eCEntity* body = remote::ControlledBody();
    if (!body) { log("REFUSED heropos: no hero"); return; }
    float at[3];
    PositionOf(body, at);
    Entity zone = Entity(body).GetZone();
    Format(line, "HEROPOS pos=%.1f,%.1f,%.1f zone=%s ani_state=%d", at[0], at[1], at[2], zone.GetInstance() ? zone.GetName().GetText() : "-",
              static_cast<int>(Entity(body).Routine.AniState));
    log(line);
}

// QA: the hero runs to a point ("X_Y_Z" in cm) the game's way (AI goto, as walkto): moving, NPCs near it can hear it.
void RunTo(char const* argument, LogFn log)
{
    char line[200];
    float target[3];
    eCEntity* body = remote::ControlledBody();
    if (!body || !ParseXyz(argument, target)) { log("REFUSED runto: no hero or not X_Y_Z"); return; }
    const GEBool started = Entity(body).StartGoto(bCVector(target[0], target[1], target[2]), gEWalkMode_Run);
    Format(line, "RUNTO started=%d target=%.0f,%.0f,%.0f", started ? 1 : 0, target[0], target[1], target[2]);
    log(line);
}

// QA: set one of the hero's attributes ("STR_300": tag and base value, the tags of hero.cpp), e.g. for a strength test.
void SetAttribute(char const* argument, LogFn log)
{
    char line[160], tag[8] = {};
    int value = 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || sscanf_s(argument, "%3[A-Z]_%d", tag, static_cast<unsigned>(sizeof(tag)), &value) != 2) { log("REFUSED setattr: no hero or not TAG_value"); return; }
    Entity self(hero);
    self.PlayerMemory.SetBaseValue(bCString(tag), value);
    Format(line, "SETATTR tag=%s value=%d strength=%d", tag, value, static_cast<int>(self.PlayerMemory.GetStrength()));
    log(line);
}

// QA: look at a named entity: the hero 4 m away from it on the line from where it stands, facing it; the camera behind
// the hero then shows the entity (the engine animates bodies on screen; deaddiag measures corpses this way).
void LookAt(char const* name, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    // "puppet<id>": the puppet of that player (its template name, PC_Hero, finds the hero itself).
    Entity target = !std::strncmp(name, "puppet", 6) ? Entity(remote::PuppetOf(std::atoi(name + 6))) : Entity::GetEntity(bCString(name));
    if (!hero || !target.GetInstance()) { log("REFUSED lookat: no hero or no such entity"); return; }
    float from[3], at[3];
    PositionOf(hero, from);
    PositionOf(target.GetInstance(), at);
    float dx = at[0] - from[0], dz = at[2] - from[2];
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length < 1.0f) dx = 0.0f, dz = 1.0f;
    else dx /= length, dz /= length;
    const float yaw = std::atan2(dx, dz);
    const float rotation[4] = {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)};
    const float place[3] = {at[0] - dx * 400.0f, at[1] + 50.0f, at[2] - dz * 400.0f};
    Entity(hero).MoveTo(body::Pose(*hero, place, rotation));
    Format(line, "LOOKAT name=%s pos=%.0f_%.0f_%.0f", name, place[0], place[1], place[2]);
    log(line);
}

void Near(char const* xyz, LogFn log)
{
    char line[260];
    float at[3];
    if (!ParseXyz(xyz, at)) { log("REFUSED near: bad X_Y_Z"); return; }
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bTObjArray<Entity> all = Entity::GetNPCs();
    int count = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == hero || remote::IsPuppet(instance)) continue;
        float p[3];
        PositionOf(instance, p);
        if (body::Distance(p, at) > 4000.0f) continue;
        Format(line, "NEAR name=%s pos=%.0f_%.0f_%.0f task=%s hp=%d dead=%d mirrored=%d", npc.GetName().GetText(), p[0], p[1], p[2],
                  npc.Routine.GetCurrentTask().GetText(), static_cast<int>(npc.DamageReceiver.HitPoints), npc.IsDead() ? 1 : 0,
                  npcs::IsMirrored(instance) ? 1 : 0);
        log(line);
        ++count;
    }
    int entities = 0;
    const bTObjArray<Entity> every = Entity::GetEntities();
    for (int i = 0; i < every.GetCount(); ++i) {
        eCEntity* instance = every.GetAt(i).GetInstance();
        if (!instance) continue;
        float p[3];
        PositionOf(instance, p);
        if (body::Distance(p, at) <= 4000.0f) ++entities;
    }
    Format(line, "NEARSUM at=%s npcs=%d entities=%d", xyz, count, entities);
    log(line);
    log("NEAR end");
}

// QA: NPCs anywhere in the world, found through the owners of dialog options (names resolve even far away?),
// as teleport targets on solid ground. FARNPC lines, then "FARNPC end".
void FarNpcs(LogFn log)
{
    char line[220];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    if (!hero || !infos) { log("REFUSED farnpcs: no hero or info manager"); return; }
    float at[3];
    PositionOf(hero, at);
    std::vector<std::string> owners;
    auto& all = infos->AccessInfos();
    for (int i = 0; i < all.GetCount(); ++i)
        if (gCInfo_PS* info = all.AccessAt(i)) {
            std::string owner = info->AccessOwner().GetText();
            if (!owner.empty() && std::find(owners.begin(), owners.end(), owner) == owners.end()) owners.push_back(owner);
        }
    int resolved = 0;
    struct Row { float distance; std::string name; float pos[3]; };
    std::vector<Row> rows;
    for (auto const& owner : owners) {
        Entity npc = Entity::GetEntity(bCString(owner.c_str()));
        if (!npc.GetInstance()) continue;
        ++resolved;
        Row row{0, owner, {}};
        PositionOf(npc.GetInstance(), row.pos);
        row.distance = body::Distance(row.pos, at);
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.distance < b.distance; });
    Format(line, "FARNPCS owners=%zu resolved=%d", owners.size(), resolved);
    log(line);
    for (size_t i = 0; i < rows.size(); i += (rows.size() > 60 ? rows.size() / 60 : 1)) {
        Format(line, "FARNPC name=%s distance_m=%.0f pos=%.0f_%.0f_%.0f", rows[i].name.c_str(), rows[i].distance / 100.0f,
                  rows[i].pos[0], rows[i].pos[1], rows[i].pos[2]);
        log(line);
    }
    log("FARNPC end");
}

// QA: one named entity wherever it is: position, task, distance to the hero. Truncated, not sprintf_s: a line longer than
// the buffer ended the game (0xC0000409; a mirrored escaped slave 1694 m off with a long-named target is 241 characters:
// the host's crashes in stageEscort 25.10. and stageEscortFar 26.10., both waiting for this reply).
void Find(char const* name, LogFn log)
{
    char line[512];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Entity::GetEntity(bCString(name));
    if (!hero || !npc.GetInstance()) { _snprintf_s(line, sizeof(line), _TRUNCATE, "FIND name=%s found=0", name); log(line); return; }
    float p[3], h[3];
    PositionOf(npc.GetInstance(), p);
    PositionOf(hero, h);
    const bool damageable = npc.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver) != nullptr;
    Entity target = npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC) ? npc.NPC.GetCurrentTarget() : Entity();
    _snprintf_s(line, sizeof(line), _TRUNCATE, "FIND name=%s found=1 pos=%.0f_%.0f_%.0f task=%s distance_m=%.0f mirrored=%d hp=%d dead=%d state=%s target=%s ai_mode=%d target_puppet=%d target_hero=%d owner=%u", name,
              p[0], p[1], p[2], npc.Routine.GetCurrentTask().GetText(), body::Distance(p, h) / 100.0f, npcs::IsMirrored(npc.GetInstance()) ? 1 : 0,
              damageable ? static_cast<int>(npc.DamageReceiver.HitPoints) : -1, npc.IsDead() ? 1 : 0, npc.Routine.GetCurrentState().GetText(),
              target.GetInstance() ? target.GetName().GetText() : "-", static_cast<int>(npc.Routine.AIMode),
              remote::PuppetOwner(target.GetInstance()), target.GetInstance() && target.GetInstance() == hero ? 1 : 0,
              npcs::OwnerOf(npc.GetInstance()));
    log(line);
}

// QA (P2, PS_QuickUse): the item on quick slot 1 and the Community Patch's key handler for it (OnPlayerUseQuickSlot, Script_paru
// 0xd450: FindStackIndexByQuickSlot, ExecuteItem), as pressing the slot's key does.
void QuickUse(char const* item, LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("REFUSED quickuse: no hero"); return; }
    Entity self(hero);
    const GEInt stack = self.Inventory.FindStackIndex(Template(item));
    if (stack < 0) { Format(line, "REFUSED quickuse %s: not carried", item); log(line); return; }
    const GEInt before = self.Inventory.GetAmount(stack);
    const GEBool assigned = self.Inventory.AssignStackToQuickSlot(stack, 1);
    const GEInt result = GetScriptAdminExt().CallScript(bCString("OnPlayerUseQuickSlot"), hero, nullptr, 1);
    Format(line, "QUICKUSE item=%s stack=%d before=%d assigned=%d result=%d task=%s", item, stack, before, assigned ? 1 : 0, result,
              self.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA (P2, PS_Swim): gECharMovementMode of the hero and of each puppet (7 swim, 8 dive, 10 wade).
void MoveMode(LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int n = Format(line, "MOVEMODE hero=%d", hero ? static_cast<int>(Entity(hero).CharacterMovement.GetMovementMode()) : -1);
    for (int id = 1; id <= 4 && n > 0; ++id)
        if (eCEntity* puppet = remote::PuppetOf(id))
            n += Format(line + n, sizeof(line) - n, " puppet%d=%d", id, static_cast<int>(Entity(puppet).CharacterMovement.GetMovementMode()));
    log(line);
}

void Sector(char const* name, LogFn log)
{
    char line[160];
    Format(line, "SECTOR name=%s on=%d", name, world.GetSectorStatus(bCString(name)) ? 1 : 0);
    log(line);
}

// QA: the game's own teleport of the hero to a named entity (as teleport stones do), not a bare MoveTo.
void TeleportTo(char const* name, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity target = Entity::GetEntity(bCString(name));
    if (!hero || !target.GetInstance()) { log("REFUSED teleportto: no hero or target"); return; }
    const GEBool ok = Entity(hero).Teleport(target);
    float p[3];
    PositionOf(hero, p);
    Format(line, "TELEPORTTO name=%s ok=%d pos=%.0f_%.0f_%.0f", name, ok ? 1 : 0, p[0], p[1], p[2]);
    log(line);
}

// QA: the GUI pages that may pause the game.
void Page(char const* what, LogFn log)
{
    char line[200];
    gCSession& session = gCSession::GetInstance();
    gCGUIManager* gui = session.GetGUIManager();
    if (!gui) { log("REFUSED page: no GUI manager"); return; }
    GEBool ok = GETrue;
    if (!std::strcmp(what, "inventory")) ok = gui->OpenInventory();
    else if (!std::strcmp(what, "menu")) session.OpenMenu();
    else if (!std::strcmp(what, "close")) { ok = gui->ClosePage(); gui->CloseMenu(); }
    else if (std::strcmp(what, "state")) { log("REFUSED page: inventory|menu|close|state"); return; }
    Format(line, "PAGE what=%s ok=%d paused=%d active_page=%d", what, ok ? 1 : 0, session.IsPaused() ? 1 : 0,
              static_cast<int>(gui->GetActivePage()));
    log(line);
}

// QA, first human test: every NPC within 40 m of the hero with its target and the motion it shows, to compare what
// the two machines show. NEARX lines, then "NEARX end".
void NearNpcs(LogFn log)
{
    char line[1000];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("NEARX end"); return; }
    float h[3];
    PositionOf(hero, h);
    const bTObjArray<Entity> all = Entity::GetNPCs();
    int puppetsListed = 0, puppetAlignment = -1, puppetSpecies = -1, puppetAIMode = -1;
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (instance && remote::IsPuppet(instance)) {
            // The game's perception scanner only looks at Entity::GetNPCs(): a puppet missing here is invisible to NPCs.
            ++puppetsListed;
            if (instance->GetPropertySet(eEPropertySetType_NPC)) {
                puppetAlignment = static_cast<int>(static_cast<gEPoliticalAlignment>(npc.NPC.PoliticalAlignment));
                puppetSpecies = static_cast<int>(static_cast<gESpecies>(npc.NPC.Species));
            }
            puppetAIMode = body::AIModeOf(instance);
        }
        if (!instance || instance == hero || remote::IsPuppet(instance)) continue;
        float p[3];
        PositionOf(instance, p);
        if (body::Distance(p, h) > 4000.0f) continue;
        Entity target = instance->GetPropertySet(eEPropertySetType_NPC) ? npc.NPC.GetCurrentTarget() : Entity();
        char motion[128] = "-";
        body::CurrentMotion(instance, motion, sizeof(motion));
        const eCEntityProxy proxy(instance);
        bCPropertyID::bSCore core;
        std::memcpy(&core, &proxy.GetEntityID().GetCore(), 16);
        const Entity held = npc.Inventory.GetItemFromSlot(gESlot_RightHand);
        // Is slot 0 really playing? A body with nothing running there stands in its bind pose (T-pose).
        int running = -1, weight = -1;
        if (auto* animation = static_cast<eCVisualAnimation_PS*>(instance->GetPropertySet(eEPropertySetType_Animation)))
            if (eCWrapper_emfx2Actor* actor = animation->GetActor()) {
                const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
                running = actor->IsMotionRunning(slot0) ? 1 : 0;
                weight = static_cast<int>(actor->GetTargetWeight(slot0) * 100.0f);
            }
        // Facing: the body's forward vector as a compass angle, to compare both machines.
        const bCVector forward = instance->GetAtVector();
        const int yaw = static_cast<int>(std::atan2(forward.GetX(), forward.GetZ()) * 57.29578f);
        Format(line, "NEARX guid=%08X%04X name=%s dead=%d mirrored=%d owner=%u target=%s target_hero=%d target_puppet=%d task=%s ani=%d hand=%s motion=%s state=%s run0=%d weight0=%d yaw=%d",
                  core.Data1, core.Data2, npc.GetName().GetText(), npc.IsDead() ? 1 : 0, npcs::IsMirrored(instance) ? 1 : 0,
                  npcs::OwnerOf(instance), target.GetInstance() ? target.GetName().GetText() : "-",
                  target.GetInstance() && target.GetInstance() == hero ? 1 : 0, remote::PuppetOwner(target.GetInstance()),
                  npc.Routine.GetCurrentTask().GetText(), static_cast<int>(npc.Routine.AniState),
                  held.GetInstance() ? held.GetName().GetText() : "-", *motion ? motion : "-",
                  npc.Routine.GetCurrentState().GetText(), running, weight, yaw);
        log(line);
    }
    Format(line, "NEARX end actions_started=%u actions_shown=%u actions_played=%u holds=%u pacifies=%u owner_releases=%u combat_moves=%u combat_move_faults=%u motion_owner=%d "
                    "attitude_installed=%d attitude_native=%d attitude_asked=%u attitude_redirected=%u attitude_flee_kept=%u puppets_in_npcs=%d puppet_alignment=%d "
                    "puppet_species=%d puppet_aimode=%d replay_started=%u/%u/%u replay_played=%u/%u/%u replay_shown=%u/%u/%u script_anis=%u script_ani_faults=%u loop_restarts=%u",
              body::actionsStarted, body::actionsShown, body::actionsPlayed, npcs::HoldChanges(), body::pacifies, body::ownerReleases, body::combatMoves, body::combatMoveFaults, body::motionOwner,
              ai::AttitudeInstalled() ? 1 : 0, ai::AttitudeNative() ? 1 : 0, ai::AttitudeAsked(), ai::AttitudeRedirected(), ai::AttitudeFleeKept(), puppetsListed, puppetAlignment,
              puppetSpecies, puppetAIMode, body::kindStarted[0], body::kindStarted[1], body::kindStarted[2], body::kindPlayed[0],
              body::kindPlayed[1], body::kindPlayed[2], body::kindShown[0], body::kindShown[1], body::kindShown[2],
              body::scriptAnis, body::scriptAniFaults, body::loopRestarts);
    log(line);
}

namespace {
bCVector swimGoal;
DWORD swimUntil = 0;
}

// QA (P2, PS_Swim; coop evening 26.10.): the hero swims toward a point by its own character movement, as body.cpp steers a
// puppet (movement goal), no key input. "DX_DZ_MS": the goal relative to where the hero is, held for MS milliseconds.
void SwimGoal(char const* argument, LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int dx = 0, dz = 0, ms = 0;
    if (!hero || sscanf_s(argument, "%d_%d_%d", &dx, &dz, &ms) != 3 || ms <= 0) { log("REFUSED swimgoal: no hero or not DX_DZ_MS"); return; }
    const bCVector& p = hero->GetWorldPosition();
    swimGoal = bCVector(p.GetX() + static_cast<float>(dx), p.GetY(), p.GetZ() + static_cast<float>(dz));
    swimUntil = GetTickCount() + static_cast<DWORD>(ms);
    Entity e(hero);
    e.CharacterMovement.SetMovementGoal(swimGoal);
    Format(line, "SWIMGOAL from=%.0f_%.0f_%.0f goal=%.0f_%.0f_%.0f ms=%d mode=%d task=%s", p.GetX(), p.GetY(), p.GetZ(), swimGoal.GetX(),
              swimGoal.GetY(), swimGoal.GetZ(), ms, static_cast<int>(e.CharacterMovement.GetMovementMode()), e.Routine.GetCurrentTask().GetText());
    log(line);
}

void SwimTick()
{
    if (!swimUntil) return;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { swimUntil = 0; return; }
    Entity e(hero);
    if (static_cast<int>(GetTickCount() - swimUntil) > 0) {
        e.CharacterMovement.StopMovement();
        swimUntil = 0;
        return;
    }
    e.CharacterMovement.SetMovementGoal(swimGoal);
}
}  // namespace g3t::explore
