// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

bool ClearContact(eCEntity* attacker, eCEntity* hero);
bool PresentedContactPoints(Entity& entity, uint32_t heldRight, float a[3], float b[3], float& thickness, int& bone);
bool SweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                float thickness, bool weapon);
void ResetIncomingSwing(Mirror& m, uint32_t instance, eCEntity* hero);
void RecordPreHitContact(Mirror& m, eCEntity* hero, float const a[3], float const b[3], int bone, float phase, uint32_t now);
bool EvaluateIncomingSwingSweep(eCEntity* hero, Mirror& m, float const oldA[3], float const oldB[3],
                                float const a[3], float const b[3], float thickness, bool weapon, char const*& hitSource);
void FinishIncomingSwingDiag(Mirror& m, net::NpcRecord const& shown, uint32_t now);
void NoteIncomingClockTransition(Mirror& m, net::NpcRecord const& shown, uint32_t now);
void LogLocalBlowContact(Mirror& m, eCEntity* hero, net::NpcRecord const& shown, uint32_t instance,
                         char const* hitSource, uint32_t now);
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
