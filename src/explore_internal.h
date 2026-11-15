// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "guid.h"

#include <Game.h>
#include <Script.h>
#include <cstdio>
#include <string>

namespace g3t::explore::detail {
inline bool ParseXyz(char const* text, float out[3])
{
    return text && sscanf_s(text, "%f_%f_%f", &out[0], &out[1], &out[2]) == 3;
}

inline void PositionOf(eCEntity* entity, float out[3])
{
    const bCVector& p = entity->GetWorldPosition();
    out[0] = p.GetX();
    out[1] = p.GetY();
    out[2] = p.GetZ();
}

inline Entity ResolveEntity(char const* nameOrGuid)
{
    if (!nameOrGuid || !*nameOrGuid) return Entity();
    Entity found = Entity::GetEntity(bCString(nameOrGuid));
    if (found.GetInstance()) return found;
    _GUID raw{};
    return guid::ParseExact(nameOrGuid, raw) ? guid::Resolve(raw) : Entity();
}

inline std::string Join(bTObjArray<bCString> const& list, char const* sep)
{
    std::string out;
    for (int i = 0; i < list.GetCount(); ++i) out += std::string(out.empty() ? "" : sep) + list.GetAt(i).GetText();
    return out.empty() ? "-" : out;
}
}
