import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

KILL_REWARD_TARGET = _testbuild.register_target(
    "combat-kill-reward-unit",
    [_testbuild.unit(
        "tests/test_combat_kill_reward_unit.cpp",
        std="c++17",
        warnings="strict",
        includes=(ROOT / "src",),
    )],
)


class CombatKillRewardTests(unittest.TestCase):
    def test_kill_confirm_policy_rewards_next_frame_without_2s_wait(self):
        with tempfile.TemporaryDirectory() as folder:
            result = KILL_REWARD_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_kill_claim_reward_and_givexp_wiring(self):
        claims = _testbuild.src_text("combat_claims.cpp")
        combat = _testbuild.src_text("combat.cpp")
        spells = _testbuild.src_text("spells.cpp")

        reward_start = claims.index("void RewardKill(")
        expect_start = claims.index("void Expect(", reward_start)
        reward_body = claims[reward_start:expect_start]
        self.assertIn("net::SendKilled(player, guid, incarnation);", reward_body)
        self.assertIn("net::Flush();", reward_body)

        confirm_start = claims.index("void ConfirmKills()")
        confirm_end = claims.index("// The species the game knocks out", confirm_start)
        confirm_body = claims[confirm_start:confirm_end]
        self.assertNotIn("now - it->due", confirm_body)
        self.assertNotIn("now + 250", confirm_body)
        self.assertIn("killconfirm::Decide(", confirm_body)
        self.assertIn("if (it->clearDefeatedOnUndo) victim.NPC.DefeatedByPlayer = GEFalse;", confirm_body)

        # Already-defeated (knocked-out) NPCs must not pay a second time on a finishing blow.
        self.assertGreaterEqual(claims.count("const bool paysXp = !target.NPC.DefeatedByPlayer;"), 3)

        tick_start = combat.index("void Tick()")
        tick_end = combat.index("void OnWorldChange()", tick_start)
        tick_body = combat[tick_start:tick_end]
        self.assertLess(tick_body.index("InstallHooks();"), tick_body.index("DrainQueuedNpcHits();"))
        self.assertLess(tick_body.index("DrainQueuedNpcHits();"), tick_body.index("ConfirmKills();"))

        spells_start = spells.index("void Start()")
        spells_tick = spells.index("void Tick()", spells_start)
        self.assertIn("InstallTick();", spells[spells_start:spells_tick])
        self.assertLess(
            spells.index("InstallTick();", spells_tick),
            spells.index("now - lastTry < 1000", spells_tick),
        )

    def test_host_kill_local_player_guard_and_shared_defeat_wiring(self):
        claims = _testbuild.src_text("combat_claims.cpp")
        spells = _testbuild.src_text("spells.cpp")
        npclease = _testbuild.src_text("npclease_runtime.cpp")

        kill_start = npclease.index("GEInt GE_STDCALL Kill(")
        install_start = npclease.index("bool Install()", kill_start)
        kill_body = npclease[kill_start:install_start]
        self.assertLess(kill_body.index("guards::LocalPlayer localPlayer;"), kill_body.index("const GEInt r = originalKill("))
        self.assertLess(kill_body.index("guards::LocalPlayer localPlayer;"), kill_body.index("const GEInt result = originalKill("))
        self.assertEqual(kill_body.count("combat::ResolveKillXp(victim, killer, wasDefeated);"), 2)

        givexp_start = spells.index("GEInt GE_STDCALL HookGiveXp(")
        spells_after = spells.index("void InstallGiveXp(", givexp_start)
        givexp_body = spells[givexp_start:spells_after]
        self.assertIn("guards::LocalPlayer localPlayer;", givexp_body)
        self.assertIn("NormalizeCoopEarner(earner, hero)", givexp_body)
        self.assertIn("Entity::GetWorldEntity().GetInstance()", givexp_body)
        self.assertIn("xpshare::counter::SharesNativeSource(isWorldEntity)", givexp_body)

        apply_start = claims.index("bool TryApplyKillReward(uint8_t const guid[16], uint32_t incarnation)\n{")
        drain_start = claims.index("void DrainPendingKillRewards(DWORD now)\n{", apply_start)
        apply_body = claims[apply_start:drain_start]
        self.assertLess(
            apply_body.index("gCSession::GetInstance().IsGameRunning()"),
            apply_body.index("rewarded.insert(identity)"),
        )
        self.assertIn("guards::LocalPlayer localPlayer;", apply_body)
        self.assertIn("ConfirmSharedKnockouts(now);", claims)


if __name__ == "__main__":
    unittest.main()
