// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace g3t::shot_sweep {

struct Cylinder {
    float cx = 0.0f;
    float cz = 0.0f;
    float bottomY = 0.0f;
    float topY = 0.0f;
    float radius = 0.0f;
};

inline bool Finite3(float const v[3])
{
    return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline Cylinder BuildTargetCylinder(float const worldPos[3], float const boxMin[3], float const boxMax[3],
                                    float characterHeight)
{
    Cylinder out{};
    if (!Finite3(worldPos)) return out;
    const float fallbackH = (std::isfinite(characterHeight) && characterHeight > 20.0f)
                                ? std::clamp(characterHeight, 40.0f, 500.0f)
                                : 180.0f;
    if (Finite3(boxMin) && Finite3(boxMax)) {
        const float dx = boxMax[0] - boxMin[0];
        const float dy = boxMax[1] - boxMin[1];
        const float dz = boxMax[2] - boxMin[2];
        const float midX = 0.5f * (boxMin[0] + boxMax[0]);
        const float midZ = 0.5f * (boxMin[2] + boxMax[2]);
        if (dx > 4.0f && dy > 10.0f && dz > 4.0f &&
            dx < 1200.0f && dy < 1200.0f && dz < 1200.0f &&
            std::fabs(midX - worldPos[0]) < 400.0f &&
            std::fabs(midZ - worldPos[2]) < 400.0f) {
            const float rawRadius = 0.5f * std::min(dx, dz);
            const float maxRadius = (std::isfinite(characterHeight) && characterHeight > 20.0f)
                                        ? std::clamp(characterHeight * 0.40f, 36.0f, 150.0f)
                                        : 140.0f;
            out.cx = midX;
            out.cz = midZ;
            out.bottomY = boxMin[1];
            out.topY = std::max(boxMax[1], out.bottomY + std::min(fallbackH * 0.85f, 160.0f));
            out.radius = std::clamp(rawRadius, 22.0f, maxRadius);
            return out;
        }
    }
    out.cx = worldPos[0];
    out.cz = worldPos[2];
    out.bottomY = worldPos[1];
    out.topY = worldPos[1] + fallbackH;
    out.radius = std::clamp(fallbackH * 0.22f, 24.0f, 90.0f);
    return out;
}

inline void SeedLaunchOrigin(float const shooterChest[3], float const curPos[3], float const dir[3], float out[3])
{
    if (!Finite3(curPos)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    out[0] = curPos[0];
    out[1] = curPos[1];
    out[2] = curPos[2];
    if (!Finite3(shooterChest) || !Finite3(dir)) return;
    const float dirLenSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!(dirLenSq > 1e-6f)) return;
    const float invLen = 1.0f / std::sqrt(dirLenSq);
    const float ux = dir[0] * invLen, uy = dir[1] * invLen, uz = dir[2] * invLen;
    const float relX = curPos[0] - shooterChest[0];
    const float relY = curPos[1] - shooterChest[1];
    const float relZ = curPos[2] - shooterChest[2];
    const float distSq = relX * relX + relY * relY + relZ * relZ;
    if (distSq > 600.0f * 600.0f) return;
    const float along = relX * ux + relY * uy + relZ * uz;
    if (along > 0.0f && along <= 400.0f) {
        out[0] = curPos[0] - ux * along;
        out[1] = curPos[1] - uy * along;
        out[2] = curPos[2] - uz * along;
    }
}

inline bool SweepSegmentCylinder(float const p0[3], float const p1[3], Cylinder const& cyl,
                                 float projectileRadius, float& tHitOut, float hitPointOut[3])
{
    if (!Finite3(p0) || !Finite3(p1) || !(cyl.radius > 0.0f) || !(cyl.topY > cyl.bottomY)) return false;
    const float dx = p1[0] - p0[0];
    const float dy = p1[1] - p0[1];
    const float dz = p1[2] - p0[2];
    const float segLenSq = dx * dx + dy * dy + dz * dz;
    if (!(segLenSq > 0.25f)) return false;

    const float pad = std::max(0.0f, projectileRadius);
    const float r = cyl.radius + pad;
    const float yMin = cyl.bottomY - pad;
    const float yMax = cyl.topY + pad;

    float tEnter = 0.0f;
    float tExit = 1.0f;

    if (std::fabs(dy) < 1e-6f) {
        if (p0[1] < yMin || p0[1] > yMax) return false;
    } else {
        const float invDy = 1.0f / dy;
        float ty0 = (yMin - p0[1]) * invDy;
        float ty1 = (yMax - p0[1]) * invDy;
        if (ty0 > ty1) std::swap(ty0, ty1);
        tEnter = std::max(tEnter, ty0);
        tExit = std::min(tExit, ty1);
        if (tEnter > tExit) return false;
    }

    const float ox = p0[0] - cyl.cx;
    const float oz = p0[2] - cyl.cz;
    const float a = dx * dx + dz * dz;
    const float c = ox * ox + oz * oz - r * r;
    if (a < 1e-6f) {
        if (c > 0.0f) return false;
    } else {
        const float b = ox * dx + oz * dz;
        const float disc = b * b - a * c;
        if (disc < 0.0f) return false;
        const float root = std::sqrt(disc);
        const float invA = 1.0f / a;
        const float txz0 = (-b - root) * invA;
        const float txz1 = (-b + root) * invA;
        tEnter = std::max(tEnter, txz0);
        tExit = std::min(tExit, txz1);
        if (tEnter > tExit) return false;
    }

    tHitOut = std::clamp(tEnter, 0.0f, 1.0f);
    if (hitPointOut) {
        hitPointOut[0] = p0[0] + dx * tHitOut;
        hitPointOut[1] = p0[1] + dy * tHitOut;
        hitPointOut[2] = p0[2] + dz * tHitOut;
    }
    return true;
}

} // namespace g3t::shot_sweep
