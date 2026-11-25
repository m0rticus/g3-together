// SPDX-License-Identifier: GPL-3.0-or-later
// Host-authoritative world sector state synchronization, persistence, and join/load reconciliation.
#pragma once

#include "net.h"

#include <cstdint>

namespace g3t::sectors {

void Start();
void Tick();
void OnWorldChange();
void PrepareAdmission();
bool AdmissionStarted();
bool AdmissionReady();
bool FinishAdmission(uint8_t player);

// Called from infocmd OnWorldState for kind == 2. Returns true if the message was handled as a
// baseline/handshake packet or refused, false if it is a normal live state delta to apply/relay.
bool HandleWire(uint8_t from, net::WorldState const& s);

// Applies a single sector state to the live world and updates gCSectorPersistence_PS.
bool ApplySector(char const* name, bool enabled, char const* source = "sync");

// Host: broadcasts any pending sector change if not already broadcast by hooks.
void BroadcastIfChanged(char const* name, bool enabled);

// Host: pushes current full sector state to all connected clients (called on world sync).
void SyncToClients();

// Stats for INFOCMD QA reporting.
void Counts(unsigned& sent, unsigned& applied);

} // namespace g3t::sectors
