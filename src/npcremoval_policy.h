// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace g3t::npcremoval::policy {
constexpr char MainWorld[] = "G3_World_01";
enum : uint8_t { Goblin = 1, Ogre = 2, Defaults = Goblin | Ogre };

inline std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

inline bool ParseNames(std::string_view text, uint8_t& names)
{
    text = Trim(text);
    if (text.empty()) { names = 0; return true; }
    uint8_t parsed = 0;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto name = Trim(text.substr(0, comma));
        const uint8_t bit = name == "Goblin" ? Goblin : name == "Ogre" ? Ogre : 0;
        if (!bit || (parsed & bit)) return false;
        parsed |= bit;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (Trim(text).empty()) return false;
    }
    names = parsed;
    return true;
}

constexpr uint8_t Hex(char value)
{
    return static_cast<uint8_t>(value <= '9' ? value - '0' : value - 'A' + 10);
}

constexpr std::array<uint8_t, 16> Id(char const (&text)[37])
{
    constexpr int offsets[] = {6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34};
    std::array<uint8_t, 16> bytes{};
    for (int i = 0; i != 16; ++i)
        bytes[i] = static_cast<uint8_t>((Hex(text[offsets[i]]) << 4) | Hex(text[offsets[i] + 1]));
    return bytes;
}

struct Entry { std::array<uint8_t, 16> guid; char const* name; uint8_t bit; };
// Reviewed instances in the installed CP world SysDyn layer; never match a species or a quest name alone.
constexpr Entry Entries[] = {
    {Id("37866214-3E06-4164-82BE-C4BCF574C002"), "Goblin", Goblin},
    {Id("B8616AD6-3552-4ADE-967D-B3304438943D"), "Goblin", Goblin},
    {Id("18FB0A54-8C5C-4CF3-8323-08B4EB771152"), "Goblin", Goblin},
    {Id("97DD86CD-1D1F-4CCC-B6A1-653624609FB5"), "Goblin", Goblin},
    {Id("BD287729-7A12-404B-8B0F-8B316E66C705"), "Goblin", Goblin},
    {Id("A8508640-EAC9-4B1C-8A93-E61AB03A42EF"), "Goblin", Goblin},
    {Id("79574179-E848-4302-BE90-34399987F17C"), "Goblin", Goblin},
    {Id("5F5AC7E1-FE52-4FFC-86DE-C1EB5004D13C"), "Goblin", Goblin},
    {Id("DF3583F2-E13F-437E-BD59-6B7761EDB096"), "Goblin", Goblin},
    {Id("5C7D5719-CE86-42E6-9AAD-2240751E25F8"), "Goblin", Goblin},
    {Id("71BD99DF-3CCE-4310-A630-4B559E4777C6"), "Goblin", Goblin},
    {Id("691F682D-5047-4A9A-B6FB-C9AE451BA607"), "Goblin", Goblin},
    {Id("7743736D-F691-4FB8-81B7-24C3300C7E5D"), "Goblin", Goblin},
    {Id("22BB7525-F195-4200-8B02-77C6862D6D0A"), "Goblin", Goblin},
    {Id("25E7233B-15EA-4C2C-8034-2BB5C99EBE48"), "Goblin", Goblin},
    {Id("6C967CA3-DB2C-4F93-9598-955564DEEC95"), "Goblin", Goblin},
    {Id("91352444-00CD-4927-9AF0-0DAA89298B42"), "Goblin", Goblin},
    {Id("02257D28-2D65-4B46-BD86-826947C854C9"), "Goblin", Goblin},
    {Id("6735E387-C9CC-4703-A0B4-CEEAC0153B42"), "Goblin", Goblin},
    {Id("DA7970F5-58DD-4256-9B0A-40476A8A87FA"), "Goblin", Goblin},
    {Id("111B1AE7-9A0C-4990-A125-1D7C4F4C6E9E"), "Goblin", Goblin},
    {Id("B61D9D91-54DD-4F75-B1AE-76970D41D7DE"), "Goblin", Goblin},
    {Id("E17F67E5-0313-4D3B-9BFD-138AC3135944"), "Ogre", Ogre},
    {Id("59055EE0-DB78-4803-8179-49E77D8CD7AB"), "Ogre", Ogre}
};
constexpr size_t Count = sizeof(Entries) / sizeof(Entries[0]);

inline int Index(uint8_t const* guid)
{
    if (guid)
        for (size_t i = 0; i != Count; ++i)
            if (!std::memcmp(Entries[i].guid.data(), guid, 16)) return static_cast<int>(i);
    return -1;
}

inline bool InRegion(float const* position)
{
    return position && position[0] >= 85000 && position[0] <= 100000 &&
        position[1] >= -1500 && position[1] <= 9000 &&
        position[2] >= -20000 && position[2] <= -5000;
}

inline bool Matches(uint8_t names, bool mainWorld, uint8_t const* guid,
                    std::string_view name, float const* position, bool player)
{
    const int index = Index(guid);
    return mainWorld && !player && index >= 0 && (names & Entries[index].bit) &&
        name == Entries[index].name && InRegion(position);
}

struct Fence {
    uint32_t incarnation = 0;
    bool Accept(uint32_t epoch, uint32_t currentEpoch, uint32_t life, uint32_t currentLife)
    {
        if (!epoch || epoch != currentEpoch || !life ||
            (currentLife && static_cast<int32_t>(life - currentLife) < 0) ||
            (incarnation && static_cast<int32_t>(life - incarnation) < 0)) return false;
        incarnation = life;
        return true;
    }
    bool Removed() const { return incarnation != 0; }
};

struct Publication {
    uint32_t epoch = 0, generation = 0, entry = 0;
    bool Needs(uint32_t worldEpoch, uint32_t peerGeneration, uint32_t worldEntry) const
    {
        return epoch != worldEpoch || generation != peerGeneration || entry != worldEntry;
    }
    void Confirm(uint32_t worldEpoch, uint32_t peerGeneration, uint32_t worldEntry)
    {
        epoch = worldEpoch; generation = peerGeneration; entry = worldEntry;
    }
};

inline bool StopTargetInstruction(bool player, bool deadOrDown, bool current, bool spell, bool other)
{
    return !player && !deadOrDown && (current || spell || other);
}
}
