// SPDX-License-Identifier: GPL-3.0-or-later
#include "doors.h"

#include "containers.h"
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
std::unordered_map<std::string, Seen> seen;    // what this machine last saw (or was told) per entity
std::unordered_map<std::string, Seen> wanted;  // states from elsewhere, applied once the entity is processed here
// Native loaded state and live changes are authoritative; clients query doors at first sight.
std::unordered_map<std::string, Seen> changed;
constexpr uint8_t Request = 0x80;
constexpr uint8_t Baseline = 0x40;
std::unordered_set<std::string> baselines;
std::unordered_map<std::string, uint32_t> requested;
struct PendingRequest { uint32_t generation[net::MaxPlayers]{}; };
std::unordered_map<std::string, PendingRequest> pendingRequests;
struct InitialDoors { uint32_t generation = 0; std::unordered_set<std::string> keys; };
std::array<InitialDoors, net::MaxPlayers> initialDoors;
bool scanned = false;
unsigned pendingNearby = 0;
bool wasConnected = false;
uint32_t lastScan = 0;
bool sync = true;  // QA negative control
unsigned sent = 0, received = 0, applied = 0, watched = 0;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

void GuidOf(eCEntity* entity, uint8_t out[16])
{
    const eCEntityProxy proxy(entity);
    std::memcpy(out, &proxy.GetEntityID().GetCore(), 16);
}

Seen Read(eCEntity* entity)
{
    Entity e(entity);
    Seen s;
    if (entity->GetPropertySet(eEPropertySetType_Door)) s.parts |= 1, s.open = e.Door.IsOpen() ? 1 : 0;
    if (entity->GetPropertySet(eEPropertySetType_Lock)) s.parts |= 2, s.locked = e.Lock.IsLocked() ? 1 : 0;
    return s;
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
    if (net::IsHost()) changed[Key(guid)] = s;
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
bool Reply(uint8_t to, std::string const& key)
{
    auto& initial = initialDoors[to];
    const uint32_t generation = net::PeerGeneration(to);
    if (initial.generation != generation) { initial = {}; initial.generation = generation; }
    if (!net::InWorld(to) && !initial.keys.count(key) && initial.keys.size() >= 1024) {
        Log("doors: initial baseline interest limit exceeded; admission refused"); return false;
    }
    _GUID raw;
    std::memcpy(&raw, key.data(), 16);
    Entity object{bCPropertyID(bCGuid(raw))};
    if (!Shared(object.GetInstance())) return false;
    const auto desired = wanted.find(key);
    const Seen state = desired == wanted.end() ? Read(object.GetInstance()) : desired->second;
    net::WorldState note{};
    note.kind = Kind; note.npcClass = Baseline | state.parts;
    note.guard = state.open; note.alignment = state.locked;
    std::memcpy(note.guid, key.data(), 16);
    if (!net::SendWorldStateTo(to, note)) return false;
    if (!net::InWorld(to)) initial.keys.insert(key);
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
        if (!entity || (!entity->GetPropertySet(eEPropertySetType_Door) && !entity->GetPropertySet(eEPropertySetType_Lock))) continue;
        if (entity->GetPropertySet(eEPropertySetType_NPC) || entity->GetPropertySet(eEPropertySetType_Item)) continue;
        if (containers::IsPersonalInventory(entity)) continue;
        ++count;
        uint8_t guid[16];
        GuidOf(entity, guid);
        const std::string key = Key(guid);
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
            if (net::IsHost()) Send(guid, now_);
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
    if (s.npcClass & Request) {  // host: a player joined; everyone gets this session's changes again (unchanged ones are ignored)
        if (net::IsHost() && net::PeerJoined(from) && net::InWorld(1)) {
            bool nonzero = false;
            for (uint8_t byte : s.guid) nonzero = nonzero || byte != 0;
            if (nonzero) {
                const std::string key = Key(s.guid);
                if (!Reply(from, key)) {
                    if (!pendingRequests.count(key) && pendingRequests.size() >= 1024) {
                        Log("doors: baseline request queue full; client will retry"); return;
                    }
                    pendingRequests[key].generation[from] = net::PeerGeneration(from);
                }
                return;
            }
            for (auto const& entry : changed) {
                uint8_t guid[16];
                std::memcpy(guid, entry.first.data(), 16);
                Send(guid, entry.second);
            }
        }
        return;
    }
    if ((s.npcClass & Baseline) && (net::IsHost() || from != 1 || !story::AdmissionStarted())) return;
    if (!(s.npcClass & Baseline) && (net::IsHost() ? !net::InWorld(from) : from != 1)) return;
    if (s.guard > 1 || s.alignment > 1 || !(s.npcClass & 3) || (s.npcClass & ~(Baseline | 3))) return;
    _GUID raw;
    std::memcpy(&raw, s.guid, 16);
    Entity object{bCPropertyID(bCGuid(raw))};
    if (containers::IsPersonalInventory(object.GetInstance())) return;
    ++received;
    Seen want;
    want.open = s.guard ? 1 : 0;
    want.locked = s.alignment ? 1 : 0;
    want.parts = s.npcClass & 3;
    const std::string key = Key(s.guid);
    wanted[key] = want;
    if (s.npcClass & Baseline) { baselines.insert(key); requested.erase(key); }
    seen[key] = want;  // so that applying it is not taken for a change made here
    if (net::IsHost()) changed[key] = want;
    if (net::IsHost()) net::SendWorldState(s, from);
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
}
void PrepareAdmission()
{
    baselines.clear(); requested.clear(); scanned = false; pendingNearby = 0; lastScan = 0;
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
        GuidOf(entity, guid);
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
bool SettleForSave()
{
    if (!net::IsHost()) return false;
    for (auto entry = wanted.begin(); entry != wanted.end();) {
        _GUID raw;
        std::memcpy(&raw, entry->first.data(), 16);
        Entity object{bCPropertyID(bCGuid(raw))};
        eCEntity* instance = object.GetInstance();
        if (!instance) { ++entry; continue; }
        if (!Shared(instance)) { entry = wanted.erase(entry); continue; }
        if (!Same(Read(instance), entry->second)) Apply(instance, entry->second);
        if (!Same(Read(instance), entry->second)) return false;
        seen[entry->first] = entry->second;
        entry = wanted.erase(entry);
    }
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
        uint8_t guid[16];
        GuidOf(entity, guid);
        const bCVector& p = entity->GetWorldPosition();
        const float d = (p - hero->GetWorldPosition()).GetMagnitude();
        Format(line, "MOVER name=%s guid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X state=%d loop=%d distance=%.0f pos=%.0f_%.0f_%.0f",
                  entity->GetName().GetText(), *reinterpret_cast<uint32_t*>(guid), *reinterpret_cast<uint16_t*>(guid + 4),
                  *reinterpret_cast<uint16_t*>(guid + 6), guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15],
                  static_cast<int>(static_cast<gEMoverState>(Entity(entity).Mover.MoverState)), mover->AccessLoopAni() ? 1 : 0, d,
                  p.GetX(), p.GetY(), p.GetZ());
        log(line);
    }
    log("MOVER end");
}
}
