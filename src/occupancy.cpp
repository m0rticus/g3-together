// SPDX-License-Identifier: GPL-3.0-or-later
#include "occupancy.h"

#include "containers.h"
#include "guid.h"
#include "guid_key.h"
#include "hud.h"
#include "i18n.h"
#include "log.h"
#include "net.h"
#include "npcplayback.h"
#include "npcremoval.h"
#include "occupancy_policy.h"
#include "remote.h"
#include "telemetry.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace g3t::occupancy {
namespace {
constexpr uint8_t KindUse = 13;  // net::WorldState kind
constexpr DWORD LookEvery = 250;  // ms
bool enabled = true;  // QA negative control: usesyncoff
DWORD lastLook = 0;
bool hasUsed = false;   // whether this machine's hero uses an object, as last reported
uint8_t usedGuid[16] = {};
char usedName[96] = "-";
std::unordered_map<GuidKey, uint8_t, GuidHash> marked;  // objects whose user here is another player's puppet
unsigned occupied = 0, released = 0, applied = 0, cleared = 0, busy = 0, refused = 0, staleCleared = 0;
bool hasRefused = false;
GuidKey refusedKey{};  // the last place this hero was kept from (one message per try)

bool UsedBy(eCEntity* object, eCEntity* user)
{
    return user && IsWorldInteractable(object) &&
           Entity(object).Interaction.GetUser().GetInstance() == user;
}

void Send(uint8_t const guid[16], bool use)
{
    net::WorldState s{};
    s.kind = KindUse;
    std::memcpy(s.guid, guid, 16);
    s.guard = net::LocalId();
    s.npc = use ? 1 : 0;
    net::SendWorldState(s, 0);
}

// The player whose puppet uses this object here, 0 if none.
uint8_t TakenBy(eCEntity* object)
{
    if (!IsWorldInteractable(object)) return 0;
    uint8_t guid[16];
    guid::CopyEntity(object, guid);
    const auto it = marked.find(ToGuidKey(guid));
    return it != marked.end() ? it->second : 0;
}

// The hero sets out to use a place another player uses (PS_Interact walks it there, then makes it the user): stop it, as
// an NPC sitting there would keep it off, and keep the other player's puppet as the user.
void Refuse(eCEntity* hero)
{
    if (!hero || !hero->GetPropertySet(eEPropertySetType_ScriptRoutine) ||
        !hero->GetPropertySet(eEPropertySetType_NPC) ||
        !hero->GetPropertySet(eEPropertySetType_Focus)) return;
    Entity self(hero);
    if (self.Routine.GetCurrentTask() != bCString("PS_Interact")) { hasRefused = false; return; }
    eCEntity* object = self.NPC.GetCurrentTarget().GetInstance();
    uint8_t player = TakenBy(object);
    if (!player) {
        object = self.Focus.GetFocusEntity().GetInstance();
        player = TakenBy(object);
    }
    eCEntity* puppet = player ? remote::PuppetOf(player) : nullptr;
    if (!puppet) return;
    self.Routine.SetTask(bCString("PS_Interact_End"));
    Entity(object).Interaction.SetUser(Entity(puppet));
    ++refused;
    Logf("occupancy: this player wanted to use %s, which player %u uses; refused", object->GetName().GetText(), player);
    uint8_t objectGuid[16] = {};
    guid::CopyEntity(object, objectGuid);
    const GuidKey currentRefused = ToGuidKey(objectGuid);
    if (hasRefused && currentRefused == refusedKey) return;
    refusedKey = currentRefused;
    hasRefused = true;
    telemetry::OnOccupancyEvent("refused_in_use", player, object, objectGuid, puppet, "local_interact_refused");
    wchar_t name[64] = {}, text[160] = {};
    if (char const* peer = net::PeerName(player))
        MultiByteToWideChar(CP_UTF8, 0, peer, -1, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
    name[63] = L'\0';
    if (wchar_t const* fmt = i18n::Text(i18n::PlaceInUse)) {
        _snwprintf_s(text, _TRUNCATE, fmt, name[0] ? name : L"?");
        hud::Message(hud::Info, text);
    }
}

// The object the hero uses: the game sets it as the object's user; the hero's target or focus points at it.
eCEntity* InUse(eCEntity* hero)
{
    if (!hero) return nullptr;
    Entity self(hero);
    if (self.IsDead() || self.IsDown()) return nullptr;
    if (hasUsed) {
        eCEntity* current = guid::Resolve(usedGuid).GetInstance();
        if (UsedBy(current, hero)) return current;
    }
    if (hero->GetPropertySet(eEPropertySetType_NPC))
        if (eCEntity* target = self.NPC.GetCurrentTarget().GetInstance(); UsedBy(target, hero)) return target;
    if (hero->GetPropertySet(eEPropertySetType_Focus))
        if (eCEntity* focus = self.Focus.GetFocusEntity().GetInstance(); UsedBy(focus, hero)) return focus;
    return nullptr;
}

occupancy_policy::InteractUserState BuildUserState(eCEntity* object)
{
    occupancy_policy::InteractUserState s{};
    s.validObject = IsWorldInteractable(object);
    if (!s.validObject) return s;
    eCEntity* user = Entity(object).Interaction.GetUser().GetInstance();
    s.hasUser = user != nullptr;
    if (!user) return s;
    s.userIsCharacter = user->GetPropertySet(eEPropertySetType_NPC) != nullptr;
    if (!s.userIsCharacter) return s;
    s.userIsLocalHero = user == remote::ControlledBody() || user == gCSession::GetInstance().GetHero();
    s.puppetOwner = static_cast<uint8_t>(remote::PuppetOwner(user));
    s.markedPlayer = TakenBy(object);
    s.userSuppressed = npcremoval::Suppressed(user);
    s.userPassive = npcplayback::IsPassive(user);
    if (user->GetPropertySet(eEPropertySetType_ScriptRoutine)) {
        Entity actor(user);
        const int aiMode = static_cast<int>(static_cast<gEAIMode>(actor.Routine.AIMode));
        s.userDeadOrDown = actor.IsDead() || actor.IsDown() ||
                           aiMode == gEAIMode_Dead || aiMode == gEAIMode_Down;
        s.userInactiveRoutine = aiMode == gEAIMode_Combat ||
                                aiMode == gEAIMode_Flee ||
                                aiMode == gEAIMode_Abandoned ||
                                aiMode == gEAIMode_Arena;
    }
    return s;
}

void ScrubRoi(eCEntity* onlyUser, char const* reason)
{
    auto const& interactions = gCInteractionAdmin::GetInstance().GetInteractionsInROI();
    for (int i = 0; i < interactions.GetCount(); ++i) {
        auto* interaction = interactions.GetAt(i);
        eCEntity* object = interaction ? interaction->GetEntity() : nullptr;
        eCEntity const* user = interaction ? interaction->GetUser().GetEntity() : nullptr;
        if (object && user && (!onlyUser || user == onlyUser))
            ClearStaleUser(object, reason);
    }
}

void ScrubStaleUsers()
{
    ScrubRoi(nullptr, "roi-scrub");
    for (auto const& entry : marked) {
        eCEntity* instance = guid::Resolve(entry.first.data()).GetInstance();
        if (!IsWorldInteractable(instance)) continue;
        if (Entity(instance).Interaction.GetUser().GetInstance()) continue;
        if (eCEntity* puppet = remote::PuppetOf(entry.second))
            Entity(instance).Interaction.SetUser(Entity(puppet));
    }
}
}

bool IsWorldInteractable(eCEntity const* object)
{
    eCEntity* mutableObject = const_cast<eCEntity*>(object);
    return mutableObject &&
           !mutableObject->GetPropertySet(eEPropertySetType_NPC) &&
           !mutableObject->GetPropertySet(eEPropertySetType_Item) &&
           mutableObject->GetPropertySet(eEPropertySetType_Interaction) &&
           !containers::IsPersonalInventory(mutableObject);
}

bool StaleUser(eCEntity* object)
{
    return occupancy_policy::ShouldClearInteractUser(BuildUserState(object));
}

bool ClearStaleUser(eCEntity* object, char const* reason)
{
    const occupancy_policy::InteractUserState state = BuildUserState(object);
    if (!occupancy_policy::ShouldClearInteractUser(state)) return false;
    Entity interact(object);
    eCEntity* user = interact.Interaction.GetUser().GetInstance();
    interact.Interaction.SetUser(Entity());
    if (occupancy_policy::ShouldDecrementAnchorUserCount(state)) {
        Entity anchor = interact.Interaction.GetAnchorPoint();
        if (anchor.GetInstance()) {
            if (auto* anchorPs = static_cast<gCAnchor_PS*>(
                    anchor.GetInstance()->GetPropertySet(eEPropertySetType_Anchor))) {
                const GEU32 count = anchorPs->GetUserCount();
                if (count > 0) anchorPs->SetUserCount(count - 1);
            }
        }
    }
    ++staleCleared;
    Logf("occupancy: cleared stale user %s on %s (%s)",
         user ? user->GetName().GetText() : "-",
         object->GetName().GetText(),
         reason ? reason : "stale");
    uint8_t objGuid[16] = {};
    guid::CopyEntity(object, objGuid);
    telemetry::OnOccupancyEvent("stale_cleared", 0, object, objGuid, user, reason ? reason : "stale");
    return true;
}

void ReleaseNpc(eCEntity* npc, bool scanRoi)
{
    if (!npc || !npc->GetPropertySet(eEPropertySetType_NPC)) return;
    if (npc->GetPropertySet(eEPropertySetType_Navigation)) {
        eCEntity* dest = Entity(npc).Navigation.GetCurrentDestinationPoint().GetInstance();
        if (dest && dest->GetPropertySet(eEPropertySetType_Interaction) &&
            Entity(dest).Interaction.GetUser().GetInstance() == npc) {
            ClearStaleUser(dest, "silence");
        }
    }
    if (!scanRoi) return;
    ScrubRoi(npc, "silence-roi");
}

void Tick()
{
    if (!net::Connected() || !gCSession::GetInstance().IsGameRunning()) {
        hasUsed = false;
        std::memset(usedGuid, 0, sizeof(usedGuid));
        strcpy_s(usedName, "-");
        hasRefused = false;
        refusedKey = {};
        return;
    }
    if (enabled && !marked.empty())
        if (eCEntity* hero = remote::ControlledBody()) Refuse(hero);  // every frame: before the hero gets there
    const DWORD now = GetTickCount();
    if (now - lastLook < LookEvery) return;
    lastLook = now;
    ScrubStaleUsers();
    eCEntity* hero = remote::ControlledBody();
    eCEntity* object = hero ? InUse(hero) : nullptr;
    uint8_t nextGuid[16] = {};
    if (object) guid::CopyEntity(object, nextGuid);
    const bool same = object ? (hasUsed && std::memcmp(nextGuid, usedGuid, sizeof(usedGuid)) == 0) : !hasUsed;
    if (same) return;
    if (hasUsed && enabled) {
        Send(usedGuid, false);
        ++released;
        Logf("occupancy: this player let go of %s", usedName);
        telemetry::OnOccupancyEvent("local_release", net::LocalId(), guid::Resolve(usedGuid).GetInstance(), usedGuid, hero, "let_go");
    }
    if (!object) {
        hasUsed = false;
        std::memset(usedGuid, 0, sizeof(usedGuid));
        strcpy_s(usedName, "-");
        return;
    }
    std::memcpy(usedGuid, nextGuid, sizeof(usedGuid));
    strncpy_s(usedName, object->GetName().GetText(), _TRUNCATE);
    hasUsed = true;
    if (!enabled) return;
    Send(usedGuid, true);
    ++occupied;
    Logf("occupancy: this player uses %s", usedName);
    telemetry::OnOccupancyEvent("local_occupy", net::LocalId(), object, usedGuid, hero, "in_use");
}

void OnState(uint8_t from, net::WorldState const& s)
{
    if (s.npc > 1 || IsZeroGuid(s.guid)) return;
    uint8_t player = 0;
    if (net::IsHost()) {
        if (from < 2 || from >= net::MaxPlayers || !net::InWorld(from)) return;
        if (s.guard && s.guard != from) return;
        player = from;
    } else {
        if (from != 1) return;
        player = s.guard ? s.guard : from;
        if (!player || player >= net::MaxPlayers || player == net::LocalId()) return;
    }
    Entity object = guid::Resolve(s.guid);
    eCEntity* instance = object.GetInstance();
    const GuidKey key = ToGuidKey(s.guid);
    if (!s.npc) {
        if (auto it = marked.find(key); it != marked.end() && it->second == player) marked.erase(it);
        if (IsWorldInteractable(instance)) {
            eCEntity* user = object.Interaction.GetUser().GetInstance();
            if (user && remote::PuppetOwner(user) == player) {
                object.Interaction.SetUser(Entity());
                ++cleared;
                Logf("occupancy: player %u let go of %s; free here too", player, instance->GetName().GetText());
                telemetry::OnOccupancyEvent("remote_release", player, instance, s.guid, user, "remote_let_go");
            }
        }
        return;
    }
    if (auto it = marked.find(key); it != marked.end() && it->second != player) {
        ++busy;
        return;
    }
    if (!instance) {
        marked[key] = player;
        return;
    }
    if (!IsWorldInteractable(instance)) return;
    eCEntity* puppet = remote::PuppetOf(player);
    ClearStaleUser(instance, "remote-takeover");
    eCEntity* user = object.Interaction.GetUser().GetInstance();
    if (user && user != puppet && (!remote::PuppetOwner(user) || remote::PuppetOwner(user) != player)) {
        ++busy;  // someone here sits there already (this hero, an NPC, or another puppet): it stays theirs
        Logf("occupancy: player %u uses %s, which %s uses here", player, instance->GetName().GetText(), Entity(user).GetName().GetText());
        telemetry::OnOccupancyEvent("remote_busy_conflict", player, instance, s.guid, user, "occupied_locally");
        return;
    }
    marked[key] = player;
    if (!puppet) return;
    object.Interaction.SetUser(Entity(puppet));
    ++applied;
    Logf("occupancy: player %u uses %s; taken here too", player, instance->GetName().GetText());
    telemetry::OnOccupancyEvent("remote_occupy", player, instance, s.guid, puppet, "remote_use");
}

void Forget(int player, eCEntity* puppet)
{
    for (auto it = marked.begin(); it != marked.end();) {
        if (it->second != player) { ++it; continue; }
        Entity object = guid::Resolve(it->first.data());
        if (object.GetInstance() && puppet && object.Interaction.GetUser().GetInstance() == puppet) {
            object.Interaction.SetUser(Entity());
            ++cleared;
        }
        it = marked.erase(it);
    }
}

void OnWorldChange()
{
    hasUsed = false;
    std::memset(usedGuid, 0, sizeof(usedGuid));
    strcpy_s(usedName, "-");
    hasRefused = false;
    refusedKey = {};
    lastLook = 0;
    marked.clear();
}

void Report(LogFn log)
{
    char line[224];
    Format(line, "OCCUPANCY enabled=%d using=%s occupied=%u released=%u applied=%u cleared=%u busy=%u marked=%u refused=%u stale_cleared=%u", enabled ? 1 : 0,
              hasUsed ? usedName : "-", occupied, released, applied, cleared, busy, static_cast<unsigned>(marked.size()), refused, staleCleared);
    log(line);
}

void Off(LogFn log)
{
    enabled = false;
    log("USESYNCOFF ok=1");
}

void StandUp(LogFn log)
{
    char line[160];
    eCEntity* hero = remote::ControlledBody();
    if (!hero) { log("REFUSED unuse: no hero"); return; }
    Entity self(hero);
    self.Routine.SetTask(bCString("PS_Interact_End"));
    Format(line, "UNUSE task=%s", self.Routine.GetCurrentTask().GetText());
    log(line);
}
}
