// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free quality policy for mirrored NPC hand-held items spawned on clients.
// Matches CP 1.75 Script_Game.dll GenerateWeaponryFromTreasureSet (0x100ac1c0):
// non-ammo NPC weapons receive gEItemQuality_Worn (256), while ammo and non-weapons preserve quality 0.
#pragma once

#include <cstdint>

namespace g3t::npc_item_quality {

inline constexpr uint32_t QualityMask = 1023u;
inline constexpr uint32_t QualityWorn = 256u; // gEItemQuality_Worn

inline constexpr uint16_t MaskQuality(uint32_t quality) noexcept
{
    return static_cast<uint16_t>(quality & QualityMask);
}

inline constexpr int CategoryWeapon = 1;    // gEItemCategory_Weapon
inline constexpr int UseArrow = 4;          // gEUseType_Arrow
inline constexpr int UseBolt = 7;           // gEUseType_Bolt
inline constexpr int UseFist = 8;           // gEUseType_Fist
inline constexpr int UseShield = 9;         // gEUseType_Shield
inline constexpr int UsePhysicalFist = 55;  // gEUseType_PhysicalFist

inline constexpr bool ShouldMarkMirroredWeaponWorn(int category, int useType) noexcept
{
    if (useType == UseArrow || useType == UseBolt || useType == UseFist || useType == UsePhysicalFist)
        return false;
    return category == CategoryWeapon || useType == UseShield;
}

inline constexpr uint32_t MirroredSpawnQuality(int category, int useType, uint32_t templateQuality) noexcept
{
    const uint32_t base = templateQuality & QualityMask;
    return ShouldMarkMirroredWeaponWorn(category, useType) ? (base | QualityWorn) : base;
}

} // namespace g3t::npc_item_quality
