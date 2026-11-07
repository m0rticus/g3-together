// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_native.h"

namespace g3t::rejoin {
enum class Wait { None, Native, Save, Interaction };
struct Boundary {
    bool nativeBusy = false, nested = false, savePending = false, interaction = false;
};
struct State {
    bool pending = false;
    bool fresh = false;
    bool failed = false;

    void Queue() { pending = true; fresh = false; failed = false; }
    void Retired() { pending = false; fresh = false; }
    void Started(bool ok) { fresh = ok; failed = !ok; }
    void Left() { fresh = false; }
    Wait Waiting(Boundary const& boundary) const
    {
        if (boundary.nativeBusy || boundary.nested) return Wait::Native;
        if (boundary.savePending) return Wait::Save;
        if (boundary.interaction) return Wait::Interaction;
        return Wait::None;
    }
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
    for (auto const& site : {Command, Loader, MenuGate, Button}) {
        const size_t count = std::strlen(site.bytes) / 2;
        if (!saving::native::Range(size, site.hint, count) ||
            !saving::native::Matches(image + site.hint, count, site.bytes)) return false;
    }
    return saving::native::Calls(image, size, 0x90a1b, Loader.hint);
}
}
}
