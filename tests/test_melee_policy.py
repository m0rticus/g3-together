import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

MELEE_POLICY_SOURCE = r'''
#include "melee_policy.h"
#include "net.h"
#include "npcwire_control.h"
#include <cassert>
using namespace g3t::melee;
int main()
{
    static_assert(g3t::net::NpcDatagramBudget == 1172);
    static_assert(g3t::net::NpcsPerSend <= g3t::net::MaxNpcsPerMessage);
    static_assert(23 + sizeof(g3t::npcwire::WireBatchHeader) +
                  sizeof(g3t::npcwire::WireControl) + 8 * sizeof(g3t::npcwire::WireMotionTick) +
                  sizeof(g3t::npcwire::WireQuaternion) <= g3t::net::NpcDatagramBudget);
    static_assert(sizeof(g3t::net::NpcRecord) == 588);
    static_assert(sizeof(g3t::net::NpcRecord{}.combatTarget) == 4);
    static_assert(sizeof(g3t::net::NpcRecord{}.hitSequence) == 2);
    static_assert(sizeof(g3t::net::MeleeContact) == 39);
    static_assert(JudgeClient(false, false) == ClientDecision::HostJudge);
    static_assert(JudgeClient(true, false) == ClientDecision::HostJudge);
    static_assert(JudgeClient(false, true) == ClientDecision::Reject);
    static_assert(JudgeClient(true, true) == ClientDecision::Predict);
    static_assert(EvaluatePlayerMelee(0, true, true) == ClientDecision::Predict);
    static_assert(EvaluatePlayerMelee(1, false, false) == ClientDecision::HostJudge);
    static_assert(EvaluatePlayerMelee(3, true, true) == ClientDecision::Reject);
    static_assert(ResolveOutgoingHitIncarnation(0, 0, 0) == UnstreamedHitIncarnation);
    static_assert(ResolveOutgoingHitIncarnation(7, 0, 0) == 7);
    static_assert(ResolveOutgoingHitIncarnation(0, 12, 0) == 0);
    static_assert(ResolveOutgoingHitIncarnation(0, 0, 1) == 0);
    static_assert(AcceptIncomingHitIncarnation(UnstreamedHitIncarnation, 0, 0, 42));
    static_assert(!AcceptIncomingHitIncarnation(UnstreamedHitIncarnation, 1, 0, 42));
    static_assert(!AcceptIncomingHitIncarnation(UnstreamedHitIncarnation, 0, 1, 42));
    static_assert(AcceptIncomingHitIncarnation(42, 10, 1, 42));
    static_assert(!AcceptIncomingHitIncarnation(41, 10, 1, 42));
    assert(ClaimedSwing(2082, 0, 2082)); // first snapshot must not permit the same unknown-life swing twice
    assert(ClaimedSwing(2082, 2082, 0));
    assert(!ClaimedSwing(2083, 2082, 2082));
    assert(!ClaimedSwing(0, 0, 0));
    g3t::net::MeleeContact contact;
    contact.melee = g3t::net::HostJudgedMelee;
    contact.motion = 2082;
    for (auto life : {0u, 1u, 123u}) {
        assert(g3t::net::ValidHitContact(0, life, 0, 0, 0, contact));
        assert(!g3t::net::ValidHitContact(-1, life, 0, 0, 0, contact));
        assert(!g3t::net::ValidHitContact(1, life, 0, 0, 0, contact));
        assert(!g3t::net::ValidHitContact(0, life, 0, 1, 0, contact));
        assert(!g3t::net::ValidHitContact(0, life, 0, 0, 1, contact));
    }
    assert(!g3t::net::ValidHitContact(0, 0, 1, 0, 0, contact)); // unknown life cannot carry old presentation
    contact.motion = 0;
    assert(!g3t::net::ValidHitContact(0, 1, 0, 0, 0, contact));
    contact.motion = 1;
    contact.melee = g3t::net::PredictedMelee;
    assert(g3t::net::ValidHitContact(-1, 1, 0, 0, 0, contact));
    assert(!g3t::net::ValidHitContact(-1, 0, 0, 0, 0, contact));
    assert(!g3t::net::ValidHitContact(0, 1, 0, 0, 0, contact));
    assert(!g3t::net::ValidHitContact(-1000001, 1, 0, 0, 0, contact));
    contact.melee = 3;
    assert(!g3t::net::ValidHitContact(-1, 1, 0, 0, 0, contact));
    for (int attitude = None; attitude <= Panic; ++attitude) {
        for (int focus = None; focus <= Panic; ++focus) {
            Victim v;
            v.hasFocus = true;
            v.relation = attitude;
            v.focusRelation = focus;
            auto expected = attitude < focus && attitude < Hostile ? Decision::Bystander : Decision::Allow;
            assert(Judge(v) == expected);
            assert(JudgeClient(Judge(v) == Decision::Allow, true) ==
                   (expected == Decision::Allow ? ClientDecision::Predict : ClientDecision::Reject));
            assert(JudgeClient(Judge(v) == Decision::Allow, false) == ClientDecision::HostJudge);
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

MELEE_POLICY_TARGET = _testbuild.register_target(
    "melee-policy",
    [
        _testbuild.inline_unit(
            "melee_policy.cpp",
            MELEE_POLICY_SOURCE,
            std="c++17",
            warnings="none",
            includes=(ROOT / "src",),
        )
    ],
)


class MeleePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.combat = _testbuild.src_text("combat.cpp")
        cls.combat_melee = _testbuild.src_text("combat_melee.cpp")
        cls.combat_claims = _testbuild.src_text("combat_claims.cpp")
        cls.combat_kill_xp = _testbuild.src_text("combat_kill_xp.cpp")
        cls.combat_all = "\n".join((cls.combat, cls.combat_melee, cls.combat_claims, cls.combat_kill_xp))
        cls.net = _testbuild.src_text("net.cpp")
        cls.net_h = _testbuild.src_text("net.h")
        cls.npcs = _testbuild.src_text("npcs.cpp")
        cls.ai = _testbuild.src_text("ai.cpp")

    def test_native_relational_rule_executes_without_the_engine(self):
        executed = MELEE_POLICY_TARGET.run(timeout=10)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_local_rule_precedes_judgement_flinch_and_perception(self):
        judge = self.combat.split("GEInt Judge(int which", 1)[1].split("GEInt GE_STDCALL FilterLogical(", 1)[0]
        gate = judge.index("!LocalMeleeAllowed(")
        for operation in ("if (which >= 2)", "npcplayback::BeginHit(", "ai::DamagePerception perception", "judge[which]("):
            self.assertLess(gate, judge.index(operation))
        self.assertIn("which == 0 ? other : self", judge)
        self.assertIn("which == 0 ? self : other", judge)
        self.assertIn("judge[which](spu, self, other, args)", judge)  # originals keep their native argument order
        self.assertNotIn(".at(", judge)

    def test_host_revalidation_precedes_all_hit_side_effects(self):
        claim = self.combat_claims.split("void OnHitNpc(", 1)[1].split("void QueueHitNpc(", 1)[0]
        fence = claim.index("if (!net::InWorld(from) || downed::CombatUnavailable(attacker)")
        gate = claim.index("melee::EvaluatePlayerMelee(contact.melee, verdict.allowed, verdict.relationsKnown)")
        self.assertLess(fence, claim.index("if (contact.melee)"))
        self.assertIn("summon::HitBlocked(target.GetInstance())", claim)
        for operation in ("Scaled(", "remote::NoteContact(", "npcplayback::HitOrigin", "HitDispatchScope pass", "++claimsApplied",
                          'CallScript("AddHitPoints"', "PlayHitEffect(", "React(", "ApplyReaction(", "witness::HitSeen("):
            self.assertLess(gate, claim.index(operation))
        self.assertLess(claim.index("OutsideBout("), claim.index("remote::NoteContact("))
        self.assertIn('if (!attacker) {', claim)
        self.assertIn("if (paysXp && target.IsDead()) RewardKill(from, guid);", claim)

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
        self.assertIn("hit.contact = contact;", self.combat_all)
        self.assertIn("hit.incarnation, hit.contact", self.combat_all)

    def test_npc_opponent_replay_and_contact_query_share_collision_checked_identity(self):
        self.assertIn("r.combatTarget = net::HashGuid(targetGuid)", self.npcs)
        self.assertIn("m.shownCombatTarget = shown.combatTarget;", self.npcs)
        query = self.npcs.split("bool CombatTarget(", 1)[1].split("void AwaitHit(", 1)[0]
        self.assertIn("NpcOpponent(IndexNpcOpponents(), record->combatTarget)", query)
        self.assertIn("CombatRecord(entity)", query)
        self.assertNotIn("Ready(", query)
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
        judge = self.combat_melee.split("MeleeVerdict JudgeMelee(", 1)[1].split("void LogMelee(", 1)[0]
        self.assertNotIn("npcs::Ready(", judge)
        self.assertIn("npcs::CombatRecord(result.focus)", judge)
        self.assertIn("npcs::CombatRecord(opponent)", judge)
        self.assertLess(judge.index('result.decision = "missing-relation-record"'), judge.index("v.attitude = RawAttitude("))

    def test_incomplete_contacts_are_sent_before_local_damage_or_reaction(self):
        judge = self.combat.split("GEInt Judge(int which", 1)[1].split("GEInt GE_STDCALL FilterLogical(", 1)[0]
        send = judge.index("SendHostJudgedMelee(")
        for operation in ("npcplayback::BeginHit(", "ai::DamagePerception perception", "judge[which](spu"):
            self.assertLess(send, judge.index(operation))
        self.assertNotIn("npcs::Ready(", judge)
        submit = self.combat_melee.split("bool SendHostJudgedMelee(", 1)[1].split("void PlayHitEffect(", 1)[0]
        self.assertIn("ClaimedPlayerSwing(identity, contact.motion)", submit)
        self.assertIn("contact.melee = net::HostJudgedMelee;", submit)
        self.assertIn("identity, 0, weapon, nullptr, net::HitUnjudged", submit)
        self.assertIn("net::HitNoReaction", submit)
        self.assertLess(submit.index("if (!hit) return false;"), submit.index("playerClaims[key] = contact.motion"))
        self.assertIn("npcs::AwaitHit(victim, hit)", submit)
        for forbidden in ("GameHit(", "npcs::LocalHit(", "PlayHitEffect(", "BeginHit(", "DamagePerception"):
            self.assertNotIn(forbidden, submit)
        host = self.combat_claims.split("void OnHitNpc(", 1)[1].split("void QueueHitNpc(", 1)[0]
        gate = host.index("melee::EvaluatePlayerMelee(contact.melee, verdict.allowed, verdict.relationsKnown)")
        self.assertLess(gate, host.index("JudgeDamage("))
        self.assertLess(host.index("IgnoreFriendlyHit("), host.index("JudgeDamage("))
        self.assertLess(host.index("OutsideBout("), host.index("JudgeDamage("))
        self.assertLess(host.index("bool finishing = false;"), host.index("JudgeDamage("))
        self.assertLess(host.index("if (target.IsDown() || since < 3000)"), host.index("JudgeDamage("))
        self.assertIn("const uint32_t hostIncarnation = npcs::Incarnation(guid);", host)
        self.assertIn("reply.result.incarnation = hostIncarnation;", host)
        self.assertIn("delta = -pending;", host)

    def test_visible_player_sweeps_and_notice_do_not_depend_on_replay_success(self):
        record = self.npcs.split("net::NpcRecord const* CombatRecord(", 1)[1].split("bool CombatTarget(", 1)[0]
        self.assertNotIn("replayReady", record)
        self.assertIn("hostSession != net::WelcomeSerial()", record)
        self.assertIn("latest.incarnation != Incarnation(identity)", record)
        sweep = self.npcs.split("void TryPlayerBlow(", 1)[1].split("void LogClientDeath(", 1)[0]
        self.assertNotIn("replayReady", sweep)
        self.assertIn("SweepsBody(victim", sweep)
        self.assertIn("ClearContact(hero, victim)", sweep)
        tick = self.npcs.split("void PassiveMirrorTick()", 1)[1].split("bool MirrorFightsHero()", 1)[0]
        self.assertIn("npcplayback::IsPassive(victim)) TryPlayerBlow(victim)", tick)
        blow = self.combat_melee.split("bool PlayerBlow(", 1)[1]
        self.assertNotIn("npcs::Ready(", blow)
        self.assertLess(blow.index("SendHostJudgedMelee("), blow.index("GameHit("))
        replay = _testbuild.src_text("npcplayback.cpp")
        self.assertNotIn("NpcStatePending", replay)
        self.assertNotIn("presentationMissingAt", replay)
        self.assertNotIn("Waiting for NPC presentation", _testbuild.src_text("i18n.cpp"))

    def test_resync_republishes_gameplay_and_queue_failure_is_not_counted_as_success(self):
        signal = self.net.split("} else if (type == MsgControlSignal)", 1)[1].split("} else if (type == MsgBoneDictionary)", 1)[0]
        self.assertIn("SendFullNpc(peers[peerId], state->second.latest, true)", signal)
        send = self.net.split("uint32_t SendHitNpc(", 1)[1].split("void SetHitResultHandler(", 1)[0]
        self.assertIn("if (!Send(server, &message, sizeof(message), ChannelCombat, true))", send)
        self.assertIn("ValidHitContact(", send)

    def test_contact_context_and_dedup_are_retained_until_send_then_cleared(self):
        self.assertIn("context->second.contact.motion == npcplayback::MotionInstance(localMeleeActor)", self.combat_all)
        self.assertIn("contact = context->second.contact;", self.combat_all)
        self.assertIn("playerClaims[claimKey] = swing;", self.combat_all)
        self.assertIn("playerMeleeContacts.clear();", self.combat_all)
        self.assertIn("refusedMelee.clear();", self.combat_all)
        self.assertIn("playerMeleeContacts.find(KillKey(identity))", self.combat_all)
        self.assertNotIn(".at(", self.combat_all)

    def test_tail_contacts_are_classified_and_local_geometry_bout_gates_remain(self):
        classify = self.combat_melee.split("bool IsLocalMelee(", 1)[1].split("net::MeleeContact CaptureMelee(", 1)[0]
        self.assertNotIn("LocallyAttacking", classify)
        self.assertIn("eEPropertySetType_Projectile", classify)
        self.assertIn("Interaction.GetSpell()", classify)
        self.assertIn('result.decision = "outside-bout";', self.combat_melee)
        local = self.combat_melee.split("bool LocalMeleeAllowed(", 1)[1].split("bool SendHostJudgedMelee(", 1)[0]
        self.assertIn("!Entity(attacker).IsInFOV(Entity(victim))", local)
        self.assertIn("contact.item && !Entity(attacker).IsInFOV(Entity(victim))", local)
        self.assertLess(local.index("IsInFOV("), local.index("playerMeleeContacts[key]"))


if __name__ == "__main__":
    unittest.main()
