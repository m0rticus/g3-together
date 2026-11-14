// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

eCEntity* AutoAimTarget(eCVisualAnimation_PS* animation);
void LogDeathFocus(eCEntity* dead, eCEntity* body, char const* phase, uint32_t elapsed, bool removed = false);
unsigned ClearLocalReferencesToDeadMirror(eCEntity* dead, unsigned* combatStops = nullptr,
                                          bool first = true, bool removed = false);
void KeepDead(Entity& npc);
bool Lies(eCEntity* entity, char* shows, size_t size, char* slots, size_t slotsSize);
void LateCheck(Mirror& m, uint32_t now, char const* kind);
bool HostForceQueued(Mirror const& m);
bool HostForceAvailable(Mirror const& m);
bool TryApplyForce(Mirror& m, uint32_t now);
bool HasPendingCorpseForce(Mirror const& m);
char const* RagdollSource(Mirror const& m);
void CaptureCorpsePose(Mirror& m, uint32_t now, bool final = false);
void RestoreCorpseActor(Mirror& m);
void UpdateCorpsePresentation(Mirror& m, uint32_t now);
void PresentDeadMirror(GuidKey const& key, Mirror& m, net::NpcRecord const& shown, uint32_t now);
void OnRagdollNote(net::RagdollNote const& note);
void LayDown(Mirror& m, uint32_t now);
void LogClientDeathImpl(Mirror& m, net::NpcRecord const& shown, uint32_t now);
void TickDeathFocusWatch(Mirror& m, uint32_t now);
void CorpseTick(uint32_t now);
void ForgetCorpseGroundAnchor(GuidKey const& key);
void ClearCorpseGroundAnchors();

} // namespace g3t::npcs::detail
