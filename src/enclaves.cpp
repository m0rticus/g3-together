// SPDX-License-Identifier: GPL-3.0-or-later
#include "enclaves.h"
#include "enclaves_internal.h"
#include "infocmd.h"

#include "config.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "reputation_policy.h"
#include "story.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace g3t::enclaves {
namespace detail {
std::map<std::string, Counters> seen;  // what this machine last saw or was told, per enclave
std::vector<Pending> pending;          // client: thefts waiting for the take's outcome
Mode mode = Shared;                    // host: from the INI; client: from the host's notes
bool sync = true;                      // QA negative control
bool statusSync = true;                // QA negative control: townstatusoff
bool keepLocal = true;                 // QA negative control: KeepLocalOff (the host's counters overwrite unreported deeds)
// Fame comes from quest rewards, which run on every machine: the host has counted it already, a client's delta counted it
// twice (Reddock at 100 early, playtest 27.10.). true: the host adds a client's fame again, as before.
bool clientFame = false;
bool politicalSync = true;             // QA negative control: politicaloff
uint32_t scanPausedUntil = 0;          // QA: no scan before this (GetTickCount), to widen the 500 ms window between deed and scan
unsigned sent = 0, received = 0, applied = 0, deltas = 0, dropped = 0, keptLocal = 0, credits = 0, statusSent = 0;
unsigned politicalSent = 0, politicalApplied = 0, politicalMoved = 0;

char const* ModeName(Mode m) { return m == Together ? "together" : m == Personal ? "personal" : "shared"; }

gCEnclave_PS* Native(Entity& e)
{
    eCEntity* instance = e.GetInstance();
    return instance ? static_cast<gCEnclave_PS*>(instance->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
}

// The game's own members for reading (the script wrappers look properties up by name, see infocmd.cpp).
Counters Read(gCEnclave_PS* enclave)
{
    Counters c;
    bTValArray<GEI32> const& crime = enclave->AccessCrimeCount();
    bTValArray<GEI32> const& suspect = enclave->AccessSuspectComment();
    for (int i = 0; i < Crimes; ++i) {
        c.crime[i] = i < crime.GetCount() ? crime.GetAt(i) : 0;
        c.suspect[i] = i < suspect.GetCount() ? suspect.GetAt(i) : 0;
    }
    c.known = static_cast<int32_t>(static_cast<gEPlayerCrime const&>(enclave->AccessKnownPlayerCrime()));
    c.fame = enclave->AccessPlayerFame();
    return c;
}

// Writing through the game's setters (SetCrimeCount/SetSuspectComment, as AddEnclaveTheftCount does).
void Write(Entity& e, Counters const& c)
{
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) return;
    for (int i = 0; i < Crimes; ++i) {
        e.Enclave.SetCrimeCount(static_cast<gEPlayerCrime>(i), c.crime[i]);
        e.Enclave.SetSuspectComment(static_cast<gEPlayerCrime>(i), c.suspect[i]);
    }
    enclave->AccessKnownPlayerCrime() = static_cast<gEPlayerCrime>(c.known);
    enclave->AccessPlayerFame() = c.fame;
}
} // namespace detail
using namespace detail;
namespace {
constexpr uint32_t ScanEvery = 500;    // ms
constexpr uint32_t TheftWait = 3000;   // ms a client holds a theft back until its take is settled

bool Same(Counters const& a, Counters const& b) { return !std::memcmp(&a, &b, sizeof(a)); }
bool SameCrimes(Counters const& a, Counters const& b)
{
    return !std::memcmp(a.crime, b.crime, sizeof(a.crime)) &&
           !std::memcmp(a.suspect, b.suspect, sizeof(a.suspect)) && a.known == b.known;
}

reputation::RequestTracker sessionRequest{};
uint32_t lastScan = 0;
uint32_t lastTakeTask = 0;  // when this hero was last in PS_Normal_Take (where the game counts a theft)
std::map<std::string, int> statusSeen;

int32_t politicalSeen[Alignments] = {};
eCEntity* politicalHero = nullptr;  // whose counts politicalSeen holds (a loaded or new hero starts over)
} // namespace

namespace detail {
bool ReadPolitical(int32_t out[Alignments])
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !hero->GetPropertySet(eEPropertySetType_PlayerMemory)) return false;
    Entity self(hero);
    for (int i = 0; i < Alignments; ++i) out[i] = self.PlayerMemory.GetPoliticalCrimeCount(static_cast<gEPoliticalAlignment>(i));
    return true;
}

// This hero's political crime count of one alignment changed by `by` (a note, or a move to the killer); the baseline follows
// so that the watch does not send it on.
void AddPolitical(int alignment, int32_t by)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || !hero->GetPropertySet(eEPropertySetType_PlayerMemory) || alignment < 0 || alignment >= Alignments) return;
    Entity self(hero);
    const auto a = static_cast<gEPoliticalAlignment>(alignment);
    const int32_t value = self.PlayerMemory.GetPoliticalCrimeCount(a) + by;
    self.PlayerMemory.SetPoliticalCrimeCount(a, value < 0 ? 0 : value);
    if (politicalHero == hero) politicalSeen[alignment] = self.PlayerMemory.GetPoliticalCrimeCount(a);
}

net::EnclaveNote PoliticalNote(int alignment, int32_t count)
{
    net::EnclaveNote n;
    n.op = net::EnclavePolitical;
    n.mode = mode;
    strncpy_s(n.name, "political", _TRUNCATE);
    n.known = alignment;
    n.fame = count;
    return n;
}
} // namespace detail
namespace {

// To every other player: a client's goes to the host, the host's to every client but the one it came from.
void SendPolitical(net::EnclaveNote const& note, uint8_t except)
{
    if (!net::IsHost()) {
        net::SendEnclave(note, 0);
        ++politicalSent;
        return;
    }
    for (int id = 2; id <= net::MaxPlayers; ++id) {
        if (id == except || !net::InWorld(static_cast<uint8_t>(id))) continue;
        net::SendEnclave(note, static_cast<uint8_t>(id));
        ++politicalSent;
    }
}

// The watch (every scan): a rise of this hero's counts is this machine's liberation.
void WatchPolitical()
{
    int32_t now[Alignments];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!ReadPolitical(now)) return;
    if (hero != politicalHero) {
        politicalHero = hero;
        std::memcpy(politicalSeen, now, sizeof(now));
        return;
    }
    for (int i = 0; i < Alignments; ++i) {
        if (now[i] <= politicalSeen[i]) { politicalSeen[i] = now[i]; continue; }
        const int32_t rise = now[i] - politicalSeen[i];
        politicalSeen[i] = now[i];
        if (!politicalSync || mode == Personal) continue;
        SendPolitical(PoliticalNote(i, rise), 0);
        Logf("enclaves: this player committed a political crime against alignment %d (+%d, now %d); sent %s", i, rise, now[i],
             net::IsHost() ? "to everyone" : "to the host");
    }
}

Mode FromConfig()
{
    char const* r = GetConfig().reputation;
    if (!_stricmp(r, "together")) return Mode::Together;
    if (!_stricmp(r, "personal")) return Personal;
    return Shared;
}

net::EnclaveNote Note(uint8_t op, std::string const& name, Counters const& c)
{
    net::EnclaveNote n;
    n.op = op;
    n.mode = mode;
    strncpy_s(n.name, name.c_str(), _TRUNCATE);
    std::memcpy(n.crime, c.crime, sizeof(n.crime));
    std::memcpy(n.suspect, c.suspect, sizeof(n.suspect));
    n.known = c.known;
    n.fame = c.fame;
    return n;
}

// Every enclave of the world by name.
template <typename Fn> void ForEach(Fn fn)
{
    const GEInt count = PSEnclave::GetEnclaveCount();
    for (GEInt i = 0; i < count; ++i) {
        Entity e = PSEnclave::GetEnclaveAt(i);
        if (gCEnclave_PS* enclave = Native(e)) fn(std::string(e.GetName().GetText()), e, enclave);
    }
}

void SendState(std::string const& name, Counters const& c, uint8_t to)
{
    net::SendEnclave(Note(net::EnclaveState, name, c), to);
    ++sent;
}

// Client: what this player's deeds changed since the last look. Counts and fame as additions; changed acknowledgements
// and known crime as the new values (-1 when unchanged so the host preserves higher unrelated values while honouring a reset to None).
void SendDelta(std::string const& name, Counters const& before, Counters const& now)
{
    Counters d = now;
    for (int i = 0; i < Crimes; ++i) {
        d.crime[i] = now.crime[i] - before.crime[i];
        d.suspect[i] = now.suspect[i] != before.suspect[i] ? now.suspect[i] : -1;
    }
    d.known = now.known != before.known ? now.known : -1;
    d.fame = now.fame - before.fame;
    Pending p{name, Note(net::EnclaveDelta, name, d), GetTickCount(), items::Returned(), items::TakesBegun(), items::TakesDone()};
    if (d.crime[gEPlayerCrime_Theft] > 0) {  // wait: the take this theft was counted for may go to someone else
        pending.push_back(p);
        return;
    }
    net::SendEnclave(p.delta, 0);
    ++sent;
}

void SettlePending()
{
    const uint32_t now = GetTickCount();
    for (size_t i = 0; i < pending.size();) {
        Pending& p = pending[i];
        // The game counts a theft before the hero takes the item (PS_Normal_Take). It was for nothing when the host gave the
        // item to someone else, or when this hero reached for an item and got none (the other player's take removed it
        // first, theftdiag both 20.10.). A theft without a take (from an NPC) goes on as counted.
        const bool reached = items::TakesBegun() > p.begun || items::TakesDone() > p.done ||
                             (lastTakeTask && static_cast<int32_t>(lastTakeTask - p.at) > -2000);
        const bool lost = items::Returned() > p.returned || (now - p.at > TheftWait && reached && items::TakesDone() == p.done);
        if (lost) {
            // Take it back here; the rest of the change (if any) still goes to the host.
            Entity e = PSEnclave::GetEnclave(bCString(p.name.c_str()));
            if (gCEnclave_PS* enclave = Native(e)) {
                Counters c = Read(enclave);
                c.crime[gEPlayerCrime_Theft] -= p.delta.crime[gEPlayerCrime_Theft];
                Write(e, c);
                seen[p.name] = c;
            }
            p.delta.crime[gEPlayerCrime_Theft] = 0;
            ++dropped;
            Logf("enclaves: theft at %s taken back (someone else got the item)", p.name.c_str());
            net::SendEnclave(p.delta, 0);
            ++sent;
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (now - p.at > TheftWait) {
            net::SendEnclave(p.delta, 0);
            ++sent;
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void OnNote(uint8_t from, net::EnclaveNote const& note)
{
    if (!sync) return;
    ++received;
    if (note.op == net::EnclavePolitical) {  // another player freed a town (shared, together), or this one did (personal: moved)
        if (!politicalSync) return;
        AddPolitical(note.known, note.fame);
        ++politicalApplied;
        Logf("enclaves: player %u: political crime against alignment %d (+%d) for this hero too (%s)", from, note.known, note.fame,
             ModeName(static_cast<Mode>(note.mode <= Personal ? note.mode : Shared)));
        if (net::IsHost() && note.mode != Personal) SendPolitical(note, from);  // and on to the other clients
        return;
    }
    if (note.op == net::EnclaveCredit) {  // personal: a murder by this machine's player, counted where the victim was simulated
        Entity e = PSEnclave::GetEnclave(bCString(note.name));
        gCEnclave_PS* enclave = Native(e);
        if (!enclave || mode != Personal) return;
        Counters c = Read(enclave);
        c.crime[gEPlayerCrime_Murder] += note.crime[gEPlayerCrime_Murder];
        Write(e, c);
        ++credits;
        Logf("enclaves: player %u counted a murder of this player at %s (murders %d)", from, note.name, c.crime[gEPlayerCrime_Murder]);
        return;
    }
    if (note.op == net::EnclaveRepDelta) {
        if (!net::IsHost() || note.fame == 0 || note.fame < -100 || note.fame > 100) return;
        story::FlushReputation();
        if (note.name[0] == '\0') {
            if (note.known < 0 || static_cast<size_t>(note.known) >= reputation::PoliticalAlignmentCount) return;
            eCEntity* hero = gCSession::GetInstance().GetHero();
            auto* memory = hero ? static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory)) : nullptr;
            if (!memory) return;
            auto& pol = memory->AccessPoliticalFame();
            const GEInt idx = static_cast<GEInt>(note.known);
            const int32_t cur = idx < pol.GetCount() ? pol.GetAt(idx) : 0;
            const int32_t next = reputation::ApplyClampedDelta(cur, note.fame);
            const GEInt oldCount = pol.GetCount();
            const GEInt needed = static_cast<GEInt>(reputation::RequiredPoliticalArrayCount(
                oldCount > 0 ? static_cast<size_t>(oldCount) : 0u, static_cast<size_t>(idx), next));
            if (oldCount < needed) {
                pol.SetCount(needed);
                for (GEInt i = oldCount < 0 ? 0 : oldCount; i < needed; ++i) pol.AccessAt(i) = 0;
            }
            if (idx < pol.GetCount()) pol.AccessAt(idx) = next;
            Logf("enclaves: player %u modified camp %d reputation %d -> %d", from, note.known, cur, next);
            story::FlushReputation();
            return;
        }
        Entity e = PSEnclave::GetEnclave(bCString(note.name));
        gCEnclave_PS* enclave = Native(e);
        if (!enclave) return;
        const int32_t cur = enclave->AccessPlayerFame();
        const int32_t next = reputation::ApplyClampedDelta(cur, note.fame);
        enclave->AccessPlayerFame() = next;
        const std::string name(note.name);
        seen[name] = Read(enclave);
        SendState(name, seen[name], 0);
        story::FlushReputation();
        Logf("enclaves: player %u modified %s reputation %d -> %d", from, note.name, cur, next);
        return;
    }
    if (net::IsHost()) {
        if (note.op == net::EnclaveRequest) {
            ForEach([&](std::string const& name, Entity& e, gCEnclave_PS* enclave) {
                SendState(name, Read(enclave), from);
                if (statusSync) infocmd::Changed(e.GetInstance());  // its status too (to everyone; the others have it already)
            });
            return;
        }
        if (note.op != net::EnclaveDelta || mode == Personal) return;
        Entity e = PSEnclave::GetEnclave(bCString(note.name));
        gCEnclave_PS* enclave = Native(e);
        if (!enclave) return;
        Counters c = Read(enclave);
        for (int i = 0; i < Crimes; ++i) {
            c.crime[i] += note.crime[i];
            if (c.crime[i] < 0) c.crime[i] = 0;
            if (note.suspect[i] == 0) c.suspect[i] = 0;
            else if (note.suspect[i] > c.suspect[i]) c.suspect[i] = note.suspect[i];
        }
        if (note.known == gEPlayerCrime_None) c.known = gEPlayerCrime_None;
        else if (note.known > c.known && note.known < Crimes) c.known = note.known;
        if (clientFame) c.fame += note.fame;
        Write(e, c);
        const std::string name(note.name);
        seen[name] = Read(enclave);
        ++deltas;
        Logf("enclaves: player %u changed %s (theft +%d, murder +%d)", from, note.name, note.crime[gEPlayerCrime_Theft],
             note.crime[gEPlayerCrime_Murder]);
        SendState(name, seen[name], 0);
        if (clientFame) story::FlushReputation();
        return;
    }
    if (note.op != net::EnclaveState) return;
    mode = static_cast<Mode>(note.mode <= Personal ? note.mode : Shared);
    Entity e = PSEnclave::GetEnclave(bCString(note.name));
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) return;
    if (mode == Personal) {
        enclave->AccessPlayerFame() = note.fame;
        seen[note.name] = Read(enclave);
        ++applied;
        return;
    }
    // This player's deeds since the last scan (every 500 ms) are not in the host's counters yet: report them first, or
    // the host's values would overwrite them and the shared counter lose them (release-review-c.md P1, 21.10.).
    const auto last = seen.find(note.name);
    if (keepLocal && last != seen.end()) {
        const Counters now = Read(enclave);
        if (clientFame ? !Same(now, last->second) : !SameCrimes(now, last->second)) {
            const Counters before = last->second;
            last->second = now;
            SendDelta(note.name, before, now);
            ++keptLocal;
        }
    }
    Counters c;
    std::memcpy(c.crime, note.crime, sizeof(c.crime));
    std::memcpy(c.suspect, note.suspect, sizeof(c.suspect));
    c.known = note.known;
    c.fame = note.fame;
    // Thefts held back until their take is settled are not in the host's counters either: keep them on top here
    // (SettlePending takes one back from this sum if its take was lost).
    if (keepLocal)
        for (Pending const& p : pending)
            if (p.name == note.name) c.crime[gEPlayerCrime_Theft] += p.delta.crime[gEPlayerCrime_Theft];
    Write(e, c);
    seen[note.name] = Read(enclave);
    ++applied;
}
} // namespace

void Start()
{
    mode = FromConfig();
    net::SetEnclaveHandler(&OnNote);
}

void Tick()
{
    if (gCSession::GetInstance().IsGameRunning()) InstallNotify();
    if (net::IsHost()) SendHeldCredits();
    if (!sync) { sessionRequest.active = false; return; }
    const bool enteredSession = reputation::UpdateSessionRequest(
        sessionRequest, net::Connected(), net::InWorld(net::LocalId()),
        gCSession::GetInstance().IsGameRunning(), net::WelcomeSerial(), net::WorldEpoch());
    if (!sessionRequest.active) return;
    if (enteredSession) {
        if (net::IsHost()) {
            mode = FromConfig();
        } else {  // admitted (or re-admitted after reconnect / host load): request the host's enclave state
            net::EnclaveNote ask;
            ask.op = net::EnclaveRequest;
            net::SendEnclave(ask, 0);
            ++sent;
        }
    }
    if (!net::IsHost()) {
        if (eCDynamicEntity* hero = gCSession::GetInstance().GetHero()) {
            if (Entity(hero).Routine.GetCurrentTask() == bCString("PS_Normal_Take")) lastTakeTask = GetTickCount();
        }
        SettlePending();
    }
    const uint32_t now = GetTickCount();
    if (now - lastScan < ScanEvery || static_cast<int32_t>(now - scanPausedUntil) < 0) return;
    lastScan = now;
    bool hostFameChanged = false;
    ForEach([&](std::string const& name, Entity& e, gCEnclave_PS* enclave) {
        if (statusSync) {
            const int status = static_cast<int>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus()));
            const auto known = statusSeen.find(name);
            if (known == statusSeen.end()) {
                statusSeen[name] = status;
            } else if (known->second != status) {
                Logf("enclaves: %s turned from status %d to %d; sent %s", name.c_str(), known->second, status,
                     net::IsHost() ? "to everyone" : "to the host");
                known->second = status;
                infocmd::Changed(e.GetInstance());
                ++statusSent;
            }
        }
        const Counters c = Read(enclave);
        auto it = seen.find(name);
        if (it == seen.end()) { seen[name] = c; return; }
        if (Same(it->second, c)) return;
        const Counters before = it->second;
        it->second = c;
        if (net::IsHost()) {
            SendState(name, c, 0);
            if (before.fame != c.fame) hostFameChanged = true;
        } else if (mode != Personal && (clientFame || !SameCrimes(before, c))) {
            SendDelta(name, before, c);
        }
    });
    if (hostFameChanged) story::FlushReputation();
    WatchPolitical();
}

void OnWorldChange()
{
    politicalHero = nullptr;
    seen.clear();
    statusSeen.clear();
    pending.clear();
    ClearRemoteKills();
    sessionRequest = {};
}

void NoteAuthoritativeFame(char const* name, int32_t fame)
{
    if (!name || !*name) return;
    auto it = seen.find(name);
    if (it != seen.end()) it->second.fame = fame;
}

void SendDialogReputationDelta(bool isPolitical, int alignment, char const* enclave, int32_t delta)
{
    if (!sync || net::IsHost() || !net::Connected() || delta == 0) return;
    net::EnclaveNote note{};
    note.op = net::EnclaveRepDelta;
    note.mode = mode;
    note.known = isPolitical ? alignment : -1;
    if (!isPolitical && enclave) strncpy_s(note.name, enclave, _TRUNCATE);
    note.fame = delta;
    net::SendEnclave(note, 0);
    ++sent;
}

void OnHostEnclaveLiberated(gCEnclave_PS* enclave, char const* name)
{
    if (!net::IsHost() || !enclave || !name || !*name) return;
    story::FlushReputation();
    Entity e = PSEnclave::GetEnclave(bCString(name));
    Counters c = Read(enclave);
    c.fame = reputation::LiberationFame;
    if (mode != Personal) {
        for (int i = 0; i < Crimes; ++i) {
            c.crime[i] = 0;
            c.suspect[i] = 0;
        }
        c.known = static_cast<int32_t>(gEPlayerCrime_None);
    }
    if (Native(e)) Write(e, c);
    else enclave->AccessPlayerFame() = reputation::LiberationFame;
    const std::string key(name);
    seen[key] = Read(enclave);
    SendState(key, seen[key], 0);
    story::FlushReputation();
    Logf("enclaves: %s liberated -> reputation %d", name, reputation::LiberationFame);
}

bool Together() { return mode == Mode::Together; }
} // namespace g3t::enclaves
