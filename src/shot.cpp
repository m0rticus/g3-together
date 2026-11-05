// SPDX-License-Identifier: GPL-3.0-or-later
#include "shot.h"

#include "body.h"
#include "combat.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"
#include "summon.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace g3t::shot {
namespace {
bool installed = false;
DWORD lastTry = 0;
gFScript originalShoot = nullptr;
gFScript originalMagicProjectile = nullptr;
bool magicInstalled = false;
bool mirror = true;     // QA negative control: MirrorOff
bool copyOwner = true;  // QA negative control: CopyHeroOff (the copy belongs to our own hero, as a naive copy would)
uint32_t counter = 0;
unsigned shots = 0, announced = 0, unresolved = 0, received = 0, copies = 0, noPuppet = 0, badTemplate = 0, noSpell = 0;
unsigned npcAnnounced = 0, npcReceived = 0, npcCopies = 0;
std::unordered_set<std::string> npcReplicas;
thread_local bool npcReplicaContact = false;
bool contactsReady = false;
bool traceTouches = false;
void InstallTouch();

// Our own shot, waiting for its arrow to show up among the world's entities (it does a frame after Shoot spawned it).
// It is announced a frame after that, as it then flies (a QA shot is re-aimed in the frame it shows up).
struct Pending {
    eCEntity* shooter = nullptr;
    eCEntity* seen = nullptr;
    uint8_t target[16] = {};
    std::vector<eCEntity*> before;  // the shooter's arrows already there
    DWORD since = 0;
    char spellItem[40] = "";  // a spell's projectile: the item cast from (its spell goes on the copy)
    char spell[64] = {};
    uint8_t targetPlayer = 0;
};
std::vector<Pending> pending;
struct TimedNpcShot { uint8_t from; net::ShotNote note; uint32_t arrived; };
std::vector<TimedNpcShot> timedNpcShots;

// QA: the latest copies and where they went in their first 1.5 s (relative to where they started, every 100 ms).
struct Copy {
    uint32_t netId = 0;
    uint8_t from = 0;
    uint8_t guid[16] = {};
    DWORD at = 0;
    DWORD next = 0;  // ms after `at` of the next sample
    bool done = false;
    bCVector start;
    char path[400] = "";  // at most 16 samples of 24 chars
};
constexpr size_t MaxCopies = 8;
std::vector<Copy> recent;

template <typename T>
T* Find(bTValMap<bCString, T*>& map, char const* name)
{
    T* found = nullptr;
    return map.GetAt(bCString(name), found) ? found : nullptr;
}

void Guid(eCEntity* entity, uint8_t out[16])
{
    const eCEntityProxy proxy(entity);
    std::memcpy(out, &proxy.GetEntityID().GetCore(), 16);
}

Entity ByGuid(uint8_t const guid[16])
{
    static const uint8_t none[16] = {};
    if (!std::memcmp(guid, none, 16)) return Entity();
    _GUID raw;
    std::memcpy(&raw, guid, 16);
    return Entity(bCPropertyID(bCGuid(raw)));
}

char const* NameOf(Entity const& e) { return e.GetInstance() ? e.GetName().GetText() : "-"; }

bool OwnCaster(eCEntity* caster)
{
    return caster && (caster == remote::ControlledBody() ||
           (caster != gCSession::GetInstance().GetHero() && !remote::IsPuppet(caster) &&
            caster->GetPropertySet(eEPropertySetType_NPC) && !npcs::OwnerOf(caster) && !npcs::IsMirrored(caster)));
}

uint8_t TargetPlayer(eCEntity* target)
{
    if (!target) return 0;
    if (target == remote::ControlledBody()) return net::LocalId();
    return static_cast<uint8_t>(remote::PuppetOwner(target));
}

// The shooter's arrows (projectiles it owns) among the world's entities.
std::vector<eCEntity*> ArrowsOf(eCEntity* shooter)
{
    std::vector<eCEntity*> out;
    const bTObjArray<Entity> all = Entity::GetEntities();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity e = all.GetAt(i);
        if (e.GetInstance() && e.GetInstance()->GetPropertySet(eEPropertySetType_Projectile) &&
            e.Interaction.GetOwner().GetInstance() == shooter)
            out.push_back(e.GetInstance());
    }
    return out;
}

// The puppet's body-part entities (Body_Player, Head_Player: its children, or so named and closer to it than to our
// hero). They collide on their own; a copy starting at the puppet's hand would stop in them, as the bot's own arrows
// did in the hero's (re/findings/arrows-p2-b.md).
std::vector<eCEntity*> PartsOf(eCEntity* puppet)
{
    std::vector<eCEntity*> out;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    const bCVector at = Entity(puppet).GetPosition();
    const bTObjArray<Entity> all = Entity::GetEntities();
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity e = all.GetAt(i);
        eCEntity* o = e.GetInstance();
        if (!o || o == puppet || o == hero) continue;
        const float d = (e.GetPosition() - at).GetMagnitude();
        if (d > 300.0f) continue;
        bool child = false;
        for (eCEntity* p = o->GetParent(); p && !child; p = p->GetParent()) child = p == puppet;
        const char* name = e.GetName().GetText();
        const size_t n = std::strlen(name);
        const bool named = n > 7 && !std::strcmp(name + n - 7, "_Player") &&
                           (!hero || d < (e.GetPosition() - Entity(hero).GetPosition()).GetMagnitude());
        if (child || named) out.push_back(o);
    }
    return out;
}

// The script Shoot (FUN_10086450 in this build's Script_Game.dll) starts with push ebp; mov ebp,esp; and esp,-8 (6
// bytes of complete, position-independent instructions), then sub esp,0x60c. The player's PS_Ranged calls it
// directly, not through the script table, so it is hooked in place.
unsigned char* NativeShoot(gFScript registered)
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x0c, 0x06, 0x00, 0x00};
    auto* code = reinterpret_cast<unsigned char*>(registered);
    return code && std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

// Jump from `target` to `detour`; the returned trampoline runs the 6 stolen bytes and continues in the original.
void* JumpTo(unsigned char* target, void* detour)
{
    constexpr size_t Stolen = 6;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    std::memcpy(trampoline, target, Stolen);
    trampoline[Stolen] = 0xE9;
    const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(target + Stolen) - reinterpret_cast<intptr_t>(trampoline + Stolen + 5));
    std::memcpy(trampoline + Stolen + 1, &back, 4);
    DWORD old = 0;
    if (!VirtualProtect(target, Stolen, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    target[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(detour) - reinterpret_cast<intptr_t>(target + 5));
    std::memcpy(target + 1, &to, 4);
    target[5] = 0x90;
    VirtualProtect(target, Stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, Stolen);
    return trampoline;
}

GEInt GE_STDCALL HookShoot(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    eCEntity* shooter = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* target = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    const bool own = mirror && OwnCaster(shooter);
    Pending p;
    if (own) {
        ++shots;
        p.shooter = shooter;
        if (target) Guid(target, p.target);
        p.targetPlayer = TargetPlayer(target);
        p.before = ArrowsOf(shooter);
        p.since = GetTickCount();
    }
    const GEInt result = originalShoot(spu, self, other, args);
    if (own) pending.push_back(std::move(p));
    return result;
}

// Spell projectiles (fireball and the like): the script MagicProjectile (re/findings/ranged-ai-paths-b.md) spawns the
// spell's projectile with the caster as owner and the spell set on it. _AI_StartCastPhase calls it by name, so the
// script table's entry is wrapped; the projectile is then announced like an arrow, with the item cast from.
GEInt GE_STDCALL WrapMagicProjectile(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    eCEntity* caster = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* target = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    const bool own = mirror && OwnCaster(caster);
    Pending p;
    if (own) {
        ++shots;
        p.shooter = caster;
        if (target) Guid(target, p.target);
        p.targetPlayer = TargetPlayer(target);
        p.before = ArrowsOf(caster);
        p.since = GetTickCount();
        strncpy_s(p.spellItem, NameOf(Entity(caster).Inventory.GetItemFromSlot(gESlot_RightHand)), _TRUNCATE);
        const Entity spell = Entity(caster).Interaction.GetSpell();
        if (spell.GetInstance()) strncpy_s(p.spell, spell.GetName().GetText(), _TRUNCATE);
    }
    const GEInt result = originalMagicProjectile(spu, self, other, args);
    if (own) {
        pending.push_back(std::move(p));
        summon::WatchHits();
    }
    return result;
}

void Install()
{
    const DWORD now = GetTickCount();
    if (!gCSession::GetInstance().IsGameRunning()) return;
    InstallTouch();
    if ((installed && magicInstalled) || now - lastTry < 1000) return;
    lastTry = now;
    if (!magicInstalled) {
        if (gSScript* script = Find(GetScriptAdminExt().GetScripts(), "MagicProjectile")) {
            originalMagicProjectile = script->m_funcScript;
            script->m_funcScript = &WrapMagicProjectile;
            magicInstalled = true;
            Log("shot: MagicProjectile wrapper installed");
        }
    }
    if (installed) return;
    gSScript* script = Find(GetScriptAdminExt().GetScripts(), "Shoot");
    if (!script) return;
    unsigned char* code = NativeShoot(script->m_funcScript);
    if (!code) {
        installed = true;  // not this build's Shoot: leave it alone
        Log("shot: Shoot does not start as expected, shots are not mirrored");
        return;
    }
    originalShoot = reinterpret_cast<gFScript>(JumpTo(code, reinterpret_cast<void*>(&HookShoot)));
    installed = true;
    Log(originalShoot ? "shot: Shoot hooked" : "shot: could not hook Shoot");
}

void Announce(Pending const& p, Entity arrow)
{
    net::ShotNote note;
    note.netId = (static_cast<uint32_t>(net::LocalId()) << 24) | (++counter & 0xFFFFFF);
    std::memcpy(note.target, p.target, 16);
    note.targetPlayer = p.targetPlayer;
    if (p.shooter != remote::ControlledBody()) Guid(p.shooter, note.caster);
    strncpy_s(note.spell, p.spell, _TRUNCATE);
    note.sent = net::Now();
    const bCVector at = arrow.GetPosition();
    const bCQuaternion q = arrow.GetInstance()->GetWorldRotation();
    const bCVector direction = static_cast<bCVector>(arrow.Projectile.TargetDirection);
    note.position[0] = at.GetX(), note.position[1] = at.GetY(), note.position[2] = at.GetZ();
    note.rotation[0] = q.GetX(), note.rotation[1] = q.GetY(), note.rotation[2] = q.GetZ(), note.rotation[3] = q.GetW();
    note.direction[0] = direction.GetX(), note.direction[1] = direction.GetY(), note.direction[2] = direction.GetZ();
    note.velocity = static_cast<float>(arrow.Projectile.ShootVelocity);
    note.path = static_cast<uint8_t>(static_cast<gEProjectilePath>(arrow.Projectile.PathStyle));
    note.touch = static_cast<uint8_t>(static_cast<gEProjectileTouchBehavior>(arrow.Projectile.TouchBehavior));
    note.damage = static_cast<GEI32>(arrow.Damage.DamageAmount);
    note.multiplier = static_cast<GEFloat>(arrow.Damage.DamageHitMultiplier);
    strncpy_s(note.arrow, NameOf(Entity(arrow.GetTemplate())), _TRUNCATE);
    strncpy_s(note.spellItem, p.spellItem, _TRUNCATE);
    net::SendShot(note);
    if (p.shooter != remote::ControlledBody()) ++npcAnnounced;
    ++announced;
    Logf("shot: own %s is net %08X at %.0f %.0f %.0f dir %.2f %.2f %.2f v %.0f path %u", note.arrow, note.netId, at.GetX(),
         at.GetY(), at.GetZ(), direction.GetX(), direction.GetY(), direction.GetZ(), note.velocity, note.path);
}

void Resolve()
{
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < pending.size();) {
        Pending& p = pending[i];
        bool done = false;
        for (eCEntity* e : ArrowsOf(p.shooter)) {
            if (std::find(p.before.begin(), p.before.end(), e) != p.before.end()) continue;
            if (p.seen == e) {
                Announce(p, Entity(e));
                done = true;
            }
            p.seen = e;
            break;
        }
        if (!done && now - p.since > 2000) {
            ++unresolved;
            done = true;
        }
        if (done) pending.erase(pending.begin() + i);
        else ++i;
    }
}

void TraceCopies()
{
    const DWORD now = GetTickCount();
    for (Copy& c : recent) {
        const DWORD t = now - c.at;
        if (c.done || t < c.next) continue;
        c.next = t + 100;
        if (t > 1500) {
            c.done = true;
            continue;
        }
        Entity e = ByGuid(c.guid);
        char point[48];
        if (!e.GetInstance()) {
            if (t < 300) continue;  // a fresh copy only shows up a frame later
            Format(point, "%lu:gone,", t);
            c.done = true;
        } else {
            const bCVector d = e.GetPosition() - c.start;
            Format(point, "%lu:%.0f_%.0f_%.0f,", t, d.GetX(), d.GetY(), d.GetZ());
        }
        strncat_s(c.path, point, _TRUNCATE);
    }
}

void SpawnShot(uint8_t from, net::ShotNote const& note)
{
    ++received;
    static const uint8_t none[16] = {};
    const bool npc = std::memcmp(note.caster, none, 16) != 0;
    if (npc) ++npcReceived;
    if (npc && !contactsReady) {
        Logf("shot: NPC projectile %08X refused; safe local contact hook is unavailable", note.netId);
        return;
    }
    eCEntity* puppetBody = npc ? npcs::NpcByGuid(note.caster) : remote::PuppetOf(from);
    if (!puppetBody) {
        ++noPuppet;
        Logf("shot: net %08X from player %u: caster %s is not loaded here", note.netId, from, npc ? "NPC" : "player");
        return;
    }
    // Numbers from the network go into the game's physics: no NaN or infinity (Fable review 22.10.).
    bool finite = std::isfinite(note.velocity) && std::isfinite(note.multiplier);
    for (float v : note.position) finite = finite && std::isfinite(v);
    for (float v : note.rotation) finite = finite && std::isfinite(v);
    for (float v : note.direction) finite = finite && std::isfinite(v);
    if (!finite) {
        ++badTemplate;
        Logf("shot: net %08X from player %u: not a number in its flight", note.netId, from);
        return;
    }
    const Template arrowTemplate(note.arrow);
    if (!arrowTemplate.IsValid()) {
        ++badTemplate;
        Logf("shot: net %08X from player %u: no template %s", note.netId, from, note.arrow);
        return;
    }
    // The copy is the shooter's: it flies as the shooter's arrow left the bow and belongs to the shooter's puppet.
    Entity puppet(puppetBody);
    Entity copy = Entity::Spawn(arrowTemplate, body::Pose(*puppetBody, note.position, note.rotation));
    if (!copy.GetInstance()) {
        ++badTemplate;
        return;
    }
    if (npc) copy.EnableProcessing(GETrue);
    copy.Damage.DamageAmount = note.damage;
    copy.Damage.DamageHitMultiplier = note.multiplier;
    copy.Damage.ManaUsed = 0;
    copy.Interaction.SetOwner(copyOwner ? puppet : Entity(gCSession::GetInstance().GetHero()));
    copy.Strip.Enabled = GETrue;
    copy.CollisionShape.CreateShape(eECollisionShapeType_Point, eEShapeGroup_Projectile, bCVector(0.0f, 0.0f, 0.0f), bCVector(0.0f, 0.0f, 0.0f));
    copy.EnableCollisionWith(puppet, GEFalse);
    const std::vector<eCEntity*> parts = PartsOf(puppetBody);
    for (eCEntity* part : parts) copy.EnableCollisionWith(Entity(part), GEFalse);
    if (Entity bow = puppet.Inventory.GetItemFromSlot(gESlot_LeftHand); bow.GetInstance()) copy.EnableCollisionWith(bow, GEFalse);
    const auto path = static_cast<gEProjectilePath>(note.path);
    copy.Projectile.PathStyle = path;
    copy.Projectile.TouchBehavior = static_cast<gEProjectileTouchBehavior>(note.touch);
    copy.Projectile.ShootVelocity = note.velocity;
    copy.Projectile.TargetDirection = bCVector(note.direction[0], note.direction[1], note.direction[2]);
    if (path == gEProjectilePath_Physics) {
        copy.RigidBody.LinearDamping = 0.01f;  // as Shoot sets them for the player's arrow
        copy.RigidBody.AngularDamping = 0.01f;
    } else if (Entity target = note.targetPlayer ? Entity(note.targetPlayer == net::LocalId()
                                                        ? remote::ControlledBody() : remote::PuppetOf(note.targetPlayer))
                                                : ByGuid(note.target); target.GetInstance()) {
        copy.Projectile.SetTarget(target);
    }
    bool haveSpell = false;
    if (*note.spell) {
        auto* scene = static_cast<eCSceneAdmin*>(eCModuleAdmin::GetInstance().FindModule(bCString("eCSceneAdmin")));
        const Entity spell(scene ? scene->GetTemplateEntityByName(bCString(note.spell)) : nullptr);
        if (spell.GetInstance() && spell.GetInstance()->GetPropertySet(eEPropertySetType_Magic)) {
            copy.Interaction.SetSpell(spell);
            haveSpell = true;
        }
    }
    if (!haveSpell && *note.spellItem && std::strcmp(note.spellItem, "-")) {
        // A spell's projectile carries its spell (MagicProjectile sets it), which what it hits may read: the puppet gets
        // the item cast from, and its spell goes on the copy, as for summons.
        const Template item(note.spellItem);
        if (item.IsValid() && puppet.Inventory.GetItemAmount(item) < 1) puppet.Inventory.CreateItems(item, 0, 1);
        const GEInt stack = item.IsValid() ? puppet.Inventory.FindStackIndex(item) : -1;
        const Entity spell = stack >= 0 ? puppet.Inventory.GetTemplateItem(stack).Item.GetSpell() : Entity();
        if (spell.GetInstance()) { copy.Interaction.SetSpell(spell); haveSpell = true; }
        else ++noSpell;
    }
    if (npc) {
        uint8_t guid[16];
        Guid(copy.GetInstance(), guid);
        npcReplicas.emplace(reinterpret_cast<char const*>(guid), 16);
        if (*note.spell && !haveSpell)
            Logf("shot: NPC projectile %08X has no local spell metadata for %s", note.netId, note.spell);
    }
    copy.Projectile.Shoot();
    if (npc) ++npcCopies;
    summon::WatchHits();
    ++copies;
    Copy c;
    c.netId = note.netId;
    c.from = from;
    Guid(copy.GetInstance(), c.guid);
    c.at = GetTickCount();
    c.start = bCVector(note.position[0], note.position[1], note.position[2]);
    if (recent.size() >= MaxCopies) recent.erase(recent.begin());
    recent.push_back(c);
    Logf("shot: net %08X from player %u: %s copy flies, path %u v %.0f, %u puppet parts excluded", note.netId, from, note.arrow, note.path,
         note.velocity, static_cast<unsigned>(parts.size()));
}

void OnShot(uint8_t from, net::ShotNote const& note)
{
    static const uint8_t none[16] = {};
    if (!std::memcmp(note.caster, none, 16)) { SpawnShot(from, note); return; }
    if (net::IsHost() || from != 1) { Log("shot: non-host NPC projectile refused"); return; }
    if (timedNpcShots.size() >= 512) { Log("shot: NPC presentation queue full; projectile refused"); return; }
    timedNpcShots.push_back({from, note, net::Now()});
}

// QA, arrows that never hurt NPCs (fable/20261020-112808-q-arrow-hits-question): does a projectile ever touch one?
// gCProjectile_PS::OnTouch(eCEntity* touched, eCContactIterator&) is exported by Game.dll and starts like Shoot
// (push ebp; mov ebp,esp; and esp,-8; sub esp,0x16c). Hooked only once the QA command asks for it.
using TouchFn = void(__fastcall*)(void* projectile, void* edx, eCEntity* touched, eCContactIterator* contact);
TouchFn originalTouch = nullptr;
constexpr int MaxTouchLines = 32;
char touchLines[MaxTouchLines][240];
int touchCount = 0;

void __fastcall HookTouch(void* projectile, void* edx, eCEntity* touched, eCContactIterator* contact)
{
    auto* set = static_cast<eCEntityPropertySet*>(projectile);
    Entity arrow(set->GetEntity());
    if (combat::IgnoreFriendlyHit(touched, arrow.GetInstance())) return;
    if (npcReplicas.empty() && !traceTouches) {
        originalTouch(projectile, edx, touched, contact);
        return;
    }
    const bool armed = static_cast<unsigned char const*>(projectile)[0x78] != 0;  // OnTouch returns at once if 0
    const Entity owner = arrow.GetInstance() ? arrow.Interaction.GetOwner() : Entity();
    const int type = contact ? static_cast<int>(contact->GetCollisionType()) : -1;  // 14 shapes, 100 bones (Animation)
    char const* bone = type == 100 ? contact->GetBone(0).GetText() : "-";
    Format(touchLines[touchCount++ % MaxTouchLines], "TOUCH t=%lu arrow=%s owner=%s owner_puppet_of=%d armed=%d touched=%s npc=%d type=%d bone=%s",
              GetTickCount(), NameOf(arrow), NameOf(owner), owner.GetInstance() ? remote::PuppetOwner(owner.GetInstance()) : 0, armed ? 1 : 0,
              touched ? Entity(touched).GetName().GetText() : "-", touched && touched->GetPropertySet(eEPropertySetType_NPC) ? 1 : 0, type,
              bone && *bone ? bone : "-");
    const bool npcCopy = NpcReplica(arrow.GetInstance());
    if (npcCopy && touched && touched != remote::ControlledBody() && touched->GetPropertySet(eEPropertySetType_NPC))
        return;  // a visual copy can contact our hero, but shared NPC hits belong to the simulator
    struct ContactScope {
        bool before;
        explicit ContactScope(bool npc) : before(npcReplicaContact) { npcReplicaContact = before || npc; }
        ~ContactScope() { npcReplicaContact = before; }
    } scope(npcCopy);
    originalTouch(projectile, edx, touched, contact);
}

void InstallTouch()
{
    static bool tried = false;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return;
    if (!tried) {
        tried = true;
        static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x6c, 0x01, 0x00, 0x00};
        auto* code = game ? reinterpret_cast<unsigned char*>(
                                GetProcAddress(game, "?OnTouch@gCProjectile_PS@@MAEXPAVeCEntity@@AAVeCContactIterator@@@Z"))
                          : nullptr;
        if (code && std::memcmp(code, expected, sizeof(expected)) == 0)
            originalTouch = reinterpret_cast<TouchFn>(JumpTo(code, reinterpret_cast<void*>(&HookTouch)));
        contactsReady = originalTouch != nullptr;
        Log(originalTouch ? "shot: projectile contacts hooked" : "shot: projectile contacts NOT hooked");
    }
}
}

void Start() { net::SetShotHandler(&OnShot); }

bool NpcReplica(eCEntity const* entity)
{
    if (!entity) return false;
    const eCEntityProxy proxy(const_cast<eCEntity*>(entity));
    const std::string guid(reinterpret_cast<char const*>(&proxy.GetEntityID().GetCore()), 16);
    return npcReplicas.count(guid) != 0;
}

bool InNpcReplicaContact() { return npcReplicaContact; }

void OnWorldChange()
{
    timedNpcShots.clear();
    pending.clear();
    recent.clear();
    npcReplicas.clear();
    npcReplicaContact = false;
    traceTouches = false;
}

void NpcCast(char const* argument, LogFn log)
{
    const size_t length = argument ? std::strlen(argument) : 0;
    if (length <= 37 || argument[36] != '_') { log("REFUSED npccast: need GUID_spell-item"); return; }
    unsigned long d1 = 0;
    unsigned d2 = 0, d3 = 0, bytes[8] = {};
    char text[37] = {};
    std::memcpy(text, argument, 36);
    if (sscanf_s(text, "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3,
                 &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5], &bytes[6], &bytes[7]) != 11) {
        log("REFUSED npccast: invalid NPC GUID");
        return;
    }
    _GUID raw{};
    raw.Data1 = d1; raw.Data2 = static_cast<unsigned short>(d2); raw.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) raw.Data4[i] = static_cast<unsigned char>(bytes[i]);
    Entity caster = Entity(bCPropertyID(bCGuid(raw)));
    eCEntity* target = remote::PuppetOf(net::IsHost() ? 2 : 1);
    const Template item(argument + 37);
    if (!caster.GetInstance() || !caster.GetInstance()->GetPropertySet(eEPropertySetType_NPC) ||
        !OwnCaster(caster.GetInstance()) || !target || !item.IsValid()) {
        log("REFUSED npccast: NPC, target player or spell item unavailable");
        return;
    }
    if (caster.Inventory.GetItemAmount(item) < 1) caster.Inventory.CreateItems(item, 0, 1);
    const GEInt stack = caster.Inventory.FindStackIndex(item);
    const Entity spell = stack >= 0 ? caster.Inventory.GetTemplateItem(stack).Item.GetSpell() : Entity();
    if (!spell.GetInstance()) { log("REFUSED npccast: item has no spell"); return; }
    caster.EnableProcessing(GETrue);
    caster.Inventory.HoldStacks(-1, stack);
    caster.Interaction.SetSpell(spell);
    caster.NPC.SetSpellTarget(Entity(target));
    caster.NPC.LastSpell = spell.GetName();
    const GEInt result = GetScriptAdminExt().CallScript(bCString("MagicProjectile"), caster.GetInstance(), target, stack);
    char line[260];
    Format(line, "NPCCAST caster=%s spell=%s target_player=%d result=%d", NameOf(caster), NameOf(spell),
           remote::PuppetOwner(target), result);
    log(line);
}

void TouchLog(LogFn log)
{
    traceTouches = true;
    InstallTouch();
    for (int i = touchCount > MaxTouchLines ? touchCount - MaxTouchLines : 0; i < touchCount; ++i) log(touchLines[i % MaxTouchLines]);
    log("TOUCHLOG end");
}

void Tick()
{
    for (auto it = timedNpcShots.begin(); it != timedNpcShots.end();) {
        eCEntity* caster = npcs::NpcByGuid(it->note.caster);
        if (caster && npcs::Ready(caster) && static_cast<int32_t>(npcs::SeenAt(caster) - it->note.sent) >= 0) {
            SpawnShot(it->from, it->note);
            it = timedNpcShots.erase(it);
        } else if (net::Now() - it->arrived > 10000) {
            Logf("shot: projectile %08x could not be presented on its NPC timeline; no damage applied", it->note.netId);
            it = timedNpcShots.erase(it);
        } else ++it;
    }
    Install();
    if (!pending.empty()) Resolve();
    if (!recent.empty()) TraceCopies();
    for (auto it = npcReplicas.begin(); it != npcReplicas.end();) {
        const auto* guid = reinterpret_cast<uint8_t const*>(it->data());
        it = ByGuid(guid).GetInstance() ? std::next(it) : npcReplicas.erase(it);
    }
}

void Report(LogFn log)
{
    char line[600];
    Format(line, "SHOTSTATE installed=%d hooked=%d magic=%d mirror=%d copy_owner=%d shots=%u announced=%u unresolved=%u received=%u copies=%u "
                    "no_puppet=%u bad_template=%u no_spell=%u npc_announced=%u npc_received=%u npc_copies=%u contacts_ready=%d",
              installed, originalShoot != nullptr, magicInstalled, mirror, copyOwner, shots, announced, unresolved, received, copies, noPuppet,
              badTemplate, noSpell, npcAnnounced, npcReceived, npcCopies, contactsReady ? 1 : 0);
    log(line);
    for (Copy const& c : recent) {
        Entity e = ByGuid(c.guid);
        const Entity owner = e.GetInstance() ? e.Interaction.GetOwner() : Entity();
        Format(line, "SHOTCOPY net=%08X from=%u alive=%d flying=%d group=%d owner=%s owner_puppet_of=%d path=%s", c.netId, c.from,
                  e.GetInstance() ? 1 : 0, e.GetInstance() && e.Projectile.IsFlying() ? 1 : 0,
                  e.GetInstance() ? static_cast<int>(e.GetInstance()->GetCollisionGroup()) : -1, NameOf(owner),
                  owner.GetInstance() ? remote::PuppetOwner(owner.GetInstance()) : 0, c.path);
        log(line);
    }
    log("SHOTSTATE end");
}

void CopyHeroOff(LogFn log)
{
    copyOwner = false;
    log("SHOTCOPYHERO copies of others' arrows belong to our own hero");
}

void MirrorOff(LogFn log)
{
    mirror = false;
    log("SHOTMIRROROFF own shots are not announced");
}
}
