// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g3t::npclease {
// Label only (diagnostics, creature-vs-human statistics). It no longer gates ownership: every class that
// npcown::Exclusion accepts may live on the client (see npcown_policy.h).
enum class NativeClass { Unsupported, Creature, Ordinary };
}
