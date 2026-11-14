// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote.h"
#include "remote_internal.h"
#include "occupancy.h"

#include "ai.h"
#include "body.h"
#include "combat.h"
#include "config.h"
#include "crashlog.h"
#include "downed.h"
#include "downed_policy.h"
#include "guid.h"
#include "guards.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "parity.h"
#include "persist.h"
#include "trade.h"
#include "world.h"
#include "npcplayback.h"
#include "pose.h"
#include "movement.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::remote {
namespace internal {
Puppet puppets[net::MaxPlayers];
QaState qa;
WearStats wearStats;
bool haveAnchor = false;
bCVector anchor;
}

namespace {
using internal::anchor;
using internal::haveAnchor;
using internal::Puppet;
using internal::puppets;
using internal::qa;
using internal::wearStats;

bool departed[net::MaxPlayers] = {};
bool started = false;
uint32_t lastSyncReport = 0;
bool templateWarned = false;

void PeerLeft(uint8_t id, char const* label)
{
    departed[id] = true;
    trade::OnPeerLeft(id);
    if (!label || !*label) return;
    wchar_t name[64] = {}, text[160];
    MultiByteToWideChar(CP_UTF8, 0, label, -1, name, 64);
    swprintf_s(text, i18n::Text(i18n::PlayerLeft), name[0] ? name : L"?");
    hud::Message(hud::Info, text);
}

void FeedPlayer(net::PlayerState const& state, uint32_t arrived, bool reset)
{
    if (!state.id || state.id >= net::MaxPlayers || state.id == net::LocalId()) return;
    Puppet& puppet = puppets[state.id];
    if (reset) {
        puppet.body.track = body::Track{};
        puppet.body.presentationSerial = 0;
        puppet.body.actionSeq = -1;
        puppet.presentationNames.clear();
        puppet.terminal = false;
        puppet.terminalInitialized = false;
        puppet.terminalSound = false;
        puppet.terminalTryAt = 0;
        puppet.terminalFails = 0;
        puppet.terminalPose.clear();
        puppet.terminalPoseFlags = 0;
        puppet.terminalReplay = npcplayback::State{};
    }
    auto remember = [&](char const* name) {
        if (!*name) return uint32_t(0);
        const uint32_t hash = net::HashName(name);
        if (puppet.presentationNames.size() < 50000 || puppet.presentationNames.count(hash))
            puppet.presentationNames[hash] = name;
        return hash;
    };
    body::MotionState motion;
    motion.motion = remember(state.animation);
    motion.heldRight = remember(state.heldRight);
    motion.heldLeft = remember(state.heldLeft);
    motion.aniState = state.aniState;
    motion.actionSeq = state.actionSeq;
    motion.motionFrac = state.motionFrac;
    motion.flags = state.flags;
    motion.locomotion = body::IsLocomotionMotion(state.animation);
    body::Feed(puppet.body.track, state.position, state.rotation, state.sent, arrived, state.movementMode, motion);
}

bool LocalState(net::PlayerState& state)
{
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* own = session.GetHero();
    state.world = CurrentWorld();
    if (!own || !session.IsGameRunning()) { state.flags = 64; return false; }
    if (persist::HeroPending()) { state.flags = 32; return false; }
    if (qa.badStates) {
        state.position[0] = std::nanf("");
        std::memset(state.name, 'X', sizeof(state.name));
        std::memset(state.shape, 'Y', sizeof(state.shape));
        state.flags = 1;
        return true;
    }
    // Transformed, the player controls a creature and the hero's own body stays hidden where the spell was cast
    // (transformdiag 19.10.: the other machine showed a human standing still while the wolf ran about).
    eCEntity* hero = ControlledBody();
    if (hero != own) strncpy_s(state.shape, Entity(hero).GetName().GetText(), _TRUNCATE);
    const bCVector& p = hero->GetWorldPosition();
    const bCQuaternion q = hero->GetWorldRotation();
    state.world = CurrentWorld();
    state.position[0] = p.GetX();
    state.position[1] = p.GetY();
    state.position[2] = p.GetZ();
    state.rotation[0] = q.GetX();
    state.rotation[1] = q.GetY();
    state.rotation[2] = q.GetZ();
    state.rotation[3] = q.GetW();
    // bit 1: knocked out, bit 2: dead, bit 3: waiting for help (an intercepted death, downed.cpp), bit 4: knocked out by
    // the game's own rules, with the gold a robber would find (ransack.cpp)
    const bool robbable = downed::NativeDown();
    state.flags = 1 | (Entity(own).IsDown() ? 2 : 0) |
                  ((Entity(own).IsDead() || downed::Terminal()) ? 4 : 0) |
                  (downed::Held() ? 8 : 0) | (robbable ? 16 : 0) | downed::LocalInteractionFlags();
    state.downedSequence = downed::Sequence();
    state.gold = robbable ? Entity(own).Inventory.GetItemAmount(Template("It_Gold")) : 0;
    state.movementMode = static_cast<uint8_t>(Entity(hero).CharacterMovement.GetMovementMode());
    state.aniState = static_cast<uint8_t>(static_cast<int>(Entity(hero).Routine.AniState));
    body::CurrentMotion(hero, state.animation, sizeof(state.animation));
    static body::ActionClock heroClock;  // protocol 58: each start of an action motion numbered
    state.actionSeq = body::ActionSeq(hero, heroClock, state.animation, &state.actionMs);
    state.motionFrac = body::MotionFraction(hero);  // protocol 59
    Entity self(hero);
    auto npcHash = [](eCEntity* entity) {
        if (!entity || !entity->GetPropertySet(eEPropertySetType_NPC)) return uint32_t(0);
        uint8_t id[16]{};
        guid::CopyEntity(entity, id);
        return net::HashGuid(id);
    };
    state.focusNpc = hero->GetPropertySet(eEPropertySetType_Focus)
        ? npcHash(self.Focus.GetFocusEntity().GetInstance()) : 0;
    state.targetNpc = hero->GetPropertySet(eEPropertySetType_NPC)
        ? npcHash(self.NPC.GetCurrentTarget().GetInstance()) : 0;
    const Entity right = self.Inventory.GetItemFromSlot(gESlot_RightHand), left = self.Inventory.GetItemFromSlot(gESlot_LeftHand);
    strncpy_s(state.heldRight, right.GetInstance() ? right.GetName().GetText() : "", _TRUNCATE);
    strncpy_s(state.heldLeft, left.GetInstance() ? left.GetName().GetText() : "", _TRUNCATE);
    Entity mine(own);
    // A worn armor is a Body_* item in the body slot 17 (UseType 114); slot 9 stays empty (re/findings/armor-equip-c.md).
    const Entity armor = mine.Inventory.GetItemFromSlot(gESlot_Body), helmet = mine.Inventory.GetItemFromSlot(gESlot_Helmet);
    strncpy_s(state.armor, armor.GetInstance() ? armor.GetName().GetText() : "", _TRUNCATE);
    strncpy_s(state.helmet, helmet.GetInstance() ? helmet.GetName().GetText() : "", _TRUNCATE);
    strncpy_s(state.look, GetConfig().look, _TRUNCATE);
    return true;
}

// NPCs that dealt with each puppet (judged it in GetAttitude, were hit by it, hit it, attacked it after a dialog), by
// GUID, taken while the NPC was in hand. Forget clears the puppet out of exactly these, looked up again by GUID: its
// walk through Entity::GetNPCs() met a freed NPC still in that list, a per-frame copy of raw entity pointers (access
// violation while the host saved, run 20261022-080317; re/findings/save-forget-npclist-c.md).
std::unordered_set<std::string> contacts[net::MaxPlayers];
unsigned contactsNoted = 0, contactsLookedUp = 0, contactsGone = 0;

std::string KeyOf(eCEntity* entity)
{
    const eCEntityProxy proxy(entity);
    return std::string(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
}

Entity ByKey(std::string const& key)
{
    _GUID raw;
    std::memcpy(&raw, key.data(), 16);
    return Entity(bCPropertyID(bCGuid(raw)));
}

void Forget(int id, eCEntity* entity)
{
    if (!entity) return;
    occupancy::Forget(id, entity);  // a stool or bench it sat on (the object's user)
    std::vector<Entity> candidates;
    if (qa.byContacts) {
        for (auto const& key : contacts[id]) {
            ++contactsLookedUp;
            Entity npc = ByKey(key);
            if (npc.GetInstance()) candidates.push_back(npc);
            else ++contactsGone;
        }
    } else {
        const bTObjArray<Entity> all = Entity::GetNPCs();
        for (int i = 0; i < all.GetCount(); ++i) candidates.push_back(all.GetAt(i));
    }
    contacts[id].clear();
    int released = 0;
    for (Entity& npc : candidates) {
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == entity || !instance->GetPropertySet(eEPropertySetType_NPC)) continue;
        if (guards::RejectRemoteWrite(instance, "remote::Forget")) continue;
        bool after = false;
        if (npc.NPC.GetCurrentTarget().GetInstance() == entity) { npc.NPC.SetCurrentTarget(Entity()); after = true; }
        if (npc.NPC.GetAlternativeTarget().GetInstance() == entity) npc.NPC.SetAlternativeTarget(Entity());
        if (npc.NPC.GetCurrentAttacker().GetInstance() == entity) { npc.NPC.SetCurrentAttacker(Entity()); after = true; }
        if (npc.NPC.GetLastAttacker().GetInstance() == entity) npc.NPC.SetLastAttacker(Entity());
        if (npc.NPC.GetSpellTarget().GetInstance() == entity) { npc.NPC.SetSpellTarget(Entity()); after = true; }
        if (npc.DamageReceiver.GetLastInflictor().GetInstance() == entity) { Entity none; npc.DamageReceiver.SetLastInflictor(none); }
        if (after && !npcs::IsMirrored(instance) && !npc.IsDead()) {
            npc.Routine.FullStop();
            npc.Routine.ContinueRoutine();
        }
        if (after) ++released;
    }
    Logf("remote: %d NPCs let go of removed puppet %d (%zu %s)", released, id, candidates.size(),
         qa.byContacts ? "known contacts" : "from Entity::GetNPCs");
}

constexpr int32_t WalkDelay = 100, WalkAhead = 250;  // ms
unsigned walkLineFrames = 0;
unsigned puppetDropsRemoved = 0;
void NoDrop(Puppet& puppet, uint32_t now)
{
    if (!qa.puppetNoDrop || static_cast<int32_t>(now - puppet.dropChecked) < 250) return;
    puppet.dropChecked = now;
    for (int i = 0; i < 2; ++i) {
        std::string& key = puppet.heldItems[i];
        if (key.size() == 16) {
            _GUID raw;
            std::memcpy(&raw, key.data(), 16);
            Entity item = Entity(bCPropertyID(bCGuid(raw)));
            if (item.GetInstance() && item.IsItem() && !item.GetOwner().GetInstance()) {
                Logf("remote: %s fell from a puppet's hand; removed (its player's own copy counts)", item.GetName().GetText());
                item.Decay();
                ++puppetDropsRemoved;
            }
        }
        key.clear();
        const Entity held = puppet.entity.Inventory.GetItemFromSlot(i ? gESlot_LeftHand : gESlot_RightHand);
        if (held.GetInstance()) {
            const eCEntityProxy proxy(held.GetInstance());
            key.assign(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
        }
    }
}
}

namespace internal {
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
}

namespace {
using internal::StackFor;
using internal::TakeOff;

template <typename P>
void Hold(P& puppet, char const* right, char const* left, uint32_t now)
{
    if (!qa.puppetHold || static_cast<int32_t>(now - puppet.holdChecked) < 250) return;
    puppet.holdChecked = now;
    const Entity inRight = puppet.entity.Inventory.GetItemFromSlot(gESlot_RightHand), inLeft = puppet.entity.Inventory.GetItemFromSlot(gESlot_LeftHand);
    const std::string haveRight = inRight.GetInstance() ? inRight.GetName().GetText() : "";
    const std::string haveLeft = inLeft.GetInstance() ? inLeft.GetName().GetText() : "";
    if (haveRight == right && haveLeft == left) return;
    puppet.entity.Inventory.HoldStacks(StackFor(puppet.entity, left), StackFor(puppet.entity, right));
}

constexpr gESlot WornSlots[2] = {gESlot_Body, gESlot_Helmet};

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

void Despawn(int id, char const* reason)
{
    Puppet& puppet = puppets[id];
    if (!puppet.spawned) return;
    eCEntity* entity = puppet.entity.GetInstance();
    if (guards::RejectRemoteWrite(entity, "remote::Despawn")) { puppet = Puppet{}; return; }
    gCWorld* current = gCWorld::GetCurrentWorld();
    if (entity && current && puppet.world == CurrentWorld()) {
        // Out of the local hero's party first, or the party keeps a member that no longer exists.
        if (puppet.entity.Party.GetPartyLeader().GetInstance()) {
            puppet.entity.Party.SetPartyLeader(Entity());
            puppet.entity.Party.PartyMemberType = gEPartyMemberType_None;
        }
        Forget(id, entity);
    }
    if (entity && current && puppet.world == CurrentWorld())
        current->DestroyDynamicEntity(static_cast<eCDynamicEntity*>(entity));
    puppet = Puppet{};
    Logf("remote: puppet %d removed (%s)", id, reason);
}

}

void DespawnAll(char const* reason)
{
    for (int id = 1; id < net::MaxPlayers; ++id) Despawn(id, reason);
}

namespace {
eCVisualAnimation_PS* StopAllPuppetMotions(eCEntity* entity, eCWrapper_emfx2Actor*& actorOut)
{
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    actorOut = animation ? animation->GetActor() : nullptr;
    if (actorOut) {
        using Owner = eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner;
        for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
            const auto type = static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot);
            actorOut->StopMotion(type, 0.0f);
            actorOut->SetTargetWeight(type, 0.0f);
            animation->SetMotionOwner(type, static_cast<Owner>(2));
        }
    }
    return animation;
}

void UpdatePuppetTerminal(Puppet& puppet, int id, eCEntity const&, uint32_t now)
{
    if (!puppet.spawned || !puppet.entity.GetInstance()) return;
    eCEntity* entity = puppet.entity.GetInstance();
    if (!puppet.terminal) return;

    puppet.entity.Routine.AIMode = gEAIMode_Dead;
    puppet.entity.CharacterMovement.StopMovement();
    puppet.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead);
    puppet.downedSequence = 0;

    if (puppet.terminalInitialized) return;
    if (static_cast<int32_t>(now - puppet.terminalTryAt) < 0) return;

    eCWrapper_emfx2Actor* actor = nullptr;
    auto* animation = StopAllPuppetMotions(entity, actor);
    if (animation && (!animation->GetRagDollEnabled() || !animation->GetRagDollController()))
        animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);

    if (animation && animation->GetRagDollEnabled() && animation->GetRagDollController()) {
        puppet.terminalInitialized = true;
        Logf("downed: puppet player=%d action=ragdoll-ready ragdoll=1 controller=1 gravity_only=1",
             id);
        return;
    }

    ++puppet.terminalFails;
    if (!puppet.terminalPose.empty())
        npcplayback::ReplayFrozenPose(entity, puppet.terminalReplay, puppet.terminalPose, puppet.terminalPoseFlags);
    world::RequestProcessingUpdate();
    puppet.terminalTryAt = now + (puppet.terminalFails < 4 ? 1500 : 5000);
    Logf("downed: puppet player=%d action=ragdoll-retry try=%u actor=%d animation=%d ready=0 gravity_only=1",
         id, puppet.terminalFails, actor != nullptr ? 1 : 0, animation != nullptr ? 1 : 0);
}

void StartPuppetTerminal(Puppet& puppet, int id, eCEntity const& hero, uint32_t now)
{
    if (puppet.terminal) return;
    puppet.terminal = true;
    puppet.terminalInitialized = false;
    puppet.terminalTryAt = now ? now + 250 : 251;
    puppet.downedSequence = 0;
    eCEntity* entity = puppet.entity.GetInstance();
    if (!entity) return;

    std::vector<pose::Bone> captured;
    uint8_t capturedFlags = 0;
    if (npcplayback::CapturePose(entity, captured, capturedFlags, nullptr)) {
        puppet.terminalPose = std::move(captured);
        puppet.terminalPoseFlags = capturedFlags;
    }
    body::Pacify(puppet.entity, puppet.body);
    puppet.entity.CharacterMovement.StopMovement();
    puppet.entity.Routine.AIMode = gEAIMode_Dead;
    puppet.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead);
    puppet.entity.StopPlayAni();

    eCWrapper_emfx2Actor* actor = nullptr;
    if (auto* animation = StopAllPuppetMotions(entity, actor))
        animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);
    world::RequestProcessingUpdate();

    if (!puppet.terminalSound) {
        puppet.terminalSound = true;
        Entity target(const_cast<eCEntity*>(&hero));
        downed::ObserveTerminal(target.GetInstance(), "StartPuppetTerminal-before-SVM");
        puppet.entity.StartSaySVM(target, bCString("DEAD"), GEFalse);
        downed::ObserveTerminal(target.GetInstance(), "StartPuppetTerminal-after-SVM");
        Logf("downed: puppet player=%d action=sound svm=DEAD requested=1 source=puppet target=local-hero",
             id);
    }
    Logf("downed: puppet player=%d action=death source=owner flags=4 ai=9 movement=14 "
         "ragdoll=starting gravity_only=1 pose_captured=%d",
         id, puppet.terminalPose.empty() ? 0 : 1);
    UpdatePuppetTerminal(puppet, id, hero, now);
}

void UpdatePuppet(int id, net::Remote const& remote, eCEntity const& hero)
{
    if (id <= 0 || id >= net::MaxPlayers || id == net::LocalId()) return;
    if (!net::IsHost() && !net::InWorld(net::LocalId())) {
        Despawn(id, "awaiting world admission");
        return;
    }
    Puppet& puppet = puppets[id];
    if (guards::RejectRemoteWrite(puppet.entity.GetInstance(), "remote::UpdatePuppet")) { puppet = Puppet{}; return; }
    const uint32_t now = net::Now();
    const int32_t activityAge = static_cast<int32_t>(now - remote.activity.ticks);
    const bool heldOwner = remote.activity.known && activityAge >= 0 &&
                           static_cast<uint32_t>(activityAge) < downed::ActivityStaleMs &&
                           (remote.activity.flags & 8) && (remote.latest.flags & 8) &&
                           remote.activity.downedSequence &&
                           remote.activity.downedSequence == remote.latest.downedSequence &&
                           remote.activity.world == remote.latest.world;
    const bool fresh = remote.connected && remote.latestTicks &&
                       (static_cast<int32_t>(now - remote.latestTicks) < 15000 || heldOwner);
    const bool sameWorld = (remote.latest.flags & 1) && remote.latest.world == CurrentWorld();
    if (!fresh || !sameWorld) {
        Despawn(id, !fresh ? "no recent state" : "other world");
        return;
    }
    char const* requested = *remote.latest.shape ? remote.latest.shape
                          : qa.remoteLooks && *remote.latest.look ? remote.latest.look : GetConfig().puppetTemplate;
    if (puppet.spawned && puppet.shape != requested) {
        const std::string from = puppet.shape;
        Despawn(id, "changed shape");
        Logf("remote: player %d changes shape: %s -> %s", id, from.c_str(), requested);
    }
    if (!puppet.spawned) {
        char const* wanted = requested;
        Template shape(wanted);
        if (!shape.IsValid() && std::strcmp(wanted, GetConfig().puppetTemplate) != 0) {  // unknown here: show the hero rather than nothing
            Logf("remote: no template %s for player %d; showing %s", wanted, id, GetConfig().puppetTemplate);
            wanted = GetConfig().puppetTemplate;
            shape = Template(wanted);
        }
        if (!shape.IsValid()) {
            if (!templateWarned) Logf("remote: puppet template %s not found", GetConfig().puppetTemplate);
            templateWarned = true;
            return;
        }
        puppet.entity = Entity::Spawn(shape, body::Pose(hero, remote.latest.position, remote.latest.rotation));
        if (!puppet.entity.GetInstance()) { Log("remote: spawn failed"); return; }
        if (guards::RejectRemoteWrite(puppet.entity.GetInstance(), "remote::Spawn")) { puppet = Puppet{}; return; }
        // Collision stays on: with it off (stage 2, when puppets were snapped every frame) no weapon could touch a
        // puppet, so the host's NPCs attacked players without ever hitting them (aggrodiag: 0 hits in 20 s of
        // Topork's power attacks on a puppet).
        body::Pacify(puppet.entity, puppet.body);
        // A puppet must never die: its template may carry a starting inventory that would drop as loot.
        if (gCNPC_PS* npc = static_cast<gCNPC_PS*>(puppet.entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC)))
            npc->AccessImmortal() = GETrue;
        // Enemies chose only this machine's hero, never a puppet (first human test: the host's orcs attacked the host 39
        // times, the client's puppet 0 times). Experiment: the puppet joins the local hero's party, as companions do,
        // whom the player's enemies attack too.
        if (qa.puppetParty) {
            puppet.entity.Party.SetPartyLeader(Entity(&hero));
            puppet.entity.Party.PartyMemberType = gEPartyMemberType_Party;
            puppet.entity.Party.Waiting = GEFalse;
        }
        puppet.spawned = true;
        puppet.body.puppet = true;  // QA (glide): counted apart from mirrored NPCs
        puppet.body.track.pendingEvents.clear();
        puppet.body.track.haveRenderTime = false;
        puppet.world = CurrentWorld();
        puppet.shape = requested;
        Logf("remote: puppet %d spawned for %s (%s)", id, remote.latest.name, wanted);
    }
    if (!puppet.entity.GetInstance()) { Despawn(id, "body gone"); return; }
    // A name of its own: under its template's name the dialogs of that NPC were offered on it, and one started by itself
    // (look = Diego, puppettalkdiag 23.10.); as PC_Hero a lookup by name could find it instead of the hero. Set right after
    // the spawn, the game named it after its template again, so it is checked on every update.
    char own[32];
    Format(own, "G3T_Player%d", id);
    if (std::strcmp(puppet.entity.GetInstance()->GetName().GetText(), own) != 0) puppet.entity.GetInstance()->SetName(bCString(own));
    // Trading between players (owner 23.10.): addressed, a puppet offers the game's own trade option ("Zeig mir deine
    // Ware!"), which opens the trade window with it (trade.cpp, offer.cpp). Checked like the name.
    puppet.entity.Dialog.TradeEnabled = trade::Available() ? GETrue : GEFalse;
    const bool dead = (remote.latest.flags & 4) != 0;
    if (dead && !puppet.terminal) {
        StartPuppetTerminal(puppet, id, hero, now);
    }
    if (puppet.terminal) {
        puppet.downedSequence = 0;
        UpdatePuppetTerminal(puppet, id, hero, now);
        return;
    }
    const float speed = body::Speed(remote.previous.position, remote.latest.position, remote.previous.sent, remote.latest.sent,
                                    remote.previousTicks, remote.latestTicks);
    // A changed shape can replace the puppet after its packets were serviced; seed that new presentation once.
    if (!puppet.body.track.count) FeedPlayer(remote.latest, remote.latestTicks, false);
    const uint32_t roundTrip = net::RoundTrip(static_cast<uint8_t>(id));
    // QA (Stufe 3.2): how far the puppet stood from where its player was (half the round trip before the state arrived)
    if (remote.latest.sent != puppet.truthSent && speed > 100.0f)
        body::NoteTruth(puppet.body, remote.latest.position, remote.latestTicks - roundTrip / 2);
    puppet.truthSent = remote.latest.sent;
    // Stufe 3.2, puppet lead: the sample half a round trip ahead of the newest state (at most 150 ms), so the puppet stands about
    // where its player is now; the host's NPCs then strike at that place, not where the player was. The movement controller
    // walks to that point too (not to the older newest state, which it would reach and stop at).
    const bool lead = (qa.puppetLead < 0 ? GetConfig().puppetLead : qa.puppetLead > 0) && roundTrip;
    const int32_t shift = lead ? static_cast<int32_t>(std::min<uint32_t>(roundTrip / 2, 150)) : -body::BufferDelay;
    // Stufe 3.2b: the lead only while the player moves steadily, rising over 200 ms; it stops, starts, brakes or turns: none at
    // once (the overshoot after a stop showed as gliding: 4 % with the lead, 1-2 % without, stageGlide 28.10.). The shown point
    // is blended from the newest state towards the predicted one, so the factor moves it smoothly.
    const float steadiness = body::Steadiness(puppet.body.track, puppet.body, now);
    const float factor = lead ? (body::leadOld ? 1.0f : steadiness) : 0.0f;
    if (lead) {
        body::glide[1].leadFactorSum += factor;
        ++body::glide[1].leadFactorFrames;
    }
    float shownAt[3], shownTurn[4];
    bool buffered = false;
    const bool timeline = GetConfig().movementSmoothing && body::interpolate;
    if (!timeline && lead && factor > 0.0f && body::Sample(puppet.body.track, now, shownAt, shownTurn, shift)) {
        for (int k = 0; k < 3; ++k) shownAt[k] = remote.latest.position[k] + factor * (shownAt[k] - remote.latest.position[k]);
        std::memcpy(shownTurn, remote.latest.rotation, sizeof(shownTurn));
        buffered = true;
    }
    float ahead[3];
    body::Target target{buffered ? shownAt : remote.latest.position, buffered ? shownTurn : remote.latest.rotation,
                        remote.latest.movementMode, remote.latest.animation, speed, remote.latest.aniState,
                        lead ? nullptr : body::SteerGoal(puppet.body.track, now, ahead, buffered ? remote.latest.position : nullptr)};
    target.smooth = lead && !body::leadOld;
    target.actionSeq = remote.latest.actionSeq;
    target.motionFrac = remote.latest.motionFrac;  // protocol 59: where its player's motion was, and how long ago
    target.motionAgeMs = now - remote.latestTicks + roundTrip / 2;
    float smoothGoal[3], goalTurn[4];
    body::Presentation presentation;
    bool presented = false;
    const int32_t presentationDelay = body::PlaybackDelay(puppet.body.track);
    auto nameOf = [&](uint32_t hash) -> char const* {
        const auto it = puppet.presentationNames.find(hash);
        return it == puppet.presentationNames.end() ? "" : it->second.c_str();
    };
    if (timeline) {
        if (body::Sample(puppet.body.track, now, shownAt, shownTurn, -presentationDelay, true, &presentation)) {
            presented = true;
            target.position = shownAt;
            target.rotation = shownTurn;
            if (presentation.movementMode >= 0 && presentation.movementMode <= 255)
                target.movementMode = static_cast<uint8_t>(presentation.movementMode);
            target.speed = presentation.speed;
            target.motion = nameOf(presentation.motion.motion);
            target.aniState = presentation.motion.aniState;
            target.actionSeq = presentation.motion.actionSeq;
            target.motionFrac = presentation.motion.motionFrac;
            target.motionAgeMs = presentation.sent - presentation.stateSent;
            target.goal = body::Sample(puppet.body.track, now, smoothGoal, goalTurn, body::GoalAhead - presentationDelay, false)
                          ? smoothGoal : shownAt;
            target.smooth = true;
            target.timeline = true;
            target.presentationSerial = puppet.body.track.serial;
        }
    }
    float lineAt[3], lineTurn[4], lineBefore[3], lineBeforeTurn[4], lineGoal[3], lineGoalTurn[4];
    if (!timeline && qa.walkLine && body::Sample(puppet.body.track, now, lineAt, lineTurn, -WalkDelay, false) &&
        body::Sample(puppet.body.track, now - 100, lineBefore, lineBeforeTurn, -WalkDelay, false) &&
        body::Sample(puppet.body.track, now, lineGoal, lineGoalTurn, -WalkDelay + WalkAhead, false)) {
        const bool stopped = speed < 40.0f;  // its newest two states: the player stands
        target.position = lineAt;
        target.rotation = lineTurn;
        target.speed = body::Distance(lineAt, lineBefore) * 10.0f;  // cm/s along the line, not from the newest two states
        target.goal = stopped ? remote.latest.position : lineGoal;
        target.smooth = true;
        target.snapAt = 600.0f;
        if (++walkLineFrames == 1 || walkLineFrames % 6000 == 0) Logf("remote: walk line drives puppet %d (%u frames)", id, walkLineFrames);
    }
    if (!*remote.latest.shape) {  // a creature holds and wears nothing
        NoDrop(puppet, now);
        Hold(puppet, presented ? nameOf(presentation.motion.heldRight) : remote.latest.heldRight,
                     presented ? nameOf(presentation.motion.heldLeft) : remote.latest.heldLeft, now);
        Wear(puppet, remote.latest.armor, remote.latest.helmet, now);
    }
    // The player's swings replay as the game's combat moves too (owner's look: players' attacks hardly visible);
    // a body whose move faults goes back to motion names (body.cpp).
    const bool held = downed::HeldPuppet(remote.latest.flags, remote.latest.downedSequence);
    const bool newHeldEpisode = held && puppet.downedSequence != remote.latest.downedSequence;
    if (held) {
        if (puppet.downedSequence != remote.latest.downedSequence) {
            puppet.body.actionUntil = 0;
            puppet.body.localHitUntil = 0;
            puppet.body.playing[0] = '\0';
            puppet.entity.StopPlayAni();
            world::RequestProcessingUpdate();
            Logf("downed: puppet player=%d sequence=%u action=hold pose_packet=%u source=owner "
                 "position=%.0f,%.0f,%.0f ani=%u",
                 id, remote.latest.downedSequence, remote.latest.sequence,
                 remote.latest.position[0], remote.latest.position[1], remote.latest.position[2], remote.latest.aniState);
        }
        // A newly streamed actor must not replay a buffered pre-knockout walk/get-up for an owner still held.
        target.position = remote.latest.position;
        target.rotation = remote.latest.rotation;
        target.goal = nullptr;
        target.speed = 0.0f;
        target.movementMode = gECharMovementMode_1; // SDK's standing mode
        target.aniState = gEAniState_LieKnockDown;
        target.motion = remote.latest.animation;
        if ((!std::strstr(target.motion, "_Lie") && !std::strstr(target.motion, "KnockDown")) ||
            std::strstr(target.motion, "GetUp") || std::strstr(target.motion, "StandUp"))
            target.motion = "";
        target.smooth = false;
        target.timeline = true;
        target.presentationSerial = puppet.body.track.serial;
        puppet.body.actionUntil = 0;
        puppet.body.localHitUntil = 0;
    } else if (puppet.downedSequence) {
        puppet.body.actionUntil = 0;
        puppet.body.playing[0] = '\0';
        Logf("downed: puppet player=%d sequence=%u action=release pose_packet=%u source=owner flags=%u",
             id, puppet.downedSequence, remote.latest.sequence, remote.latest.flags);
    }
    puppet.downedSequence = held ? remote.latest.downedSequence : 0;
    puppet.body.combatMoves = qa.puppetCombat;
    body::Drive(puppet.entity, puppet.body, target, now);
    if (held) {
        // Mode 8/9 takes PS_GonnaLoot before AssessTalk (Script_Game +0x81e72). Lie is presentation only.
        puppet.entity.Routine.AIMode = gEAIMode_Abandoned;
        puppet.entity.CharacterMovement.StopMovement();
        puppet.entity.Routine.AniState = gEAniState_LieKnockDown;
        if (newHeldEpisode) LogPuppetInteraction(puppet.entity.GetInstance(), "publish-talk", 1);
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

unsigned CurrentWorld()
{
    gCWorld* current = gCWorld::GetCurrentWorld();
    return current ? net::HashWorld(current->GetFileBaseName().GetText()) : 0;
}

eCEntity* ControlledBody()
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* player = Entity::GetPlayer().GetInstance();
    return qa.followShape && hero && player && player != hero && !IsPuppet(player) ? player : hero;
}

bool IsPuppet(eCEntity const* entity)
{
    for (auto& puppet : puppets)
        if (puppet.spawned && puppet.entity.GetInstance() == entity) return true;
    return false;
}

int PuppetOwner(eCEntity const* entity)
{
    for (int id = 1; id < net::MaxPlayers; ++id)
        if (entity && puppets[id].spawned && puppets[id].entity.GetInstance() == entity) return id;
    return 0;
}

bool IsHeldPuppet(eCEntity const* entity)
{
    const int id = PuppetOwner(entity);
    if (!id) return false;
    net::Remote const& r = net::Remotes()[id];
    return r.connected && r.latestTicks && r.latest.world == CurrentWorld() &&
           downed::HeldPuppet(r.latest.flags, r.latest.downedSequence);
}

void LogPuppetInteraction(eCEntity* entity, char const* action, int result)
{
    const int id = PuppetOwner(entity);
    if (!id) return;
    static unsigned notes = 0;
    static uint32_t lastNote = 0;
    const uint32_t now = net::Now();
    if (++notes > 40 && now - lastNote < 5000) return;
    lastNote = now;
    net::Remote const& r = net::Remotes()[id];
    Entity player(entity);
    const bool routine = entity->GetPropertySet(eEPropertySetType_ScriptRoutine) != nullptr;
    const bool inventory = entity->GetPropertySet(eEPropertySetType_Inventory) != nullptr;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bool focused = hero && hero->GetPropertySet(eEPropertySetType_Focus) &&
                         Entity(hero).Focus.GetFocusEntity().GetInstance() == entity;
    Logf("DOWNEDINTERACT role=%s player=%d episode=%u flags=%u held=%d ai=%d native_down=%d native_dead=%d "
         "hp=%d inventory=%d focused=%d action=%s result=%d",
         net::IsHost() ? "host" : "client", id, r.latest.downedSequence, r.latest.flags,
         IsHeldPuppet(entity) ? 1 : 0, routine ? static_cast<int>(player.Routine.AIMode) : -1,
         routine && player.IsDown() ? 1 : 0, routine && player.IsDead() ? 1 : 0,
         entity->GetPropertySet(eEPropertySetType_PlayerMemory) ? player.PlayerMemory.GetHitPoints() : -1,
         inventory ? 1 : 0, focused ? 1 : 0, action, result);
}

eCEntity* PuppetOf(int playerId)
{
    if (playerId <= 0 || playerId >= net::MaxPlayers || !puppets[playerId].spawned) return nullptr;
    return puppets[playerId].entity.GetInstance();
}

void Start()
{
    Config const& config = GetConfig();
    parity::Compute();
    net::SetPlayerStateHandler(&FeedPlayer);
    net::SetPeerLeftHandler(&PeerLeft);
    net::SetSendRate(config.sendRate);
    net::SetBandwidthBudget(static_cast<uint32_t>(config.networkBudgetKib) * 1024);
    started = net::Start(config.host);
}

void OnWorldDeactivate()
{
    for (auto& puppet : puppets) puppet = Puppet{};
    for (auto& pending : departed) pending = false;
    for (auto& set : contacts) set.clear();
    haveAnchor = false;
}

void CaptureCacheInventory()
{
    unsigned count = 0;
    eCEntity* first = nullptr;
    for (auto const& puppet : puppets)
        if (auto* entity = puppet.entity.GetInstance()) {
            ++count;
            if (!first) first = entity;
        }
    crashlog::CacheObjects(crashlog::CacheKind::Puppets, count, first);
}

void NoteContact(eCEntity* npc, eCEntity* puppet)
{
    const int id = PuppetOwner(puppet);
    if (!qa.noting || !id || !npc || npc == puppet) return;
    if (contacts[id].insert(KeyOf(npc)).second) ++contactsNoted;
}

void Tick()
{
    if (!started) return;
    net::PlayerState local;
    bool haveLocal = false;
    { movement::Stage stage("remote capture"); haveLocal = LocalState(local); }
    { movement::Stage stage("ENet service"); net::Tick(local, haveLocal); }
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    net::Remote const* remotes = net::Remotes();
    for (int id = 1; id < net::MaxPlayers; ++id) {
        if (id == net::LocalId()) continue;
        movement::Stage stage("remote puppet update");
        if (departed[id]) { Despawn(id, "peer left"); departed[id] = false; }
        if (haveLocal && hero) UpdatePuppet(id, remotes[id], *hero);
        else if (puppets[id].spawned) Despawn(id, "local game not running");
    }
    const uint32_t now = GetTickCount();
    if (haveLocal && (!lastSyncReport || now - lastSyncReport >= 5000)) {
        lastSyncReport = now ? now : 1;
        LogPlayerStreams(net::Now());
        body::SyncReport(&Log);
        npcs::SyncReport(&Log);
        combat::SyncReport(&Log);
    }
}
}
