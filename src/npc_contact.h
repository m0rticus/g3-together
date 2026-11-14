// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

bool ClearContact(eCEntity* attacker, eCEntity* hero);
bool PresentedContactPoints(Entity& entity, uint32_t heldRight, float a[3], float b[3], float& thickness, int& bone);
bool SweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                float thickness, bool weapon);
void SamplePlayerContact(uint32_t now);
bool StaggerState(char const* name);
void OnHitResult(net::HitResult const& result);
void BeginPassiveMirrorFrame(uint32_t now);
void ResetUnloadedMirror(Mirror& m, Entity const& live, eCEntity* previous);
void ReconcileMirrorAlive(GuidKey const& key, Mirror& m);
void SyncMirrorEquipmentAndDamage(Mirror& m, net::NpcRecord const& shown);
void RecordMirrorWalkTelemetry(Mirror& m, net::NpcRecord const& shown, uint32_t now);
bool MirrorFightsHeroImpl();

} // namespace g3t::npcs::detail
