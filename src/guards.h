// SPDX-License-Identifier: GPL-3.0-or-later
// Guards and the other players (P7, owner 25.10.: as in single-player). The game checks a drawn weapon and a guarded
// place only against its own player: AssessActivePerception hands the player it perceived to AssessPlayerZone and
// AssessPlayerWeapon, which give up for anyone else (IsPlayer, GetPlayer), and the guard's reaction states follow
// Entity::GetPlayer (ZS_ReactToZone_Loop, ZS_GotoPlayerZone, ZS_ObserveSuspect_Loop). So a guard of this machine never
// warned the other player (trespassdiag 25.10.: the host's hero drew its weapon at Brenton or walked into Muntasir's
// place and was attacked, the client's nothing). Here an NPC this machine runs that perceived nothing of its own checks
// the same two rules against each puppet near it, and while it does, and while one of those reaction states runs with a
// puppet as its target, the game's Entity::GetPlayer and IsPlayer (Script_Game.dll's imports) name that puppet.
#pragma once

class eCEntity;

namespace g3t::guards {
void Tick();  // installs the hooks once the game's scripts are registered

// While alive, the game's player is `puppet` for Script_Game.dll (none: no change).
// Read-only queries pass keepChapter=false to avoid changing the puppet's PlayerMemory.
class Acting {
public:
    explicit Acting(eCEntity* puppet, bool keepChapter = true);
    ~Acting();
    Acting(Acting const&) = delete;
    Acting& operator=(Acting const&) = delete;

private:
    eCEntity* before;
};

// Local script entry points must not inherit a nested NPC's puppet identity.
class LocalPlayer {
public:
    explicit LocalPlayer(bool local = true);
    ~LocalPlayer();
    LocalPlayer(LocalPlayer const&) = delete;
    LocalPlayer& operator=(LocalPlayer const&) = delete;

private:
    eCEntity* before;
};

// The puppet that stands for the game's player right now, none for the local hero.
eCEntity* CurrentActor();
// This machine's own player body (its hero, or the creature it controls). Acting() makes the game's IsPlayer false for it, so a
// script that runs with this body as self under an Acting scope treats the player as an NPC (it watched a fight, applauded and
// walked around, 2 Nov. play session). Local script boundaries use LocalPlayer to clear even an inherited Acting scope.
bool IsLocalPlayer(eCEntity const* entity);
// Remote presentation never owns either local body. Admission/save restore, New Game placement,
// downed/revive, dialogue/trade and deliberate script teleports use their explicit native paths.
bool RejectRemoteWrite(eCEntity const* entity, char const* writer);
bool PlayerContextReady();  // both native GetPlayer/IsPlayer imports were redirected

// ai.cpp, before an AI state of `self` runs: the puppet it reacts to when that state follows the player, else none.
eCEntity* StateActor(eCEntity* self, char const* state);
}
