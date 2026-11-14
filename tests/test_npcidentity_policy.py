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


NPCIDENTITY_POLICY_TARGET = _testbuild.register_target(
    "npcidentity-policy",
    [
        _testbuild.unit(
            "tests/npcidentity_policy_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcIdentityTests(unittest.TestCase):
    def test_production_identity_policy(self):
        result = NPCIDENTITY_POLICY_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reconciliation_runs_before_passive_playback(self):
        frame = _testbuild.src_text("Script_G3Together.cpp")
        self.assertLess(frame.index("npcs::ReconcileClientEntities()"), frame.index("npcplayback::Tick()"))
        playback = _testbuild.src_text("npcplayback.cpp")
        tick = playback[playback.index("void Tick()"):]
        self.assertLess(tick.index("RedundantClientEntity(entity)"), tick.index("HoldUnavailable("))
        self.assertLess(tick.index("RedundantClientEntity(entity)"), tick.index("unavailable[key]"))

    def test_census_is_pointer_specific_and_resettable(self):
        identity_source = _testbuild.src_text("npc_identity.cpp")
        npcs_source = _testbuild.src_text("npcs.cpp")
        census = identity_source[identity_source.index("void ReconcileClientEntities()"):
                                 identity_source.index("namespace detail {")]
        for required in ("Entity::GetEntities()", "world::ProcessedEntities()", "npcidentity::AddPointer",
                         "npcidentity::Quarantine", "ActorDescendant(entity)", "guid_collisions=%u",
                         "duplicates=%u", "NPCNEAR_DIFFERENT_GUID", "NPCDUP_QUARANTINE",
                         "NPCDUP_RESTORE", "saved->second.guid != identity"):
            self.assertIn(required, census)
        self.assertIn("npcremoval::Suppressed(saved->second.rootGuid.data())", census)
        helper = identity_source[identity_source.index("bool RedundantClientEntity("):
                                 identity_source.index("void ReconcileClientEntities(")]
        self.assertIn("quarantinedEntities.find(entity)", helper)
        self.assertIn("identity == found->second.guid", helper)
        reset = npcs_source[npcs_source.index("void OnWorldDeactivate()"):]
        self.assertIn("quarantinedEntities.clear()", reset)
        self.assertIn("identityCensusAt = identityLogAt = 0", reset)

    def test_quarantine_never_kills_or_deletes_native_entities(self):
        source = _testbuild.src_text("npc_identity.cpp")
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
        npcs_source = _testbuild.src_text("npcs.cpp")
        presentation = npcs_source[npcs_source.index("bool HostPresentationAvailable("):
                                   npcs_source.index("bool HostAttitude(")]
        self.assertIn("mirror.entity.GetInstance() != entity", presentation)
        combat = npcs_source[npcs_source.index("net::NpcRecord const* CombatRecord("):
                             npcs_source.index("bool CombatTarget(")]
        self.assertIn("found->second.entity.GetInstance() != entity", combat)
