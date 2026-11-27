// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class eCEntity;
class gCDynamicCollisionCircle_PS;

namespace g3t::navzone_guard {
void Install();
void MigrateZone(eCEntity const* previousZone, eCEntity const* lastZone, eCEntity const* newZone,
                 gCDynamicCollisionCircle_PS* navDcc, gCDynamicCollisionCircle_PS* entityDcc);
void OnEntityDestroyed(eCEntity* entity);
void OnWorldChange();
}
