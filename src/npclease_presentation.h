// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace g3t::npclease {
constexpr uint32_t PresentationGraceMs = 350;
constexpr uint32_t PresentationJitterMs = 100;

inline bool PresentationFresh(bool connected, bool released, uint32_t age, uint32_t interval)
{
    return connected && !released &&
        age <= std::max(PresentationGraceMs, interval * 4 + PresentationJitterMs);
}

inline bool CharacterPresentation(char const* name, bool npc, bool routine, bool movement, bool damage)
{
    return name && std::strncmp(name, "G3_Object_", 10) != 0 && npc && routine && movement && damage;
}

inline float IdleTime(uint32_t elapsed, float duration)
{
    return duration > 0.0f && std::isfinite(duration)
        ? std::fmod(duration * 0.25f + elapsed * 0.001f, duration) : 0.0f;
}

inline float ReactionTime(uint32_t elapsed, float duration)
{
    return duration > 0.0f && std::isfinite(duration)
        ? std::min(elapsed * 0.001f, std::nextafter(duration, 0.0f)) : 0.0f;
}

inline bool CosmeticContact(bool client, bool passive, bool hostPresentation, bool localPlayer,
                            bool fenced, bool protectedLife, uint32_t swing)
{
    return client && passive && !hostPresentation && localPlayer && !fenced && !protectedLife && swing != 0;
}

enum class PoseProvenance { None, Canonical, Native, Idle, Reaction };
struct PresentationRig {
    void const* actor = nullptr;
    void const* instance = nullptr;
    void const* nodes = nullptr;
    int count = 0;
    bool facial = false;
    bool operator==(PresentationRig const& other) const
    {
        return actor == other.actor && instance == other.instance && nodes == other.nodes &&
               count == other.count && facial == other.facial;
    }
};
struct PresentationProof {
    PresentationRig rig;
    PoseProvenance provenance = PoseProvenance::None;
    bool Usable(PresentationRig const& current) const
    {
        return provenance != PoseProvenance::None && current.actor && current.instance &&
               current.nodes && current.count > 0 && rig == current;
    }
    void Record(PresentationRig const& current, PoseProvenance source, bool evaluated, bool captured)
    {
        provenance = evaluated && captured ? source : PoseProvenance::None;
        rig = current;
    }
};
}
