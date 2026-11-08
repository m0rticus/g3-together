// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "options_policy.h"

namespace g3t {
struct Config;
namespace options {
// Getter overlays leave native setup and all config files untouched.
bool Install();
policy::Snapshot Capture();
bool Adopt(policy::Snapshot const& host, uint32_t epoch);
bool WorldAnnounced(); // queue pre-existing local worlds through the existing safe re-entry path if needed
void Restore();
void ApplyMod(Config& config);
wchar_t const* Hint();
// Claims judged against a host-presented NPC use that NPC owner's difficulty;
// player victims and locally simulated NPCs keep this machine's difficulty.
struct NpcJudgement {
    explicit NpcJudgement(bool hostPresented);
    ~NpcJudgement();
    NpcJudgement(NpcJudgement const&) = delete;
    NpcJudgement& operator=(NpcJudgement const&) = delete;
    bool entered;
};
}
}
