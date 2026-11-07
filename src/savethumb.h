// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

// HUD.SaveGame never captures: it hands the file manager's retained image [manager+8] to World.SaveGameWorld.
// Native fills that slot when the in-game menu opens (before its window is shown) and for the quicksave
// message. A capture taken while the menu or a page is on screen is a picture of that window.
namespace g3t::saving::thumb {
inline constexpr size_t ImageOffset = 8;

struct View {
    bool menuOpen = false;        // in-game menu is on screen; the world is paused
    bool pageOpen = false;        // inventory/journal/map style page is on screen
    bool minimized = false;       // game window minimized or Alt-Tabbed; the back buffer is not a rendered scene
    bool retained = false;        // the file manager holds an image
    bool retainedCurrent = false; // that image was taken in the current world epoch
};

enum class Choice : uint8_t {
    CaptureScene,    // framebuffer is the clean scene, as for the native quicksave
    KeepRetained,    // keep the native menu-open image or our last good scene
    CaptureFallback  // nothing usable is retained; a picture with an overlay beats refusing the save
};

constexpr Choice Choose(View const& v)
{
    if (!v.menuOpen && !v.pageOpen && !v.minimized) return Choice::CaptureScene;
    if (v.retained && (v.menuOpen || v.retainedCurrent)) return Choice::KeepRetained;
    return Choice::CaptureFallback;
}

constexpr char const* Name(Choice choice)
{
    return choice == Choice::CaptureScene ? "capture" : choice == Choice::KeepRetained ? "retained" : "fallback";
}

// Epoch of the retained image. The epoch changes with every world, so a stamp never outlives its world.
class Tracker {
    uint32_t stamped = 0;
  public:
    void Stamp(uint32_t epoch) { stamped = epoch; }
    bool Current(uint32_t epoch) const { return epoch && stamped == epoch; }
};
}
