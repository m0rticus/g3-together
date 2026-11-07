// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace g3t::npclease {
constexpr uint32_t PresentationGraceMs = 350;
constexpr uint32_t PresentationJitterMs = 100;

inline bool PresentationFresh(bool connected, bool released, uint32_t age, uint32_t interval)
{
    return connected && !released &&
        age <= std::max(PresentationGraceMs, interval * 4 + PresentationJitterMs);
}

enum class PoseProvenance { None, Canonical, Idle };
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
