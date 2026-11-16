// SPDX-License-Identifier: GPL-3.0-or-later
#include "navzone_guard.h"

#include "ai.h"
#include "log.h"
#include "patch.h"

#include <Game.h>
#include <Windows.h>
#include <cstring>

namespace g3t::navzone_guard {
namespace {
using DetectCollisionCircleExitFn = GEBool(__fastcall*)(gCNavZone_PS* self, void* edx, gCNavigation_PS* nav, bCVector* out);
DetectCollisionCircleExitFn originalDetectCollisionCircleExit = nullptr;
bool navZoneGuardSearched = false;
unsigned navZoneNullBlocked = 0;

// gCNavigationMap::DetectEvades (Game.dll+0x134740) reads CurrentZoneEntityProxy->GetPropertySet(NavZone) without
// checking for null (e.g. when CurrentZoneEntityProxy points to a gCNavPath_PS entity) before calling
// gCNavZone_PS::DetectCollisionCircleExit (Game.dll+0x117c00), crashing at +0x117c09 reading [ecx+0xd4].
GEBool __fastcall SafeDetectCollisionCircleExit(gCNavZone_PS* self, void* edx, gCNavigation_PS* nav, bCVector* out)
{
    if (!self || !nav || !out) {
        if (!ai::ShuttingDown() && ++navZoneNullBlocked == 1)
            Log("navzone: blocked null-zone gCNavZone_PS::DetectCollisionCircleExit call");
        return GEFalse;
    }
    return originalDetectCollisionCircleExit ? originalDetectCollisionCircleExit(self, edx, nav, out) : GEFalse;
}
}

void Install()
{
    if (navZoneGuardSearched || ai::ShuttingDown()) return;
    HMODULE game = GetModuleHandleA("Game.dll");
    if (!game) return;
    navZoneGuardSearched = true;
    auto* entry = reinterpret_cast<unsigned char*>(GetProcAddress(
        game, "?DetectCollisionCircleExit@gCNavZone_PS@@QAE_NPAVgCNavigation_PS@@AAVbCVector@@@Z"));
    // CP 1.75 Game.dll+0x117c00: sub esp, 0xc; push ebx; push ebp; mov ebx, ecx (7 bytes).
    static const unsigned char head[] = {0x83, 0xec, 0x0c, 0x53, 0x55, 0x8b, 0xd9};
    if (entry && (!std::memcmp(entry, head, sizeof(head)) || patch::IsPatchedByUs(entry, head, sizeof(head)))) {
        if (void* trampoline = ai::JumpTo(entry, reinterpret_cast<void*>(&SafeDetectCollisionCircleExit), sizeof(head))) {
            originalDetectCollisionCircleExit = reinterpret_cast<DetectCollisionCircleExitFn>(trampoline);
            Log("navzone: null-zone DetectCollisionCircleExit navigation guard installed");
            return;
        }
    }
    Log("navzone: null-zone DetectCollisionCircleExit guard UNAVAILABLE; export/prologue did not match");
}
}
