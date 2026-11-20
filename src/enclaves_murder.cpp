// SPDX-License-Identifier: GPL-3.0-or-later
#include "enclaves_internal.h"
#include "ai.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>
#include <cstring>

namespace g3t::enclaves {
namespace detail {
bool notifyNative = false;
bool remoteMurders = true;
bool creditQueue = true;      // QA negative control (creditqueueoff): such a credit is lost, as before 24.10.
uint32_t creditHoldUntil = 0; // QA (credithold <ms>): the killer counts as out of the world until then
unsigned remoteCounted = 0, remoteCredited = 0, remoteNative = 0, remoteNotCounted = 0, remoteExpired = 0, remoteMoved = 0;
unsigned creditsHeld = 0, creditsLate = 0, creditsLost = 0;

namespace {
constexpr uintptr_t NotifyRva = 0x5ef70;   // NotifyEnclave in Script_Game.dll (CP 1.75.14), C death-callers-c.c
constexpr uint32_t RemoteKillWait = 60000; // ms a marked victim waits for its Kill (the death task runs a frame later)

struct RemoteKill {
    uint8_t player = 0;
    uint32_t generation = 0; // host: that slot's connection when it struck (net::PeerGeneration)
    std::string actor;       // who struck: the player's puppet or a companion (the game's Montera filter checks the killer)
    uint32_t at = 0;
};
std::map<std::string, RemoteKill> remoteKills; // by victim GUID
gFScript originalNotify = nullptr;
bool notifyInstalled = false;
uint32_t lastNotifyTry = 0;

// personal: a credit for a killer who is not in the host's world just then (loading) waits for that same connection to come
// back in (C p7-implementation-review-20261024-c.md 3); one that left (another connection in its slot) is dropped.
struct HeldCredit {
    uint8_t player = 0;
    uint32_t generation = 0, at = 0;
    std::string town, victim;
};
std::vector<HeldCredit> heldCredits;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

std::string KeyOf(eCEntity* entity)
{
    const eCEntityProxy proxy(entity);
    return Key(reinterpret_cast<uint8_t const*>(&proxy.GetEntityID().GetCore()));
}

unsigned char* NativeNotify()
{
    static const unsigned char expected[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x84, 0x08, 0x00, 0x00};
    HMODULE module = GetModuleHandleA("Script_Game.dll");
    if (!module) return nullptr;
    auto* code = reinterpret_cast<unsigned char*>(module) + NotifyRva;
    return std::memcmp(code, expected, sizeof(expected)) == 0 ? code : nullptr;
}

// personal: one murder into the killer's own counters, on its machine.
void Credit(RemoteKill const& kill, char const* town, char const* victim)
{
    net::EnclaveNote credit;
    credit.op = net::EnclaveCredit;
    credit.mode = mode;
    strncpy_s(credit.name, town, _TRUNCATE);
    credit.crime[gEPlayerCrime_Murder] = 1;
    // A client reaches the host only; the host only the connection that struck (a slot taken again is someone else, C
    // p7-implementation-review-20261024-c.md 4). Not kept for later: a killer loading or gone just then loses it (3, open).
    if ((!net::IsHost() && kill.player != 1) || (net::IsHost() && net::PeerGeneration(kill.player) != kill.generation)) {
        ++remoteNotCounted;
        Logf("enclaves: player %u murdered %s at %s; personal: not counted (that player is not reachable)", kill.player, victim, town);
        return;
    }
    if (net::IsHost() && (!net::InWorld(kill.player) || static_cast<int32_t>(GetTickCount() - creditHoldUntil) < 0)) {
        if (!creditQueue) {
            ++creditsLost;
            Logf("enclaves: player %u murdered %s at %s; personal: lost (that player is not in the world)", kill.player, victim, town);
            return;
        }
        HeldCredit held;
        held.player = kill.player;
        held.generation = kill.generation;
        held.at = GetTickCount();
        held.town = town;
        held.victim = victim;
        heldCredits.push_back(held);
        ++creditsHeld;
        Logf("enclaves: player %u murdered %s at %s; personal: held until that player is in the world", kill.player, victim, town);
        return;
    }
    net::SendEnclave(credit, net::IsHost() ? kill.player : 0);
    ++sent;
    ++remoteCredited;
    Logf("enclaves: player %u murdered %s at %s; personal: counted on that player's machine", kill.player, victim, town);
}

// NotifyEnclave for another player's kill: if the town turns to Flee here, the game booked the political crime on this
// machine's hero; personal: it is the killer's (as a murder is), so it moves to that player's hero.
GEInt NotifyFreeing(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args, RemoteKill const& kill)
{
    int32_t before[Alignments], after[Alignments];
    const bool watched = mode == Personal && politicalSync && ReadPolitical(before);
    const GEInt result = originalNotify(spu, self, other, args);
    if (!watched || !ReadPolitical(after)) return result;
    for (int i = 0; i < Alignments; ++i) {
        if (after[i] <= before[i]) continue;
        const int32_t rise = after[i] - before[i];
        AddPolitical(i, -rise);
        ++politicalMoved;
        if ((!net::IsHost() && kill.player != 1) || (net::IsHost() && net::PeerGeneration(kill.player) != kill.generation)) {
            Logf("enclaves: player %u freed a town (alignment %d); personal: not counted (that player is not reachable)", kill.player, i);
            continue;
        }
        net::SendEnclave(PoliticalNote(i, rise), net::IsHost() ? kill.player : 0);
        ++politicalSent;
        Logf("enclaves: player %u freed a town (alignment %d, +%d); personal: counted on that player's hero", kill.player, i, rise);
    }
    return result;
}

// Whether the game counts this murder itself: its killer (the victim's CurrentAttacker) is our hero or of its party.
bool CountsNatively(eCEntity* killer)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    return killer && hero && (killer == hero || (killer->GetPropertySet(eEPropertySetType_Party) &&
                                                 Entity(killer).Party.GetPartyLeader().GetInstance() == hero));
}

void CountRemote(eCEntity* victim, RemoteKill const& kill)
{
    Entity dead(victim);
    Entity town = dead.GetEnclave();
    gCEnclave_PS* enclave = Native(town);
    if (char const* why = NotCounted(victim, town, enclave, kill.actor.c_str())) {
        ++remoteNotCounted;
        telemetry::OnEnclaveEvent("remote_murder_exempt", town.GetInstance() ? town.GetName().GetText() : "-", kill.player, why, nullptr, nullptr, -1, -1, 0, 0);
        Logf("enclaves: player %u killed %s: no murder (%s)", kill.player, dead.GetName().GetText(), why);
        return;
    }
    if (!remoteMurders) { ++remoteNotCounted; return; }
    char const* name = town.GetName().GetText();
    if (mode == Personal) {
        Credit(kill, name, dead.GetName().GetText());
        return;
    }
    const Counters before = Read(enclave);
    Counters c = before; // the scan carries it on (host: to everyone; client: as this machine's delta)
    ++c.crime[gEPlayerCrime_Murder];
    Write(town, c);
    ++remoteCounted;
    telemetry::OnEnclaveEvent("remote_murder_counted", name, kill.player, ModeName(mode), before.crime, c.crime, before.known, c.known, before.fame, c.fame);
    Logf("enclaves: player %u murdered %s at %s (murders %d)", kill.player, dead.GetName().GetText(), name, c.crime[gEPlayerCrime_Murder]);
}

GEInt GE_STDCALL Notify(gCScriptProcessingUnit* spu, GELPVoid self, GELPVoid other, GEInt args)
{
    if (ai::ShuttingDown()) return originalNotify ? originalNotify(spu, self, other, args) : 0;
    if (args != 0 || remoteKills.empty()) return originalNotify(spu, self, other, args);
    eCEntity* killer = self ? static_cast<Entity*>(self)->GetInstance() : (spu ? spu->GetSelfEntity() : nullptr);
    eCEntity* victim = other ? static_cast<Entity*>(other)->GetInstance() : (spu ? spu->GetOtherEntity() : nullptr);
    if (!victim) return originalNotify(spu, self, other, args);
    const auto it = remoteKills.find(KeyOf(victim));
    if (it == remoteKills.end()) return originalNotify(spu, self, other, args);
    const RemoteKill kill = it->second;
    remoteKills.erase(it); // once per death
    if (!CountsNatively(killer)) {
        CountRemote(victim, kill); // first, as the game counts before its other steps
        return NotifyFreeing(spu, self, other, args, kill);
    }
    ++remoteNative;
    if (mode != Personal || !remoteMurders) return NotifyFreeing(spu, self, other, args, kill);
    // personal: the game books it on this machine's player (its killer is an old CurrentAttacker, this hero's knock-out),
    // but the deadly blow was the other player's: that one murder goes to the other's record (C review 2).
    Entity town = Entity(victim).GetEnclave();
    gCEnclave_PS* enclave = Native(town);
    const int before = enclave ? Read(enclave).crime[gEPlayerCrime_Murder] : 0;
    const GEInt result = NotifyFreeing(spu, self, other, args, kill);
    if (enclave && Read(enclave).crime[gEPlayerCrime_Murder] > before) {
        Counters c = Read(enclave);
        --c.crime[gEPlayerCrime_Murder];
        Write(town, c);
        ++remoteMoved;
        Credit(kill, town.GetName().GetText(), Entity(victim).GetName().GetText());
    }
    return result;
}
} // namespace

// The game's conditions of NotifyEnclave mode 0 for the victim and its town (C 12:28, section 2); null: it counts.
char const* NotCounted(eCEntity* victim, Entity& town, gCEnclave_PS* enclave, char const* actor)
{
    if (!enclave) return "no town";
    char const* name = victim->GetName().GetText();
    if (!std::strcmp(town.GetName().GetText(), "Faring") &&
        (!std::strcmp(name, "Orc_Scout_05") || !std::strcmp(name, "Orc_Scout_06") || !std::strcmp(name, "Orc_Warrior_12")))
        return "Faring exception";
    if (static_cast<gEEnclaveStatus>(enclave->AccessStatus()) == gEEnclaveStatus_Flee) return "town flees";
    if (GetScriptAdminExt().CallScript(bCString("GetPoliticalAttitude"), victim, town.GetInstance(), 0) != 1) return "not political";
    for (char const* orc : {"Montera_Orc_02", "Montera_Orc_03", "Montera_Orc_04", "Montera_Orc_05"})
        if (!std::strcmp(actor, orc)) return "excepted killer";
    return nullptr;
}

void InstallNotify()
{
    if (notifyInstalled) return;
    const uint32_t now = GetTickCount();
    if (now - lastNotifyTry < 1000) return;
    lastNotifyTry = now;
    gSScript* script = nullptr;
    if (!GetScriptAdminExt().GetScripts().GetAt(bCString("NotifyEnclave"), script) || !script || !script->m_funcScript) return;
    // Kill calls it directly, not by name: only the jump in the native function sees those calls.
    if (unsigned char* code = NativeNotify(); code && reinterpret_cast<void*>(script->m_funcScript) == code) {
        originalNotify = reinterpret_cast<gFScript>(ai::JumpTo(code, reinterpret_cast<void*>(&Notify)));
        notifyNative = originalNotify != nullptr;
    }
    notifyInstalled = true;
    Logf("enclaves: NotifyEnclave %s",
         notifyNative ? "wrapped (native jump)" : "not wrapped (another build): murders by other players count nowhere");
}

// Host: held credits for players back in the world (same connection) go out; for players gone they are dropped.
void SendHeldCredits()
{
    if (heldCredits.empty()) return;
    const uint32_t now = GetTickCount();
    for (auto it = heldCredits.begin(); it != heldCredits.end();) {
        const bool same = net::PeerJoined(it->player) && net::PeerGeneration(it->player) == it->generation;
        if (!same || now - it->at > 600000) {
            ++creditsLost;
            Logf("enclaves: a held murder of %s at %s for player %u dropped (%s)", it->victim.c_str(), it->town.c_str(), it->player,
                 same ? "ten minutes passed" : "that player left");
            it = heldCredits.erase(it);
            continue;
        }
        if (!net::InWorld(it->player) || static_cast<int32_t>(now - creditHoldUntil) < 0) { ++it; continue; }
        net::EnclaveNote credit;
        credit.op = net::EnclaveCredit;
        credit.mode = mode;
        strncpy_s(credit.name, it->town.c_str(), _TRUNCATE);
        credit.crime[gEPlayerCrime_Murder] = 1;
        net::SendEnclave(credit, it->player);
        ++sent;
        ++remoteCredited;
        ++creditsLate;
        Logf("enclaves: the held murder of %s at %s sent to player %u", it->victim.c_str(), it->town.c_str(), it->player);
        it = heldCredits.erase(it);
    }
}

void ClearRemoteKills() { remoteKills.clear(); }
unsigned RemoteKillsSize() { return static_cast<unsigned>(remoteKills.size()); }
} // namespace detail

void NoteRemoteKill(uint8_t const victim[16], uint8_t player, eCEntity* actor)
{
    using namespace detail;
    if (!notifyNative) return;
    const uint32_t now = GetTickCount();
    for (auto it = remoteKills.begin(); it != remoteKills.end();) {
        if (now - it->second.at > RemoteKillWait) { ++remoteExpired; it = remoteKills.erase(it); }
        else ++it;
    }
    RemoteKill& kill = remoteKills[Key(victim)];
    kill.player = player;
    kill.generation = net::IsHost() ? net::PeerGeneration(player) : 0;
    kill.actor = actor ? actor->GetName().GetText() : "";
    kill.at = now;
}
} // namespace g3t::enclaves
