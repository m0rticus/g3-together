// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g3t::processing {
struct Sphere {
    float at[3];
    float radius;
};

inline bool IncludesNpc(bool native, bool npc, float const box[6], Sphere const* spheres, int count)
{
    if (native || !npc) return native;
    for (int i = 0; i < count; ++i) {
        Sphere const& s = spheres[i];
        if (box[0] <= s.at[0] + s.radius && s.at[0] - s.radius <= box[3] &&
            box[1] <= s.at[1] + s.radius && s.at[1] - s.radius <= box[4] &&
            box[2] <= s.at[2] + s.radius && s.at[2] - s.radius <= box[5])
            return true;
    }
    return false;
}
}
