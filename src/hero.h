// SPDX-License-Identifier: GPL-3.0-or-later
// Bound v4 hero record codec and engine state snapshot/restore: level, experience, learn points,
// attributes, learned skills/spells, inventory with quality/equipment/quick slots, downed state, and pose.
#pragma once

#include "saving_policy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace g3t::hero {
bool SaveRecord(char const* path, saving::Key const& key);
bool LoadRecord(char const* path, saving::Key const& key);
bool ValidateRecord(char const* path, saving::Key const& key);
bool LoadRecordFromMemory(uint8_t const* data, size_t size, saving::Key const& key);
bool ValidateRecordMemory(uint8_t const* data, size_t size, saving::Key const& key);
bool LoadRecordFromMemory(std::vector<uint8_t> const& bytes, saving::Key const& key);
bool ValidateRecordMemory(std::vector<uint8_t> const& bytes, saving::Key const& key);
char const* LastRefusal();
}
