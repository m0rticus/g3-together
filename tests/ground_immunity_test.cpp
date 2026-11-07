// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/downed_policy.h"
#include <cassert>

using namespace g3t::downed;

int main()
{
    for (unsigned flags = 0; flags < 256; ++flags)
        for (unsigned state = 0; state < 8; ++state) {
            const bool held = state & 1, terminal = state & 2, recovering = state & 4;
            assert(!HarmImmune(false, held, terminal, flags, recovering));
            assert(HarmImmune(true, held, terminal, flags, recovering) ==
                   (!terminal && !(flags & 4) && (held || (flags & 8) || recovering)));
        }
    assert(!HarmImmune(true, false, false, 2, false)); // ordinary unconscious state is not co-op recovery
    assert(HarmImmune(true, true, false, 0, false)); // immediate: no native pose or packet required
    assert(!HarmImmune(true, true, true, 0, true)); // terminal Kill wins over every stale immunity input
    for (int pose = -1; pose <= 62; ++pose)
        assert(VanillaProne(pose) == (pose == 15 || pose == 16));

    Recovery recovery;
    assert(!recovery.Protected(0));
    recovery.Begin();
    assert(recovery.Protected(0) && recovery.Protected(1000000));
    recovery.Observe(false, 1000000); // delay through the whole native rising animation, not a revive-message timer
    assert(recovery.rising && recovery.Protected(1000001));
    recovery.Observe(true, 1000001);
    assert(!recovery.rising && recovery.grace && recovery.stoodAt == 1000001);
    assert(recovery.Protected(1000001 + ReviveGraceMs - 1));
    assert(!recovery.Protected(1000001 + ReviveGraceMs));
    recovery.Observe(false, 1000001 + ReviveGraceMs);
    assert(!recovery.grace);

    recovery.Begin();
    recovery.Observe(true, UINT32_MAX - 100);
    assert(recovery.Protected((UINT32_MAX - 100) + ReviveGraceMs - 1));
    assert(!recovery.Protected((UINT32_MAX - 100) + ReviveGraceMs));
    recovery.Begin();
    assert(recovery.rising && !recovery.grace); // new paid life cannot reuse the previous grace
    recovery.Clear();
    assert(!recovery.rising && !recovery.grace && !recovery.Protected(0));

    for (int pose = -1; pose <= 62; ++pose) {
        assert(RecoveryStanding(pose, false, "Hum_Walk") == (pose >= 2 && pose <= 4));
        assert(!RecoveryStanding(pose, true, "Hum_Walk"));
    }
    for (char const* motion : {"", "Hum_GetUp_00", "Hum_StandUp_00"})
        assert(!RecoveryStanding(2, false, motion));
    assert(!RecoveryStanding(2, false, nullptr));

    PeerRecovery peer;
    peer.Observe(true, 1, true, 0);
    assert(!peer.recovery.Protected(0)); // joining alive does not begin a revive episode
    peer.Observe(true, 9, false, 10);
    assert(peer.held && !peer.recovery.Protected(10));
    peer.Observe(true, 3, true, 20);
    assert(!peer.held && peer.recovery.rising); // owner remains native Down while starting recovery
    peer.Observe(true, 1, false, 1000000);
    assert(peer.recovery.rising);
    peer.Observe(true, 1, true, 1000001);
    assert(peer.recovery.grace && peer.recovery.Protected(1000001 + ReviveGraceMs - 1));
    assert(!peer.recovery.Protected(1000001 + ReviveGraceMs));
    for (unsigned flags = 0; flags < 256; ++flags) {
        peer.Observe(true, 9, false, 10);
        peer.Observe(true, flags, true, 20);
        assert(peer.held == ((flags & 1) && !(flags & 4) && (flags & 8)));
        assert(peer.recovery.Protected(20) == ((flags & 1) && !(flags & 4) && !(flags & 8)));
    }
    peer.Observe(false, 9, false, 30);
    assert(!peer.held && !peer.recovery.Protected(30)); // disconnect or another world cancels the old episode
    peer.Observe(true, 9, false, 40);
    peer.Clear();
    assert(!peer.held && !peer.recovery.Protected(40));

    Life life;
    life.Hold();
    assert(!life.Revive(life.sequence + 1) && life.held);
    assert(life.Revive(life.sequence));
    recovery.Begin();
    assert(HarmImmune(true, life.held, life.terminal, 0, recovery.Protected(0)));
    life.End();
    assert(!HarmImmune(true, life.held, life.terminal, 0, recovery.Protected(0)));

    assert(HarmfulReaction("ZS_Stumble"));
    assert(HarmfulReaction("ZS_ParadeStumble_Loop"));
    assert(HarmfulReaction("ZS_Freeze"));
    assert(HarmfulReaction("ZS_RagDollDead"));
    for (char const* safe : {"", "PS_Normal", "ZS_Talk", "ZS_Unconscious_Loop", "_AI_StandUp",
                             "ZS_SitKnockDown_GetUpAttack", "ZS_SitKnockDown_GetUpParade", "_AI_GetUpAttack"})
        assert(!HarmfulReaction(safe));
}
