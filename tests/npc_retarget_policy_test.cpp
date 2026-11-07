// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npc_retarget_policy.h"

#include <cassert>
#include <limits>

using namespace g3t;

int main()
{
    using namespace retarget;
    using namespace downed;
    for (unsigned flags = 0; flags < 16; ++flags)
        for (int mode = 0; mode < 16; ++mode) {
            const bool npc = flags & 1, self = flags & 2, unavailable = flags & 4;
            assert(EligibleTarget(npc, self, unavailable, mode) ==
                   (npc && !self && !unavailable && mode != 6 && mode != 8 && mode != 9));
        }
    for (int attitude = -1; attitude < 10; ++attitude)
        assert(EnemyAttitude(attitude) == (attitude == 4 || attitude == 5));
    assert(VisibleFallback(0) && VisibleFallback(1999.9f));
    assert(!VisibleFallback(-1) && !VisibleFallback(2000) && !VisibleFallback(2500));
    assert(!VisibleFallback(std::numeric_limits<float>::infinity()));
    assert(!VisibleFallback(std::numeric_limits<float>::quiet_NaN()));

    PlayerCandidate players[] = {{true, 100}, {true, 200}, {true, 50}};
    for (unsigned bindings = 0; bindings < 128; ++bindings) {
        for (unsigned candidates = 0; candidates < 8; ++candidates) {
            const bool current = candidates & 1, native = candidates & 2, alternative = candidates & 4;
            auto selected = Select(bindings, current, native, alternative, players, 3);
            const auto expected = !StopTargetInstruction(bindings) ? Source::Unchanged :
                !(bindings & Current) && current ? Source::Current :
                native ? Source::Native : alternative ? Source::Alternative : Source::Player;
            assert(selected.source == expected);
            if (expected == Source::Player) assert(selected.player == 2);
        }
    }
    // A stale spell never takes an NPC away from its still-valid current opponent.
    assert(Select(Spell, true, true, true, players, 3).source == Source::Current);
    assert(Select(Current, true, false, false, nullptr, 0).source == Source::None);
    // Vanilla's choice wins; a friendly/defeated alternative cannot be an automatic replacement.
    assert(Select(Current, false, true, true, players, 3).source == Source::Native);
    assert(Select(Current, false, false, true, players, 3).source == Source::Alternative);
    players[2].eligible = false;
    assert(Select(Current, false, false, false, players, 3).player == 0);
    players[1].distanceCm = 100;
    assert(Select(Current, false, false, false, players, 3).player == 0); // stable equal-distance tie
    players[0].distanceCm = std::numeric_limits<float>::quiet_NaN();
    assert(Select(Current, false, false, false, players, 3).player == 1);
    players[1].distanceCm = 2000;
    assert(Select(Current, false, false, false, players, 3).source == Source::None);

    Life life;
    for (unsigned episode = 0; episode < 3; ++episode) {
        life.Hold();
        const auto heldEpisode = life.sequence;
        for (unsigned npc = 0; npc < 12; ++npc) {
            const bool unavailable = CombatUnavailable(true, life.held, life.terminal, false, false, 0);
            PlayerCandidate helpers[] = {{!unavailable, 100}, {true, 300}};
            const auto next = Select(Current | Alternative | Alignment, false, false, false, helpers, 2);
            assert(next.source == Source::Player && next.player == 1);
            // Every NPC loses the downed binding; the next scan is historical-only/no-op for this fight.
            assert(Select(0, true, false, false, helpers, 2).source == Source::Unchanged);
            assert(Select(LastAttacker | Inflictor, true, false, false, helpers, 2).source == Source::Unchanged);
        }
        assert(!life.Revive(heldEpisode + 1));
        assert(life.Revive(heldEpisode));
        PlayerCandidate revived[] = {{!CombatUnavailable(true, life.held, life.terminal, false, false, 0), 100}};
        assert(Select(Current, false, false, false, revived, 1).source == Source::Player);
        assert(!life.Revive(heldEpisode)); // duplicate revival cannot change the episode
    }
    life.Hold();
    life.End();
    assert(!life.Revive(life.sequence));
    assert(!EligibleTarget(true, false, CombatUnavailable(true, life.held, life.terminal, false, false, 0), 10));
}
