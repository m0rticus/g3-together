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
uint32_t worldSerial = 0;
uint32_t streamSerial = 0;
uint32_t earned = 0;    // what this machine's hero earned itself since the game started
uint32_t sentEarned = 0xffffffff;
DWORD lastSend = 0;
int waiting = 0;        // experience to add as soon as the hero can take it
constexpr DWORD Heartbeat = 2000;
constexpr int MaxGain = 10000000;  // a sanity bound for one step

struct Peer : counter::Receiver {
    uint32_t total = 0;    // its hero's experience in its last message
    DWORD at = 0;
    bool wasInWorld = false;
    uint32_t generation = 0;
    counter::Sender outgoing;
    uint32_t ackedNonce = 0;
    DWORD lastBaseline = 0;
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

void SendLive()
{
    eCDynamicEntity* hero = Hero();
    if (!hero || !net::Connected() || !net::InWorld(net::LocalId())) return;
    const uint32_t total = static_cast<uint32_t>(Experience(hero) > 0 ? Experience(hero) : 0);
    bool delivered = false;
    const uint8_t first = net::IsHost() ? 2 : 1, end = net::IsHost() ? net::MaxPlayers : 2;
    for (uint8_t id = first; id < end; ++id) {
        Peer const& peer = peers[id];
        if (!peer.wasInWorld || !peer.outgoing.nonce || peer.ackedNonce != peer.outgoing.nonce || !net::InWorld(id)) continue;
        net::WorldState s{};
        s.kind = 19;
        s.npcClass = net::LocalId();
        s.dialog = counter::Live;
        const uint32_t relative = peer.outgoing.Total(earned);
        std::memcpy(s.guid, &peer.outgoing.nonce, 4);
        std::memcpy(s.guid + 4, &relative, 4);
        std::memcpy(s.guid + 8, &total, 4);
        if (net::SendWorldStateTo(id, s)) { ++sent; delivered = true; }
    }
    if (delivered) { sentEarned = earned; lastSend = GetTickCount(); }
}

void SendBaselineTo(uint8_t to)
{
    eCDynamicEntity* hero = Hero();
    if (!hero || !net::Connected() || !peers[to].outgoing.nonce) return;
    net::WorldState s{};
    s.kind = 19;
    s.dialog = counter::Baseline;
    s.npcClass = net::LocalId();
    const uint32_t total = static_cast<uint32_t>(Experience(hero) > 0 ? Experience(hero) : 0);
    std::memcpy(s.guid, &peers[to].outgoing.nonce, 4);
    std::memcpy(s.guid + 4, &counter::BaselineEarned, 4);
    std::memcpy(s.guid + 8, &total, 4);
    net::SendWorldStateTo(to, s);
    peers[to].lastBaseline = GetTickCount();
}

void SendAckTo(uint8_t to, uint32_t theirNonce)
{
    if (!net::Connected()) return;
    net::WorldState s{};
    s.kind = 19;
    s.dialog = counter::Ack;
    s.npcClass = net::LocalId();
    std::memcpy(s.guid, &theirNonce, 4);
    net::SendWorldStateTo(to, s);
}

void SendRequestTo(uint8_t to)
{
    if (!net::Connected()) return;
    net::WorldState s{};
    s.kind = 19;
    s.dialog = counter::Request;
    s.npcClass = net::LocalId();
    net::SendWorldStateTo(to, s);
}

void PrepareAdmissions(DWORD now)
{
    if (!net::Connected()) return;
    const uint8_t first = net::IsHost() ? 2 : 1, end = net::IsHost() ? net::MaxPlayers : 2;
    for (uint8_t id = first; id < end; ++id) {
        Peer& peer = peers[id];
        const bool joined = net::IsHost() ? net::PeerJoined(id) : net::Joined();
        const uint32_t generation = net::IsHost() ? net::PeerGeneration(id) : net::WelcomeSerial();
        if (!joined) { peer = Peer{}; continue; }
        if (peer.generation != generation) { peer = Peer{}; peer.generation = generation; }
        const bool inWorld = net::InWorld(net::LocalId()) && net::InWorld(id);
        if (!inWorld) {
            peer.wasInWorld = false;
            peer.outgoing = counter::Sender{};
            peer.ackedNonce = 0;
            continue;
        }
        if (!peer.wasInWorld) {
            peer.wasInWorld = true;
            uint32_t stream = 0;
            while (!stream) stream = nonce ^ (++streamSerial * 2654435761u);
            peer.outgoing.Begin(stream, earned);
            peer.ackedNonce = 0;
            peer.lastBaseline = 0;
        }
        if (peer.ackedNonce != peer.outgoing.nonce && (!peer.lastBaseline || now - peer.lastBaseline >= Heartbeat)) {
            SendBaselineTo(id);
        }
    }
}

}

void NoteGiveXp(int gained)
{
    if (!enabled || applying || gained <= 0 || gained > MaxGain) return;
    // Capture a new peer's origin before adding its first future reward.
    PrepareAdmissions(GetTickCount());
    earned += static_cast<uint32_t>(gained);
    Logf("xpshare: the hero earned %d (%u in all)", gained, earned);
    SendLive();
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
    if (s.dialog == counter::Ack) {
        if (theirNonce == peer.outgoing.nonce) peer.ackedNonce = theirNonce;
        peer.at = GetTickCount();
        SendLive();
        return;
    }
    if (s.dialog == counter::Request) {
        PrepareAdmissions(GetTickCount());
        peer.ackedNonce = 0;
        peer.lastBaseline = 0;
        SendBaselineTo(from);
        peer.at = GetTickCount();
        return;
    }
    if (s.dialog == counter::Baseline) {
        if (!peer.Snapshot(theirNonce, theirEarned)) {
            Log("xpshare: refused invalid admission baseline");
            return;
        }
        peer.total = theirTotal;
        peer.at = GetTickCount();
        SendAckTo(from, theirNonce);
        return;
    }
    if (s.dialog != counter::Live) return;
    if (!peer.known || peer.nonce != theirNonce) {
        // A live packet without a baseline for this epoch is not proof of a future gain.
        peer.at = GetTickCount();
        SendRequestTo(from);
        return;
    }
    const uint32_t gain = peer.Take(theirNonce, theirEarned, MaxGain);
    if (gain > 0) {
        Gain(static_cast<int>(gain));
    }
    peer.total = theirTotal;
    peer.at = GetTickCount();
}

void OnWorldChange()
{
    // A loaded hero keeps its saved XP/level. Only gains earned after this baseline are shared.
    waiting = 0;
    earned = 0;
    sentEarned = 0xffffffff;
    nonce = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (++worldSerial * 2654435761u);
    if (!nonce) nonce = 1;
    for (auto& peer : peers) peer = Peer{};
}

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
    PrepareAdmissions(GetTickCount());
    if (!net::Connected()) return;
    const DWORD now = GetTickCount();
    if (earned != sentEarned || now - lastSend >= Heartbeat) SendLive();
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
