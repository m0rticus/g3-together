import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "bandwidth_goods_policy", [ROOT / "tests" / "bandwidth_goods_policy_test.cpp"],
    respect_env=False,
)


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
        cls.net = _testbuild.src_text("net.cpp")
        cls.trade = _testbuild.src_text("trade.cpp")

    def test_engine_free_policy(self):
        TARGET.run(check=True)

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
