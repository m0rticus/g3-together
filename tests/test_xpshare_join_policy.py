import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class XPShareJoinPolicyTests(unittest.TestCase):
    def test_join_rejoin_and_rewind_counter_policy_without_engine(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required for engine-free XP counter fixture")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "xpshare-join-policy.exe"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "xpshare_join_policy_test.cpp"), "-o", str(executable)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)

    def test_origins_are_captured_before_first_reward_and_controls_pass_both_gates(self):
        source = (ROOT / "src" / "xpshare.cpp").read_text(encoding="utf-8")
        start = source.index("void NoteGiveXp(")
        self.assertLess(source.index("PrepareAdmissions(", start), source.index("earned +=", start))
        self.assertIn("peer.outgoing.Begin(stream, earned)", source)
        self.assertIn("peer.outgoing.Total(earned)", source)
        self.assertIn("peer.Snapshot(theirNonce, theirEarned)", source)
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        self.assertIn("!XpHandshake(data, size)", net)
        self.assertIn("!Open(peer)) { ++gatedOut; return false; }", net)
        self.assertIn("XpHandshake(packet->data, packet->dataLength)", net)
        self.assertIn("peers[from] != peer || message.state.npcClass != from", net)


if __name__ == "__main__":
    unittest.main()
