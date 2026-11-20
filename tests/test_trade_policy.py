import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRADE_H = ROOT / "src" / "trade.h"
TRADE_CPP = ROOT / "src" / "trade.cpp"
OFFER_CPP = ROOT / "src" / "offer.cpp"
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


def function_body(source, signature):
    match = re.search(signature, source)
    if not match:
        return None
    opening = source.find("{", match.end())
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    return None


TRADE_POLICY_CODE = r"""
#undef NDEBUG
#include <cassert>
#include "trade_policy.h"

int main()
{
    using namespace g3t::trade_policy;

    long v = 0;
    assert(ParseDecimalLong("0", 0, 100, v) && v == 0);
    assert(ParseDecimalLong("42", 1, 100, v) && v == 42);
    assert(!ParseDecimalLong("", 0, 100, v));
    assert(!ParseDecimalLong(" 42", 0, 100, v));
    assert(!ParseDecimalLong("+42", 0, 100, v));
    assert(!ParseDecimalLong("42abc", 0, 100, v));
    assert(!ParseDecimalLong("101", 0, 100, v));

    std::string name = "It_Sword-q1023";
    int q = -1;
    assert(ParseQualitySuffix(name, q) && name == "It_Sword" && q == 1023);
    name = "It_Sword-q1024";
    assert(!ParseQualitySuffix(name, q));
    name = "It_Sword-q+5";
    assert(!ParseQualitySuffix(name, q));

    int id = 0;
    assert(ParsePuppetId("puppet2", 8, id) && id == 2);
    assert(!ParsePuppetId("puppet0", 8, id));
    assert(!ParsePuppetId("puppet8", 8, id));
    assert(!ParsePuppetId("puppet2x", 8, id));
    return 0;
}
"""

TRADE_POLICY_TARGET = _testbuild.register_target(
    "trade_policy_parse",
    [_testbuild.inline_unit("trade_policy_parse_test.cpp", TRADE_POLICY_CODE)],
    msvc_vcvars=False,
)


class TradePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trade_h = _testbuild.src_text("trade.h")
        cls.trade_cpp = _testbuild.src_text("trade.cpp")
        cls.offer_cpp = _testbuild.src_text("offer.cpp")

    def test_decimal_and_suffix_parsing(self):
        result = TRADE_POLICY_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shared_classifier_is_declared_and_has_one_stack_definition(self):
        self.assertRegex(self.trade_h, r"\bTradableItem\s*\(")
        self.assertRegex(self.trade_h, r"\bTradableStack\s*\(")
        stack_definitions = re.findall(
            r"\bbool\s+TradableStack\s*\(\s*Entity\s*&",
            self.trade_cpp + "\n" + self.offer_cpp,
        )
        self.assertEqual(len(stack_definitions), 1)

    def test_stack_and_item_rules_are_split_and_complete(self):
        item_body = function_body(
            self.trade_cpp,
            r"\bbool\s+TradableItem\s*\(\s*Entity\s+const\s*&\s*item\s*\)",
        )
        stack_body = function_body(
            self.trade_cpp,
            r"\bbool\s+TradableStack\s*\(\s*Entity\s*&\s*self\s*,\s*int\s+stack\s*\)",
        )
        self.assertIsNotNone(item_body)
        self.assertIsNotNone(stack_body)
        self.assertIn("GetInstance()", item_body)
        self.assertIn("IsMissionItem()", item_body)
        self.assertIn("gEItemCategory_Skill", item_body)
        self.assertIn("gEItemCategory_Spellbook", item_body)
        self.assertIn("Fist", item_body)
        self.assertIn("GetSlotFromStack(stack)", stack_body)
        self.assertLess(
            stack_body.index("GetSlotFromStack(stack)"),
            stack_body.index("TradableItem("),
        )

    def test_trade_and_offer_use_the_shared_stack_classifier(self):
        for name in ("Read", "BuildExchange", "Exchange"):
            body = function_body(
                self.trade_cpp,
                rf"\b(?:bool|void)\s+{name}\s*\(",
            )
            self.assertIsNotNone(body, name)
            self.assertIn("TradableStack(", body)

        for name in ("Amount", "Take"):
            body = function_body(
                self.offer_cpp,
                rf"\b(?:bool|int)\s+{name}\s*\(",
            )
            self.assertIsNotNone(body, name)
            self.assertIn("trade::TradableStack(", body)

        self.assertNotRegex(
            self.offer_cpp,
            r"\bbool\s+(?:Tradable|TradableStack)\s*\(",
        )

    def test_remote_offer_metadata_uses_the_shared_item_classifier(self):
        apply_body = function_body(
            self.offer_cpp,
            r"\bbool\s+Apply\s*\(\s*net::OfferNote\s+const\s*&\s*note\s*\)",
        )
        self.assertIsNotNone(apply_body)
        self.assertIn("trade::TradableItem(entity)", apply_body)
        self.assertNotIn("entity.Item.IsMissionItem()", apply_body)
        self.assertNotIn("entity.Item.GetCategory()", apply_body)

    def test_offer_same_and_take_mask_quality_bits(self):
        same_body = function_body(
            self.offer_cpp,
            r"\bbool\s+Same\s*\(",
        )
        take_body = function_body(
            self.offer_cpp,
            r"\bint\s+Take\s*\(",
        )
        self.assertIsNotNone(same_body)
        self.assertIsNotNone(take_body)
        self.assertIn("GetQuality(stack) & trade_policy::QualityMask", same_body)
        self.assertIn("GetQuality(i) & trade_policy::QualityMask", take_body)

    def test_exchange_quality_mask_overflow_and_rollback_safety(self):
        exchange_body = function_body(self.trade_cpp, r"\bbool\s+Exchange\s*\(")
        self.assertIn("if (!inventory) return false;", exchange_body)
        self.assertIn("GetQuality(i) & trade_policy::QualityMask", exchange_body)

        read_body = function_body(self.trade_cpp, r"\bvoid\s+Read\s*\(")
        self.assertIn("int64_t goldTotal", read_body)
        publish_body = function_body(self.trade_cpp, r"\buint32_t\s+Publish\s*\(")
        self.assertIn("std::min(amount, trade_policy::MaxStackAmount)", publish_body)

        build_body = function_body(self.trade_cpp, r"\bbool\s+BuildExchange\s*\(")
        self.assertIn("if (amount < 0) return false;", build_body)

        tick_body = function_body(self.trade_cpp, r"\bvoid\s+Tick\s*\(")
        self.assertIn("if (!puppetDialog && !windowOpen) partner = 0;", tick_body)

        undo_body = function_body(self.offer_cpp, r"\bvoid\s+Undo\s*\(")
        self.assertIn("Take(self, it->item, it->quality, it->amount, none, true)", undo_body)

        peer_left_body = function_body(self.offer_cpp, r"\bvoid\s+OnPeerLeft\s*\(")
        self.assertIn("it->note.seller == player && it->delivered", peer_left_body)
        self.assertIn("sent.erase(it)", peer_left_body)

        apply_body = function_body(self.offer_cpp, r"\bbool\s+Apply\s*\(")
        self.assertIn("!inventoryHealthy", apply_body)


if __name__ == "__main__":
    unittest.main()
