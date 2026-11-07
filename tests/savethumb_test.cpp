// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/savethumb.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace g3t::saving::thumb;

namespace {
void Check(bool ok, char const* what)
{
    if (ok) return;
    std::fprintf(stderr, "save thumbnail policy failed: %s\n", what);
    std::exit(1);
}

View Make(bool menu, bool page, bool minimized, bool retained, bool current)
{
    View view;
    view.menuOpen = menu; view.pageOpen = page; view.minimized = minimized;
    view.retained = retained; view.retainedCurrent = current;
    return view;
}

// Independent statement of the rule: a clean framebuffer is captured; otherwise only an image that is known to be a
// scene may be reused (the menu-open image while the menu is up, or one stamped for this world).
Choice Expected(View const& v)
{
    const bool dirty = v.menuOpen || v.pageOpen || v.minimized;
    if (!dirty) return Choice::CaptureScene;
    const bool known = v.menuOpen ? v.retained : (v.retained && v.retainedCurrent);
    return known ? Choice::KeepRetained : Choice::CaptureFallback;
}
}

int main()
{
    // The reported bug: menu save with the save page on screen must reuse the menu-open scene and never capture.
    Check(Choose(Make(true, false, false, true, true)) == Choice::KeepRetained, "menu save keeps the menu-open image");
    Check(Choose(Make(true, true, false, true, false)) == Choice::KeepRetained, "menu with a page still keeps it");
    Check(Choose(Make(true, false, false, false, false)) == Choice::CaptureFallback, "menu with no image falls back");

    // Quicksave and host-executed client requests with a clean scene capture fresh, whatever is retained.
    Check(Choose(Make(false, false, false, false, false)) == Choice::CaptureScene, "clean scene without image");
    Check(Choose(Make(false, false, false, true, false)) == Choice::CaptureScene, "clean scene ignores stale image");
    Check(Choose(Make(false, false, false, true, true)) == Choice::CaptureScene, "clean scene recaptures a current image");

    // Inventory/journal pages and a minimized window reuse the last good scene of this world only.
    Check(Choose(Make(false, true, false, true, true)) == Choice::KeepRetained, "page keeps a current scene");
    Check(Choose(Make(false, true, false, true, false)) == Choice::CaptureFallback, "page rejects a stale image");
    Check(Choose(Make(false, false, true, true, true)) == Choice::KeepRetained, "minimized keeps a current scene");
    Check(Choose(Make(false, false, true, false, false)) == Choice::CaptureFallback, "minimized without image falls back");

    for (unsigned bits = 0; bits < 32; ++bits) {
        const View view = Make(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16);
        Check(Choose(view) == Expected(view), "exhaustive decision table");
    }

    static_assert(Choose(View{}) == Choice::CaptureScene);
    static_assert(ImageOffset == 8);

    Tracker tracker;
    Check(!tracker.Current(0) && !tracker.Current(7), "unstamped tracker is never current");
    tracker.Stamp(7);
    Check(tracker.Current(7), "stamped epoch is current");
    Check(!tracker.Current(8), "a new world epoch invalidates the stamp");
    Check(!tracker.Current(0), "epoch zero is never current");
    tracker.Stamp(8);
    Check(tracker.Current(8) && !tracker.Current(7), "restamping moves to the new world");

    Check(std::string_view(Name(Choice::CaptureScene)) == "capture" &&
          std::string_view(Name(Choice::KeepRetained)) == "retained" &&
          std::string_view(Name(Choice::CaptureFallback)) == "fallback", "log names");
}
