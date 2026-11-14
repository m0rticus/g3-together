// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g3t::focus_diag {
struct Header {
    bool lost = false;
    bool regained = false;
    bool resumed = false;
    bool longGap = false;
    bool sampleDue = false;
    bool watching = false;
    uint32_t gap = 0;
    uint32_t awayMs = 0;
    unsigned serial = 0;
};

struct NpcSample {
    uintptr_t instance = 0;
    int weapon = 0;
    int ai = 0;
    uint16_t fraction = UINT16_MAX;
    char const* guid = "";
    char const* motion = "";
};

struct NpcChange {
    bool logDetail = false;
    int prevWeapon = 0;
    int prevAi = 0;
    bool draw = false;
};

struct SettleReport {
    unsigned serial = 0;
    uint32_t windowMs = 0;
    size_t sampledNpcs = 0;
    size_t weaponChanged = 0;
    size_t aiChanged = 0;
    size_t drawMotion = 0;
    unsigned actorReplacements = 0;
};

void Reset();
Header AdvanceFrame(uint32_t now, bool focused, bool paused, bool inWorld);
void BeginSample(uint32_t now);
NpcChange RecordNpc(NpcSample const& sample);
bool FinishSample(uint32_t now, SettleReport& out);
unsigned CountResourceReset();
unsigned CountResourceRestore();
}
