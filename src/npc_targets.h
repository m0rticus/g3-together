// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class eCEntity;
class gCScriptProcessingUnit;

namespace g3t::npc_targets {
struct ScrubStats {
    unsigned targetCleared = 0;
    unsigned alternativeCleared = 0;
    unsigned attackerCleared = 0;
    unsigned lastAttackerCleared = 0;
    unsigned spellTargetCleared = 0;
    unsigned instructionsRemoved = 0;
    unsigned combatCancelled = 0;
};

ScrubStats ScrubEntityReferences(eCEntity* target, bool removeInstructions = true, bool cancelCombat = false);
bool ReleaseDownedTargets(eCEntity* entity, char const* reason, gCScriptProcessingUnit* spu = nullptr);
void SyncDownedTargets();
bool RetargetDownedBeforeCancel(eCEntity* self, gCScriptProcessingUnit* spu);
} // namespace g3t::npc_targets
