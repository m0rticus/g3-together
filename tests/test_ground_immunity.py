import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_downed_policy import function_body


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
from _g3copy import win32_tool_environment


class GroundImmunityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = {
            name: (ROOT / "src" / f"{name}.cpp").read_text(encoding="utf-8")
            for name in ("downed", "combat", "spells", "npcs", "npcplayback")
        }

    def body(self, source, signature):
        result = function_body(self.sources[source], signature)
        self.assertIsNotNone(result, signature)
        return result

    def test_exact_pose_life_flags_recovery_and_terminal_policy(self):
        with tempfile.TemporaryDirectory(prefix="ground-policy-") as temporary:
            directory = Path(temporary)
            executable = directory / ("ground.exe" if os.name == "nt" else "ground")
            source = str(ROOT / "tests" / "ground_immunity_test.cpp")
            compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++")
            if compiler:
                env = os.environ.copy()
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", source, "-o", str(executable)]
            else:
                env = win32_tool_environment(directory)
                cl = shutil.which("cl", path=env.get("PATH"))
                self.assertIsNotNone(cl, "The engine-free immunity test needs a C++ compiler")
                command = [cl, "/nologo", "/std:c++17", "/EHsc", source, f"/Fe:{executable}", f"/Fo:{directory}\\"]
            compiled = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=directory, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_synthetic_contact_restores_vanilla_pose_gate_before_judgement(self):
        hit = self.body("combat", r"\bGEInt\s+GameHit\s*\(")
        self.assertLess(hit.index("RejectDownedHit"), hit.index("JudgeDamage"))
        self.assertLess(hit.index("SyntheticProne"), hit.index("JudgeDamage"))
        self.assertLess(hit.index("*pending = 0"), hit.index("SyntheticProne"))
        self.assertIn("DamageAmount = 0", hit)
        self.assertIn("vanilla-prone-reject", hit)
        predicate = self.body("downed", r"\bbool\s+SyntheticProne\s*\(")
        self.assertIn("victim == hero || victim == remote::ControlledBody()", predicate)
        self.assertIn("VanillaProne", predicate)
        # Do not add pose-only invulnerability to native DOT/environment/status judgement.
        for entry in ("Judge", "Filter"):
            signature = rf"\bGEInt\s+(?:GE_STDCALL\s+)?{entry}\s*\("
            self.assertNotIn("SyntheticProne", self.body("combat", signature))

    def test_immunity_is_owner_local_and_targeting_remains_separate(self):
        predicate = self.body("downed", r"\bbool\s+HarmProtected\s*\(")
        for required in ("life.held", "life.terminal", "recovery.Protected(net::Now())",
                         "entity == hero || entity == remote::ControlledBody()",
                         "persist::NativeLoading()", "persist::HeroPending()"):
            self.assertIn(required, predicate)
        self.assertNotIn("player.IsDown()", predicate)
        targeting = self.body("downed", r"\bbool\s+CombatUnavailable\s*\(\s*eCEntity")
        self.assertNotIn("recovery", targeting)
        for source in ("remote", "net", "body"):
            # Protocol and pose/heartbeat encoders are not part of the immunity contract.
            self.assertNotIn("HarmImmune", (ROOT / "src" / f"{source}.cpp").read_text(encoding="utf-8"))

    def test_recovery_lifecycle_and_native_standing_detection(self):
        tick = self.body("downed", r"\bvoid\s+Tick\s*\(")
        revive = function_body(tick, r"\bif\s*\(\s*revivedBy\s*\)")
        self.assertLess(revive.index("life.Revive("), revive.index("recovery.Begin()"))
        self.assertLess(revive.index("recovery.Begin()"), revive.index("StandUp(hero)"))
        for required in ("body::CurrentMotion", "RecoveryStanding(pose, hero.IsDown()",
                         "recovery.Observe", "recovery standing", "reason=no-current-motion"):
            self.assertIn(required, tick)
        for function in ("Hold", "Die", "OnWorldChange"):
            self.assertIn("recovery.Clear()", self.body("downed", rf"\bvoid\s+{function}\s*\("))
        reaction = self.body("downed", r"\bbool\s+RejectReaction\s*\(")
        self.assertIn("life.held", reaction)
        self.assertIn("DownTask(state)", reaction)

    def test_puppet_recovery_uses_owner_snapshots_without_changing_target_exclusion(self):
        tracker = self.body("downed", r"\bbool\s+TrackPuppetRecovery\s*\(")
        for required in ("r.connected", "r.latestTicks", "r.latest.world == remote::CurrentWorld()",
                         "peer.Observe", "r.latest.flags", "r.latest.aniState", "r.latest.animation"):
            self.assertIn(required, tracker)
        self.assertNotIn("PuppetOf", tracker) # native talkable AIMode 10 does not describe the owner's recovery
        predicate = self.body("downed", r"\bbool\s+HarmProtected\s*\(")
        self.assertIn("r.latest.flags, TrackPuppetRecovery(id)", predicate)
        targeting = self.body("downed", r"\bbool\s+CombatUnavailable\s*\(\s*eCEntity")
        self.assertIn("CombatUnavailable(true, false, false, false, false, r.latest.flags)", targeting)
        tick = self.body("downed", r"\bvoid\s+Tick\s*\(")
        self.assertIn("TrackPuppetRecovery(i)", tick)
        world = self.body("downed", r"\bvoid\s+OnWorldChange\s*\(")
        self.assertIn("puppetRecovery[i].Clear()", world)

    def test_all_damage_and_effect_entry_points_consult_immunity(self):
        for signature in (r"\bGEInt\s+Judge\s*\(", r"\bGEInt\s+GE_STDCALL\s+Filter\s*\(",
                          r"\bvoid\s+OnHitPlayer\s*\(", r"\bbool\s+LocalBlow\s*\(",
                          r"\bGEInt\s+GE_STDCALL\s+TouchDamage\s*\(", r"\bvoid\s+ApplyReaction\s*\("):
            body = self.body("combat", signature)
            self.assertIn("RejectDownedHit", body, signature)
        touch = self.body("combat", r"\bGEInt\s+GE_STDCALL\s+TouchDamage\s*\(")
        self.assertIn("other ? static_cast<Entity*>(other)->GetInstance()", touch)
        self.assertLess(touch.index("RejectDownedHit"), touch.rindex("originalTouchDamage("))
        self.assertIn('WrapDamageScript(*touch, &TouchDamage, "OnTouchDamage")', self.sources["combat"])
        for signature in (r"\bGEInt\s+GE_STDCALL\s+Wrap\s*\(", r"\bvoid\s+OnNpcSpell\s*\("):
            body = self.body("spells", signature)
            self.assertIn("HarmfulTarget", body)
            self.assertIn("downed::RejectHarm", body)
            self.assertLess(body.index("downed::RejectHarm"), body.rindex("originals["))
        area = self.body("spells", r"\bbTObjArray<Entity>\s+GE_STDCALL\s+AreaTargets\s*\(")
        self.assertIn('downed::RejectHarm(targets.GetAt(i).GetInstance(), areaCaster, "area-spell")', area)
        force = self.body("npcs", r"\bvoid\s+__fastcall\s+HookAddForce\s*\(")
        self.assertLess(force.index("RejectHarm"), force.index("originalAddForce(self, edx, force, at)"))

    def test_native_hp_decreases_and_reaction_bypasses_are_guarded(self):
        for function in ("SetHp", "AddHp", "SetValue"):
            body = self.body("downed", rf"\bGEBool\s+__fastcall\s+{function}\s*\(")
            self.assertIn("RejectHp", body)
            self.assertIn("LogGroundEvent", body)
        hp = self.body("downed", r"\bbool\s+RejectHp\s*\(")
        self.assertIn('tag != "HP" || maintainingHp', hp)
        self.assertIn("additive ? value < 0 : value < before", hp)
        self.assertIn("RejectHarm", hp)
        for function in ("KeepDown",):
            self.assertIn("MaintainHp maintenance", self.body("downed", rf"\bvoid\s+{function}\s*\("))
        guard = self.body("downed", r"\bbool\s+GuardState\s*\(")
        self.assertIn("MaintainHp maintenance", guard)
        self.assertIn("RejectReaction", guard)
        for function in ("SetTask", "SetState"):
            self.assertIn("RejectReaction", self.body("downed", rf"\bvoid\s+__fastcall\s+{function}\s*\("))
        task = self.body("npcplayback", r"\bbool\s+TaskBlocked\s*\(")
        self.assertLess(task.index("RejectReaction"), task.index("Passive(entity)"))
        for method in ("AddBaseValue", "SetValue", "SetBaseValue"):
            self.assertIn(f"?{method}@gCPlayerMemory_PS", self.sources["downed"])

    def test_ground_diagnostics_are_unsampled_and_contain_required_fields(self):
        log = self.body("downed", r"\bvoid\s+LogGroundEvent\s*\(")
        for field in ("GROUNDHIT", "path=%s", "attacker=%s", "hp=%d->%d", "held=%d", "terminal=%d",
                      "rising=%d", "grace_ms=%u", "ai=%d", "ani=%d", "task=%s", "state=%s", "decision=%s"):
            self.assertIn(field, log)
        for forbidden in ("lastLog", "refused <=", "decisions >", "% 40"):
            self.assertNotIn(forbidden, log)


if __name__ == "__main__":
    unittest.main()
