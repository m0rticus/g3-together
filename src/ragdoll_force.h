// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace g3t::ragdoll {
struct Vector { float x, y, z; };

inline bool Finite(Vector const& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline Vector RelativePoint(Vector const& world, Vector const& root)
{
    return {world.x - root.x, world.y - root.y, world.z - root.z};
}

inline Vector WorldPoint(Vector const& relative, Vector const& root)
{
    return {root.x + relative.x, root.y + relative.y, root.z + relative.z};
}

// CP 1.75 Script_Game+0x1c985: explosion direction is normalized horizontally, then Y becomes 0.2;
// its distance factor is at most 1 and its force scale is 80000. This is a defensive bound, not a native global clamp.
constexpr double MaximumDeathForce = 80000.0 * 1.019803902718557;

inline double Magnitude(Vector const& v)
{
    return std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
}

inline bool LimitForce(Vector const& input, Vector& output)
{
    if (!Finite(input)) return false;
    const double length = Magnitude(input);
    const double scale = length > MaximumDeathForce ? MaximumDeathForce / length : 1.0;
    output = {float(input.x * scale), float(input.y * scale), float(input.z * scale)};
    const double rounded = Magnitude(output);
    if (rounded > MaximumDeathForce) {
        const float correction = std::nextafter(float(MaximumDeathForce / rounded), 0.0f);
        output = {output.x * correction, output.y * correction, output.z * correction};
    }
    return true;
}

enum class Source { Pending, Host, Gravity };

struct DeathImpulse {
    Source source = Source::Pending;
    unsigned applied = 0;

    bool Pending() const { return source == Source::Pending; }
    bool HostApplied() const { return source == Source::Host; }
    bool Choose(Source next)
    {
        if (!Pending() || next == Source::Pending) return false;
        source = next;
        applied = next == Source::Host ? 1 : 0;
        return true;
    }
};
}
