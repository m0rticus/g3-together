// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class eCEntity;

namespace g3t::tips {

// Tracks world activations so each client world entry applies the default once.
void OnWorldActivate();

// Ensures the tutorial dialog's "Do not show tips" checkbox is checked before
// native NewGame/LoadGame calls gCSession::Start -> OpenTutorial("TUT_Start").
void PrepareForWorldEntry(char const* reason);

// Enables gCPlayerMemory_PS::HideTips and the tutorial checkbox right after a
// client world entry succeeds (including before template capture runs).
void OnWorldEntered(char const* reason, eCEntity* hero, uint32_t welcomeSerial);

// Applies the default when a client joins while already in a world or reconnects,
// and resets per-join state on disconnect. No-op on the host.
void Tick(bool isHost, bool joined, bool inRunningWorld, eCEntity* hero, uint32_t welcomeSerial);

} // namespace g3t::tips
