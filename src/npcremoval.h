// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class eCEntity;
namespace g3t::npcremoval {
void Start();
void Tick();
void OnWorldDeactivate();
bool ExcludeFromProcessing(eCEntity const* entity);
bool Suppressed(eCEntity const* entity);
bool Suppressed(uint8_t const guid[16]);
}
