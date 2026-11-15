// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Engine.h>
#include "processing.h"

namespace g3t::world::collision {
bool EnsureHooksInstalled();
bool SyncSphereStaticCollision(eCEntityAdmin* admin, processing::Sphere const* spheres, int sphereCount);
bool IsNpcCollisionReady(eCEntity const* entity);
void OnEntityDestroyed(eCEntity* entity);
void Retire();
}
