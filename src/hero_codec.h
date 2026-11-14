// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "saving_policy.h"

#include <cstddef>
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
};

char const* const* AttributeTags(size_t& count);
char const* ValidateLogicalContents(std::vector<Stack> const& stacks, std::vector<std::string> const& spells,
                                    std::vector<Quick> const& quicks, Schema const& schema);
char const* ParseBoundRecord(char const* data, size_t size, saving::Key const& expected,
                             Schema const& schema, ParsedRecord& out);
bool SameStacks(std::vector<Stack> const& a, std::vector<Stack> const& b);

} // namespace g3t::hero::codec
