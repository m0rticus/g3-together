import re
import os
import subprocess
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
        end = tick.index("if (hero.IsDead()", start)
        terminal = tick[start:end]
        self.assertNotIn("SetTask", terminal)
        self.assertNotIn("diedAt", terminal)
        self.assertNotIn("dying again", terminal)
        self.assertIn("hero.PlayerMemory.GetHitPoints() > 0", terminal)

    def test_revive_and_world_change_clear_terminal_death(self):
        revived = function_body(
            self.downed_cpp,
            r"\bvoid\s+Revived\s*\(\s*int\s+byPlayer\s*,\s*uint32_t\s+sequence\s*\)",
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

    def test_native_state_and_loading_cannot_clear_the_downed_latch(self):
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(\s*\)")
        world_change = function_body(self.downed_cpp, r"\bvoid\s+OnWorldChange\s*\(\s*\)")
        self.assertIn("if (!down && !life.held)", tick)
        self.assertNotIn("life.held = false;", world_change)
        self.assertIn("life.Rebind()", world_change)
        self.assertIn('KeepDown(hero, "tick")', tick)
        before_terminal = tick[:tick.index("if (dying)")]
        self.assertNotIn("life.held = false", before_terminal)
        self.assertNotIn("terminalKillSent = false", before_terminal)
        ai = (ROOT / "src" / "ai.cpp").read_text(encoding="utf-8")
        self.assertLess(ai.index("downed::GuardState(self, name)"), ai.index("if (SpectatorState(name))"))
        keys = function_body(ai, r"\bGEInt\s+GE_STDCALL\s+FilterPlayerKeys\s*\(")
        self.assertIn("if (downed::Held()) return 0;", keys)
        self.assertIn("persist::NativeLoading()", self.downed_cpp)
        self.assertIn("if (persist::HeroPending())", tick)
        redirect = function_body(self.downed_cpp, r"\bbool\s+RedirectDeath\s*\(")
        self.assertIn("!persist::HeroPending()", redirect)

    def test_records_preserve_downed_state_before_restoring_hp(self):
        hero = (ROOT / "src" / "hero.cpp").read_text(encoding="utf-8")
        save = function_body(hero, r"\bbool\s+SaveRecordImpl\s*\(")
        load = function_body(hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn('fprintf(file, "downed %d\\n"', save)
        self.assertIn('downed::Restore(savedDowned != 0, "hero-record")', load)
        self.assertLess(load.index("downed::Restore"), load.index("self.Inventory.HoldStacks"))
        self.assertIn("downed::Held()", load)

    def test_paid_revive_is_bound_to_sender_and_down_episode(self):
        self.assertIn("sequence != life.sequence", self.downed_cpp)
        self.assertIn("life.Revive(pendingReviveSequence)", self.downed_cpp)
        self.assertIn("hero.IsDown() || hero.IsDead()", self.downed_cpp)
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        self.assertIn("message.state.npcClass = from;", net)
        self.assertIn("constexpr uint16_t ProtocolVersion = saving::Protocol;", net)

    def test_engine_free_transition_and_ordering_rules(self):
        build = ROOT / "build" / "downed-policy-tests"
        configure = ["cmake", "-S", str(ROOT / "tests" / "downed_policy"), "-B", str(build)]
        if os.name == "nt":
            configure += ["-A", "Win32"]
        for command in (configure, ["cmake", "--build", str(build), "--config", "Debug"]):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        executable = build / "Debug" / "downed_policy_test.exe" if os.name == "nt" else build / "downed_policy_test"
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
