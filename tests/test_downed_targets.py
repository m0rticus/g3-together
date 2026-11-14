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
        for (unsigned state = 0; state < 16; ++state) {
            const bool held = state & 1, terminal = state & 2, down = state & 4, dead = state & 8;
            assert(!CombatUnavailable(false, held, terminal, down, dead, flags));
            assert(CombatUnavailable(true, held, terminal, down, dead, flags)
                   == (state != 0 || (flags & 14) != 0));
        }
    }
    // Neither the in-game flag nor robbability is a combat exclusion on its own.
    assert(!CombatUnavailable(true, false, false, false, false, 1 | 16));
    assert(CombatUnavailable(true, false, false, false, false, 9)); // held before native Down
    assert(CombatUnavailable(true, false, false, false, false, 11));
    assert(CombatUnavailable(true, false, false, false, false, 5)); // terminal
    for (unsigned bindings = 0; bindings < 128; ++bindings)
        assert(StopTargetInstruction(bindings) == ((bindings & (Current | Spell)) != 0));
    assert(!StopTargetInstruction(Alternative | Attacker | LastAttacker | Inflictor | Alignment));
    Life life;
    life.Hold();
    const auto episode = life.sequence;
    assert(CombatUnavailable(true, life.held, life.terminal, false, false, 0)); // native wake drift
    assert(!life.Revive(episode + 1));
    assert(life.Revive(episode));
    assert(!CombatUnavailable(true, life.held, life.terminal, false, false, 0));
    // Remote interaction mode is not an input to combat exclusion.
    assert(!CombatUnavailable(true, false, false, false, false, 1));
    life.Hold();
    life.End();
    assert(CombatUnavailable(true, life.held, life.terminal, false, false, 0));
    assert(!life.Revive(life.sequence));
}
'''
TARGET = _testbuild.register_target(
    "downed_targets",
    [_testbuild.inline_unit("downed_targets.cpp", HARNESS, warnings="none", includes=(ROOT / "src",))],
    msvc_vcvars=False,
)


class DownedTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ai = _testbuild.src_text("ai.cpp")
        cls.combat = _testbuild.src_text("combat.cpp")
        cls.downed = _testbuild.src_text("downed.cpp")

    def test_player_life_and_binding_policy_executes_without_engine(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_remote_truth_does_not_read_published_native_mode_or_timeout(self):
        query = function_body(self.downed, r"\bbool\s+CombatUnavailable\s*\(\s*eCEntity\s*\*\s*entity\s*\)")
        self.assertIn("life.held, life.terminal, player.IsDown(), player.IsDead()", query)
        remote = query.split("const int id", 1)[1]
        for required in ("r.connected", "r.latestTicks", "r.latest.world == remote::CurrentWorld()", "r.latest.flags"):
            self.assertIn(required, remote)
        for forbidden in ("player.IsDown()", "player.IsDead()", "StaleState", "net::Now()", "r.previous"):
            self.assertNotIn(forbidden, remote)
        publication = function_body(self.ai, r"\bvoid\s+SyncDownedTargets\s*\(")
        self.assertIn("if (!gCSession::GetInstance().IsGameRunning() || !downed::UnavailablePlayers()) return;", publication)
        self.assertIn("npclease::LocalAuthority(instance)", publication)
        self.assertIn("downed::UnavailablePlayers()", publication)
        self.assertIn("ReleaseDownedTargets", publication)
        for forbidden in ("AIMode =", "SetHitPoints", "gEAIMode_Dead", "SetTask", "SetPartyLeader"):
            self.assertNotIn(forbidden, publication)

    def test_exact_references_are_released_after_instruction_stop(self):
        release = function_body(self.ai, r"\bbool\s+ReleaseDownedTargets\s*\(")
        host = function_body(self.ai, r"\bbool\s+HostNpc\s*\(")
        self.assertIn("net::IsHost() || npclease::LocalAuthority(entity)", host)
        self.assertIn("!guards::IsLocalPlayer(entity)", host)
        self.assertIn("!remote::IsPuppet(entity)", host)
        self.assertIn("!npcs::IsMirrored(entity) || npclease::LocalAuthority(entity)", host)
        for flag, setter in (("Current", "SetCurrentTarget"), ("Alternative", "SetAlternativeTarget"),
                             ("Attacker", "SetCurrentAttacker"), ("LastAttacker", "SetLastAttacker"),
                             ("Spell", "SetSpellTarget"), ("Alignment", "SetAlignmentTarget"),
                             ("Inflictor", "SetLastInflictor")):
            self.assertIn(f"if (bindings & downed::{flag})", release)
            self.assertLess(release.index("npc.Routine.FullStop()"), release.index(setter))
            self.assertLess(release.index("npc.Routine.StopAICombatMove()"), release.index(setter))
        self.assertIn("downed::StopTargetInstruction(bindings)", release)
        self.assertLess(release.index("SetCurrentTarget(none)"), release.index("SelectReplacement("))
        self.assertLess(release.index("spu->SetOtherEntity(nullptr)"), release.index("SelectReplacement("))
        self.assertIn("npc.Routine.StopAIGoto()", release)
        replacement = function_body(release, r"\bif\s*\(\s*next\.GetInstance\(\)\s*\)")
        self.assertNotIn("npc.NPC.CombatState = 0", replacement)
        self.assertNotIn("npc.Routine.AIMode =", release)
        self.assertNotIn("ContinueRoutine", replacement)
        self.assertIn('bCString("AssessTarget")', replacement)
        self.assertIn("npc.SetAlignmentTarget(next)", replacement)
        relax = function_body(release, r"\bif\s*\(\s*!next\.GetInstance\(\)\s*\)")
        fighting = function_body(relax, r"\bif\s*\(\s*fighting\s*\)")
        self.assertIn('npc.Routine.SetState(bCString("ZS_Attack_End"))', fighting)
        self.assertNotIn("CombatState = 0", fighting)
        self.assertNotIn("ContinueRoutine", fighting)
        self.assertIn("->GetSPU()", release)
        self.assertIn("spu->SetOtherEntity(nullptr)", release)
        self.assertNotIn('CallScript(bCString("Defeat")', release)
        self.assertNotIn("AttitudeToPlayer2 =", release)
        run = function_body(self.ai, r"\bGEBool\s+Run\s*\(")
        self.assertLess(run.index("npcplayback::IsPassive(self)"), run.index("ReleaseDownedTargets"))
        self.assertLess(run.index("ReleaseDownedTargets"), run.rindex("it->second(stack, spu)"))
        tick = function_body(self.ai, r"\bvoid\s+Tick\s*\(")
        self.assertLess(tick.index("SyncDownedTargets()"), tick.index("if ((installed || searched)"))

    def test_native_acquisition_and_perception_are_gated_before_overrides(self):
        attitude = function_body(self.ai, r"\bGEInt\s+GE_STDCALL\s+FilterAttitude\s*\(")
        self.assertLess(attitude.index("RejectDownedTarget"), attitude.index("playerAttitudeQueries"))
        self.assertLess(attitude.index("RejectDownedTarget"), attitude.index("remote::NoteContact"))
        for signature in (r"\bGEInt\s+GE_STDCALL\s+FilterScript\s*\(",
                          r"\bGEBool\s+GE_STDCALL\s+FilterCallback\s*\("):
            body = function_body(self.ai, signature)
            self.assertLess(body.index("RejectDownedTarget"), body.index("receiver == detail::damageReceiver"))
        assess = function_body(self.ai, r"\bGEInt\s+GE_STDCALL\s+FilterAssessTarget\s*\(")
        self.assertIn('RejectDownedTarget(Self(spu, self), target, "assessment-refused")', assess)
        wrap = function_body(self.ai, r"\bvoid\s+WrapAssessTarget\s*\(")
        for required in ("0x31470", "std::memcmp(code, head, sizeof(head))", "JumpTo(", "TrackPointer(", "SCRIPT ONLY"):
            self.assertIn(required, wrap)

    def test_harmful_gates_precede_effects_and_forwarding_but_allow_healing(self):
        judge = function_body(self.combat, r"\bGEInt\s+Judge\s*\(")
        self.assertLess(judge.index("recipientArg"), judge.index("RejectDownedHit"))
        self.assertLess(judge.index("RejectDownedHit"), judge.index("IsPuppetMeleeJudgement"))
        self.assertLess(judge.index("RejectDownedHit"), judge.index("if (which >= 2)"))
        hp = function_body(self.combat, r"\bGEInt\s+GE_STDCALL\s+Filter\s*\(")
        self.assertIn("args < 0 && RejectDownedHit", hp)
        self.assertNotIn("bypass", hp)
        self.assertLess(hp.index("RejectDownedHit"), hp.index("if (forwarded)"))
        reject = function_body(self.combat, r"\bbool\s+RejectDownedHit\s*\(")
        self.assertIn("player.DamageReceiver.DamageAmount = 0", reject)
        self.assertIn("downed::CombatUnavailable(victim)", reject)
        visible = function_body(self.combat, r"\bbool\s+LocalBlow\s*\(")
        self.assertLess(visible.index("RejectDownedHit"), visible.index("GameHit("))
        received = function_body(self.combat, r"\bvoid\s+OnHitPlayer\s*\(")
        self.assertLess(received.index("RejectDownedHit"), received.index("++playerHitsTaken"))
        self.assertLess(received.index("RejectDownedHit"), received.index("GameHit("))


if __name__ == "__main__":
    unittest.main()
