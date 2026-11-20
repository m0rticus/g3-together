// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free CP 1.75 HUD entity-slot layout contract and scrubbing policy.
// Native gCHUDDialogMain2 caches raw eCEntity* slots[0..6] across page opens without
// clearing them on ClosePage/CloseDialog, and SetActivePage pushes all 7 slots to the
// newly opened page BEFORE BeginLoot/BeginTrade/OpenAlchemy overwrite slots[2]/[6].
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::hud::entities {

constexpr uintptr_t ClearAllEntitiesWrapperRva = 0x7c260;
constexpr uintptr_t DialogClearAllEntitiesRva = 0x8b620;
constexpr uintptr_t SetActivePageRva = 0x8b340;

constexpr size_t ManagerRootOffset = 0x04;
constexpr size_t RootSlotsOffset = 0x10;
constexpr size_t RootPagesOffset = 0x38;
constexpr size_t RootHudOffset = 0x7c;
constexpr size_t RootActivePageOffset = 0x84;
constexpr size_t SetEntityVtableOffset = 0x118;

constexpr int EntitySlotCount = 7;
constexpr int PageSlotCount = 15;

constexpr int PlayerSlot = 0;
constexpr int FocusSlot = 1; // Updated each frame by gCGUIManager::OnProcess for the HUD target HP bar.
constexpr int FirstTransientPageSlot = 2;

// ?ClearAllEntities@gCGUIManager@@QAE_NXZ at Game.dll + 0x7c260 (17 bytes).
constexpr uint8_t ClearAllEntitiesWrapperBytes[] = {
    0x57, 0x8b, 0x79, 0x04, 0x85, 0xff, 0x74, 0x09,
    0xe8, 0xb3, 0xf3, 0x00, 0x00, 0xb0, 0x01, 0x5f, 0xc3
};

// gCHUDDialogMain2::ClearAllEntities at Game.dll + 0x8b620 (87 bytes).
constexpr uint8_t DialogClearAllEntitiesBytes[] = {
    0x53, 0x56, 0x33, 0xf6, 0x8d, 0x5f, 0x10, 0xeb, 0x07,
    0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00,
    0xc7, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x83, 0x7f, 0x7c, 0x00, 0x74, 0x10,
    0x8b, 0x4f, 0x7c, 0x8b, 0x01, 0x8b, 0x90, 0x18, 0x01, 0x00, 0x00,
    0x56, 0x6a, 0x00, 0xff, 0xd2,
    0x8b, 0x87, 0x84, 0x00, 0x00, 0x00,
    0x83, 0xf8, 0xff, 0x74, 0x15,
    0x8b, 0x4c, 0x87, 0x38, 0x85, 0xc9, 0x74, 0x0d,
    0x8b, 0x01, 0x8b, 0x90, 0x18, 0x01, 0x00, 0x00,
    0x56, 0x6a, 0x00, 0xff, 0xd2,
    0x83, 0xc6, 0x01, 0x83, 0xc3, 0x04, 0x83, 0xfe, 0x07, 0x7c, 0xb9,
    0x5e, 0x5b, 0xc3
};

// Prefix of gCHUDDialogMain2::SetActivePage at Game.dll + 0x8b340 (31 bytes) proving
// activePage at +0x84, 15 page slots (cmp edi, 0x0f), and hud pointer at +0x7c.
constexpr uint8_t SetActivePagePrefixBytes[] = {
    0x51, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x10, 0x56,
    0x57, 0x8b, 0x7c, 0x24, 0x1c, 0x3b, 0xbd, 0x84,
    0x00, 0x00, 0x00, 0x0f, 0x84, 0x6c, 0x01, 0x00,
    0x00, 0x83, 0xff, 0x0f, 0x8b, 0x4d, 0x7c
};

inline bool VerifyHudEntityLayout(uint8_t const* gameBase, uintptr_t clearWrapperVa) noexcept
{
    if (!gameBase) return false;
    if (clearWrapperVa != reinterpret_cast<uintptr_t>(gameBase + ClearAllEntitiesWrapperRva)) return false;
    if (std::memcmp(gameBase + ClearAllEntitiesWrapperRva, ClearAllEntitiesWrapperBytes,
                    sizeof(ClearAllEntitiesWrapperBytes)) != 0)
        return false;
    if (std::memcmp(gameBase + DialogClearAllEntitiesRva, DialogClearAllEntitiesBytes,
                    sizeof(DialogClearAllEntitiesBytes)) != 0)
        return false;
    if (std::memcmp(gameBase + SetActivePageRva, SetActivePagePrefixBytes,
                    sizeof(SetActivePagePrefixBytes)) != 0)
        return false;
    return true;
}

constexpr bool ValidActivePage(int32_t activePage) noexcept
{
    return activePage >= 0 && activePage < PageSlotCount;
}

constexpr bool ShouldScrubSlotOnDestroy(void const* slotEntity, void const* destroyedEntity) noexcept
{
    return destroyedEntity != nullptr && slotEntity == destroyedEntity;
}

constexpr bool ShouldScrubIdleSlot(int slot, int32_t activePage) noexcept
{
    return activePage < 0 && slot >= FirstTransientPageSlot && slot < EntitySlotCount;
}

} // namespace g3t::hud::entities
