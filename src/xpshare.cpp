// SPDX-License-Identifier: GPL-3.0-or-later
#include "xpshare.h"

#include "log.h"
#include "net.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace g3t::xpshare {
namespace {
bool enabled = true;  // QA negative control: dialogxpoff
bool applying = false;  // our own GiveXP for another player's experience: not this hero's gain
uint32_t nonce = 0;     // this process, so that a restarted player starts over
uint32_t earned = 0;    // what this machine's hero earned itself since the game started
uint32_t sentEarned = 0xffffffff;
DWORD lastSend = 0;
int waiting = 0;        // experience to add as soon as the hero can take it
bool lift = false;      // the hero is another one (a load): take the others' total
constexpr DWORD Heartbeat = 2000;
constexpr int MaxGain = 10000000;  // a sanity bound for one step

struct Peer {
    bool known = false;
    uint32_t nonce = 0;
    uint32_t applied = 0;  // of its earned total, what this machine counted
    uint32_t total = 0;    // its hero's experience in its last message
    DWORD at = 0;
};
Peer peers[net::MaxPlayers + 1];
unsigned sent = 0, received = 0, gainsApplied = 0, lifts = 0, queued = 0, repaired = 0;
int appliedTotal = 0;

eCDynamicEntity* Hero()
{
    gCSession& session = gCSession::GetInstance();
    return session.IsGameRunning() ? session.GetHero() : nullptr;
}

int Experience(eCDynamicEntity* hero) { return static_cast<int>(Entity(hero).PlayerMemory.XP); }
unsigned LevelOf(eCDynamicEntity* hero) { return static_cast<unsigned>(Entity(hero).NPC.Level); }

// Adds experience that another player earned. The game's own GiveXP does it (the notice, the level-up and the learning
// points); it takes the amount in fives when nobody earns it from a victim.
bool Add(int amount)
{
    eCDynamicEntity* hero = Hero();
    if (!hero || amount <= 0) return false;
    const int before = Experience(hero);
    const unsigned level = LevelOf(hero);
    applying = true;
    const int whole = amount / 5;
    if (whole > 0) GetScriptAdminExt().CallScript(bCString("GiveXP"), nullptr, hero, whole);
    if (amount >= 1000) {  // a lift over several levels: the game checks the level only twice per call
        for (int i = 0; i < 10; ++i) {
            const unsigned now = LevelOf(hero);
            GetScriptAdminExt().CallScript(bCString("GiveXP"), nullptr, hero, 0);
            if (LevelOf(hero) == now) break;
        }
    }
    applying = false;
    const int gained = Experience(hero) - before;
    if (gained != amount) {  // the remainder under five, or a call the game refused
        Entity(hero).PlayerMemory.XP = before + amount;
        if (amount - gained >= 5) ++repaired;
    }
    appliedTotal += amount;
    Logf("xpshare: %+d experience from the others: xp %d -> %d, level %u -> %u", amount, before, Experience(hero), level, LevelOf(hero));
    return true;
}

void Gain(int amount)
{
    if (amount <= 0 || amount > MaxGain) return;
    if (Add(amount)) ++gainsApplied;
    else { waiting += amount; ++queued; }
}

void Send()
{
    eCDynamicEntity* hero = Hero();
    if (!hero || !net::Connected()) return;
    net::WorldState s{};
    s.kind = 19;
    s.npcClass = net::LocalId();
    const uint32_t total = static_cast<uint32_t>(Experience(hero) > 0 ? Experience(hero) : 0);
    std::memcpy(s.guid, &nonce, 4);
    std::memcpy(s.guid + 4, &earned, 4);
    std::memcpy(s.guid + 8, &total, 4);
    net::SendWorldState(s, 0);
    ++sent;
    sentEarned = earned;
    lastSend = GetTickCount();
}

void LiftTo(int total, uint8_t from)
{
    eCDynamicEntity* hero = Hero();
    if (!hero) { lift = true; return; }
    const int mine = Experience(hero);
    if (total <= mine) return;
    Logf("xpshare: lifted to player %u's experience (%d -> %d)", from, mine, total);
    ++lifts;
    Add(total - mine);
}
}

void NoteGiveXp(int gained)
{
    if (!enabled || applying || gained <= 0 || gained > MaxGain) return;
    earned += static_cast<uint32_t>(gained);
    Logf("xpshare: the hero earned %d (%u in all)", gained, earned);
    Send();
}

void OnState(net::WorldState const& s)
{
    if (!enabled) return;
    const uint8_t from = s.npcClass;
    if (from == 0 || from >= net::MaxPlayers || from == net::LocalId()) return;
    uint32_t theirNonce, theirEarned, theirTotal;
    std::memcpy(&theirNonce, s.guid, 4);
    std::memcpy(&theirEarned, s.guid + 4, 4);
    std::memcpy(&theirTotal, s.guid + 8, 4);
    ++received;
    Peer& peer = peers[from];
    if (!peer.known || peer.nonce != theirNonce) {
        // First word from this player (or from its restarted game): what it earned so far is in its total already.
        peer.known = true;
        peer.nonce = theirNonce;
        peer.applied = theirEarned;
        if (theirTotal <= static_cast<uint32_t>(MaxGain) * 100) LiftTo(static_cast<int>(theirTotal), from);
    } else if (theirEarned > peer.applied) {
        const uint32_t gain = theirEarned - peer.applied;
        peer.applied = theirEarned;
        Gain(static_cast<int>(gain));
    }
    peer.total = theirTotal;
    peer.at = GetTickCount();
}

void OnWorldChange() { lift = true; }

void Tick()
{
    if (!enabled) return;
    eCDynamicEntity* hero = Hero();
    if (!nonce) nonce = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x9e3779b9u;
    if (!hero) return;
    if (waiting > 0) {
        const int amount = waiting;
        waiting = 0;
        Gain(amount);
    }
    if (lift) {
        lift = false;
        const DWORD now = GetTickCount();
        for (int id = 1; id < net::MaxPlayers; ++id)
            if (peers[id].known && now - peers[id].at < 15000) LiftTo(static_cast<int>(peers[id].total), static_cast<uint8_t>(id));
    }
    if (!net::Connected()) return;
    const DWORD now = GetTickCount();
    if (earned != sentEarned || now - lastSend >= Heartbeat) Send();
}

void Report(LogFn log)
{
    char line[500];
    eCDynamicEntity* hero = Hero();
    int n = Format(line, "XPSHARE enabled=%d xp=%d level=%u earned=%u sent=%u received=%u applied=%d gains=%u lifts=%u queued=%u waiting=%d repaired=%u peers=", enabled ? 1 : 0,
                   hero ? Experience(hero) : -1, hero ? LevelOf(hero) : 0, earned, sent, received, appliedTotal, gainsApplied, lifts, queued, waiting, repaired);
    for (int id = 1; id < net::MaxPlayers; ++id)
        if (peers[id].known) n += Format(line + n, sizeof(line) - n, "%d:%u:%u:%u,", id, peers[id].nonce, peers[id].applied, peers[id].total);
    log(line);
}

void Off(LogFn log)
{
    enabled = false;
    log("XPSHAREOFF ok=1");
}
}
