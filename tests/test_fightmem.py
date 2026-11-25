import os
from pathlib import Path
import struct
import sys
import unittest

from test_downed_policy import function_body


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

HARNESS = r'''
#include "downed_policy.h"
#include <cassert>
using namespace g3t::downed;
int main()
{
    // Stale non-lethal NPC hits never downgrade attackerless deaths (fall, drowning, fire, trap, script kill).
    assert(!UseJudgedHitForDeathRedirect(true, false, false));
    assert(UseJudgedHitForDeathRedirect(true, false, true));
    assert(UseJudgedHitForDeathRedirect(true, true, false));
    assert(!UseJudgedHitForDeathRedirect(false, false, true));
    assert(!UseJudgedHitForDeathRedirect(false, true, true));

    // Game.dll gCQuest_PS::OnNPCDefeated dereferences CurrentAttacker without null check only for
    // active (status == 1) Arena (5) and Sparring (12) quests when target is the local hero.
    assert(SafeQuestDefeatNeedsAttackerGuard(true, 5, 1, false));
    assert(SafeQuestDefeatNeedsAttackerGuard(true, 12, 1, false));
    assert(!SafeQuestDefeatNeedsAttackerGuard(true, 5, 1, true));
    assert(!SafeQuestDefeatNeedsAttackerGuard(true, 12, 1, true));
    assert(!SafeQuestDefeatNeedsAttackerGuard(true, 3, 1, false));
    assert(!SafeQuestDefeatNeedsAttackerGuard(true, 5, 2, false));
    assert(!SafeQuestDefeatNeedsAttackerGuard(false, 5, 1, false));
    assert(!SafeQuestDefeatNeedsAttackerGuard(false, 12, 1, false));

    // Fallback Defeat cleanup runs only when hero has no attacker and the Game.dll quest guard is absent.
    assert(SafeDefeatNeedsHeroFallback(true, false, false));
    assert(!SafeDefeatNeedsHeroFallback(true, false, true));
    assert(!SafeDefeatNeedsHeroFallback(true, true, false));
    assert(!SafeDefeatNeedsHeroFallback(false, false, false));

    // Self-defeat winner normalization never treats the victim as its own victorious attacker.
    assert(ShouldClearDefeatWinner(true, true));
    assert(!ShouldClearDefeatWinner(true, false));
    assert(!ShouldClearDefeatWinner(false, true));
    assert(!ShouldClearDefeatWinner(false, false));
    return 0;
}
'''
TARGET = _testbuild.register_target(
    "fightmem_policy",
    [_testbuild.inline_unit("fightmem_policy.cpp", HARNESS, warnings="none", includes=(ROOT / "src",))],
    msvc_vcvars=False,
)


class FightmemTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fightmem = _testbuild.src_text("fightmem.cpp")

    def test_attackerless_defeat_and_redirect_policy_executes_without_engine(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_quest_defeat_and_defeat_script_are_guarded_against_null_and_self_attackers(self):
        install = function_body(self.fightmem, r"\bvoid\s+InstallQuestDefeatGuard\s*\(")
        self.assertIsNotNone(install)
        self.assertIn("?OnNPCDefeated@gCQuest_PS@@QAEXABVeCEntity@@@Z", install)
        self.assertIn("0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8", install)
        self.assertIn("ai::JumpTo(", install)
        self.assertLess(install.index("std::memcmp"), install.index("ai::JumpTo("))

        quest = function_body(self.fightmem, r"\bvoid\s+__fastcall\s+QuestDefeated\s*\(")
        self.assertIsNotNone(quest)
        self.assertIn("SafeQuestDefeatNeedsAttackerGuard(", quest)
        self.assertIn("AttackerOf(hero)", quest)
        self.assertLess(quest.index("SafeQuestDefeatNeedsAttackerGuard("),
                        quest.rindex("originalQuestDefeated(self, edx, entity)"))

        defeat = function_body(self.fightmem, r"\bGEInt\s+GE_STDCALL\s+Defeat\s*\(")
        self.assertIsNotNone(defeat)
        self.assertIn("InstallQuestDefeatGuard();", defeat)
        self.assertIn("SafeDefeatNeedsHeroFallback(", defeat)
        self.assertIn("ShouldClearDefeatWinner(", defeat)
        self.assertLess(defeat.index("InstallQuestDefeatGuard();"),
                        defeat.index("originalDefeat(spu, winnerArg, other, args)"))
        self.assertLess(defeat.index("SafeDefeatNeedsHeroFallback("),
                        defeat.index("originalDefeat(spu, winnerArg, other, args)"))

        tick = function_body(self.fightmem, r"\bvoid\s+Tick\s*\(")
        self.assertIn("0x27510", tick)
        self.assertIn("InstallQuestDefeatGuard();", tick)
        self.assertIn("PruneAndSyncGrudges();", tick)
        self.assertIn("IsAngryFightOutcome(", self.fightmem)
        self.assertIn("gELastFightAgainstPlayer_None", self.fightmem)

        read_fn = function_body(self.fightmem, r"\bMemory\s+Read\s*\(")
        self.assertIn("guards::IsLocalPlayer(npc)", read_fn)
        prune_fn = function_body(self.fightmem, r"\bvoid\s+PruneAndSyncGrudges\s*\(")
        self.assertNotIn("if (!net::IsHost() ||", prune_fn)
        self.assertIn("net::Remotes()[player].connected", prune_fn)

        state_actor = function_body(self.fightmem, r"\beCEntity\*\s+StateActor\s*\(")
        self.assertIsNotNone(state_actor)
        self.assertIn("guards_policy::SelectFightStateActor(", state_actor)
        self.assertIn("guards::IsLocalPlayer(target)", state_actor)

    def test_real_native_defeat_and_quest_prologues_read_only(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("Set G3T_GAME_DLL to check the actual companion Game.dll and Script_Game.dll read-only")

        def read_rva(path, rva, size):
            data = _testbuild.cached_bytes(path)
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            sections = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
            count = struct.unpack_from("<H", data, pe + 6)[0]
            for index in range(count):
                virtual_size, virtual, raw_size, raw = struct.unpack_from("<IIII", data, sections + index * 40 + 8)
                if virtual <= rva and rva + size <= virtual + min(virtual_size, raw_size):
                    offset = raw + rva - virtual
                    return data[offset:offset + size]
            self.fail(f"Unmapped native RVA {rva:#x} in {path}")

        game_path = Path(game)
        script_path = game_path.parent / "scripts" / "Script_Game.dll"
        self.assertEqual(read_rva(game_path, 0x159490, 6), bytes.fromhex("55 8b ec 83 e4 f8"))
        self.assertEqual(read_rva(script_path, 0x27510, 6), bytes.fromhex("55 8b ec 83 e4 f8"))


if __name__ == "__main__":
    unittest.main()
