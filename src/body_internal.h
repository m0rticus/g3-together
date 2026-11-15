// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

namespace g3t::body {
extern bool settleStance;
extern bool stopMotion;
extern unsigned stanceStops;
extern unsigned stopOwners[8];
extern std::map<std::string, unsigned> glideMotions[2];
extern std::map<std::string, unsigned> stillMotions[2];
extern std::vector<float> motionLags[2];
extern bool lagMsUnit;
extern unsigned combatTargets[2];
extern unsigned followSkips[2][6];
extern unsigned otherNamesLogged[2];

inline constexpr double MotionUnitMs(double maxTime) noexcept
{
    return maxTime > 30.0 ? 1.0 : 1000.0;
}
}
