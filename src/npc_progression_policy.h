// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free encoding, merge, and authorship rules for cross-town NPC progression state.
#pragma once

#include <cstdint>
#include <cstring>

namespace g3t::npc_progression_policy {

constexpr uint8_t DialogTeach = 0x01;
constexpr uint8_t DialogTrade = 0x02;
constexpr uint8_t DialogParty = 0x04;
constexpr uint8_t DialogMob = 0x08;
constexpr uint8_t DialogSlavery = 0x10;
constexpr uint8_t DialogTalkedToPlayer = 0x20;
constexpr uint8_t DialogPresent = 0x80;

constexpr uint8_t NpcPresent = 0x01;
constexpr uint8_t NpcHasDefeated = 0x02;
constexpr uint8_t NpcDefeatedByPlayer = 0x04;

enum class FriendMemoryFlag : uint8_t {
    None = 0,
    Diego = 1,
    Gorn = 2,
    Milten = 3,
    Lester = 4,
};

inline constexpr uint8_t PackDialog(bool teach, bool trade, bool party, bool mob, bool slavery,
                                    bool talkedToPlayer) noexcept
{
    return static_cast<uint8_t>(
        DialogPresent |
        (teach ? DialogTeach : 0) |
        (trade ? DialogTrade : 0) |
        (party ? DialogParty : 0) |
        (mob ? DialogMob : 0) |
        (slavery ? DialogSlavery : 0) |
        (talkedToPlayer ? DialogTalkedToPlayer : 0));
}

inline constexpr bool HasDialog(uint8_t dialog) noexcept
{
    return (dialog & DialogPresent) != 0;
}

inline constexpr bool UnpackTalkedToPlayer(uint8_t dialog) noexcept
{
    return HasDialog(dialog) && (dialog & DialogTalkedToPlayer) != 0;
}

inline constexpr uint8_t MergeHostDialog(uint8_t incomingDialog, bool hostTalkedToPlayer) noexcept
{
    if (!HasDialog(incomingDialog)) return incomingDialog;
    return hostTalkedToPlayer ? static_cast<uint8_t>(incomingDialog | DialogTalkedToPlayer) : incomingDialog;
}

inline constexpr uint8_t PackNpcFlags(bool hasNpc, bool defeatedByPlayer) noexcept
{
    if (!hasNpc) return 0;
    return static_cast<uint8_t>(NpcPresent | NpcHasDefeated | (defeatedByPlayer ? NpcDefeatedByPlayer : 0));
}

inline constexpr bool HasNpcFlags(uint8_t npcFlags) noexcept
{
    return (npcFlags & NpcPresent) != 0;
}

inline constexpr bool HasDefeatedFlag(uint8_t npcFlags) noexcept
{
    return (npcFlags & (NpcPresent | NpcHasDefeated)) == (NpcPresent | NpcHasDefeated);
}

inline constexpr bool UnpackDefeatedByPlayer(uint8_t npcFlags) noexcept
{
    return HasDefeatedFlag(npcFlags) && (npcFlags & NpcDefeatedByPlayer) != 0;
}

inline constexpr uint8_t MergeHostNpcFlags(uint8_t incomingNpcFlags, bool hostDefeatedByPlayer) noexcept
{
    if (!HasNpcFlags(incomingNpcFlags)) return incomingNpcFlags;
    uint8_t merged = static_cast<uint8_t>(incomingNpcFlags | NpcHasDefeated);
    if (hostDefeatedByPlayer) merged = static_cast<uint8_t>(merged | NpcDefeatedByPlayer);
    return merged;
}

inline FriendMemoryFlag FriendMemoryForName(char const* entityName) noexcept
{
    if (!entityName || !*entityName) return FriendMemoryFlag::None;
    if (std::strcmp(entityName, "Diego") == 0) return FriendMemoryFlag::Diego;
    if (std::strcmp(entityName, "Gorn") == 0) return FriendMemoryFlag::Gorn;
    if (std::strcmp(entityName, "Milten") == 0) return FriendMemoryFlag::Milten;
    if (std::strcmp(entityName, "Lester") == 0) return FriendMemoryFlag::Lester;
    return FriendMemoryFlag::None;
}

struct ObservedProgression {
    uint8_t dialog = 0;
    uint8_t guard = 0;
    uint8_t npcClass = 0;
    uint8_t alignment = 0;
    uint8_t immortal = 0;
    uint8_t npc = 0;

    constexpr bool operator==(ObservedProgression const& other) const noexcept
    {
        return dialog == other.dialog &&
               guard == other.guard &&
               npcClass == other.npcClass &&
               alignment == other.alignment &&
               immortal == other.immortal &&
               npc == other.npc;
    }

    constexpr bool operator!=(ObservedProgression const& other) const noexcept
    {
        return !(*this == other);
    }
};

inline constexpr bool IsNonDefaultSeedCandidate(bool talkedToPlayer, bool defeatedByPlayer) noexcept
{
    return talkedToPlayer || defeatedByPlayer;
}

inline constexpr bool ShouldAuthorProgressionDiff(bool isHost, bool isHero, bool isPuppet, bool suppressed,
                                                  bool clientLocalAuthority, bool clientDialogPartner) noexcept
{
    if (isHero || isPuppet || suppressed) return false;
    if (isHost) return true;
    return clientLocalAuthority || clientDialogPartner;
}

} // namespace g3t::npc_progression_policy
