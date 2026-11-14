#include "../src/combat_claims.h"

#include <cassert>
#include <cstdint>

int main()
{
    using g3t::combat::killconfirm::Action;
    using g3t::combat::killconfirm::DeathTaskExpiryMs;
    using g3t::combat::killconfirm::Decide;
    using g3t::combat::killconfirm::UndoDelayMs;

    const uint32_t t0 = 50000;
    const uint32_t undoDue = t0 + UndoDelayMs;
    const uint32_t expires = t0 + DeathTaskExpiryMs;

    // 1. Next-frame confirmed death (t0 + 16 ms) rewards immediately without waiting 2,000 ms.
    assert(Decide(t0 + 16, undoDue, expires, true, true, true, false, 2, true) == Action::Reward);

    // 2. Confirmed death with ineligible/disconnected claimant or no claimant drops without rewarding.
    assert(Decide(t0 + 16, undoDue, expires, true, true, true, false, 2, false) == Action::Drop);
    assert(Decide(t0 + 16, undoDue, expires, true, true, true, false, 0, false) == Action::Drop);

    // 3. Stale incarnation drops immediately on any tick.
    assert(Decide(t0 + 16, undoDue, expires, false, true, true, false, 2, true) == Action::Drop);

    // 4. Before UndoDelayMs while ZS_RagDollDead has not yet set AIMode_Dead, keep checking every frame.
    assert(Decide(t0 + 16, undoDue, expires, true, true, false, true, 2, true) == Action::Wait);
    assert(Decide(t0 + 1999, undoDue, expires, true, true, false, false, 2, true) == Action::Wait);

    // 5. If OnCheatKill did not take (!dead && !deathTask) after UndoDelayMs, undo without paying XP.
    assert(Decide(undoDue, undoDue, expires, true, true, false, false, 2, true) == Action::UndoAndDrop);

    // 6. If still in death task past UndoDelayMs, keep checking per-frame up to DeathTaskExpiryMs,
    //    rewarding on the exact frame IsDead() commits.
    assert(Decide(undoDue + 10, undoDue, expires, true, true, false, true, 2, true) == Action::Wait);
    assert(Decide(undoDue + 26, undoDue, expires, true, true, true, false, 2, true) == Action::Reward);
    assert(Decide(expires, undoDue, expires, true, true, false, true, 2, true) == Action::Drop);

    return 0;
}
