import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MeleePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        cls.net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        cls.net_h = (ROOT / "src" / "net.h").read_text(encoding="utf-8")
        cls.npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        cls.ai = (ROOT / "src" / "ai.cpp").read_text(encoding="utf-8")

    def test_native_relational_rule_executes_without_the_engine(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "The engine-free C++ policy test needs clang++, g++, or cl on PATH")
        source = r'''
#include "melee_policy.h"
#include "net.h"
#include <cassert>
using namespace g3t::melee;
int main()
{
    static_assert(23 + g3t::net::NpcsPerSend * sizeof(g3t::net::NpcRecord) <= 1200);
    static_assert(g3t::net::NpcsPerSend == 2);
    static_assert(sizeof(g3t::net::NpcRecord) == 588);
    static_assert(sizeof(g3t::net::NpcRecord{}.combatTarget) == 4);
    static_assert(sizeof(g3t::net::NpcRecord{}.hitSequence) == 2);
    static_assert(sizeof(g3t::net::MeleeContact) == 39);
    for (int attitude = None; attitude <= Panic; ++attitude) {
        for (int focus = None; focus <= Panic; ++focus) {
            Victim v;
            v.hasFocus = true;
            v.relation = attitude;
            v.focusRelation = focus;
            auto expected = attitude < focus && attitude < Hostile ? Decision::Bystander : Decision::Allow;
            assert(Judge(v) == expected);
            v.combat = true;
            v.attitude = attitude;
            v.focusAttitude = focus;
            expected = attitude != None && attitude < Hostile && attitude < focus ? Decision::Bystander
                : attitude == Friendly ? Decision::FriendlyCombatant
                : attitude == Neutral ? Decision::NeutralCombatant : Decision::Allow;
            assert(Judge(v) == expected);
            v.targetsPlayer = true;
            assert(Judge(v) == Decision::Allow);  // the current opponent can defend itself
            v.targetOnly = true;
            assert(Judge(v) == Decision::TargetOnly);
            v.targeted = true;
            assert(Judge(v) == Decision::Allow);
        }
        Victim v;
        v.combat = true;
        v.attitude = attitude;
        v.hasTarget = true;
        v.targetAttitude = Hostile;
        assert(Judge(v) == (attitude < Hostile ? Decision::FightingEnemy : Decision::Allow));
        v.hasFocus = v.focused = v.targeted = true;
        assert(Judge(v) == (attitude < Hostile ? Decision::FightingEnemy : Decision::Allow));
        v.targetAttitude = Angry;  // the native comparison is exactly Hostile, not >= Angry
        assert(Judge(v) == (attitude == Friendly ? Decision::FriendlyCombatant : Decision::Allow));
        v.targetsPlayerParty = true;
        assert(Judge(v) == Decision::Allow);
    }
    Victim v;
    v.attitude = v.relation = Friendly;
    v.focusAttitude = v.focusRelation = Hostile;
    v.hasFocus = true;
    assert(Judge(v) == Decision::Bystander);  // rebel beside an orc, outside combat
    v.combat = true;
    assert(Judge(v) == Decision::Bystander);  // rebel beside an orc, in combat
    v.hasFocus = v.focused = v.targeted = true;
    assert(Judge(v) == Decision::FriendlyCombatant);
    v.item = false;
    assert(Judge(v) == Decision::Allow);  // native item-only gates do not apply to a creature's body
    v.item = true;
    v.combat = false;
    assert(Judge(v) == Decision::Allow);  // deliberate friendly attack outside another fight
    v.hasFocus = v.focused = v.targeted = false;
    assert(Judge(v) == Decision::Allow);  // no invented blanket friendly-fire prohibition
    v.partyMember = true;
    assert(Judge(v) == Decision::Party);
    v.targeted = true;
    assert(Judge(v) == Decision::Allow);  // separate existing player friendly-fire setting still applies
    v.combat = true;
    assert(Judge(v) == Decision::FriendlyCombatant);
    v.targetsPlayer = true;
    assert(Judge(v) == Decision::Allow);

    v = Victim{};
    v.combat = v.hasFocus = true;
    v.attitude = Hostile;
    v.playerLedByVictim = true;
    assert(Judge(v) == Decision::Party);  // guide, not merely a follower
    v.playerLedByVictim = false;
    v.ledByPlayer = true;
    assert(Judge(v) == Decision::Party);
    v.ledByPlayer = false;
    v.attitude = Neutral;
    assert(Judge(v) == Decision::NeutralCombatant);
    v.targeted = true;
    assert(Judge(v) == Decision::Allow);  // deliberate neutral attack
    v.targeted = false;
    v.targetsPlayerParty = true;
    assert(Judge(v) == Decision::Allow);
    v.targetsPlayerParty = false;
    v.attitude = None;
    v.focusAttitude = Hostile;
    assert(Judge(v) == Decision::Allow);  // None exemption is combat-only
    v.combat = false;
    v.relation = None;
    v.focusRelation = Hostile;
    assert(Judge(v) == Decision::Bystander);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cpp = directory / "melee_policy.cpp"
            executable = directory / ("melee_policy.exe" if os.name == "nt" else "melee_policy")
            cpp.write_text(source, encoding="ascii")
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{ROOT / 'src'}", str(cpp), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-I", str(ROOT / "src"), str(cpp), "-o", str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True, timeout=10)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_local_rule_precedes_judgement_flinch_and_perception(self):
        judge = self.combat.split("GEInt Judge(int which", 1)[1].split("// Owner of the NPC:", 1)[0]
        gate = judge.index("!LocalMeleeAllowed(")
        for operation in ("if (which >= 2)", "npcplayback::BeginHit(", "ai::DamagePerception perception", "judge[which]("):
            self.assertLess(gate, judge.index(operation))
        self.assertIn("which == 0 ? other : self", judge)
        self.assertIn("which == 0 ? self : other", judge)
        self.assertIn("judge[which](spu, self, other, args)", judge)  # originals keep their native argument order

    def test_host_revalidation_precedes_all_hit_side_effects(self):
        claim = self.combat.split("void OnHitNpc(", 1)[1].split("void QueueHitNpc(", 1)[0]
        gate = claim.index("if (!verdict.allowed) return;")
        for operation in ("Scaled(", "remote::NoteContact(", "npcplayback::HitOrigin", "struct Pass", "++claimsApplied",
                          'CallScript("AddHitPoints"', "PlayHitEffect(", "React(", "ApplyReaction(", "witness::HitSeen("):
            self.assertLess(gate, claim.index(operation))
        self.assertLess(claim.index("OutsideBout("), claim.index("remote::NoteContact("))
        self.assertIn('if (!attacker) {', claim)

    def test_attitude_query_does_not_redirect_or_register_contact(self):
        query = self.ai.split("bool TryPlayerAttitude(", 1)[1].split("unsigned AttitudeAsked(", 1)[0]
        self.assertIn("guards::Acting acting(player, false)", query)
        self.assertIn("!guards::PlayerContextReady()", query)
        self.assertIn("originalAttitude(nullptr, &npc, &claimant, 0)", query)
        self.assertNotIn("CallScript", query)
        self.assertNotIn("NoteContact", query)
        self.assertIn("Query() { ++playerAttitudeQueries; }", query)
        self.assertIn("~Query() { --playerAttitudeQueries; }", query)
        wrapper = self.ai.split("GEInt GE_STDCALL FilterAttitude(", 1)[1].split("GEInt GE_STDCALL FilterContinue(", 1)[0]
        self.assertLess(wrapper.index("if (playerAttitudeQueries)"), wrapper.index("remote::NoteContact("))

    def test_wire_carries_contact_intent_not_client_eligibility(self):
        contact = self.net_h.split("struct MeleeContact {", 1)[1].split("#pragma pack(pop)", 1)[0]
        self.assertIn("focus[16]", contact)
        self.assertIn("target[16]", contact)
        self.assertIn("motion", contact)
        self.assertNotIn("attitude", contact)
        self.assertNotIn("allowed", contact)
        self.assertIn("constexpr uint16_t ProtocolVersion = saving::Protocol;", self.net)
        self.assertIn("message.incarnation, message.contact", self.net)
        self.assertIn("hit.contact = contact;", self.combat)
        self.assertIn("hit.incarnation, hit.contact", self.combat)

    def test_npc_opponent_replay_and_contact_query_share_collision_checked_identity(self):
        self.assertIn("r.combatTarget = net::HashGuid(targetGuid)", self.npcs)
        self.assertIn("m.shownCombatTarget = shown.combatTarget;", self.npcs)
        query = self.npcs.split("bool CombatTarget(", 1)[1].split("bool PresentationPending(", 1)[0]
        self.assertIn("NpcOpponent(IndexNpcOpponents(), mirror.shownCombatTarget)", query)
        self.assertIn("return target != nullptr;", query)
        self.assertNotIn("SetEntity", query)
        self.assertNotIn("SetCurrentTarget", query)
        index = self.npcs.split("NpcOpponents IndexNpcOpponents()", 1)[1].split("struct TargetingWatch", 1)[0]
        self.assertIn("net::HashGuid(bytes)", index)
        self.assertIn("inserted.first->second != entity", index)
        self.assertIn("inserted.first->second = nullptr", index)
        replay = self.npcs.split("m.shownCombatTarget = shown.combatTarget;", 1)[1].split("auto* receiver =", 1)[0]
        self.assertIn("if (!m.killed)", replay)
        self.assertIn("shown.combatTarget ? NpcOpponent(opponents, shown.combatTarget)", replay)
        self.assertIn("AccessCurrentTargetEntity().SetEntity(opponent)", replay)
        self.assertNotIn("CallScript", replay)
        self.assertNotIn("SetCurrentTarget", replay)
        judge = self.combat.split("MeleeVerdict JudgeMelee(", 1)[1].split("void LogMelee(", 1)[0]
        self.assertIn("!npcs::Ready(result.focus)", judge)
        self.assertIn("!npcs::Ready(opponent)", judge)
        self.assertLess(judge.index('result.decision = "unready-relations"'), judge.index("v.attitude = RawAttitude("))

    def test_contact_context_and_dedup_are_retained_until_send_then_cleared(self):
        self.assertIn("context->second.contact.motion == npcplayback::MotionInstance(localMeleeActor)", self.combat)
        self.assertIn("contact = context->second.contact;", self.combat)
        self.assertIn("playerClaims[claimKey] = swing;", self.combat)
        self.assertIn("playerMeleeContacts.clear();", self.combat)
        self.assertIn("refusedMelee.clear();", self.combat)
        self.assertIn("playerMeleeContacts.at(KillKey(guid)).verdict", self.combat)

    def test_tail_contacts_are_classified_and_local_geometry_bout_gates_remain(self):
        classify = self.combat.split("bool IsLocalMelee(", 1)[1].split("net::MeleeContact CaptureMelee(", 1)[0]
        self.assertNotIn("LocallyAttacking", classify)
        self.assertIn("eEPropertySetType_Projectile", classify)
        self.assertIn("Interaction.GetSpell()", classify)
        self.assertIn('result.decision = "outside-bout";', self.combat)
        local = self.combat.split("bool LocalMeleeAllowed(", 1)[1].split("// Stufe 4.1:", 1)[0]
        self.assertIn("!Entity(attacker).IsInFOV(Entity(victim))", local)
        self.assertIn("verdict.allowed && contact.item", local)
        self.assertLess(local.index("IsInFOV("), local.index("playerMeleeContacts[key]"))


if __name__ == "__main__":
    unittest.main()
