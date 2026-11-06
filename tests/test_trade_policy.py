import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRADE_H = ROOT / "src" / "trade.h"
TRADE_CPP = ROOT / "src" / "trade.cpp"
OFFER_CPP = ROOT / "src" / "offer.cpp"


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


class TradePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trade_h = TRADE_H.read_text(encoding="utf-8")
        cls.trade_cpp = TRADE_CPP.read_text(encoding="utf-8")
        cls.offer_cpp = OFFER_CPP.read_text(encoding="utf-8")

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


if __name__ == "__main__":
    unittest.main()
