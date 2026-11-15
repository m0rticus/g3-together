// SPDX-License-Identifier: GPL-3.0-or-later
#include "enclaves_internal.h"
#include "config.h"
#include "log.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3t::enclaves {
using namespace detail;

void Report(LogFn log)
{
    char line[640];
    Format(line,
           "ENCLAVESYNC sync=%d mode=%s config=%s sent=%u received=%u applied=%u deltas=%u dropped=%u pending=%u seen=%u kept_local=%u "
           "notify=%d remote_kills=%u remote_counted=%u remote_credited=%u remote_native=%u remote_not_counted=%u remote_expired=%u credits=%u "
           "remote_moved=%u status_sync=%d status_sent=%u credits_held=%u credits_late=%u credits_lost=%u political_sync=%d political_sent=%u "
           "political_applied=%u political_moved=%u",
           sync ? 1 : 0, ModeName(mode), GetConfig().reputation, sent, received, applied, deltas, dropped,
           static_cast<unsigned>(pending.size()), static_cast<unsigned>(seen.size()), keptLocal, notifyNative ? 1 : 0,
           RemoteKillsSize(), remoteCounted, remoteCredited, remoteNative, remoteNotCounted, remoteExpired, credits,
           remoteMoved, statusSync ? 1 : 0, statusSent, creditsHeld, creditsLate, creditsLost, politicalSync ? 1 : 0, politicalSent,
           politicalApplied, politicalMoved);
    log(line);
}

void PoliticalOff(LogFn log)
{
    politicalSync = false;
    log("POLITICALOFF ok=1");
}

void TownStatusOff(LogFn log)
{
    statusSync = false;
    log("TOWNSTATUSOFF ok=1");
}

void RemoteMurdersOff(LogFn log)
{
    remoteMurders = false;
    log("REMOTEMURDERSOFF ok=1");
}

void CreditQueueOff(LogFn log)
{
    creditQueue = false;
    log("CREDITQUEUEOFF ok=1");
}

void CreditHold(char const* ms, LogFn log)
{
    creditHoldUntil = GetTickCount() + static_cast<uint32_t>(std::atoi(ms));
    log("CREDITHOLD ok=1");
}

void SyncOff(LogFn log)
{
    sync = false;
    log("ENCLAVESYNCOFF ok=1");
}

void KeepLocalOff(LogFn log)
{
    keepLocal = false;
    log("KEEPLOCALOFF ok=1");
}

void PauseScan(char const* ms, LogFn log)
{
    scanPausedUntil = GetTickCount() + static_cast<uint32_t>(std::atoi(ms));
    log("ENCLAVESCANPAUSE ok=1");
}

void MurderCandidates(char const* name, LogFn log)
{
    char line[400];
    Entity e = PSEnclave::GetEnclave(bCString(name));
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) { log("REFUSED murdercand: no such enclave"); return; }
    const int status = static_cast<int>(static_cast<gEEnclaveStatus const&>(enclave->AccessStatus()));
    const Counters c = Read(enclave);
    bTObjArray<eCEntityProxy> members = enclave->GetMembers();
    int shown = 0;
    for (GEInt m = 0; m < members.GetCount() && shown < 40; ++m) {
        eCEntity* member = members.AccessAt(m).GetEntity();
        if (!member || !member->GetPropertySet(eEPropertySetType_NPC)) continue;
        Entity npc(member);
        char const* who = member->GetName().GetText();
        const GEInt political = GetScriptAdminExt().CallScript(bCString("GetPoliticalAttitude"), member, e.GetInstance(), 0);
        char const* why = NotCounted(member, e, enclave, "");
        const eCEntityProxy proxy(member);
        bCPropertyID::bSCore g; // for a claim on a member far from every player (stageLiberate)
        std::memcpy(&g, &proxy.GetEntityID().GetCore(), 16);
        Format(line, "MURDERCAND enclave=%s status=%d murders=%d known=%d member=%s alive=%d political=%d excepted=%d counts=%d "
               "guid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               name, status, c.crime[gEPlayerCrime_Murder], c.known, who, npc.IsDead() ? 0 : 1, political,
               why && !std::strcmp(why, "Faring exception") ? 1 : 0, why ? 0 : 1, g.Data1, g.Data2, g.Data3, g.Data4[0],
               g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        log(line);
        ++shown;
    }
    Format(line, "MURDERCAND end enclave=%s status=%d murders=%d members=%d", name, status, c.crime[gEPlayerCrime_Murder],
           members.GetCount());
    log(line);
}

void TownFarKill(char const* name, LogFn log)
{
    char line[200];
    Entity e = PSEnclave::GetEnclave(bCString(name));
    gCEnclave_PS* enclave = Native(e);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!enclave || !hero) { log("REFUSED townfarkill: no such enclave or no hero"); return; }
    const bCVector& h = hero->GetWorldPosition();
    bTObjArray<eCEntityProxy> members = enclave->GetMembers();
    int killed = 0, kept = 0, alive = 0;
    for (GEInt m = 0; m < members.GetCount(); ++m) {
        eCEntity* member = members.AccessAt(m).GetEntity();
        if (!member || !member->GetPropertySet(eEPropertySetType_NPC) || !member->GetPropertySet(eEPropertySetType_DamageReceiver)) continue;
        Entity npc(member);
        if (npc.IsDead()) continue;
        ++alive;
        const bCVector& p = member->GetWorldPosition();
        const float dx = p.GetX() - h.GetX(), dy = p.GetY() - h.GetY(), dz = p.GetZ() - h.GetZ();
        if (dx * dx + dy * dy + dz * dz < 4000.0f * 4000.0f) { ++kept; continue; }
        auto& admin = GetScriptAdminExt();
        admin.CallScript(bCString("AddHitPoints"), member, hero, -static_cast<GEInt>(npc.DamageReceiver.HitPoints));
        admin.CallScript(bCString("OnCheatKill"), hero, member, 0);
        ++killed;
    }
    Format(line, "TOWNFARKILL enclave=%s members=%d alive=%d killed=%d near=%d", name, members.GetCount(), alive, killed, kept);
    log(line);
}

void CheatKill(char const* name, LogFn log)
{
    char line[300];
    Entity npc = Entity::GetEntity(bCString(name));
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!npc.GetInstance() || !hero) { log("REFUSED cheatkill: no such NPC or no hero"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("OnCheatKill"), hero, npc.GetInstance(), 0); // self kills other
    Format(line, "CHEATKILL name=%s result=%d dead=%d task=%s", name, result, npc.IsDead() ? 1 : 0,
           npc.Routine.GetCurrentTask().GetText());
    log(line);
}

// QA stand-in for a deed: "<enclave>-<crime>-<n>" (the QA channel passes only [A-Za-z0-9_-]) adds n to that crime here.
void AddCrime(char const* argument, LogFn log)
{
    char name[64] = {};
    int index = -1, n = 0;
    if (sscanf_s(argument, "%63[^-]-%d-%d", name, static_cast<unsigned>(sizeof(name)), &index, &n) != 3 || index < 0 || index >= Crimes) {
        log("REFUSED crimeadd: <enclave>-<crime 0..4>-<n>");
        return;
    }
    Entity e = PSEnclave::GetEnclave(bCString(name));
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) { log("REFUSED crimeadd: no such enclave"); return; }
    Counters c = Read(enclave);
    c.crime[index] += n;
    Write(e, c);
    char line[120];
    Format(line, "CRIMEADD enclave=%s crime=%d now=%d", name, index, c.crime[index]);
    log(line);
}

// QA (guard news, 26.10.): how many deeds of a kind the town's guards have commented on (SuspectComment; crimeadd left it
// at the new count, crimenewsdiag 26.10.). A guard has news while the count is above it (HasGuardNews, re/out/news2-b.c).
void SetSuspect(char const* argument, LogFn log)
{
    char name[64] = {};
    int index = -1, n = 0;
    if (sscanf_s(argument, "%63[^-]-%d-%d", name, static_cast<unsigned>(sizeof(name)), &index, &n) != 3 || index < 0 || index >= Crimes) {
        log("REFUSED crimesuspect: <enclave>-<crime 0..4>-<n>");
        return;
    }
    Entity e = PSEnclave::GetEnclave(bCString(name));
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) { log("REFUSED crimesuspect: no such enclave"); return; }
    Counters c = Read(enclave);
    c.suspect[index] = n;
    Write(e, c);
    const Counters now = Read(enclave);
    char line[120];
    Format(line, "CRIMESUSPECT enclave=%s crime=%d suspect=%d count=%d", name, index, now.suspect[index], now.crime[index]);
    log(line);
}

// QA (guard news, 26.10.): what a town knows of the player ("<enclave>-<gEPlayerCrime>", 3 theft, 4 murder), as
// MemorizePlayerCrime leaves it; shared like any change of the counters.
void SetKnown(char const* argument, LogFn log)
{
    char name[64] = {};
    int value = -1;
    if (sscanf_s(argument, "%63[^-]-%d", name, static_cast<unsigned>(sizeof(name)), &value) != 2 || value < 0 || value >= Crimes) {
        log("REFUSED crimeknown: <enclave>-<crime 0..4>");
        return;
    }
    Entity e = PSEnclave::GetEnclave(bCString(name));
    gCEnclave_PS* enclave = Native(e);
    if (!enclave) { log("REFUSED crimeknown: no such enclave"); return; }
    Counters c = Read(enclave);
    c.known = value;
    Write(e, c);
    char line[120];
    Format(line, "CRIMEKNOWN enclave=%s known=%d", name, value);
    log(line);
}

// QA negative controls of the play-evening fixes (27.10.): each switches its fix off (on: clientfameon).
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "clientfameon")) { clientFame = true; log("SWITCH clientfameon ok=1"); return true; }
    return false;
}
} // namespace g3t::enclaves
