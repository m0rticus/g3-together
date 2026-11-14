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


if __name__ == "__main__":
    unittest.main()
