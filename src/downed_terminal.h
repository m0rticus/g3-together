// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "downed_policy.h"
#include "net.h"
#include <Game.h>
#include <Script.h>
#include <string>

namespace g3t::downed {
struct LocalLifeState : Life {
    Recovery recovery;
    HeldConvergence convergence;
    bool maintainingHp = false, lethal = false, lethalSeenDown = false, nativeNoted = false;
    bool recoveryUnknownNoted = false, wasDown = false, nativeTerminalKillChecked = false;
    uint32_t lethalAt = 0, lethalBlowAt = 0, aloneSince = 0, pendingReviveSequence = 0;
    uint32_t terminalAt = 0, terminalSampleAt = 0, terminalMutationAt = 0, helpSaidAt = 0, lastOffer = 0;
    uint32_t lastCorrection = 0, lastStandRefusal = 0;
    int lastHp = -1, revivedBy = 0, terminalHp = -1, terminalAi = -1, terminalMovement = -1;
    unsigned redirects = 0, revives = 0, deaths = 0, potionsPaid = 0, refusedNoPotion = 0;
    unsigned nativeLethal = 0, terminalStateEntries = 0, terminalStateReturns = 0, nativeTerminalKills = 0;
    unsigned terminalMutations = 0, transitions = 0, corrections = 0, standRefusals = 0;
    unsigned helpNotSaid = 0, optionsOffered = 0, optionsChosen = 0;
    std::string terminalTask, terminalState, inHand[2];

    void Reset(bool preserveHeld = false) {
        const bool waiting = preserveHeld && held;
        const uint32_t previousSequence = sequence;
        *this = LocalLifeState{};
        held = waiting;
        sequence = previousSequence;
        lethal = waiting;
    }
};
struct PeerLifeState {
    RevivePaymentSlot payment;
    bool told = false, toldDead = false, hinted = false, helpStarted = false;
    void Reset() { *this = PeerLifeState{}; }
};
extern LocalLifeState life;
extern PeerLifeState peers[net::MaxPlayers];
struct MaintainHp {
    bool before = life.maintainingHp;
    MaintainHp() { life.maintainingHp = true; }
    ~MaintainHp() { life.maintainingHp = before; }
};
using SetModeFn = void(__fastcall*)(gCScriptRoutine_PS*, void*, bTPropertyContainer<gEAIMode> const&);
using SetHpFn = GEBool(__fastcall*)(gCPlayerMemory_PS*, void*, bCString const&, GEInt);
using SetTaskFn = void(__fastcall*)(gCScriptProcessingUnit*, void*, bCString, GEBool);
using SetStateFn = void(__fastcall*)(gCScriptProcessingUnit*, void*, bCString);
extern SetModeFn originalSetMode;
extern SetHpFn originalSetHp, originalAddHp, originalSetValue;
extern SetTaskFn originalSetTask;
extern SetStateFn originalSetState;
void __fastcall SetMode(gCScriptRoutine_PS*, void*, bTPropertyContainer<gEAIMode> const&);
GEBool __fastcall SetHp(gCPlayerMemory_PS*, void*, bCString const&, GEInt);
GEBool __fastcall AddHp(gCPlayerMemory_PS*, void*, bCString const&, GEInt);
GEBool __fastcall SetValue(gCPlayerMemory_PS*, void*, bCString const&, GEInt);
void __fastcall SetTask(gCScriptProcessingUnit*, void*, bCString, GEBool);
void __fastcall SetState(gCScriptProcessingUnit*, void*, bCString);
void InstallHpGuards(void* game);
bool DownTask(char const*);
bool LocalTerminal(eCEntity*);
void Mutation(char const*, char const*, char const*, bool, void* caller = nullptr);
void EndStep(char const*);
void KeepTerminal(Entity&, char const*);
void Transition(char const*, char const*);
void CancelAlone(char const*);
void StandRefused(char const*, char const*);
bool RejectHeldTransition(eCEntity*, char const*, char const*);
Availability HelperState(net::Remote const&, uint32_t);
bool Able(net::Remote const&, uint32_t);
bool AllOthersDown(net::Remote const*, uint32_t);
bool PartnerPresent(net::Remote const*, uint32_t);
void LogHelpers(Entity*, char const*, uint32_t);
void Die(Entity&, char const*);
}
