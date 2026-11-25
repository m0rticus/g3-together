// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free back-slot equipment replication and lootability policy for mirrored NPCs and corpses.
#pragma once

#include <cstddef>
#include <cstdint>

namespace g3t::npc_equipment {

inline constexpr size_t BackSlotCount = 4;
// Canonical wire order matching GameEnum.h gESlot values: BackLeft=5, BackRight=6, Bow=7, Crossbow=8.
inline constexpr int BackSlotIndices[BackSlotCount] = {5, 6, 7, 8};

struct BackEquipment {
    uint32_t incarnation = 0;
    uint32_t slots[BackSlotCount] = {};

    bool operator==(BackEquipment const& other) const noexcept
    {
        if (incarnation != other.incarnation) return false;
        for (size_t i = 0; i < BackSlotCount; ++i)
            if (slots[i] != other.slots[i]) return false;
        return true;
    }
    bool operator!=(BackEquipment const& other) const noexcept { return !(*this == other); }
};

inline constexpr bool HasAnyBackSlot(BackEquipment const& value) noexcept
{
    for (uint32_t hash : value.slots)
        if (hash != 0) return true;
    return false;
}

inline constexpr bool ShouldSendBackEquipment(bool haveSent, BackEquipment const& sent,
                                              BackEquipment const& current) noexcept
{
    if (!current.incarnation) return false;
    return !haveSent ? HasAnyBackSlot(current) : sent != current;
}

inline constexpr bool ShouldAcceptBackEquipment(uint32_t currentIncarnation, uint32_t noteIncarnation) noexcept
{
    if (!noteIncarnation) return false;
    if (!currentIncarnation) return true;
    return static_cast<int32_t>(noteIncarnation - currentIncarnation) >= 0;
}

inline constexpr bool ShouldEquipBackSlot(uint32_t wantHash, uint32_t heldRightHash, uint32_t heldLeftHash,
                                          uint32_t currentBackHash, bool currentBackMerchandise) noexcept
{
    if (!wantHash) return false;
    // Never pull a weapon out of an NPC's active hand slot while it is held there.
    if (wantHash == heldRightHash || wantHash == heldLeftHash) return false;
    return currentBackHash != wantHash || currentBackMerchandise;
}

inline constexpr bool ShouldUnlinkDeadHandSlot(bool killed, uint32_t shownHeldHash, uint32_t localHeldHash) noexcept
{
    return killed && !shownHeldHash && localHeldHash != 0;
}

inline constexpr bool IsMirrorLootStackEligible(bool merchandise, int amount) noexcept
{
    return !merchandise && amount > 0;
}

inline constexpr bool ShouldEnsureCorpseOnState(bool local, bool isChest, bool isCorpse) noexcept
{
    return local && (isChest || isCorpse);
}

} // namespace g3t::npc_equipment
