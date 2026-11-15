// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "enclaves.h"
#include "net.h"

#include <Game.h>
#include <Script.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace g3t::enclaves::detail {
enum Mode : uint8_t { Shared = 0, Together = 1, Personal = 2 };
constexpr int Crimes = gEPlayerCrime_Count;
constexpr int Alignments = 13; // gEPoliticalAlignment 0..12

struct Counters {
    int32_t crime[Crimes] = {}, suspect[Crimes] = {};
    int32_t known = 0, fame = 0;
};

struct Pending {
    std::string name;
    net::EnclaveNote delta;
    uint32_t at = 0;
    unsigned returned = 0; // items::Returned() when the theft was counted
    unsigned begun = 0, done = 0; // items::TakesBegun()/TakesDone() then
};

extern std::map<std::string, Counters> seen;
extern std::vector<Pending> pending;
extern Mode mode;
extern bool sync;
extern bool statusSync;
extern bool keepLocal;
extern bool clientFame;
extern bool politicalSync;
extern uint32_t scanPausedUntil;
extern unsigned sent, received, applied, deltas, dropped, keptLocal, credits, statusSent;
extern unsigned politicalSent, politicalApplied, politicalMoved;

extern bool notifyNative;
extern bool remoteMurders;
extern bool creditQueue;
extern uint32_t creditHoldUntil;
extern unsigned remoteCounted, remoteCredited, remoteNative, remoteNotCounted, remoteExpired, remoteMoved;
extern unsigned creditsHeld, creditsLate, creditsLost;

char const* ModeName(Mode m);
gCEnclave_PS* Native(Entity& e);
Counters Read(gCEnclave_PS* enclave);
void Write(Entity& e, Counters const& c);
bool ReadPolitical(int32_t out[Alignments]);
void AddPolitical(int alignment, int32_t by);
net::EnclaveNote PoliticalNote(int alignment, int32_t count);

char const* NotCounted(eCEntity* victim, Entity& town, gCEnclave_PS* enclave, char const* actor);
void InstallNotify();
void SendHeldCredits();
void ClearRemoteKills();
unsigned RemoteKillsSize();
} // namespace g3t::enclaves::detail
