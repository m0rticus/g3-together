// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_native.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace g3t::rejoin {
enum class Wait { None, Native, Save, Interaction };

// Client template slot: a native save of the starting world with the fresh New Game hero, made once on the
// player's own machine and entered through the engine's own LoadGame. The native New Game restarts the world
// that is already loaded in place (RestartWorld(false)), which vanilla never does and which froze the client.
namespace tmpl {
inline constexpr char Slot[] = "G3T_client_template";
inline constexpr uint32_t DelayMs = 2000;      // let the first frames settle before the one native save
inline constexpr uint32_t DeadlineMs = 25000;  // never hold the account record longer than this
inline constexpr uint32_t RetryDelayMs = 1500; // between the (at most two) saves of one hold window
inline constexpr unsigned MaxTries = 2;
inline constexpr uint32_t Sidecar = 2;         // .g3t extension index in persist.cpp

// Only this exact slot may be created, replaced or deleted by the mod; every other slot name is a player's.
inline bool OwnSlot(char const* name) { return name && std::strcmp(name, Slot) == 0; }

// The save after a New Game, taken before the account record replaces the fresh hero. The hold lasts until the save
// (native call + the engine's asynchronous world write + sealing) is done or the deadline passes; one retry inside it.
struct Capture {
    bool wanted = false, busy = false;
    unsigned tries = 0;
    uint32_t due = 0, deadline = 0;
    void Want(uint32_t now) { wanted = true; busy = false; tries = 0; due = now + DelayMs; deadline = now + DeadlineMs; }
    void Done() { wanted = busy = false; }
    void Began() { busy = true; }
    // A failed attempt: retry once more after a pause, otherwise give up (the hold ends with it).
    void Failed(uint32_t now)
    {
        busy = false;
        if (++tries >= MaxTries) wanted = false;
        else due = now + RetryDelayMs;
    }
    bool Expired(uint32_t now) const { return wanted && static_cast<int32_t>(now - deadline) >= 0; }
    bool Due(uint32_t now) const { return wanted && !busy && static_cast<int32_t>(now - due) >= 0 && !Expired(now); }
    bool Holding(uint32_t now) const { return wanted && !Expired(now); }
};
}

// A New Game while this process already entered a world is the in-place same-world restart.
inline bool NewGameAllowed(bool worldEntered) { return !worldEntered; }

struct Boundary {
    bool nativeBusy = false, nested = false, savePending = false, interaction = false;
};
inline Wait Waiting(Boundary const& boundary)
{
    if (boundary.nativeBusy || boundary.nested) return Wait::Native;
    if (boundary.savePending) return Wait::Save;
    if (boundary.interaction) return Wait::Interaction;
    return Wait::None;
}
struct State {
    bool pending = false;
    bool fresh = false;
    bool failed = false;

    void Queue() { pending = true; fresh = false; failed = false; }
    void Retired() { pending = false; fresh = false; }
    void Started(bool ok) { fresh = ok; failed = !ok; }
    void Left() { fresh = false; }
    Wait Waiting(Boundary const& boundary) const { return rejoin::Waiting(boundary); }
    bool CanRetire(Boundary const& boundary) const { return pending && Waiting(boundary) == Wait::None; }
    bool CanUseFresh(bool running, bool hostReady) const { return fresh && !pending && !failed && running && hostReady; }
};
inline bool ReplacesWorld(uint32_t oldEpoch, uint32_t nextEpoch, bool oldHostReady, bool localPlayer, bool pending)
{
    return oldEpoch != nextEpoch && oldEpoch && localPlayer && (oldHostReady || pending);
}
inline bool MenuEnabled(bool client, bool hooks, bool nativeBusy)
{
    return client && hooks && !nativeBusy;
}

namespace native {
using saving::native::Site;
inline constexpr Site Command = {
    "Menu.NewGame", 0x90870, "558bec6aff68????????68????????64a100000000506489250000000083ec0c", nullptr
};
inline constexpr Site Loader = {
    "HUD.NewGame", 0xa9740, "8b44240483ec1883780400752c8b0d????????8b11526aff6a006a0068????????6a03", nullptr
};
// The command's confirmed branch (IDYES): a no-argument cdecl that builds/gets the HUD file-manager singleton and calls the
// loader. Autoload calls it to start what a confirmed click starts, without the modal dialog; it is not hooked.
inline constexpr Site Confirmed = {
    "Menu.NewGame confirmed", 0x909e0,
    "b8010000008405????????75290905????????33c068????????c705????????????????a3????????a3????????e8????????83c40468????????e8????????c3",
    nullptr
};
constexpr uint32_t ConfirmedCall = 0x909ca; // Command's call to Confirmed
constexpr size_t ConfirmedSingleton = 0x37;  // Confirmed's push of the HUD file-manager singleton (the loader's argument)
constexpr size_t CommandStolen = 5, LoaderStolen = 7;
// These identify the client-only control and the dangerous native player-present branch.
inline constexpr Site MenuGate = {
    "Menu.NewGame eligibility", 0x8f7c0,
    "83ec088b0d????????53568bf08b411c85c00f95c385c057885c2410740ee8????????84c0c644240c017505c644240c008b8e880000008b51048b3d????????84db0f94c08d8c328800000050ffd7",
    nullptr
};
inline constexpr Site Button = {
    "Menu.NewGame control", 0x8f6f4,
    "8d868800000085c0740c8b088b51048d84328800000050685b50000057ffd3", nullptr
};
inline bool Matches(uint8_t const* image, size_t size)
{
    if (!image) return false;
    for (auto const& site : {Command, Loader, MenuGate, Button, Confirmed}) {
        const size_t count = std::strlen(site.bytes) / 2;
        if (!saving::native::Range(size, site.hint, count) ||
            !saving::native::Matches(image + site.hint, count, site.bytes)) return false;
    }
    // The confirmed branch must hand the loader the very singleton FileManagerGetter returns.
    const size_t getter = saving::native::Sites[4].hint;
    return saving::native::Calls(image, size, 0x90a1b, Loader.hint) &&
           saving::native::Calls(image, size, ConfirmedCall, Confirmed.hint) &&
           saving::native::Range(size, getter, 32) && saving::native::Range(size, Confirmed.hint, ConfirmedSingleton + 4) &&
           saving::native::Word(image, Confirmed.hint + ConfirmedSingleton) == saving::native::Word(image, getter + 28);
}
}
}
