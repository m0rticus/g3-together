import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOWNED_H = ROOT / "src" / "downed.h"
DOWNED_CPP = ROOT / "src" / "downed.cpp"
SCRIPT_CPP = ROOT / "src" / "Script_G3Together.cpp"


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


class DownedPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.downed_h = DOWNED_H.read_text(encoding="utf-8")
        cls.downed_cpp = DOWNED_CPP.read_text(encoding="utf-8")
        cls.script_cpp = SCRIPT_CPP.read_text(encoding="utf-8")

    def test_die_is_idempotent_and_calls_native_kill_once(self):
        body = function_body(
            self.downed_cpp,
            r"\bvoid\s+Die\s*\(\s*Entity\s*&\s*hero\s*\)",
        )
        self.assertIsNotNone(body)
        self.assertIn("if (dying || terminalKillSent) return;", body)
        self.assertIn("terminalKillSent = true;", body)
        kill = 'GetScriptAdminExt().CallScript(bCString("Kill"), hero.GetInstance(), nullptr, 0)'
        self.assertEqual(body.count(kill), 1)
        self.assertLess(body.index("terminalKillSent = true;"), body.index(kill))
        self.assertNotIn("SetTask", body)

    def test_terminal_tick_latch_never_restarts_a_death_task(self):
        tick = function_body(
            self.downed_cpp,
            r"\bvoid\s+Tick\s*\(\s*\)",
        )
        self.assertIsNotNone(tick)
        start = tick.index("if (dying) {")
        end = tick.index("if (hero.IsDead())", start)
        terminal = tick[start:end]
        self.assertNotIn("SetTask", terminal)
        self.assertNotIn("diedAt", terminal)
        self.assertNotIn("dying again", terminal)
        self.assertIn("hero.PlayerMemory.GetHitPoints() > 0", terminal)

    def test_revive_and_world_change_clear_terminal_death(self):
        revived = function_body(
            self.downed_cpp,
            r"\bvoid\s+Revived\s*\(\s*int\s+byPlayer\s*\)",
        )
        world_change = function_body(
            self.downed_cpp,
            r"\bvoid\s+OnWorldChange\s*\(\s*\)",
        )
        self.assertIsNotNone(revived)
        self.assertIn("dying", revived)
        self.assertIn("terminalKillSent", revived)
        self.assertIsNotNone(world_change)
        self.assertIn("dying = false;", world_change)
        self.assertIn("terminalKillSent = false;", world_change)
        self.assertIn("void OnWorldChange();", self.downed_h)
        self.assertIn("g3t::downed::OnWorldChange();", self.script_cpp)

    def test_all_down_and_absence_grace_rules_are_preserved(self):
        all_down = function_body(
            self.downed_cpp,
            r"\bbool\s+AllOthersDown\s*\(\s*net::Remote\s+const\*\s+remotes\s*,\s*uint32_t\s+now\s*\)",
        )
        tick = function_body(
            self.downed_cpp,
            r"\bvoid\s+Tick\s*\(\s*\)",
        )
        self.assertIsNotNone(all_down)
        self.assertIn("r.latest.flags & 14", all_down)
        self.assertIn("Present(r, now)", all_down)
        self.assertIn("constexpr uint32_t AloneGrace = 5000;", self.downed_cpp)
        self.assertIsNotNone(tick)
        self.assertIn("AllOthersDown(remotes, now)", tick)
        self.assertIn("now - aloneSince >= AloneGrace", tick)


if __name__ == "__main__":
    unittest.main()
