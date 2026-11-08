import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcLeasePresentationTests(unittest.TestCase):
    def test_unavailable_policy(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix=".npclease-presentation-", dir=ROOT / "tests") as directory:
            build = Path(directory)
            executable = build / ("presentation.exe" if os.name == "nt" else "presentation")
            source = ROOT / "tests" / "npclease_presentation_test.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", str(source),
                           f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source),
                           "-o", str(executable)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=build, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_release_keeps_presentation_and_detach_is_inert(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        passive = source[source.index("void PassiveMirrorTick()"):source.index("bool MirrorFightsHero()")]
        self.assertNotIn("if (m.released) { it = mirrors.erase(it); continue; }", passive)
        self.assertIn("if (!HostPresentationAvailable(m.entity.GetInstance()))", passive)
        self.assertIn("m.deathLife.Observe(m.latest.incarnation, true)", passive)
        detach = source[source.index("void LeaseReleaseMirror("):source.index("uint32_t HostPresentationAge(")]
        self.assertNotIn("body::Release(", detach)
        self.assertIn("mirror.defeatedBefore", detach)
        self.assertIn("strncpy_s(m.body.routine", passive)

    def test_hold_does_not_kill_or_restart_ai(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        hold = source[source.index("bool HoldUnavailable("):source.index("void UpdatePresentation(")]
        self.assertIn("npc.IsDead() || npc.IsDown()", hold)
        self.assertIn("ReplayFrozenPose(", hold)
        self.assertNotIn("ContinueRoutine", hold)
        self.assertNotIn("HitPoints =", hold)
        self.assertNotIn("Kill", hold)

    def test_idle_bootstrap_is_evaluated_and_script_free(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        idle = source[source.index("eCResourceDataEntity* NativeMotion("):source.index("bool CosmeticHit(")]
        for required in ("npc.GetAni(action, phase)", "GetAniVariation",
                         "QueryMotionDataEntity", "gEUseType_None", "EvaluateIdle(",
                         "gEAction_Ambient, gEPhase_Loop", "PoseProvenance::Idle"):
            self.assertIn(required, idle)
        for forbidden in ("ContinueRoutine", "sAIPlayAni", "SetTask", "Movement", "OnPreProcess",
                          "UpdatePresentation("):
            self.assertNotIn(forbidden, idle)
        self.assertNotIn('direction("None")', idle)
        replay = source[source.index("bool Replay("):source.index("bool ReplayFrozenPose(")]
        self.assertIn("!hostPose.evaluated || !hostPose.applied || !hostPose.valid", replay)
        self.assertIn("PoseProvenance::Canonical", replay)
        self.assertGreater(replay.index("PoseProvenance::Canonical"), replay.index("!hostPose.evaluated"))
        hold = source[source.index("bool HoldUnavailable("):source.index("void ReleaseUnavailable(")]
        self.assertIn("HostPresentationAvailable", hold)
        self.assertIn("proof.Usable", hold)
        self.assertNotIn("CapturePose(", hold)
        evaluate = source[source.index("bool EvaluateIdle("):source.index("bool IdleFailure(")]
        for required in ("SetPlayTime", "IdleTime(elapsed, duration)", "ActorProcess(",
                         "evaluation.evaluated", "GETrue, GEFalse, 0.0f"):
            self.assertIn(required, evaluate)
        self.assertIn("EvaluateIdle(entity, *animation, net::Now()", hold)

    def test_cosmetic_contact_is_script_free_finite_and_host_superseded(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        hit = source[source.index("bool CosmeticHit("):source.index("bool LocalHit(")]
        for required in ("HostPresentationAvailable", "npclease::Fenced", "RedundantClientEntity",
                         "waiting.hitSwing == swing", "gEAction_QuickStumble, gEPhase_Hit",
                         "PoseProvenance::Reaction", "EvaluateIdle(entity, *animation, 1, false)"):
            self.assertIn(required, hit)
        for forbidden in ("ContinueRoutine", "HitPoints =", "Kill(", "SendHitNpc", "SetTask"):
            self.assertNotIn(forbidden, hit)
        hold = source[source.index("bool HoldUnavailable("):source.index("void ReleaseUnavailable(")]
        self.assertIn("reactionDuration", hold)
        self.assertLess(hold.index("HostPresentationAvailable"), hold.index("PoseProvenance::Reaction"))
        combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        contact = combat[combat.index("bool SendHostJudgedMelee("):combat.index("// Stufe 4.1:")]
        self.assertLess(contact.index("CosmeticHit"), contact.index("net::SendHitNpc"))
        self.assertIn("contact.motion", contact)
        self.assertIn("visual=%u", source)

    def test_fallback_scans_processed_npcs_and_bounds_resource_retries(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        tick = source[source.index("void Tick()"):]
        self.assertIn("world::ProcessedEntities()", tick)
        self.assertNotIn("Entity::GetNPCs()", tick)
        self.assertLess(tick.index("eEPropertySetType_NPC"), tick.index("Entity npc(entity)"))
        hold = source[source.index("bool HoldUnavailable("):source.index("void ReleaseUnavailable(")]
        self.assertIn("waiting.retry && now - waiting.attempted < 1000", hold)
        self.assertLess(hold.index("waiting.retry &&"), hold.index("BootstrapIdle(entity"))
        self.assertLess(hold.index("provenPoses.find(key)"), hold.index("BootstrapIdle(entity"))

    def test_missing_pose_never_changes_rendering(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        self.assertNotIn("EnableRendering(", source)
        self.assertIn("retaining visibility contacts=off", source)
        self.assertIn("PresentationCensus(cleanupNow)", source)
        for counter in ("host=%u", "held=%u", "idle=%u", "native=%u", "unproven=%u",
                        "hidden=%u", "static=%u", "top_reason=%s"):
            self.assertIn(counter, source)
        tick = source[source.index("void Tick()"): ]
        self.assertLess(tick.index("if (!IsCharacter(entity))"), tick.index("Entity npc(entity)"))
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        send = npcs[npcs.index("void SendOwned("):npcs.index("const bTObjArray<Entity> all", npcs.index("void SendOwned("))]
        self.assertIn("npcplayback::IsCharacter(instance)", send)

    def test_native_pose_is_preserved_before_creature_stop(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        native = source[source.index("bool RememberNativePresentation("):source.index("bool CosmeticHit(")]
        for required in ("GetTargetWeight", "GetPlayTime", "originalActorProcess(",
                         "GETrue, GEFalse, 0.0f", "PoseProvenance::Native"):
            self.assertIn(required, native)
        runtime = (ROOT / "src" / "npclease_runtime.cpp").read_text(encoding="utf-8")
        stop = runtime[runtime.index("void Stop("):runtime.index("bool Import(")]
        self.assertIn("if (!entity || Player(entity) || !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;", stop)
        self.assertLess(stop.index("RememberNativePresentation(entity)"), stop.index("npc.Routine.FullStop()"))
        imported = runtime[runtime.index("bool Import("):runtime.index("bool Activate(")]
        self.assertLess(imported.index("Stop(entity)"), imported.index("entity->SetToWorldMatrix(pose)"))
