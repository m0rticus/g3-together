import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

XPSHARE_JOIN_POLICY_TARGET = _testbuild.register_target(
    "xpshare-join-policy",
    [_testbuild.unit(
        "tests/xpshare_join_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

XPSHARE_SYNC_UNIT_TARGET = _testbuild.register_target(
    "xpshare-sync-unit",
    [_testbuild.unit(
        "tests/test_xpshare_sync_unit.cpp",
        std="c++17",
        warnings="strict",
    )],
)

XPSHARE_ADMISSION_GUARD_TARGET = _testbuild.register_target(
    "xpshare-admission-guard",
    [_testbuild.inline_unit(
        "xpshare_admission_guard",
        r'''
#include "xpshare.h"
#include <cassert>
#include <cstdint>

int main()
{
    using g3t::xpshare::counter::ClientReconnectState;
    using g3t::xpshare::counter::EpochLedger;
    using g3t::xpshare::counter::ShouldSuppressKillGiveXp;

    // 1. Unattributed NPC death GiveXP (SG+0x1c2b8 / SG+0x27839 with other=None) is suppressed.
    assert(ShouldSuppressKillGiveXp(false, false, true));
    assert(!ShouldSuppressKillGiveXp(true, false, true));
    assert(!ShouldSuppressKillGiveXp(false, true, true));
    assert(!ShouldSuppressKillGiveXp(false, false, false));

    // 2. Host ledger captures save baseline and relays other peers' gains without echoing recipient's own.
    EpochLedger<8> ledger;
    ledger.Add(1, 50);
    ledger.Add(2, 40);
    ledger.Add(3, 20);
    ledger.BeginSave();
    ledger.Add(1, 25);  // after save prepare: not in saved baseline
    ledger.CommitSave();
    assert(ledger.saved[1] == 50 && ledger.saved[2] == 40 && ledger.saved[3] == 20);
    ledger.Add(2, 15);
    ledger.Add(3, 30);
    assert(ledger.RelayedSince(2, ledger.saved) == 30);
    assert(ledger.RelayedSince(3, ledger.saved) == 15);

    EpochLedger<8> canceled = ledger;
    canceled.BeginSave();
    canceled.Add(1, 100);
    canceled.CancelSave();
    assert(canceled.saved[1] == 50);

    // 3. Client reconnect guard preserves pre-admission XP after record restore and deduplicates live blips.
    ClientReconnectState restored;
    restored.SetHasSavedOrigin(true);
    assert(restored.FilterHostGain(225) == 225);  // XP earned between capture and admission cutover
    restored.BeginSave();
    assert(restored.FilterHostGain(10) == 10);
    restored.CommitSave();
    assert(restored.savedFromHost == 225 && restored.appliedFromHost == 235);
    restored.OnReconnect();
    assert(restored.FilterHostGain(10) == 0);   // already applied after last save
    assert(restored.FilterHostGain(35) == 35);  // new gain during/after reconnect

    ClientReconnectState fresh;
    fresh.SetHasSavedOrigin(false);
    assert(fresh.FilterHostGain(50) == 50);
    fresh.OnReconnect();
    assert(fresh.FilterHostGain(20) == 20);
    return 0;
}
''',
        includes=[ROOT / "src"],
        std="c++17",
        warnings="strict",
    )],
)


class XPShareJoinPolicyTests(unittest.TestCase):
    def test_join_rejoin_and_rewind_counter_policy_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = XPSHARE_JOIN_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_immediate_sync_pre_ack_and_request_recovery_without_double_pay(self):
        with tempfile.TemporaryDirectory() as folder:
            result = XPSHARE_SYNC_UNIT_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_origins_are_captured_before_first_reward_and_controls_pass_both_gates(self):
        source = _testbuild.src_text("xpshare.cpp")
        start = source.index("void NoteGiveXp(")
        self.assertLess(source.index("PrepareAdmissions(", start), source.index("earned +=", start))
        self.assertIn("peer.outgoing.Begin(stream, earned)", source)
        self.assertIn("peer.outgoing.Total(earned)", source)
        self.assertIn("peer.Snapshot(theirNonce, theirEarned)", source)
        on_state = source.index("void OnState(")
        self.assertLess(
            source.index("SyncPeerGeneration(from)", on_state),
            source.index("peer.Snapshot(theirNonce, theirEarned)", on_state),
        )
        send_baseline = source.index("bool SendBaselineTo(")
        send_ack = source.index("bool SendAckTo(", send_baseline)
        baseline_body = source[send_baseline:send_ack]
        self.assertLess(
            baseline_body.index("if (!net::SendWorldStateTo(to, s)) return false;"),
            baseline_body.index("MarkBaselineSent("),
        )
        send_live = source.index("void SendLive(")
        prepare = source.index("void PrepareAdmissions(", send_live)
        self.assertIn("net::Flush();", source[send_live:prepare])
        net = _testbuild.src_text("net.cpp")
        self.assertIn("!XpHandshake(data, size)", net)
        self.assertIn("!Open(peer)) { ++gatedOut; return false; }", net)
        self.assertIn("XpHandshake(packet->data, packet->dataLength)", net)
        self.assertIn("peers[from] != peer || message.state.npcClass != from", net)
        add_start = source.index("bool Add(int amount)")
        gain_start = source.index("void Gain(int amount)", add_start)
        self.assertIn("guards::LocalPlayer localPlayer;", source[add_start:gain_start])
        self.assertIn("peers[other].outgoing.Relay(gain);", source[on_state:])

    def test_admission_origin_and_unattributed_kill_xp_guard(self):
        with tempfile.TemporaryDirectory() as folder:
            result = XPSHARE_ADMISSION_GUARD_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        source = _testbuild.src_text("xpshare.cpp")
        self.assertIn("EnsureUnattributedXpGuard();", source)
        self.assertIn("base + 0x1c2b3", source)
        self.assertIn("base + 0x27834", source)
        self.assertIn("GuardUnattributedDeathGiveXp", source)
        self.assertIn("PreparePeerAdmissionOrigin(", source)
        self.assertIn("BeginSaveCapture()", source)
        self.assertIn("CommitSaveCapture()", source)
        self.assertIn("CancelSaveCapture()", source)
        self.assertIn("OnClientRecordRestored(", source)


if __name__ == "__main__":
    unittest.main()
