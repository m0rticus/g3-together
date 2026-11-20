from pathlib import Path
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
    for (unsigned flags = 0; flags < 256; ++flags) {
        assert(!HeldPuppet(flags, 0));
        assert(HeldPuppet(flags, 1) == ((flags & 8) && !(flags & 4)));
        assert(HeldPuppet(flags, UINT32_MAX) == HeldPuppet(flags, 1));
    }
    assert(HeldPuppet(9, 1)); // hold arrives before native knockdown
    assert(HeldPuppet(11, 1));
    assert(HeldPuppet(27, 1)); // held beats a leftover native-robbery bit
    assert(!HeldPuppet(19, 1)); // nonlethal knockout still follows native robbery
    assert(!HeldPuppet(13, 1)); // terminal beats held
    assert(!HeldPuppet(1, 1)); // revived

    Life life;
    unsigned potions = 3;
    uint32_t paid = 0;
    life.Hold();
    const uint32_t first = life.sequence;
    assert(UnpaidEpisode(first, paid));
    --potions;
    paid = first;
    for (unsigned seconds = 0; seconds < 100000; ++seconds)
        assert(!UnpaidEpisode(first, paid)); // never charge twice after cooldown/late owner update
    assert(life.Revive(first));
    assert(!life.Revive(first)); // duplicate network application
    life.Hold();
    const uint32_t second = life.sequence;
    assert(UnpaidEpisode(second, paid)); // new episode can be helped immediately
    // Failed potion removal changes neither payment nor the held life.
    assert(paid == first && life.held && potions == 2);
    --potions;
    paid = second;
    assert(life.Revive(second) && potions == 1);
    assert(!UnpaidEpisode(second, paid));
    assert(!UnpaidEpisode(0, paid));
    assert(UnpaidEpisode(UINT32_MAX, 1));
    assert(UnpaidEpisode(1, UINT32_MAX));

    RevivePaymentSlot slot;
    slot.ObserveConnection(true, 10);
    assert(slot.NeedsPayment(1));
    slot.MarkPaid(4);
    assert(!slot.NeedsPayment(1) && !slot.NeedsPayment(4));
    slot.ObserveConnection(false, 0);
    slot.ObserveConnection(true, 11);
    assert(slot.NeedsPayment(1)); // rejoining player starting back at sequence 1 is revivable
    slot.MarkPaid(3);
    slot.ObserveConnection(true, 12); // generation bump within a single tick also resets payment state
    assert(slot.NeedsPayment(1));
}
'''
TARGET = _testbuild.register_target(
    "downed_interaction",
    [_testbuild.inline_unit("downed_interaction.cpp", HARNESS, warnings="none", includes=(ROOT / "src",))],
    msvc_vcvars=False,
)


class DownedInteractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = {
            name: _testbuild.src_text(f"{name}.cpp")
            for name in ("remote", "downed", "containers", "talk", "ransack", "pickpocket", "ai")
        }

    def test_held_and_payment_policy_executes_without_engine(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_replicas_publish_talkable_mode_after_held_pose_drive(self):
        update = function_body(self.source["remote"], r"\bvoid\s+UpdatePuppet\s*\(")
        self.assertIn("downed::HeldPuppet(remote.latest.flags, remote.latest.downedSequence)", update)
        publication = update[update.index("body::Drive("):]
        self.assertIn("if (held)", publication)
        self.assertIn("Routine.AIMode = gEAIMode_Abandoned", publication)
        self.assertIn("Routine.AniState = gEAniState_LieKnockDown", publication)
        self.assertIn("CharacterMovement.StopMovement()", publication)
        self.assertNotIn("net::IsHost()", publication)
        self.assertNotIn("gEAIMode_Down", publication)
        for preserved in ("heldOwner", "newHeldEpisode", "target.position = remote.latest.position",
                          "target.goal = nullptr", "target.speed = 0.0f", "GetUp", "StandUp"):
            self.assertIn(preserved, update)
        truth = function_body(self.source["remote"], r"\bbool\s+IsHeldPuppet\s*\(")
        self.assertIn("r.connected && r.latestTicks && r.latest.world == CurrentWorld()", truth)
        self.assertIn("downed::HeldPuppet(r.latest.flags, r.latest.downedSequence)", truth)
        for forbidden in ("IsDown()", "IsDead()", "net::Now()", "latestTicks <"):
            self.assertNotIn(forbidden, truth)

    def test_native_and_script_loot_guard_precedes_window_and_keeps_trade(self):
        tick = function_body(self.source["containers"], r"\bvoid\s+Tick\s*\(")
        install = function_body(self.source["containers"], r"\bbool\s+InstallAccessGuards\s*\(")
        self.assertIn("InstallAccessGuards()", tick)
        self.assertIn("constexpr uintptr_t OpenLootRva = 0x6bfd0", self.source["containers"])
        self.assertIn("0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x54, 0x01, 0x00, 0x00", install)
        for required in ("std::memcmp", "ai::JumpTo", "loot->m_funcScript = &OpenLoot",
                         "reinterpret_cast<void*>(nativeLoot)", "SCRIPT ONLY"):
            self.assertIn(required, install)
        self.assertIn("reinterpret_cast<void*>(nativeLoot) == base + OpenLootRva", install)
        self.assertIn("!std::memcmp(base + OpenLootRva, head, sizeof(head))", install)
        self.assertLess(install.index("std::memcmp"), install.index("ai::JumpTo"))
        self.assertLess(install.index("ai::JumpTo"), install.index("loot->m_funcScript = &OpenLoot"))
        self.assertIn("trade->m_funcScript = &OpenTrade", install)
        loot = function_body(self.source["containers"], r"\bGEInt\s+GE_STDCALL\s+OpenLoot\s*\(")
        self.assertLess(loot.index("remote::PuppetOwner"), loot.index("Opened("))
        self.assertLess(loot.index("remote::IsHeldPuppet"), loot.index('CallScript(bCString("AssessTalk")'))
        self.assertIn("!downed::CombatUnavailable(actor)", loot)
        self.assertIn("actor == gCSession::GetInstance().GetHero()", loot)
        self.assertIn("loot-rerouted-to-talk", loot)
        self.assertIn("loot-refused", loot)
        self.assertIn("if (!result) ExitGonnaLoot(actor)", loot)
        self.assertIn("ExitGonnaLoot(actor);\n        return 0;", loot)
        exit_loot = function_body(self.source["containers"], r"\bvoid\s+ExitGonnaLoot\s*\(")
        self.assertIsNotNone(exit_loot)
        self.assertIn('"PS_GonnaLoot"', exit_loot)
        self.assertIn("hero.Routine.FullStop()", exit_loot)
        self.assertIn("hero.Routine.ContinueRoutine()", exit_loot)
        quick = function_body(self.source["containers"], r"\bGEBool\s+__fastcall\s+QuickTake\s*\(")
        self.assertLess(quick.index("remote::PuppetOwner(owner)"), quick.rindex("self->TransferAmountTo("))
        trade = function_body(self.source["containers"], r"\bGEInt\s+GE_STDCALL\s+OpenTrade\s*\(")
        self.assertIn("trade::Available()", trade)
        self.assertIn("Opened(spu, other, true)", trade)
        self.assertIn("originalTrade(spu, self, other, args)", trade)

    def test_held_robbery_and_pickpocket_are_stopped_before_native_effects(self):
        rob = function_body(self.source["ransack"], r"\bGEBool\s+__fastcall\s+RobTransfer\s*\(")
        self.assertLess(rob.index("remote::IsHeldPuppet(owner)"), rob.index("const GEBool moved"))
        self.assertIn("downed::Held() && owner == gCSession::GetInstance().GetHero()", rob)
        state = function_body(self.source["ransack"], r"\bbool\s+OnRobbing\s*\(")
        self.assertLess(state.index("remote::IsHeldPuppet(target)"), state.index("remote::PuppetOwner(target)"))
        self.assertIn("robber.Routine.FullStop()", state)
        self.assertIn("robber.Routine.ContinueRoutine()", state)
        self.assertIn("return true", state)
        run = function_body(self.source["ai"], r"\bGEBool\s+Run\s*\(")
        self.assertIn('strcmp(name, "ZS_RansackBody") == 0 && ransack::OnRobbing(spu)', run)
        tick = function_body(self.source["ransack"], r"\bvoid\s+Tick\s*\(")
        self.assertIn("const bool held = remote::IsHeldPuppet(body);", tick)
        self.assertIn("!held", tick)
        self.assertIn("(r.latest.flags & 16)", tick)
        pay = function_body(self.source["ransack"], r"\bvoid\s+Pay\s*\(")
        self.assertLess(pay.index("if (downed::Held())"), pay.index("TakeGold(hero"))
        pick = function_body(self.source["pickpocket"], r"\bGEInt\s+GE_STDCALL\s+Detour\s*\(")
        self.assertLess(pick.index("remote::IsPuppet(victim)"), pick.index("const GEInt result = originalPick"))

    def test_successful_payment_and_option_suppression_are_episode_bound(self):
        payment = function_body(self.source["downed"], r"\bvoid\s+PayAndHelp\s*\(")
        self.assertIn("!peers[i].payment.NeedsPayment(r.latest.downedSequence)", payment)
        self.assertIn("if (!hero.Inventory.DeleteItems(stack, 1))", payment)
        failure = function_body(payment, r"\bif\s*\(\s*!hero\.Inventory\.DeleteItems")
        self.assertIn("potion-removal-failed", failure)
        self.assertIn("return;", failure)
        for forbidden in ("SendToPlayer", "++potionsPaid", "MarkPaid"):
            self.assertNotIn(forbidden, failure)
        self.assertLess(payment.index("hero.Inventory.DeleteItems"), payment.index("peers[i].payment.MarkPaid("))
        self.assertLess(payment.index("peers[i].payment.MarkPaid("), payment.index("SendToPlayer(8"))
        offer = function_body(self.source["downed"], r"\bvoid\s+OfferHelp\s*\(")
        self.assertIn("peers[i].payment.NeedsPayment(r.latest.downedSequence)", offer)
        self.assertIn("peers[i].helpStarted", offer)
        self.assertIn("PayAndHelp(hero, i, remotes[i], now)", offer)

    def test_diagnostics_observe_real_calls_without_speculative_assessment(self):
        log = function_body(self.source["remote"], r"\bvoid\s+LogPuppetInteraction\s*\(")
        for field in ("role=%s", "player=%d", "episode=%u", "flags=%u", "held=%d", "ai=%d",
                      "native_down=%d", "native_dead=%d", "hp=%d", "inventory=%d",
                      "focused=%d", "action=%s", "result=%d"):
            self.assertIn(field, log)
        self.assertIn("notes > 40", log)
        self.assertIn("now - lastNote < 5000", log)
        for forbidden in ("CallScript", "SetTask", "SetFocus", "SetHitPoints"):
            self.assertNotIn(forbidden, log)
        assess = function_body(self.source["talk"], r"\bGEInt\s+GE_STDCALL\s+AssessTalkDetour\s*\(")
        self.assertLess(assess.index('"talk-attempt"'), assess.index("const GEInt result = originalAssessTalk"))
        self.assertLess(assess.index("const GEInt result = originalAssessTalk"), assess.index('"talk-assessed"'))
        self.assertIn("remote::IsPuppet(npc)", assess)
        self.assertIn("remote::IsPuppet(talker)", assess)
        self.assertIn("talk_policy::ShouldRefusePuppetTalk", assess)
        self.assertIn('"talk-refused-downed"', assess)
        self.assertLess(assess.index('"talk-refused-downed"'), assess.index("const GEInt result = originalAssessTalk"))


if __name__ == "__main__":
    unittest.main()
