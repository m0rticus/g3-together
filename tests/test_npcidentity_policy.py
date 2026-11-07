import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcIdentityTests(unittest.TestCase):
    def test_production_identity_policy(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix=".npcidentity-", dir=ROOT / "tests") as directory:
            build = Path(directory)
            executable = build / ("identity.exe" if os.name == "nt" else "identity")
            source = ROOT / "tests" / "npcidentity_policy_test.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                           str(source), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           str(source), "-o", str(executable)]
            result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=build, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reconciliation_runs_before_passive_playback(self):
        frame = (ROOT / "src" / "Script_G3Together.cpp").read_text(encoding="utf-8")
        self.assertLess(frame.index("npcs::ReconcileClientEntities()"), frame.index("npcplayback::Tick()"))
        playback = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        tick = playback[playback.index("void Tick()"):]
        self.assertLess(tick.index("RedundantClientEntity(entity)"), tick.index("HoldUnavailable("))
        self.assertLess(tick.index("RedundantClientEntity(entity)"), tick.index("unavailable[key]"))

    def test_census_is_pointer_specific_and_resettable(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        census = source[source.index("void ReconcileClientEntities()"):source.index("bool Ready(")]
        for required in ("Entity::GetEntities()", "world::ProcessedEntities()", "npcidentity::AddPointer",
                         "npcidentity::Quarantine", "ActorDescendant(entity)", "guid_collisions=%u",
                         "duplicates=%u", "NPCNEAR_DIFFERENT_GUID", "NPCDUP_QUARANTINE",
                         "NPCDUP_RESTORE", "saved->second.guid != identity"):
            self.assertIn(required, census)
        self.assertIn("npcremoval::Suppressed(saved->second.rootGuid.data())", census)
        helper = source[source.index("bool RedundantClientEntity("):source.index("void ReconcileClientEntities(")]
        self.assertIn("quarantinedEntities.find(entity)", helper)
        self.assertIn("identity == found->second.guid", helper)
        reset = source[source.index("void OnWorldDeactivate()"):]
        self.assertIn("quarantinedEntities.clear()", reset)
        self.assertIn("identityCensusAt = identityLogAt = 0", reset)

    def test_quarantine_never_kills_or_deletes_native_entities(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        quarantine = source[source.index("void QuarantineTree("):source.index("void LogIdentity(")]
        for required in ("DisableProcessing(GETrue)", "EnableRendering(GEFalse)",
                         "EnablePicking(GEFalse, GEFalse)", "EnableCollision(GEFalse)",
                         "!IndependentActor(child)"):
            self.assertIn(required, quarantine)
        for forbidden in ("Kill", "Destroy(", "RemoveFromWorld", "HitPoints", "SetID", "ContinueRoutine"):
            self.assertNotIn(forbidden, quarantine)
        candidate = source[source.index("npcidentity::Candidate IdentityCandidate("):
                           source.index("bool IndependentActor(")]
        for required in ("eEPropertySetType_Item", "eEPropertySetType_Magic", "Interaction.GetOwner",
                         "GetPartyLeader", "partyPlayer", "guidePlayer", "talk::IsHeld"):
            self.assertIn(required, candidate)

    def test_only_the_bound_pointer_has_host_presentation_and_combat(self):
        source = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        presentation = source[source.index("bool HostPresentationAvailable("):
                              source.index("bool HostCanPresent(")]
        self.assertIn("mirror.entity.GetInstance() != entity", presentation)
        combat = source[source.index("net::NpcRecord const* CombatRecord("):
                        source.index("bool CombatTarget(")]
        self.assertIn("found->second.entity.GetInstance() != entity", combat)
