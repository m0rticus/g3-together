// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_internal.h"

#include "ai.h"
#include "body.h"
#include "crashlog.h"
#include "log.h"
#include "net.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::remote {
namespace internal {
namespace {
constexpr gESlot WornSlots[2] = {gESlot_Body, gESlot_Helmet};
unsigned puppetDropsRemoved = 0;
}

GEInt StackFor(Entity& body, char const* name)
{
    if (!name || !*name) return -1;
    for (int pass = 0; pass < 2; ++pass) {
        const GEInt count = body.Inventory.GetStackCount();
        for (GEInt i = 0; i < count; ++i)
            if (Entity item = body.Inventory.GetTemplateItem(i); item.GetInstance() && !std::strcmp(item.GetName().GetText(), name)) return i;
        if (pass == 0) body.Inventory.CreateItems(Template(name), 0, 1);
    }
    return -1;
}

void TakeOff(Entity& body, gESlot slot)
{
    for (GEInt i = 0; i < body.Inventory.GetStackCount(); ++i)
        if (body.Inventory.GetSlotFromStack(i) == slot) body.Inventory.UnEquipStack(i);
}

void NoDrop(Puppet& puppet, uint32_t now)
{
    if (!qa.puppetNoDrop || static_cast<int32_t>(now - puppet.dropChecked) < 250) return;
    puppet.dropChecked = now;
    for (int i = 0; i < 2; ++i) {
        GuidKey& key = puppet.heldItems[i];
        if (!IsZeroGuid(key)) {
            Entity item = ByKey(key);
            if (item.GetInstance() && item.IsItem() && !item.GetOwner().GetInstance()) {
                Logf("remote: %s fell from a puppet's hand; removed (its player's own copy counts)", item.GetName().GetText());
                item.Decay();
                ++puppetDropsRemoved;
            }
        }
        key = {};
        const Entity held = puppet.entity.Inventory.GetItemFromSlot(i ? gESlot_LeftHand : gESlot_RightHand);
        if (held.GetInstance()) key = KeyOf(held.GetInstance());
    }
}

void Wear(Puppet& puppet, char const* armor, char const* helmet, uint32_t now)
{
    if (!qa.puppetWear || static_cast<int32_t>(now - puppet.wearChecked) < 250) return;
    puppet.wearChecked = now;
    char const* const wanted[2] = {armor, helmet};
    for (int i = 0; i < 2; ++i) {
        const Entity worn = puppet.entity.Inventory.GetItemFromSlot(WornSlots[i]);
        const std::string have = worn.GetInstance() ? worn.GetName().GetText() : "";
        if (have == wanted[i]) continue;
        if (!have.empty()) TakeOff(puppet.entity, WornSlots[i]);
        const GEInt stack = StackFor(puppet.entity, wanted[i]);
        if (stack < 0) continue;
        ++wearStats.changes;
        if (puppet.entity.Inventory.EquipStack(stack) && puppet.entity.Inventory.GetSlotFromStack(stack) == WornSlots[i]) {
            ++wearStats.equipped;
            continue;
        }
        auto* inventory = static_cast<gCInventory_PS*>(puppet.entity.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        if (inventory && inventory->LinkStackToSlot(stack, WornSlots[i], GETrue)) ++wearStats.linked;
        else ++wearStats.failed;
    }
}

void LogPlayerStreams(uint32_t now)
{
    auto const* remotes = net::Remotes();
    for (int id = 1; id < net::MaxPlayers; ++id) {
        auto const& r = remotes[id];
        if (id == net::LocalId() || !r.connected) continue;
        auto const& puppet = puppets[id];
        auto const& track = puppet.body.track;
        auto age = [&](uint32_t sent) {
            return static_cast<unsigned>(std::max(static_cast<int32_t>(now - static_cast<uint32_t>(track.offset) - sent), 0));
        };
        Logf("PLAYERSTREAM role=%s player=%d connected=%d world_gate=%d same_world=%d accepted=%u "
             "metadata_waits=%u stale=%u cached_entry=%u packet=%u arrival_age_ms=%u puppet=%d",
             net::IsHost() ? "host" : "client", id, r.connected ? 1 : 0,
             net::InWorld(net::IsHost() ? static_cast<uint8_t>(id) : net::LocalId()) ? 1 : 0,
             r.latest.world == CurrentWorld() ? 1 : 0, r.acceptedStates, r.metadataWaits, r.staleStates,
             r.preEntryStates, r.latest.sequence, r.latestTicks ? now - r.latestTicks : 0u,
             puppet.spawned && puppet.entity.GetInstance() ? 1 : 0);
        if (!track.count) continue;
        char shows[128] = {};
        if (puppet.spawned && puppet.entity.GetInstance())
            body::CurrentMotion(puppet.entity.GetInstance(), shows, sizeof(shows));
        Logf("PUPPETLATENCY player=%d samples=%d buffer_ms=%d shown_lag_ms=%u newest_sample_age_ms=%u "
             "oldest_sample_age_ms=%u transit_excess_ms=%d backlog_ms=%d catchup=%d catchup_frames=%u "
             "pending_events=%zu requested_motion=%s shown_motion=%s",
             id, track.count, body::PlaybackDelay(track), track.haveRenderTime ? age(track.renderSent) : 0u,
             age(track.snaps[track.count - 1].sent), age(body::OldestRetainedSent(track)), track.transitExcessMs,
             track.backlogMs, track.catchUp ? 1 : 0, track.catchUpFrames, body::PendingEvents(track),
             *r.latest.animation ? r.latest.animation : "-", *shows ? shows : "-");
    }
}
}

void CaptureCacheInventory()
{
    unsigned count = 0;
    eCEntity* first = nullptr;
    for (auto const& puppet : internal::puppets)
        if (auto* entity = puppet.entity.GetInstance()) {
            ++count;
            if (!first) first = entity;
        }
    crashlog::CacheObjects(crashlog::CacheKind::Puppets, count, first);
}

namespace {
using internal::anchor;
using internal::haveAnchor;
using internal::puppets;
using internal::qa;
using internal::StackFor;
using internal::TakeOff;
using internal::wearStats;

void LogMotions(char const* who, Entity& entity, LogFn log)
{
    eCEntity* engine = entity.GetInstance();
    eCVisualAnimation_PS* animation =
        engine ? static_cast<eCVisualAnimation_PS*>(engine->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    char line[900];
    int length = Format(line, "MOTIONS who=%s", who);
    for (int type = 0; animation && type < 6 && length > 0; ++type) {
        char name[128] = {};
        const bool ok = body::MotionName(animation, type, name, sizeof(name));
        length += Format(line + length, sizeof(line) - length, " %d=%s", type, *name ? name : "-");
        if (!ok) break;
    }
    log(line);
}
}

void ShapeOff(LogFn log)
{
    qa.followShape = false;
    log("SHAPEOFF transformed players are sent as their hero");
}

void BadStates(char const* argument, LogFn log)
{
    qa.badStates = std::strcmp(argument, "off") != 0;
    log(qa.badStates ? "BADSTATES on" : "BADSTATES off");
}

void ContactsOff(LogFn log)
{
    qa.noting = false;
    log("CONTACTSOFF ok=1");
}

void ForgetNpcListOn(LogFn log)
{
    qa.byContacts = false;
    log("FORGETNPCLIST ok=1 (puppets are forgotten by walking Entity::GetNPCs, as before)");
}

void Report(LogFn log)
{
    char line[400];
    Format(line, "NET local_id=%u connected=%d world=%08x perception_filter=%d blocked=%u wrapped=%u skipped=%u unknown=%u",
           net::LocalId(), net::Connected(), CurrentWorld(), ai::PerceptionFiltered(), ai::BlockedCount(),
           ai::WrappedCount(), ai::SkippedCount(), ai::UnknownCount());
    log(line);
    net::Remote const* remotes = net::Remotes();
    const uint32_t now = net::Now();
    for (int id = 1; id < net::MaxPlayers; ++id) {
        net::Remote const& r = remotes[id];
        if (id == net::LocalId() || !r.connected) continue;
        float puppetPos[3] = {};
        eCEntity* entity = puppets[id].spawned ? puppets[id].entity.GetInstance() : nullptr;
        if (entity) {
            const bCVector& p = entity->GetWorldPosition();
            puppetPos[0] = p.GetX(); puppetPos[1] = p.GetY(); puppetPos[2] = p.GetZ();
        }
        Format(line, "REMOTE id=%d name=%s world=%08x same_world=%d age_ms=%u pos=%.1f,%.1f,%.1f puppet=%d puppet_pos=%.1f,%.1f,%.1f",
               id, r.latest.name, r.latest.world, r.latest.world == CurrentWorld(), r.latestTicks ? now - r.latestTicks : 0,
               r.latest.position[0], r.latest.position[1], r.latest.position[2], entity != nullptr,
               puppetPos[0], puppetPos[1], puppetPos[2]);
        log(line);
        Format(line, "ANIM id=%d remote=%s requested=%s", id, *r.latest.animation ? r.latest.animation : "-",
               *puppets[id].body.playing ? puppets[id].body.playing : "-");
        log(line);
        {
            char shows[128] = "-";
            int running = -1;
            if (entity) {
                body::CurrentMotion(entity, shows, sizeof(shows));
                if (auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)))
                    if (eCWrapper_emfx2Actor* actor = animation->GetActor())
                        running = actor->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(0)) ? 1 : 0;
            }
            Format(line, "PUPPETPOSE id=%d remote=%s shows=%s run0=%d shape=%s body=%s",
                   id, *r.latest.animation ? r.latest.animation : "-", *shows ? shows : "-", running,
                   *r.latest.shape ? r.latest.shape : "-",
                   puppets[id].spawned && puppets[id].entity.GetInstance() ? puppets[id].shape.c_str() : "-");
            log(line);
        }
        {
            Entity body(entity);
            const Entity held = entity ? body.Inventory.GetItemFromSlot(gESlot_RightHand) : Entity();
            Format(line, "HELD id=%d remote=%s puppet=%s", id, *r.latest.heldRight ? r.latest.heldRight : "-",
                   held.GetInstance() ? held.GetName().GetText() : "-");
            log(line);
            const Entity armor = entity ? body.Inventory.GetItemFromSlot(gESlot_Body) : Entity();
            const Entity helmet = entity ? body.Inventory.GetItemFromSlot(gESlot_Helmet) : Entity();
            Format(line, "WEAR id=%d remote_armor=%s remote_helmet=%s puppet_armor=%s puppet_helmet=%s look=%s template=%s changes=%u"
                         " equipped=%u linked=%u failed=%u", id,
                   *r.latest.armor ? r.latest.armor : "-", *r.latest.helmet ? r.latest.helmet : "-",
                   armor.GetInstance() ? armor.GetName().GetText() : "-", helmet.GetInstance() ? helmet.GetName().GetText() : "-",
                   *r.latest.look ? r.latest.look : "-", puppets[id].spawned ? puppets[id].shape.c_str() : "-",
                   wearStats.changes, wearStats.equipped, wearStats.linked, wearStats.failed);
            log(line);
        }
        if (entity) {
            char who[16];
            Format(who, "puppet%d", id);
            LogMotions(who, puppets[id].entity, log);
        }
    }
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (hero) {
        Format(line, "PLAYER_CHECK getplayer_is_hero=%d by_name_is_hero=%d",
               Entity::GetPlayer().GetInstance() == hero, Entity::GetEntity(bCString("PC_Hero")).GetInstance() == hero);
        log(line);
        Entity self(hero);
        LogMotions("hero", self, log);
    }
}

void WalkTo(int index, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !gCSession::GetInstance().IsGameRunning()) { log("REFUSED walkto: no hero"); return; }
    if (!haveAnchor) { anchor = hero->GetWorldPosition(); haveAnchor = true; }
    const float angle = index * 0.785398f;
    const bCVector target(anchor.GetX() + 1500.0f * std::cos(angle), anchor.GetY(), anchor.GetZ() + 1500.0f * std::sin(angle));
    const GEBool ok = Entity(hero).StartGoto(target, gEWalkMode_Run);
    Format(line, "WALKTO index=%d started=%d target=%.1f,%.1f,%.1f", index, ok, target.GetX(), target.GetY(), target.GetZ());
    log(line);
}

void WalkRel(char const* argument, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !gCSession::GetInstance().IsGameRunning()) { log("REFUSED walkrel: no hero"); return; }
    if (!haveAnchor) { anchor = hero->GetWorldPosition(); haveAnchor = true; }
    Entity self(hero);
    if (argument && !std::strcmp(argument, "stop")) {
        self.Routine.FullStop();
        self.CharacterMovement.StopMovement();
        const bCVector& p = hero->GetWorldPosition();
        Format(line, "WALKREL stop pos=%.1f,%.1f,%.1f", p.GetX(), p.GetY(), p.GetZ());
        log(line);
        return;
    }
    float dx = 0.0f, dz = 0.0f;
    if (!argument || std::sscanf(argument, "%f_%f", &dx, &dz) != 2) { log("REFUSED walkrel: need dx_dz"); return; }
    const bCVector target(anchor.GetX() + dx, anchor.GetY(), anchor.GetZ() + dz);
    const GEBool ok = self.StartGoto(target, gEWalkMode_Run);
    Format(line, "WALKREL started=%d target=%.1f,%.1f,%.1f", ok, target.GetX(), target.GetY(), target.GetZ());
    log(line);
}

void Step(int index, LogFn log)
{
    char line[200];
    eCEntity* hero = gCSession::GetInstance().GetHero() ? ControlledBody() : nullptr;
    if (!hero || !gCSession::GetInstance().IsGameRunning()) { log("REFUSED step: no hero"); return; }
    if (!haveAnchor) { anchor = hero->GetWorldPosition(); haveAnchor = true; }
    const float angle = index * 0.785398f;
    const float position[3] = {anchor.GetX() + 300.0f * std::cos(angle), anchor.GetY(), anchor.GetZ() + 300.0f * std::sin(angle)};
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, position, rotation));
    const bCVector& p = hero->GetWorldPosition();
    Format(line, "STEP index=%d target=%.1f,%.1f,%.1f pos=%.1f,%.1f,%.1f", index, position[0], position[1], position[2],
           p.GetX(), p.GetY(), p.GetZ());
    log(line);
}

void PuppetPartyOff(LogFn log)
{
    qa.puppetParty = false;
    for (int id = 1; id < net::MaxPlayers; ++id)
        if (puppets[id].spawned && puppets[id].entity.GetInstance()) {
            puppets[id].entity.Party.SetPartyLeader(Entity());
            puppets[id].entity.Party.PartyMemberType = gEPartyMemberType_None;
        }
    log("PUPPETPARTYOFF ok=1");
}

void PuppetHoldOff(LogFn log)
{
    qa.puppetHold = false;
    log("PUPPETHOLDOFF ok=1");
}

void PuppetCombatOff(LogFn log)
{
    qa.puppetCombat = false;
    log("PUPPETCOMBATOFF ok=1");
}

void WearHero(char const* argument, LogFn log)
{
    char line[1000];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !argument || !*argument) { log("REFUSED wear: no hero or no item"); return; }
    Entity self(hero);
    if (!std::strcmp(argument, "-armor") || !std::strcmp(argument, "-helmet")) {
        TakeOff(self, argument[1] == 'a' ? gESlot_Body : gESlot_Helmet);
    } else {
        const bool known = Template(argument).IsValid();
        const GEInt stack = StackFor(self, argument);
        const GEBool ok = stack >= 0 ? self.Inventory.EquipStack(stack) : GEFalse;
        if (!ok) {
            int length = Format(line, "REFUSED wear: %s template=%d stack=%d not equipped", argument, known, stack);
            auto* inventory = static_cast<gCInventory_PS*>(hero->GetPropertySet(eEPropertySetType_Inventory));
            if (inventory && stack >= 0) {
                bTObjArray<gCInventory_PS::SMissingAttrib> attribs;
                bTValArray<gCInventory_PS::SMissingSkill> skills;
                const GEBool can = inventory->CanEquip(stack, attribs, skills);
                length += Format(line + length, sizeof(line) - length, " can=%d", can ? 1 : 0);
                for (GEInt i = 0; i < attribs.GetCount() && i < 6 && length < 800; ++i)
                    length += Format(line + length, sizeof(line) - length, " attrib:%.24s-%d", attribs[i].MissingAttribName.GetText(),
                                     attribs[i].MissingAttribValue);
                for (GEInt i = 0; i < skills.GetCount() && i < 6 && length < 800; ++i)
                    length += Format(line + length, sizeof(line) - length, " skill:%.40s",
                                     skills[i].MissingSkill ? skills[i].MissingSkill->GetName().GetText() : "?");
                Format(line + length, sizeof(line) - length, " use=%d", static_cast<int>(self.Inventory.GetUseType(stack)));
            }
            log(line);
            return;
        }
    }
    const Entity armor = self.Inventory.GetItemFromSlot(gESlot_Body), helmet = self.Inventory.GetItemFromSlot(gESlot_Helmet);
    int length = Format(line, "WEARHERO armor=%s helmet=%s equipped=", armor.GetInstance() ? armor.GetName().GetText() : "-",
                        helmet.GetInstance() ? helmet.GetName().GetText() : "-");
    for (GEInt i = 0; i < self.Inventory.GetStackCount() && length > 0 && length < 900; ++i) {
        const int slot = static_cast<int>(self.Inventory.GetSlotFromStack(i));
        const Entity item = slot ? self.Inventory.GetTemplateItem(i) : Entity();
        if (item.GetInstance()) length += Format(line + length, sizeof(line) - length, "%d:%.40s,", slot, item.GetName().GetText());
    }
    log(line);
}

void WornScan(LogFn log)
{
    char line[1000];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    int count = 0, slotted = 0;
    if (hero) {
        const bCVector& h = hero->GetWorldPosition();
        const float at[3] = {h.GetX(), h.GetY(), h.GetZ()};
        const bTObjArray<Entity> all = Entity::GetNPCs();
        for (int i = 0; i < all.GetCount() && count < 40; ++i) {
            Entity npc = all.GetAt(i);
            eCEntity* instance = npc.GetInstance();
            if (!instance || IsPuppet(instance)) continue;
            const bCVector& p = instance->GetWorldPosition();
            const float there[3] = {p.GetX(), p.GetY(), p.GetZ()};
            if (body::Distance(at, there) > 10000.0f) continue;
            if (slotted < 5) {
                ++slotted;
                int length = Format(line, "WORNSLOTS npc=%s stacks=%d", npc.GetName().GetText(), npc.Inventory.GetStackCount());
                for (int slot = 1; slot <= 18 && length > 0; ++slot) {
                    const Entity in = npc.Inventory.GetItemFromSlot(static_cast<gESlot>(slot));
                    if (in.GetInstance()) length += Format(line + length, sizeof(line) - length, " %d:%.40s", slot, in.GetName().GetText());
                }
                log(line);
            }
            for (GEInt k = 0; k < npc.Inventory.GetStackCount() && count < 40; ++k) {
                Entity item = npc.Inventory.GetTemplateItem(k);
                if (!item.GetInstance()) continue;
                const int use = static_cast<int>(item.Interaction.GetUseType());
                const bool armorCategory = static_cast<int>(item.Item.GetCategory()) == gEItemCategory_Armor;
                if (use != gEUseType_Armor && use != gEUseType_Helmet && !armorCategory) continue;
                Format(line, "WORN npc=%s item=%s kind=%s slot=%d use=%d", npc.GetName().GetText(), item.GetName().GetText(),
                       use == gEUseType_Helmet ? "helmet" : "armor", static_cast<int>(npc.Inventory.GetSlotFromStack(k)), use);
                log(line);
                ++count;
            }
        }
    }
    Format(line, "WORN end count=%d", count);
    log(line);
}

void WearOff(LogFn log)
{
    qa.puppetWear = false;
    log("WEAROFF ok");
}

void LookOff(LogFn log)
{
    qa.remoteLooks = false;
    log("LOOKOFF ok");
}

void PuppetInfo(char const* argument, LogFn log)
{
    char line[400], source[96] = {};
    const int id = std::atoi(argument);
    char const* sep = std::strchr(argument, '_');
    if (sep) strncpy_s(source, sep + 1, _TRUNCATE);
    eCEntity* puppet = PuppetOf(id);
    gCInfoManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    gCInfo_PS* from = manager && *source ? manager->GetInfo(bCString(source)) : nullptr;
    if (!puppet || !from) { log("REFUSED puppetinfo: <id>_<Info> with a puppet and a known info"); return; }
    char own[32], name[128];
    Format(own, "G3T_Player%d", id);
    Format(name, "G3T_%s_%d", source, id);
    puppet->SetName(bCString(own));
    gCInfo_PS* info = manager->GetInfo(bCString(name));
    const bool added = !info;
    if (!info) info = manager->AddInfo(bCString(name));
    if (!info) { log("REFUSED puppetinfo: AddInfo failed"); return; }
    info->CopyFrom(*from);
    info->AccessName() = bCString(name);
    info->AccessOwner() = bCString(own);
    info->AccessInfoGiven() = GEFalse;
    info->AccessPermanent() = GETrue;
    info->AccessConditionType() = static_cast<gEInfoCondType const&>(from->AccessConditionType());
    info->AccessType() = static_cast<gEInfoType const&>(from->AccessType());
    Entity body(puppet);
    body.Dialog.TradeEnabled = GETrue;
    gCInfoScript_PS* script = info->AccessInfoScript().GetObject();
    Format(line, "PUPPETINFO id=%d puppet=%s by_name=%d info=%s added=%d cond_type=%d type=%d owner=%s lines=%d infos=%d has_info=%d trade=%d",
           id, puppet->GetName().GetText(), Entity::GetEntity(bCString(own)).GetInstance() == puppet ? 1 : 0, name, added ? 1 : 0,
           static_cast<int>(static_cast<gEInfoCondType const&>(info->AccessConditionType())),
           static_cast<int>(static_cast<gEInfoType const&>(info->AccessType())), info->AccessOwner().GetText(),
           script ? script->AccessInfoScriptLines().GetCount() : -1, manager->GetInfos().GetCount(), manager->HasInfos(puppet) ? 1 : 0,
           body.Dialog.TradeEnabled ? 1 : 0);
    log(line);
}

void PuppetTradeOn(char const* argument, LogFn log)
{
    char line[200];
    eCEntity* puppet = PuppetOf(std::atoi(argument));
    if (!puppet) { log("REFUSED puppettradeon: no such puppet"); return; }
    Entity body(puppet);
    body.Dialog.TradeEnabled = GETrue;
    gCInfoManager_PS* manager = gCSession::GetInstance().IsGameRunning() ? gCInfoManager_PS::GetInstance() : nullptr;
    Format(line, "PUPPETTRADE id=%s name=%s trade=%d has_info=%d", argument, puppet->GetName().GetText(),
           body.Dialog.TradeEnabled ? 1 : 0, manager && manager->HasInfos(puppet) ? 1 : 0);
    log(line);
}

bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "puppetnodropoff")) { qa.puppetNoDrop = false; log("SWITCH puppetnodropoff ok=1"); return true; }
    if (!std::strcmp(action, "puppetleadon")) { qa.puppetLead = 1; log("SWITCH puppetleadon ok=1"); return true; }
    if (!std::strcmp(action, "walklineon")) { qa.walkLine = true; log("SWITCH walklineon ok=1"); return true; }
    if (!std::strcmp(action, "puppetleadoff")) { qa.puppetLead = 0; log("SWITCH puppetleadoff ok=1"); return true; }
    if (!std::strcmp(action, "offsetwindowoff")) { body::offsetWindow = false; log("SWITCH offsetwindowoff ok=1"); return true; }
    if (!std::strcmp(action, "goalaheadon")) { body::goalAhead = true; log("SWITCH goalaheadon ok=1"); return true; }
    if (!std::strcmp(action, "staggereffectsoff")) { body::staggerEffects = false; log("SWITCH staggereffectsoff ok=1"); return true; }
    if (!std::strcmp(action, "mirrorleadon")) { body::mirrorLead = true; log("SWITCH mirrorleadon ok=1"); return true; }
    if (!std::strcmp(action, "leadoldon")) { body::leadOld = true; log("SWITCH leadoldon ok=1"); return true; }
    if (!std::strcmp(action, "goalaheadoff")) { body::goalAhead = false; log("SWITCH goalaheadoff ok=1"); return true; }
    return false;
}
}
