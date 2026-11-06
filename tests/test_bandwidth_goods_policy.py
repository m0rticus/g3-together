import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def body(source, signature):
    match = re.search(signature, source)
    if not match:
        raise AssertionError(f"missing {signature}")
    start = source.index("{", match.end())
    depth = 1
    for i in range(start + 1, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if not depth:
                return source[start + 1:i]
    raise AssertionError(f"unterminated {signature}")


class BandwidthGoodsPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        cls.trade = (ROOT / "src" / "trade.cpp").read_text(encoding="utf-8")

    def test_engine_free_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        # Keep compiler intermediates inside the worktree, not the system temp directory.
        stem = f"bandwidth_goods_policy_{os.getpid()}"
        executable = ROOT / "tests" / (stem + (".exe" if os.name == "nt" else ""))
        environment = dict(os.environ, TMP=str(ROOT / "tests"), TEMP=str(ROOT / "tests"))
        try:
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "bandwidth_goods_policy_test.cpp"), "-o", str(executable)],
                check=True, capture_output=True, text=True, cwd=ROOT, env=environment,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True, cwd=ROOT)
        finally:
            executable.unlink(missing_ok=True)

    def test_measurement_uses_common_delivery_points(self):
        send = body(self.net, r"\bbool\s+Send\s*\(")
        receive = body(self.net, r"\bvoid\s+OnReceive\s*\(")
        self.assertLess(send.index("enet_peer_send(peer, channel, packet)"), send.index("bandwidth.Tx("))
        self.assertLess(receive.index("bandwidth.Rx("), receive.index("Gated(type)"))
        report = body(self.net, r"\bvoid\s+BandwidthLog\s*\(")
        for counter in ("totalSentData", "totalSentPackets", "totalReceivedData", "totalReceivedPackets"):
            self.assertIn(counter, report)
        self.assertIn("ForEachKindLine", report)
        self.assertIn("bandwidth = detail::BandwidthCounters{}", report)

    def test_goods_join_replay_retries_and_does_not_change_wire_format(self):
        receive = body(self.net, r"\bvoid\s+OnReceive\s*\(")
        self.assertIn("cache.assign(packet->data, packet->data + packet->dataLength)", receive)
        self.assertIn("header.epoch > previous.epoch", receive)
        self.assertIn("header.serial > previous.serial", receive)
        world = body(self.net, r"\bvoid\s+SetInWorld\s*\(")
        self.assertIn("++worldEntries", world)
        self.assertIn("goodsReplay[id]", world)
        self.assertIn("goodsCache[id].clear()", world)
        replay = body(self.net, r"\bvoid\s+FlushGoodsReplays\s*\(")
        self.assertIn("Send(peers[to], snapshot.data(), snapshot.size(), ChannelReliable, true)", replay)
        self.assertIn("FlushGoodsReplays();", body(self.net, r"\bvoid\s+Service\s*\("))
        publish = body(self.trade, r"\buint32_t\s+Publish\s*\(")
        self.assertIn("++nextSerial", publish)
        self.assertIn("goodsSchedule.Published(", publish)
        self.assertIn("goodsSchedule.Due(", body(self.trade, r"\bvoid\s+Tick\s*\("))


if __name__ == "__main__":
    unittest.main()
