// SPDX-License-Identifier: GPL-3.0-or-later
#include "doors.h"

#include "containers.h"
#include "guid.h"
#include "guid_key.h"
#include "log.h"
#include "story.h"
#include "world.h"

#include <Engine.h>
#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace g3t::doors {
namespace {
constexpr uint8_t Kind = 10;  // world state kind: guard = open, alignment = locked, npcClass bits 1 door, 2 lock
constexpr uint32_t ScanEvery = 500;  // ms

struct Seen {
    uint8_t open = 0, locked = 0, parts = 0;
};
std::unordered_map<GuidKey, Seen, GuidHash> seen;    // what this machine last saw (or was told) per entity
std::unordered_map<GuidKey, Seen, GuidHash> wanted;  // states from elsewhere, applied once the entity is processed here
// Native loaded state and live changes are authoritative; clients query doors at first sight.
std::unordered_map<GuidKey, Seen, GuidHash> changed;
constexpr uint8_t Request = 0x80;
constexpr uint8_t Baseline = 0x40;
std::unordered_set<GuidKey, GuidHash> baselines;
std::unordered_map<GuidKey, uint32_t, GuidHash> requested;
struct PendingRequest { uint32_t generation[net::MaxPlayers]{}; };
std::unordered_map<GuidKey, PendingRequest, GuidHash> pendingRequests;
struct InitialDoors { uint32_t generation = 0; bool admitted = false; std::unordered_set<GuidKey, GuidHash> keys; };
std::array<InitialDoors, net::MaxPlayers> initialDoors;
bool scanned = false;
unsigned pendingNearby = 0;
bool wasConnected = false;
uint32_t lastScan = 0;
uint32_t settleStart = 0;
bool sync = true;  // QA negative control
unsigned sent = 0, received = 0, applied = 0, watched = 0;

GuidKey Key(uint8_t const guid[16]) { return ToGuidKey(guid); }

Entity ResolveKey(GuidKey const& key)
{
    return guid::Resolve(key.data());
}

Seen Read(eCEntity* entity)
{
    Entity e(entity);
    Seen s;
    if (entity->GetPropertySet(eEPropertySetType_Door)) s.parts |= 1, s.open = e.Door.IsOpen() ? 1 : 0;
    if (entity->GetPropertySet(eEPropertySetType_Lock)) s.parts |= 2, s.locked = e.Lock.IsLocked() ? 1 : 0;
    return s;
}

Seen MergeSeen(Seen dst, Seen const& src)
{
    if (src.parts & 1) { dst.parts |= 1; dst.open = src.open; }
    if (src.parts & 2) { dst.parts |= 2; dst.locked = src.locked; }
    return dst;
}

bool Same(Seen const& a, Seen const& b)
{
    return (a.parts & b.parts) == b.parts && (!(b.parts & 1) || a.open == b.open) &&
           (!(b.parts & 2) || a.locked == b.locked);
}

// Bring the entity to the wanted state the way the game's own code changes it (PSLock status, PSDoor::Open/Close).
void Apply(eCEntity* entity, Seen const& want)
{
    Entity e(entity);
    if ((want.parts & 2) && entity->GetPropertySet(eEPropertySetType_Lock))
        e.Lock.Status = want.locked ? gELockStatus_Locked : gELockStatus_Unlocked;
    if ((want.parts & 1) && entity->GetPropertySet(eEPropertySetType_Door)) {
        const bool open = e.Door.IsOpen() != GEFalse;
        if (want.open && !open) e.Door.Open(GEFalse);
        else if (!want.open && open) e.Door.Close(GEFalse);
    }
    ++applied;
}

void Send(uint8_t const guid[16], Seen const& s)
{
    if (net::IsHost()) {
        const GuidKey key = Key(guid);
        changed[key] = MergeSeen(changed[key], s);
    }
    net::WorldState w{};
    w.kind = Kind;
    std::memcpy(w.guid, guid, 16);
    w.guard = s.open;
    w.alignment = s.locked;
    w.npcClass = s.parts;
    net::SendWorldState(w, 0);
    ++sent;
}

bool Shared(eCEntity* entity)
{
    return entity && (entity->GetPropertySet(eEPropertySetType_Door) || entity->GetPropertySet(eEPropertySetType_Lock)) &&
           !entity->GetPropertySet(eEPropertySetType_NPC) && !entity->GetPropertySet(eEPropertySetType_Item) &&
           !containers::IsPersonalInventory(entity);
}
bool Reply(uint8_t to, GuidKey const& key)
{
    auto& initial = initialDoors[to];
    const uint32_t generation = net::PeerGeneration(to);
    const bool inWorld = net::InWorld(to);
    if (inWorld) initial.admitted = true;
    if (initial.generation != generation || (!inWorld && initial.admitted)) {
        initial = {};
        initial.generation = generation;
    }
    if (!inWorld && !initial.keys.count(key) && initial.keys.size() >= 1024) {
        Log("doors: initial baseline interest limit exceeded; admission refused"); return false;
    }
    Entity object = ResolveKey(key);
    eCEntity* instance = object.GetInstance();
    if (instance && !Shared(instance)) return false;
    Seen state{};
    bool known = false;
    if (Shared(instance)) {
        state = Read(instance);
        known = true;
    }
    if (auto it = changed.find(key); it != changed.end()) {
        state = known ? MergeSeen(state, it->second) : it->second;
        known = true;
    }
    if (auto it = wanted.find(key); it != wanted.end()) {
        state = known ? MergeSeen(state, it->second) : it->second;
        known = true;
    }
    net::WorldState note{};
    note.kind = Kind;
    note.npcClass = Baseline | (known ? state.parts : 0);
    note.guard = state.open;
    note.alignment = state.locked;
    std::memcpy(note.guid, key.data(), 16);
    if (!net::SendWorldStateTo(to, note)) return false;
    if (!inWorld) initial.keys.insert(key);
    return true;
}
}

void Tick()
{
    if (!sync || !net::Joined() || !gCSession::GetInstance().IsGameRunning() ||
        (!net::IsHost() && !story::AdmissionStarted())) { wasConnected = false; return; }
    if (!wasConnected) {
        wasConnected = true;
        if (!net::IsHost()) {  // joined (or joined again): ask the host for this session's changes
            net::WorldState w{};
            w.kind = Kind;
            w.npcClass = Request;
            net::SendWorldState(w, 0);
        }
    }
    const uint32_t now = GetTickCount();
    if (now - lastScan < ScanEvery) return;
    lastScan = now;
    if (net::IsHost())
        for (auto request = pendingRequests.begin(); request != pendingRequests.end();) {
            bool pending = false;
            for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
                auto& generation = request->second.generation[id];
                if (!generation) continue;
                if (!net::PeerJoined(id) || generation != net::PeerGeneration(id) || Reply(id, request->first)) generation = 0;
                else pending = true;
            }
            if (!pending) request = pendingRequests.erase(request); else ++request;
        }
    bTSmallArray<eCEntity*> const* every = world::ProcessedEntities();
    unsigned count = 0;
    pendingNearby = 0;
    scanned = every != nullptr;
    for (GEU16 i = 0; every && i < every->GetCount(); ++i) {
        eCEntity* entity = every->GetAt(i);
        if (!Shared(entity)) continue;
        ++count;
        uint8_t guid[16];
        guid::CopyEntity(entity, guid);
        const GuidKey key = Key(guid);
        Seen now_ = Read(entity);
        if (!net::IsHost() && !baselines.count(key)) {
            ++pendingNearby;
            auto& next = requested[key];
            if (!next || static_cast<int32_t>(now - next) >= 0) {
                next = now + 1000;
                net::WorldState request{};
                request.kind = Kind; request.npcClass = Request;
                std::memcpy(request.guid, guid, 16);
                net::SendWorldStateTo(1, request);
            }
            seen[key] = now_;
            continue;
        }
        if (auto want = wanted.find(key); want != wanted.end()) {
            if (!Same(now_, want->second)) {
                Apply(entity, want->second);
                now_ = Read(entity);
                Logf("doors: %s set to open=%u locked=%u (now open=%u locked=%u)", entity->GetName().GetText(), want->second.open,
                     want->second.locked, now_.open, now_.locked);
            }
            if (Same(now_, want->second)) wanted.erase(want);
            else if (!net::IsHost() && !net::InWorld(net::LocalId())) ++pendingNearby;
            seen[key] = now_;
            continue;
        }
        auto last = seen.find(key);
        if (last == seen.end()) {
            seen[key] = now_;
            continue;
        }
        if (!net::IsHost() && !net::InWorld(net::LocalId())) continue;
        if (!Same(now_, last->second)) {
            last->second = now_;
            Send(guid, now_);
            Logf("doors: %s changed here: open=%u locked=%u", entity->GetName().GetText(), now_.open, now_.locked);
        }
    }
    watched = count;
}

void OnState(uint8_t from, net::WorldState const& s)
{
    if (!sync) return;
    if (s.npcClass & Request) {  // host: a player joined; send this session's changes directly as baselines
        if (net::IsHost() && net::PeerJoined(from) && net::InWorld(1)) {
            if (!IsZeroGuid(s.guid)) {
                const GuidKey key = Key(s.guid);
                Entity object = guid::Resolve(s.guid);
                if (object.GetInstance() && !Shared(object.GetInstance())) return;
                if (!Reply(from, key)) {
                    if (!pendingRequests.count(key) && pendingRequests.size() >= 1024) {
                        Log("doors: baseline request queue full; client will retry"); return;
                    }
                    pendingRequests[key].generation[from] = net::PeerGeneration(from);
                }
                return;
            }
            for (auto const& entry : changed) {
                net::WorldState note{};
                note.kind = Kind;
                note.npcClass = Baseline | entry.second.parts;
                note.guard = entry.second.open;
                note.alignment = entry.second.locked;
                std::memcpy(note.guid, entry.first.data(), 16);
                net::SendWorldStateTo(from, note);
            }
        }
        return;
    }
    if ((s.npcClass & Baseline) && (net::IsHost() || from != 1 || !story::AdmissionStarted())) return;
    if (!(s.npcClass & Baseline) && (net::IsHost() ? !net::InWorld(from) : from != 1)) return;
    if (IsZeroGuid(s.guid) || s.guard > 1 || s.alignment > 1 || (s.npcClass & ~(Baseline | 3))) return;
    Entity object = guid::Resolve(s.guid);
    eCEntity* instance = object.GetInstance();
    if (instance && !Shared(instance)) return;
    const GuidKey key = Key(s.guid);
    if ((s.npcClass & Baseline) && !(s.npcClass & 3)) {
        ++received;
        baselines.insert(key);
        requested.erase(key);
        return;
    }
    if (!(s.npcClass & 3)) return;
    ++received;
    Seen want;
    want.open = s.guard ? 1 : 0;
    want.locked = s.alignment ? 1 : 0;
    want.parts = s.npcClass & 3;
    const Seen base = seen.count(key) ? seen[key] : (Shared(instance) ? Read(instance) : Seen{});
    wanted[key] = MergeSeen(wanted.count(key) ? wanted[key] : base, want);
    if (s.npcClass & Baseline) { baselines.insert(key); requested.erase(key); }
    seen[key] = MergeSeen(base, want);  // so that applying it is not taken for a change made here
    if (net::IsHost()) {
        changed[key] = MergeSeen(changed[key], want);
        net::SendWorldState(s, from);
    }
}

void OnWorldChange()
{
    seen.clear();
    wanted.clear();
    changed.clear();  // the host loaded a save: the save is the state now
    baselines.clear(); requested.clear(); pendingRequests.clear();
    initialDoors = {};
    scanned = false; pendingNearby = 0;
    wasConnected = false;  // a client that just loaded the host's save asks again
    settleStart = 0;
}
void PrepareAdmission()
{
    baselines.clear(); requested.clear(); wanted.clear(); seen.clear();
    scanned = false; pendingNearby = 0; lastScan = 0; wasConnected = false;
}
bool AdmissionReady()
{
    if (net::IsHost()) return true;
    if (!scanned || pendingNearby) return false;
    auto const* every = world::ProcessedEntities();
    if (!every) return false;
    for (GEU16 i = 0; i < every->GetCount(); ++i) {
        auto* entity = every->GetAt(i);
        if (!Shared(entity)) continue;
        uint8_t guid[16];
        guid::CopyEntity(entity, guid);
        const auto key = Key(guid);
        if (!baselines.count(key) || wanted.count(key)) return false;
    }
    return true;
}
bool FinishAdmission(uint8_t player)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers) return false;
    auto& initial = initialDoors[player];
    if (initial.generation && initial.generation != net::PeerGeneration(player)) return false;
    for (auto const& key : initial.keys) if (!Reply(player, key)) return false;
    return true;
}
void ResetSettle() { settleStart = 0; }

bool SettleForSave()
{
    if (!net::IsHost()) return false;
    if (wanted.empty()) { settleStart = 0; return true; }
    const uint32_t now = GetTickCount();
    if (!settleStart) settleStart = now ? now : 1;
    for (auto entry = wanted.begin(); entry != wanted.end();) {
        Entity object = ResolveKey(entry->first);
        eCEntity* instance = object.GetInstance();
        if (!instance) { ++entry; continue; }
        if (!Shared(instance)) { entry = wanted.erase(entry); continue; }
        Seen current = Read(instance);
        if (!Same(current, entry->second)) {
            Apply(instance, entry->second);
            current = Read(instance);
        }
        seen[entry->first] = MergeSeen(current, entry->second);
        if (!Same(current, entry->second) && now - settleStart < 500) return false;
        entry = wanted.erase(entry);
    }
    settleStart = 0;
    return true;
}

void Report(LogFn log)
{
    char line[200];
    Format(line, "DOORS sync=%d watched=%u sent=%u received=%u applied=%u pending=%zu changed=%zu", sync ? 1 : 0, watched, sent, received,
              applied, wanted.size(), changed.size());
    log(line);
}

void SyncOff(LogFn log)
{
    sync = false;
    log("DOORSYNCOFF ok=1");
}

// QA (movers, P6): the mechanisms (PSMover: gates, lifts; telekinesis toggles one, MagicTelekinesis -> PSMover::Toggle)
// this machine processes, with their state (0 open, 1 opening, 2 closed, 3 closing) and whether they loop. Not synchronized:
// moverscan 25.10. found none at the first members of 31 towns, so a sync could not be tested.
void Movers(LogFn log)
{
    char line[300];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    bTSmallArray<eCEntity*> const* every = world::ProcessedEntities();
    for (GEU16 i = 0; hero && every && i < every->GetCount(); ++i) {
        eCEntity* entity = every->GetAt(i);
        auto* mover = entity ? static_cast<gCMover_PS*>(entity->GetPropertySet(eEPropertySetType_Mover)) : nullptr;
        if (!mover) continue;
        char guid[48];
        guid::FormatEntity(entity, guid, sizeof(guid));
        const bCVector& p = entity->GetWorldPosition();
        const float d = (p - hero->GetWorldPosition()).GetMagnitude();
        Format(line, "MOVER name=%s guid=%s state=%d loop=%d distance=%.0f pos=%.0f_%.0f_%.0f",
                  entity->GetName().GetText(), guid,
                  static_cast<int>(static_cast<gEMoverState>(Entity(entity).Mover.MoverState)), mover->AccessLoopAni() ? 1 : 0, d,
                  p.GetX(), p.GetY(), p.GetZ());
        log(line);
    }
    log("MOVER end");
}
}
