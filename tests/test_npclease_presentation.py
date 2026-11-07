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
        idle = source[source.index("bool BootstrapIdle("):source.index("\n}\n\nbool LocalHit(")]
        for required in ("GetAniName@gCScriptProcessingUnit", "GetAniVariation",
                         "QueryMotionDataEntity", "IdleResourceName", "SetPlayTime",
                         "ActorProcess(", "evaluation.evaluated", "PoseProvenance::Idle"):
            self.assertIn(required, idle)
        for forbidden in ("ContinueRoutine", "sAIPlayAni", "SetTask", "Movement", "OnPreProcess",
                          "UpdatePresentation("):
            self.assertNotIn(forbidden, idle)
        replay = source[source.index("bool Replay("):source.index("bool ReplayFrozenPose(")]
        self.assertIn("!hostPose.evaluated || !hostPose.applied || !hostPose.valid", replay)
        self.assertIn("PoseProvenance::Canonical", replay)
        self.assertGreater(replay.index("PoseProvenance::Canonical"), replay.index("!hostPose.evaluated"))
        hold = source[source.index("bool HoldUnavailable("):source.index("void ReleaseUnavailable(")]
        self.assertIn("HostPresentationAvailable", hold)
        self.assertIn("proof.Usable", hold)
        self.assertNotIn("CapturePose(", hold)

    def test_fallback_scans_processed_npcs_and_bounds_resource_retries(self):
        source = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        tick = source[source.index("void Tick()"):]
        self.assertIn("world::ProcessedEntities()", tick)
        self.assertNotIn("Entity::GetNPCs()", tick)
        self.assertLess(tick.index("eEPropertySetType_NPC"), tick.index("Entity npc(entity)"))
        self.assertIn("held.failed && cleanupNow - held.attempted < 1000", tick)
