// SPDX-License-Identifier: GPL-3.0-or-later
// QA for the world interactions of the inventory (INVENTAR-SKRIPTE.md, agent A, 20.10.): doors, ore, campfire, anvil,
// shrine, beds, water barrels, boulders. The hero uses an interact object the way the game's OnPlayerAction does: the
// object as its target and focus, then the player state PS_Interact, which picks PS_Interact_<kind> by the object.
#pragma once

#include <Engine.h>
#include "hostsim_status.h"

namespace g3t::world {
void RequestProcessingUpdate();  // incremental range refresh; never exit the local hero to force recollection
// Every entity the engine processes here, static ones included (doors, ore veins); null before the scene exists.
bTSmallArray<eCEntity*> const* ProcessedEntities();
struct ProcessingMembership {
    bool range = false, processable = false;
};
ProcessingMembership ProcessingOf(eCEntity const* entity);
// Registration is an optimization (engine range/processable lists); `why` names the exact reason when it returns false.
bool QueueEntityProcessing(eCEntity* entity, char const** why = nullptr);
void Tick();  // host: NPC-only extra ranges; native camera, ROI and static culling stay unchanged
void Start();
void OnWorldActivate();
void CaptureCacheInventory();  // engine thread only; publishes counts/pointers for worker-safe crash reporting
void OnWorldDeactivate();
// host_sim = elastic or spheres: whether the host's game processes around the other players too in this frame, and whether
// it processes this place (margin cm inside the edge of its sphere or of one of its spheres).
bool Extended();
bool HostProcesses(float const at[3], float margin);
// Compact state of the host's simulation around the other players for the 5-second `npc-stream:` line. Engine thread only.
HostSimStatus HostSimulationStatus();
}
