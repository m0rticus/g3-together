// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_policy.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace g3t::hero::codec {

inline constexpr int QuickSlots = 16;
inline constexpr size_t MaxRecordBytes = 4 * 1024 * 1024;
inline constexpr size_t MaxRecordEntries = 100000;

struct Stack {
    std::string id;
    int amount = 0;
    unsigned quality = 0;
    int slot = 0;
    int category = 0;
    int learned = -1;
};

struct Quick {
    int slot = 0;
    std::string id;
    unsigned quality = 0;
};

struct Schema {
    int minSlot = 0;
    int maxSlot = 8;
    int categoryCount = 16;
    int skillCategory = 10;
    int spellbookCategory = 9;
};

struct ExtraSection {
    uint32_t type = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> bytes;
};

struct ParsedRecord {
    saving::Key key;
    saving::RecordPose pose;
    int downed = 0;
    unsigned level = 0;
    unsigned levelMax = 0;
    int xp = 0;
    int lpAttribs = 0;
    int lpPerks = 0;
    std::vector<saving::RecordAttribute> attrs;
    std::vector<Stack> stacks;
    std::vector<std::string> spells;
    std::vector<Quick> quicks;
    uint32_t schemaVersion = saving::HeroSchemaVersion;
    uint32_t minReaderVersion = saving::HeroMinReaderVersion;
    std::vector<ExtraSection> extraSections;
};

char const* const* AttributeTags(size_t& count);
char const* ValidateLogicalContents(std::vector<Stack> const& stacks, std::vector<std::string> const& spells,
                                    std::vector<Quick> const& quicks, Schema const& schema);
char const* EncodeRecord(ParsedRecord const& record, Schema const& schema, std::vector<uint8_t>& out);
char const* ParseBoundRecord(char const* data, size_t size, saving::Key const& expected,
                             Schema const& schema, ParsedRecord& out);
bool SameStacks(std::vector<Stack> const& a, std::vector<Stack> const& b);
std::vector<size_t> RestoreStackOrder(std::vector<Stack> const& stacks, int skillCategory, int noneSlot = 0);

// gCInventoryStackList::CreateItems merges into existing unlinked (or non-splitting slot) stacks with the same
// template and quality; split the newly added amount back out before equipping so duplicate stacks survive intact.
template <typename Ops>
char const* RestoreItemStack(Stack const& s, int noneSlot, Ops&& ops)
{
    const int before = ops.StackCount();
    int index = ops.CreateItems(s);
    if (index < 0) return "could not create an item";
    if (index < before && ops.GetAmount(index) > s.amount) {
        index = ops.SplitStack(index, s.amount);
        if (index < 0) return "could not split a merged item stack";
    }
    if (s.slot != noneSlot && !ops.EquipStackToSlot(index, s.slot)) return "could not equip an item";
    return nullptr;
}

} // namespace g3t::hero::codec
