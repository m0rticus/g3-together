// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai.h"

#include <Game.h>
#include <Script.h>
#include <string>
#include <unordered_map>
#include <utility>

namespace g3t::ai::detail {
template <typename T>
T* Find(bTValMap<bCString, T*>& map, char const* name)
{
    T* found = nullptr;
    return map.GetAt(bCString(name), found) ? found : nullptr;
}

extern gFScript originalContinue;
extern bool continueBlock;
extern unsigned continueBlocked, continueRun;
extern unsigned playerSpectatorRefused, heroPerceptionBlocked;

extern gFScript originalMoveLength;
extern bool moveLengthGuard;
extern unsigned moveLengthCalls, moveLengthSanitized, moveLengthUnknown;

extern bool attitudeOn;
extern bool fleeKeep;

extern std::string countPrefix;
extern std::unordered_map<std::string, std::pair<unsigned, unsigned>> counted;
extern std::unordered_map<std::string, gFScriptAIState> originalStates;
extern std::unordered_map<std::string, gFScriptAIFunction> originalFunctions;
} // namespace g3t::ai::detail
