// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore.h"
#include "explore_internal.h"

#include "ai.h"
#include "body.h"
#include "guards.h"
#include "infocmd.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace g3t::explore {
namespace {
// An enclave by its own name, else the enclave of the NPC of that name.
Entity EnclaveNamed(char const* name)
{
    const GEInt count = PSEnclave::GetEnclaveCount();
    for (GEInt i = 0; i < count; ++i) {
        Entity e = PSEnclave::GetEnclaveAt(i);
        if (e.GetInstance() && !_stricmp(e.GetName().GetText(), name)) return e;
    }
    Entity npc = Entity::GetEntity(bCString(name));
    auto* ps = npc.GetInstance() ? static_cast<gCNPC_PS*>(npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) : nullptr;
    eCEntity* town = ps ? ps->GetEnclaveProxy().GetEntity() : nullptr;
    return town ? Entity(town) : Entity();
}
}

void Attitude(char const* name, LogFn log)
{
    char line[260];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Entity::GetEntity(bCString(name));
    if (!hero || !npc.GetInstance()) { log("REFUSED attitude: no hero or no such NPC"); return; }
    auto& admin = GetScriptAdminExt();
    const GEInt toHero = admin.CallScript(bCString("GetAttitude"), npc.GetInstance(), hero, 0);
    eCEntity* puppet = remote::PuppetOf(2);
    const GEInt toPuppet = puppet ? admin.CallScript(bCString("GetAttitude"), npc.GetInstance(), puppet, 0) : -1;
    // P7 (26.10.): the parts the game asks for its own player, as this machine's hero sees them (the focus name's colour).
    const GEInt toPlayer = admin.CallScript(bCString("GetAttitudeToPlayer"), npc.GetInstance(), hero, 0);
    const GEInt focus = admin.CallScript(bCString("GetFocusNameAttitude"), npc.GetInstance(), hero, 0);
    Format(line, "ATTITUDE name=%s hero=%d puppet=%d task=%s to_player=%d focus=%d", name, toHero, toPuppet,
           npc.Routine.GetCurrentTask().GetText(), toPlayer, focus);
    log(line);
}

// QA, regional-authority spike: kill a named NPC wherever it is, the way the host kills for a client
// (hit points to 0, then the kill cheat with the hero as killer). Far NPCs are not processed by the engine.
void FarKill(char const* name, LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = detail::ResolveEntity(name);
    if (!hero || !npc.GetInstance() || npc.IsDead() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver)) {
        Format(line, "REFUSED farkill %s: no hero, no such NPC, already dead or not damageable", name);
        log(line);
        return;
    }
    auto& admin = GetScriptAdminExt();
    admin.CallScript(bCString("AddHitPoints"), npc.GetInstance(), hero, -static_cast<GEInt>(npc.DamageReceiver.HitPoints));
    admin.CallScript(bCString("OnCheatKill"), hero, npc.GetInstance(), 0);
    Format(line, "FARKILL name=%s hp=%d dead=%d task=%s", name, static_cast<int>(npc.DamageReceiver.HitPoints), npc.IsDead() ? 1 : 0,
           npc.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA (main story): the chapter lives in the local hero's PlayerMemory (SetChapter, info command 53); GetAttitude reads it
// through Entity::GetPlayer (chapter 2: the chosen god's enemies, re/out/notify-enclave-predicate-20261024-c.c).
void Chapter(LogFn log)
{
    char line[80];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Format(line, "CHAPTER value=%d", hero ? static_cast<int>(Entity(hero).PlayerMemory.Chapter) : -1);
    log(line);
}

// QA (P7 attitudes, 26.10. pair 1): set the local hero's chapter (story.cpp shares it with the others).
void SetChapter(char const* value, LogFn log)
{
    char line[120];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !value || !*value) { log("REFUSED setchapter: no hero or no value"); return; }
    Entity(hero).PlayerMemory.Chapter = std::atoi(value);
    Format(line, "SETCHAPTER value=%d", static_cast<int>(Entity(hero).PlayerMemory.Chapter));
    log(line);
}

// QA (stageEscort): a named NPC wherever it is gets its hit points back (the weak escaped slave died to wolves on the
// way to Freman, 25.10.).
void FarHeal(char const* name, LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Entity::GetEntity(bCString(name));
    if (!hero || !npc.GetInstance() || npc.IsDead() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver)) {
        Format(line, "REFUSED farheal %s: no hero, no such NPC, dead or not damageable", name);
        log(line);
        return;
    }
    const int before = static_cast<int>(npc.DamageReceiver.HitPoints);
    GetScriptAdminExt().CallScript(bCString("AddHitPoints"), npc.GetInstance(), hero, 100000);
    Format(line, "FARHEAL name=%s before=%d hp=%d", name, before, static_cast<int>(npc.DamageReceiver.HitPoints));
    log(line);
}

// QA, regional-authority spike: move a named entity wherever it is ("X_Y_Z" in cm), keeping its rotation.
void FarMove(char const* argument, LogFn log)
{
    char line[200], name[96] = {};
    float target[3];
    // QA commands take one token of [A-Za-z0-9_-]: X_Y_Z_<name>, the name after the third underscore.
    int consumed = 0;
    if (!argument || sscanf_s(argument, "%f_%f_%f_%n", &target[0], &target[1], &target[2], &consumed) != 3 ||
        !consumed || std::strlen(argument + consumed) >= sizeof(name)) {
        log("REFUSED farmove: X_Y_Z_<name>");
        return;
    }
    strcpy_s(name, argument + consumed);
    Entity entity = Entity::GetEntity(bCString(name));
    if (!entity.GetInstance()) { Format(line, "REFUSED farmove %s: not found", name); log(line); return; }
    const bCQuaternion q = entity.GetInstance()->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    entity.MoveTo(body::Pose(*entity.GetInstance(), target, rotation));
    float now[3];
    detail::PositionOf(entity.GetInstance(), now);
    Format(line, "FARMOVE name=%s target=%.0f_%.0f_%.0f pos=%.0f_%.0f_%.0f", name, target[0], target[1], target[2], now[0], now[1],
           now[2]);
    log(line);
}

// QA, stage H2: dialog-relevant state of a named NPC; with `change`, flip its trade flag and guard status the way
// SetTradeEnabled / SetGuardStatus would, and pass the result on like after a dialog command.
void NpcState(char const* name, bool change, LogFn log)
{
    char line[300];
    Entity npc = Entity::GetEntity(bCString(name));
    eCEntity* instance = npc.GetInstance();
    if (!instance || !instance->GetPropertySet(eEPropertySetType_Dialog) || !instance->GetPropertySet(eEPropertySetType_NPC)) {
        Format(line, "REFUSED npcstate %s: no such NPC with a dialog", name);
        log(line);
        return;
    }
    if (change) {
        npc.Dialog.TradeEnabled = !npc.Dialog.TradeEnabled;
        npc.NPC.GuardStatus = npc.NPC.GuardStatus == gEGuardStatus_Active ? gEGuardStatus_Inactive : gEGuardStatus_Active;
        infocmd::Changed(instance);
    }
    auto* nav = static_cast<gCNavigation_PS*>(instance->GetPropertySet(eEPropertySetType_Navigation));
    Format(line, "NPCSTATE name=%s trade=%d teach=%d guard=%d routine=%s", name, npc.Dialog.TradeEnabled ? 1 : 0,
           npc.Dialog.TeachEnabled ? 1 : 0, static_cast<int>(npc.NPC.GuardStatus), nav ? nav->AccessRoutine().GetText() : "-");
    log(line);
}

// QA (followdiag): the NPC stops and chooses its routine again, as at a dialog's end (FullStop, ContinueRoutine): with a
// follow quest running, SelectRoutineTask starts its guiding.
void NpcRoutine(char const* name, LogFn log)
{
    char line[200];
    Entity npc = Entity::GetEntity(bCString(name));
    if (!npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) { log("REFUSED npcroutine: no such NPC"); return; }
    npc.Routine.FullStop();
    npc.Routine.ContinueRoutine();
    Format(line, "NPCROUTINE name=%s task=%s", name, npc.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA, stage H3: let a named NPC attack player 2's puppet ("Name_Reason", reason a gEAttackReason number), the way
// the dialog command Attack does (the perception script AssessTarget).
void Aggro(char const* argument, LogFn log)
{
    char line[300], name[96] = {};
    char const* sep = argument ? std::strrchr(argument, '_') : nullptr;
    if (!sep || sep - argument >= static_cast<ptrdiff_t>(sizeof(name))) { log("REFUSED aggro: Name_Reason"); return; }
    std::memcpy(name, argument, sep - argument);
    const int reason = std::atoi(sep + 1);
    Entity npc = Entity::GetEntity(bCString(name));
    eCEntity* puppet = remote::PuppetOf(net::IsHost() ? 2 : 1);  // the other player of a two-player test
    if (!npc.GetInstance() || !puppet) { log("REFUSED aggro: no such NPC or no puppet"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTarget"), npc.GetInstance(), puppet, reason);
    Format(line, "AGGRO name=%s reason=%d result=%d task=%s", name, reason, result, npc.Routine.GetCurrentTask().GetText());
    log(line);
}

// The same against this machine's own hero (stage Ransack: the host's baseline of a robbery).
void AssessHero(char const* argument, LogFn log)
{
    char line[300], name[96] = {};
    char const* sep = argument ? std::strrchr(argument, '_') : nullptr;
    if (!sep || sep - argument >= static_cast<ptrdiff_t>(sizeof(name))) { log("REFUSED assesshero: Name_Reason"); return; }
    std::memcpy(name, argument, sep - argument);
    const int reason = std::atoi(sep + 1);
    Entity npc = Entity::GetEntity(bCString(name));
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!npc.GetInstance() || !hero) { log("REFUSED assesshero: no such NPC or no hero"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTarget"), npc.GetInstance(), hero, reason);
    Format(line, "ASSESSHERO name=%s reason=%d result=%d task=%s", name, reason, result, npc.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA, stage E4: "Attacker_Target": an NPC attacks another NPC (AssessTarget, reason Enemy). The target's name is the
// part after the last underscore; "hero": this machine's hero (upsetdiag 26.10., the single-player reference).
void NpcFight(char const* argument, LogFn log)
{
    char line[300];
    std::string arg = argument ? argument : "";
    const size_t cut = arg.rfind('_');
    if (cut == std::string::npos) { log("REFUSED npcfight: Attacker_Target"); return; }
    Entity attacker = Entity::GetEntity(bCString(arg.substr(0, cut).c_str()));
    const std::string targetSpec = arg.substr(cut + 1);
    Entity target = targetSpec == "hero" ? Entity(gCSession::GetInstance().GetHero())
                                         : detail::ResolveEntity(targetSpec.c_str());
    if (!attacker.GetInstance() || !target.GetInstance()) { log("REFUSED npcfight: no such NPC"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTarget"), attacker.GetInstance(), target.GetInstance(), gEAttackReason_Enemy);
    Format(line, "NPCFIGHT attacker=%s target=%s result=%d task=%s", arg.substr(0, cut).c_str(), targetSpec.c_str(), result,
           attacker.Routine.GetCurrentTask().GetText());
    log(line);
}

// The game's script for an NPC that gives up a fight (CancelAttack 0x1a4d0: LastFightAgainstPlayer Cancel and the time for
// its player, back to its routine), against its current target: where the chase would end it (upsetdiag 26.10.).
void CancelAttack(char const* name, LogFn log)
{
    char line[300];
    Entity npc = Entity::GetEntity(bCString(name));
    if (!npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) { log("REFUSED cancelattack: no such NPC"); return; }
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    const GEInt result = GetScriptAdminExt().CallScript(bCString("CancelAttack"), npc.GetInstance(), target, 0);
    Format(line, "CANCELATTACK name=%s target=%s result=%d task=%s last=%d", name, target ? Entity(target).GetName().GetText() : "-", result,
           npc.Routine.GetCurrentTask().GetText(), static_cast<int>(npc.NPC.LastFightAgainstPlayer));
    log(line);
}

// As a duel starts (attack reason Duel): the game ends it with a knock-out (Defeat), not a death (PLAN-2026-10-22 2.2).
void DuelHero(char const* name, LogFn log)
{
    char line[300];
    Entity attacker = Entity::GetEntity(bCString(name));
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!attacker.GetInstance() || !hero) { log("REFUSED duelhero: no such NPC or no hero"); return; }
    if (npcs::IsMirrored(attacker.GetInstance())) {  // the host's NPC: its owner starts the duel, as for a dialog's duel
        net::WorldState s{};
        s.kind = 3;
        guid::CopyEntity(attacker.GetInstance(), s.guid);
        s.guard = net::LocalId();
        strncpy_s(s.routine, "Duel", _TRUNCATE);
        net::SendWorldState(s, 0);
        Format(line, "DUELHERO attacker=%s result=sent_to_owner task=%s", name, attacker.Routine.GetCurrentTask().GetText());
        log(line);
        return;
    }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTarget"), attacker.GetInstance(), hero, gEAttackReason_Duel);
    Format(line, "DUELHERO attacker=%s result=%d task=%s", name, result, attacker.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA (P7, GetZoneAttitudeToPlayer): an NPC's zone attitude and attitude to the player (the scripts read GetPlayer's zone,
// security level and fame) for the hero, and for puppet 2 asked plainly and with the puppet as the game's player.
void ZoneAttitude(char const* name, LogFn log)
{
    char line[700];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bool noNpc = !name || !*name || !std::strcmp(name, "-");
    Entity npc = noNpc ? Entity() : Entity::GetEntity(bCString(name));
    if (!hero || (!noNpc && !npc.GetInstance())) { log("REFUSED zoneatt: no hero or no such NPC"); return; }
    auto& admin = GetScriptAdminExt();
    eCEntity* puppet = remote::PuppetOf(2);
    auto zoneOf = [](eCEntity* e, char* out, size_t size) {
        Entity zone = e ? Entity(e).GetZone() : Entity();
        Entity owner = zone.GetInstance() ? zone.GetAIZoneOwner() : Entity();
        Format(out, size, "%s/%d/%s", zone.GetInstance() ? zone.GetName().GetText() : "-",
               zone.GetInstance() ? static_cast<int>(zone.GetAIZoneSecurityLevel()) : -1, owner.GetInstance() ? owner.GetName().GetText() : "-");
    };
    char heroZone[160], puppetZone[160];
    zoneOf(hero, heroZone, sizeof(heroZone));
    zoneOf(puppet, puppetZone, sizeof(puppetZone));
    GEInt heroZoneAtt = -9, heroAtt = -9, plainZoneAtt = -9, actingZoneAtt = -9, actingAtt = -9, puppetAtt = -9;
    if (npc.GetInstance()) {
        heroZoneAtt = admin.CallScript(bCString("GetZoneAttitudeToPlayer"), npc.GetInstance(), hero, 0);
        heroAtt = admin.CallScript(bCString("GetAttitudeToPlayer"), npc.GetInstance(), hero, 0);
    }
    eCEntity* heroZoneEntity = Entity(hero).GetZone().GetInstance();
    const GEInt zoneZatt = heroZoneEntity ? admin.CallScript(bCString("GetZoneAttitudeToPlayer"), heroZoneEntity, nullptr, 0) : -9;
    GEInt puppetZoneZatt = -9;
    if (puppet) {
        eCEntity* puppetZoneEntity = Entity(puppet).GetZone().GetInstance();
        if (npc.GetInstance()) {
            plainZoneAtt = admin.CallScript(bCString("GetZoneAttitudeToPlayer"), npc.GetInstance(), puppet, 0);
            puppetAtt = admin.CallScript(bCString("GetAttitude"), npc.GetInstance(), puppet, 0);
        }
        guards::Acting acting(puppet);
        if (npc.GetInstance()) {
            actingZoneAtt = admin.CallScript(bCString("GetZoneAttitudeToPlayer"), npc.GetInstance(), puppet, 0);
            actingAtt = admin.CallScript(bCString("GetAttitudeToPlayer"), npc.GetInstance(), puppet, 0);
        }
        if (puppetZoneEntity) puppetZoneZatt = admin.CallScript(bCString("GetZoneAttitudeToPlayer"), puppetZoneEntity, nullptr, 0);
    }
    Format(line, "ZONEATT name=%s hero_zone=%s hero_zoneatt=%d hero_att=%d puppet_zone=%s puppet_zoneatt=%d puppet_zoneatt_acting=%d"
                 " puppet_att_acting=%d puppet_attitude=%d zone_zatt=%d puppet_zone_zatt=%d", noNpc ? "-" : name, heroZone, heroZoneAtt, heroAtt,
           puppetZone, plainZoneAtt, actingZoneAtt, actingAtt, puppetAtt, zoneZatt, puppetZoneZatt);
    log(line);
}

// QA (P7 attitudes, 26.10. pair 1): the parts of GetAttitude for an NPC and this machine's hero, and for puppet 2.
void AttitudeChain(char const* name, LogFn log)
{
    char line[700];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Entity::GetEntity(bCString(name));
    if (!hero || !npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) { log("REFUSED attchain: no hero or no such NPC"); return; }
    auto& admin = GetScriptAdminExt();
    auto ask = [&](char const* script, eCEntity* other) { return other ? static_cast<int>(admin.CallScript(bCString(script), npc.GetInstance(), other, 0)) : -9; };
    auto align = [](eCEntity* e) { return e && e->GetPropertySet(eEPropertySetType_NPC) ? static_cast<int>(static_cast<gEPoliticalAlignment>(Entity(e).NPC.PoliticalAlignment)) : -1; };
    auto species = [](eCEntity* e) { return e && e->GetPropertySet(eEPropertySetType_NPC) ? static_cast<int>(static_cast<gESpecies>(Entity(e).NPC.Species)) : -1; };
    auto chapter = [](eCEntity* e) { return e && e->GetPropertySet(eEPropertySetType_PlayerMemory) ? static_cast<int>(Entity(e).PlayerMemory.Chapter) : -1; };
    eCEntity* puppet = remote::PuppetOf(2);
    Format(line, "ATTCHAIN name=%s npc_align=%d npc_species=%d hero_align=%d hero_species=%d puppet_align=%d puppet_species=%d"
                 " political_hero=%d species_hero=%d revolution_hero=%d to_player=%d attitude_hero=%d"
                 " political_puppet=%d species_puppet=%d attitude_puppet=%d attitude_puppet_plain=%d hero_chapter=%d puppet_chapter=%d task=%s",
           name, align(npc.GetInstance()), species(npc.GetInstance()), align(hero), species(hero), align(puppet), species(puppet),
           ask("GetPoliticalAttitude", hero), ask("GetSpeciesAttitude", hero), ask("GetRevolutionAttitude", hero), ask("GetAttitudeToPlayer", hero),
           ask("GetAttitude", hero), ask("GetPoliticalAttitude", puppet), ask("GetSpeciesAttitude", puppet), ask("GetAttitude", puppet),
           puppet ? ai::PlainAttitude(npc.GetInstance(), puppet) : -9, chapter(hero), chapter(puppet), npc.Routine.GetCurrentTask().GetText());
    log(line);
    auto* ps = static_cast<gCNPC_PS*>(npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC));
    Entity zone = npc.GetZone();
    Entity owner = zone.GetInstance() ? zone.GetAIZoneOwner() : Entity();
    eCEntity* town = ps ? ps->GetEnclaveProxy().GetEntity() : nullptr;
    auto political = [&](eCEntity* other) { return other ? static_cast<int>(admin.CallScript(bCString("GetPoliticalAttitude"), npc.GetInstance(), other, 0)) : -9; };
    const int ownerHero = political(owner.GetInstance()), townHero = political(town);
    int ownerActing = -9, townActing = -9;
    if (puppet) {
        guards::Acting acting(puppet);
        ownerActing = political(owner.GetInstance());
        townActing = political(town);
    }
    Format(line, "ATTCHAIN2 name=%s zone_owner=%s political_owner=%d political_owner_acting=%d town=%s political_town=%d political_town_acting=%d puppet_chapter=%d",
           name, owner.GetInstance() ? owner.GetName().GetText() : "-", ownerHero, ownerActing, town ? town->GetName().GetText() : "-", townHero,
           townActing, chapter(puppet));
    log(line);
}

// QA (Basis, OnAnimalSleep): animals (neither human nor orc) this machine has, with a sleeping point.
void Sleepers(char const* argument, LogFn log)
{
    char line[480];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("REFUSED sleepers: no hero"); return; }
    float h[3];
    detail::PositionOf(hero, h);
    const bool all = argument && !std::strcmp(argument, "all");
    const bTObjArray<Entity> npcsHere = Entity::GetNPCs();
    int listed = 0, animals = 0;
    for (int i = 0; i < npcsHere.GetCount() && listed < 80; ++i) {
        Entity npc = npcsHere.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == hero || remote::IsPuppet(instance) || npc.IsDead() || !instance->GetPropertySet(eEPropertySetType_NPC)) continue;
        const gESpecies species = npc.NPC.Species;
        if (species == gESpecies_Human || species == gESpecies_Orc) continue;
        ++animals;
        Entity bed = npc.Navigation.GetSleepingPoint();
        Entity dest = npc.Navigation.GetCurrentDestinationPoint();
        if (!bed.GetInstance() && !all) continue;
        float p[3], b[3] = {0, 0, 0};
        detail::PositionOf(instance, p);
        if (bed.GetInstance()) detail::PositionOf(bed.GetInstance(), b);
        const int bedUse = bed.GetInstance() && bed.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) ? static_cast<int>(bed.Interaction.GetUseType()) : -1;
        const int destUse = dest.GetInstance() && dest.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) ? static_cast<int>(dest.Interaction.GetUseType()) : -1;
        Format(line, "SLEEPER name=%.40s species=%d pos=%.0f_%.0f_%.0f distance_m=%.0f bed=%.40s bed_pos=%.0f_%.0f_%.0f bed_use=%d dest=%.40s dest_use=%d task=%s state=%s mirrored=%d",
               npc.GetName().GetText(), static_cast<int>(species), p[0], p[1], p[2], body::Distance(p, h) / 100.0f,
               bed.GetInstance() ? bed.GetName().GetText() : "-", b[0], b[1], b[2], bedUse, dest.GetInstance() ? dest.GetName().GetText() : "-",
               destUse, npc.Routine.GetCurrentTask().GetText(), npc.Routine.GetCurrentState().GetText(), npcs::IsMirrored(instance) ? 1 : 0);
        log(line);
        ++listed;
    }
    Format(line, "SLEEPERS end animals=%d listed=%d", animals, listed);
    log(line);
}

// QA (OnAnimalSleep): "GUID" (or a name): that animal falls asleep as ZS_AnimalSleep ends.
void AnimalSleep(char const* argument, LogFn log)
{
    char line[320];
    std::string id = argument ? argument : "";
    const bool show = id.size() > 5 && id.compare(id.size() - 5, 5, "_show") == 0;
    if (show) id.resize(id.size() - 5);
    Entity npc = detail::ResolveEntity(id.c_str());
    if (!npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC) || npc.IsDead()) { log("REFUSED animalsleep: no such living NPC"); return; }
    if (!show) {
        npc.Routine.AIMode = static_cast<gEAIMode>(7);
        npc.Routine.SetTaskCallback(bCString("OnAnimalSleep"));
        npc.Routine.SetState(bCString("ZS_AnimalSleep_SleepGround"));
    }
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity target = npc.NPC.GetCurrentTarget();
    Format(line, "ANIMALSLEEP name=%s task=%s state=%s ai_mode=%d target=%s target_puppet=%d target_hero=%d mirrored=%d", npc.GetName().GetText(),
           npc.Routine.GetCurrentTask().GetText(), npc.Routine.GetCurrentState().GetText(), static_cast<int>(static_cast<gEAIMode>(npc.Routine.AIMode)),
           target.GetInstance() ? target.GetName().GetText() : "-", remote::PuppetOwner(target.GetInstance()),
           target.GetInstance() && target.GetInstance() == hero ? 1 : 0, npcs::IsMirrored(npc.GetInstance()) ? 1 : 0);
    log(line);
}

// QA (ZS_WaitForFreeDestination_Loop): "NAME": that NPC waits for its destination to become free now.
void WaitFree(char const* argument, LogFn log)
{
    char line[320];
    std::string name = argument ? argument : "";
    const bool show = name.size() > 5 && name.compare(name.size() - 5, 5, "_show") == 0;
    if (show) name.resize(name.size() - 5);
    Entity npc = Entity::GetEntity(bCString(name.c_str()));
    if (!npc.GetInstance() || !npc.GetInstance()->GetPropertySet(eEPropertySetType_NPC)) { log("REFUSED waitfree: no such NPC"); return; }
    Entity dest = npc.Navigation.GetCurrentDestinationPoint();
    if (!dest.GetInstance()) {
        Format(line, "WAITFREE name=%s dest=- task=%s state=%s", name.c_str(), npc.Routine.GetCurrentTask().GetText(),
               npc.Routine.GetCurrentState().GetText());
        log(line);
        return;
    }
    float d[3];
    detail::PositionOf(dest.GetInstance(), d);
    if (!show) {
        npc.Routine.FullStop();
        npc.Routine.SetTask(bCString("ZS_WaitForFreeDestination"));
    }
    float n[3];
    detail::PositionOf(npc.GetInstance(), n);
    const int use = dest.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) ? static_cast<int>(dest.Interaction.GetUseType()) : -1;
    Format(line, "WAITFREE name=%s dest=%.40s dest_pos=%.0f_%.0f_%.0f dest_use=%d dest_off_cm=%.0f mirrored=%d task=%s state=%s", name.c_str(),
           dest.GetName().GetText(), d[0], d[1], d[2], use, body::Distance(n, d), npcs::IsMirrored(npc.GetInstance()) ? 1 : 0,
           npc.Routine.GetCurrentTask().GetText(), npc.Routine.GetCurrentState().GetText());
    log(line);
}

// QA (P4 dialog conditions, 26.10. pair 2): the local hero's political memory.
void PolMem(char const* argument, LogFn log)
{
    char line[700];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !hero->GetPropertySet(eEPropertySetType_PlayerMemory)) { log("REFUSED polmem: no hero"); return; }
    Entity self(hero);
    int pal = -1, crime = -1, suspect = -1, fame = -1;
    if (argument && *argument && sscanf_s(argument, "%d_%d_%d_%d", &pal, &crime, &suspect, &fame) >= 1 && pal >= 0 && pal <= 12) {
        const auto a = static_cast<gEPoliticalAlignment>(pal);
        if (crime >= 0) self.PlayerMemory.SetPoliticalCrimeCount(a, crime);
        if (suspect >= 0) self.PlayerMemory.SetPoliticalSuspectComment(a, suspect);
        if (fame >= 0) self.PlayerMemory.SetPoliticalFame(a, fame);
    }
    std::string crimes, suspects, fames;
    for (int i = 0; i <= 12; ++i) {
        const auto a = static_cast<gEPoliticalAlignment>(i);
        crimes += (i ? "," : "") + std::to_string(self.PlayerMemory.GetPoliticalCrimeCount(a));
        suspects += (i ? "," : "") + std::to_string(self.PlayerMemory.GetPoliticalSuspectComment(a));
        fames += (i ? "," : "") + std::to_string(self.PlayerMemory.GetPoliticalFame(a));
    }
    Format(line, "POLMEM crime=%s suspect=%s fame=%s", crimes.c_str(), suspects.c_str(), fames.c_str());
    log(line);
}

// QA (P4, condition 24 EnclaveFriend): "Name[-fame]": read or set the PlayerFame of an enclave.
void EnclaveFame(char const* argument, LogFn log)
{
    char line[200], name[80] = {};
    int fame = -1;
    if (!argument || !*argument) { log("REFUSED enclavefame: no name"); return; }
    strncpy_s(name, argument, _TRUNCATE);
    if (char* dash = std::strrchr(name, '-')) { fame = std::atoi(dash + 1); *dash = 0; }
    Entity e = EnclaveNamed(name);
    auto* enclave = e.GetInstance() ? static_cast<gCEnclave_PS*>(e.GetInstance()->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
    if (!enclave) { Format(line, "REFUSED enclavefame %s: no enclave", name); log(line); return; }
    if (fame >= 0) enclave->AccessPlayerFame() = fame;
    Format(line, "ENCLAVEFAME enclave=%s fame=%d status=%d alignment=%d", e.GetName().GetText(), static_cast<int>(enclave->AccessPlayerFame()),
           static_cast<int>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus())),
           static_cast<int>(static_cast<gEPoliticalAlignment const&>(enclave->AccessPoliticalAlignment())));
    log(line);
}

// QA (P4 HasBossNews): "Name-status": the game's ChangeEnclaveStatus for that enclave here.
void EnclaveStatus(char const* argument, LogFn log)
{
    char line[200], name[80] = {};
    int status = -1;
    if (argument) strncpy_s(name, argument, _TRUNCATE);
    if (char* dash = std::strrchr(name, '-')) { status = std::atoi(dash + 1); *dash = 0; }
    if (status < 0) { log("REFUSED enclavestatus: Name-status"); return; }
    Entity e = EnclaveNamed(name);
    auto* enclave = e.GetInstance() ? static_cast<gCEnclave_PS*>(e.GetInstance()->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
    if (!enclave) { Format(line, "REFUSED enclavestatus %s: no enclave", name); log(line); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("ChangeEnclaveStatus"), e.GetInstance(), e.GetInstance(), status);
    Format(line, "ENCLAVESTATUS enclave=%s result=%d status=%d", e.GetName().GetText(), static_cast<int>(result),
           static_cast<int>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus())));
    log(line);
}
}
