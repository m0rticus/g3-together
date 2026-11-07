// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease.h"
#include <cstdio>
#include <string>

namespace g3t::npclease {
inline constexpr char SavePrefix[] = "npclease1 ";
inline constexpr size_t MaxOverlayText = 2 * (20 + MaxLeases * detail::SavedWireSize) + sizeof(SavePrefix);

// Compatibility entry points only: client-owned NPC state is no longer durable.
inline bool EncodeSaveText(SaveOverlay const&, std::string& output)
{
    output.clear();
    return true;
}

inline bool DecodeSaveText(std::string const&, SaveOverlay& output)
{
    output.clear();
    return true;
}

inline bool ReadSaveStream(FILE* file, SaveOverlay& output)
{
    output.clear();
    if (!file) return false;
    char chunk[512];
    while (fread(chunk, 1, sizeof(chunk), file)) {}
    return !ferror(file);
}
}
