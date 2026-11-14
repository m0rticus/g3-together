// SPDX-License-Identifier: GPL-3.0-or-later
// Short on-screen messages through the game's passive HUD text list: centred, fades after 6 s, colour by type.
// Message only queues text. Tick presents it on the engine module thread without GUI activation or hero script calls.
#pragma once

namespace g3t::hud {
enum Kind { Info = 0, Good = 8, Warning = 12, Bad = 13 };  // gEGameMessageType: gold, green, orange, red
void Message(Kind kind, wchar_t const* text);
void Tick();
void OnWorldChange();  // discard delayed observations, never inspect a body from the old world
bool InventoryOpen();  // the inventory page, not merely a cached active-page enum
// The main menu's top line (the Community Patch's version line) with a second line of the mod's under it, for a player
// waiting there (the game's messages do not show in the main menu); nullptr puts it back. False: no such line here.
bool MenuLine(wchar_t const* text);
bool ClientNewGameEnabled(bool enabled); // paired with persist's guarded client command
}
