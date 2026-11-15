import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys
import unittest

from test_downed_policy import function_body


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPC_RETARGET_TARGET = _testbuild.register_target(
    "npc-retarget",
    [
        _testbuild.unit(
            "tests/npc_retarget_policy_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcRetargetTests(unittest.TestCase):
    def test_selection_and_episode_policy_without_engine(self):
        ran = NPC_RETARGET_TARGET.run(timeout=30)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_native_cancel_and_attack_guards_precede_fight_exit(self):
        ai = _testbuild.src_text("ai.cpp")
        fightmem = _testbuild.src_text("fightmem.cpp")
        release = function_body(ai, r"\bbool\s+ReleaseDownedTargets\s*\(")
        self.assertLess(release.index("DownedReleaseScope scope"), release.index("npc.Routine.FullStop()"))
        self.assertLess(release.index("instance == releasingDownedNpc"), release.index("if (!bindings)"))
        cancel = function_body(fightmem, r"\bGEInt\s+GE_STDCALL\s+Cancel\s*\(")
        self.assertLess(cancel.index("RetargetDownedBeforeCancel"), cancel.index("guards::Acting"))
        self.assertLess(cancel.index("RetargetDownedBeforeCancel"), cancel.index("const GEInt result = originalCancel"))
        guard = function_body(ai, r"\bGEInt\s+GE_STDCALL\s+FilterRetargetAttack\s*\(")
        self.assertLess(guard.index("ReleaseDownedTargets"), guard.index("return originalRetargetAttack"))
        wrap = function_body(ai, r"\bvoid\s+WrapRetargetAttack\s*\(")
        for required in ("0x147C0", "std::memcmp(code, head, sizeof(head))", "JumpTo(", "TrackPointer(", "UNAVAILABLE"):
            self.assertIn(required, wrap)
        choose = function_body(ai, r"\bEntity\s+SelectReplacement\s*\(")
        self.assertLess(choose.index("EligibleOpponent"), choose.index("select(npc)"))
        self.assertIn("ReplacementEnemy(npc.GetInstance(), native, false)", choose)
        self.assertIn("ReplacementEnemy(npc.GetInstance(), alternative, true)", choose)
        self.assertIn("remote::ControlledBody()", choose)
        self.assertIn("remote::PuppetOf(id)", choose)
        eligible = function_body(ai, r"\bbool\s+EligibleOpponent\s*\(")
        self.assertIn("downed::CombatUnavailable(instance)", eligible)
        self.assertIn("npclease::ContactAllowed", eligible)
        replacement = function_body(ai, r"\bbool\s+ReplacementEnemy\s*\(")
        for required in ("EligibleOpponent", "VisibleFallback", "IsFreeLOS", 'bCString("GetAttitude")', "EnemyAttitude"):
            self.assertIn(required, replacement)
        for required in ("DOWNEDRETARGET", "old=", "new=", "reason=", "task=", "state=", "weapon="):
            self.assertIn(required, release)

    def test_publication_after_release_and_passive_mirror_target_timeline(self):
        script = _testbuild.src_text("Script_G3Together.cpp")
        self.assertLess(script.index("&g3t::remote::Tick"), script.index("&g3t::ai::Tick"))
        self.assertLess(script.index("&g3t::ai::Tick"), script.index("&g3t::npcs::Tick"))
        npcs = _testbuild.src_text("npcs.cpp")
        for required in ("self.NPC.GetCurrentTarget()", "r.combatTarget = net::HashGuid(targetGuid)",
                         "shown.combatTarget = presentation.motion.combatTarget",
                         "routine->AccessAIMode().AccessValue()", "npcProperties->AccessCurrentTargetEntity().SetEntity(opponent)"):
            self.assertIn(required, npcs)
        policy = _testbuild.src_text("npcsend_policy.h")
        essential = function_body(policy, r"\bbool\s+EssentialChanged\s*\(")
        self.assertIn("0xBFu", essential)
        self.assertIn("sent.aiMode != current.aiMode", essential)
        self.assertIn("sent.combatTarget != current.combatTarget", essential)

    def test_host_hit_attacker_history_and_stumble_puppet_scope(self):
        claims = _testbuild.src_text("combat_claims.cpp")
        fightmem = _testbuild.src_text("fightmem.cpp")
        guards = _testbuild.src_text("guards.cpp")

        claim = claims.split("void OnHitNpc(", 1)[1].split("void QueueHitNpc(", 1)[0]
        shift_last = claim.index("target.NPC.SetLastAttacker(target.NPC.GetCurrentAttacker());")
        set_cur = claim.index("target.NPC.SetCurrentAttacker(Entity(attacker));")
        apply_rx = claim.index("ApplyReaction(target, attacker, reaction);")
        plan_call = claim.index("combat_retarget::PlanHostNpcHit(")
        self.assertLess(shift_last, set_cur)
        self.assertLess(set_cur, apply_rx)
        self.assertLess(apply_rx, plan_call)
        self.assertIn("if (plan.immediateCombatTarget)", claim)
        self.assertIn("if (plan.runImmediateAssessAgressor) React(target, attacker);", claim)

        state_actor = function_body(fightmem, r"\beCEntity\*\s+StateActor\s*\(")
        self.assertIn("StumbleReactionState(state)", state_actor)
        self.assertIn("AttackerOf(self)", state_actor)
        stumble_pred = function_body(fightmem, r"\bbool\s+StumbleReactionState\s*\(")
        for task in ("ZS_Stumble", "ZS_QuickStumble", "ZS_PierceStumble", "ZS_SitKnockDown", "ZS_StandUp"):
            self.assertIn(task, stumble_pred)

        follows = function_body(guards, r"\bbool\s+FollowsPlayer\s*\(")
        self.assertIn("ZS_ReactToDamage", follows)
        guard_actor = function_body(guards, r"\beCEntity\*\s+StateActor\s*\(")
        self.assertIn("GetCurrentAttacker()", guard_actor)

    def test_real_native_selector_and_attack_signatures_read_only(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("Set G3T_GAME_DLL to check the actual companion Script_Game.dll read-only")
        image = Path(game).parent / "scripts" / "Script_Game.dll"
        data = _testbuild.cached_bytes(image)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "cdd9c46120abac49ebb6d53e5bed79512a528e5940238d6d88d150fe9aef3cdc")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        sections = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
        count = struct.unpack_from("<H", data, pe + 6)[0]

        def read(rva, size):
            for index in range(count):
                virtual_size, virtual, raw_size, raw = struct.unpack_from("<IIII", data, sections + index * 40 + 8)
                if virtual <= rva and rva + size <= virtual + min(virtual_size, raw_size):
                    offset = raw + rva - virtual
                    return data[offset:offset + size]
            self.fail(f"Unmapped native RVA {rva:#x}")

        self.assertEqual(read(0xA070, 17), bytes.fromhex("81 ec e0 08 00 00 53 55 56 8b b4 24 f0 08 00 00 57"))
        self.assertEqual(read(0xAF1B, 3), bytes.fromhex("c2 ac 00"))
        self.assertEqual(read(0x147C0, 12), bytes.fromhex("55 8b ec 83 e4 f8 81 ec 54 03 00 00"))
        self.assertEqual(read(0x14D54, 3), bytes.fromhex("c2 10 00"))
        self.assertEqual(read(0x1A792, 6), bytes.fromhex("81 ec a8 00 00 00"))
        self.assertEqual(read(0x1A7B0, 1), b"\xe8")
        self.assertEqual(0x1A7B5 + struct.unpack("<i", read(0x1A7B1, 4))[0], 0xA070)


if __name__ == "__main__":
    unittest.main()
