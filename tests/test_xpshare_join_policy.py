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


class XPShareJoinPolicyTests(unittest.TestCase):
    def test_join_rejoin_and_rewind_counter_policy_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = XPSHARE_JOIN_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_origins_are_captured_before_first_reward_and_controls_pass_both_gates(self):
        source = _testbuild.src_text("xpshare.cpp")
        start = source.index("void NoteGiveXp(")
        self.assertLess(source.index("PrepareAdmissions(", start), source.index("earned +=", start))
        self.assertIn("peer.outgoing.Begin(stream, earned)", source)
        self.assertIn("peer.outgoing.Total(earned)", source)
        self.assertIn("peer.Snapshot(theirNonce, theirEarned)", source)
        net = _testbuild.src_text("net.cpp")
        self.assertIn("!XpHandshake(data, size)", net)
        self.assertIn("!Open(peer)) { ++gatedOut; return false; }", net)
        self.assertIn("XpHandshake(packet->data, packet->dataLength)", net)
        self.assertIn("peers[from] != peer || message.state.npcClass != from", net)


if __name__ == "__main__":
    unittest.main()
