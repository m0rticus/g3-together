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


NPCLEASE_PRESENTATION_TARGET = _testbuild.register_target(
    "npclease-presentation",
    [
        _testbuild.unit(
            "tests/npclease_presentation_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcLeasePresentationTests(unittest.TestCase):
    def test_unavailable_policy(self):
        result = NPCLEASE_PRESENTATION_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_release_keeps_presentation_and_detach_is_inert(self):
        source = _testbuild.src_text("npcs.cpp")
        passive = source[source.index("void PassiveMirrorTick()"):source.index("bool MirrorFightsHero()")]
        self.assertNotIn("if (m.released) { it = mirrors.erase(it); continue; }", passive)
        self.assertIn("if (!HostPresentationAvailable(m.entity.GetInstance()))", passive)
        self.assertIn("m.deathLife.Observe(m.latest.incarnation, true)", passive)
        detach = source[source.index("void LeaseReleaseMirror("):source.index("uint32_t HostPresentationAge(")]
        self.assertNotIn("body::Release(", detach)
        self.assertIn("mirror.defeatedBefore", detach)
        self.assertIn("strncpy_s(m.body.routine", passive)

    def test_hold_does_not_kill_or_restart_ai(self):
        source = _testbuild.src_text("npcplayback.cpp")
        hold = source[source.index("bool HoldUnavailable("):source.index("void UpdatePresentation(")]
        self.assertIn("npc.IsDead() || npc.IsDown()", hold)
        self.assertIn("ReplayFrozenPose(", hold)
        self.assertNotIn("ContinueRoutine", hold)
        self.assertNotIn("HitPoints =", hold)
        self.assertNotIn("Kill", hold)

    def test_idle_bootstrap_is_evaluated_and_script_free(self):
        source = _testbuild.src_text("npcplayback.cpp")
        skeleton = _testbuild.src_text("npcplayback_skeleton.cpp")
        idle = source[source.index("eCResourceDataEntity* NativeMotion("):source.index("bool CosmeticHit(")]
        query = skeleton[skeleton.index("eCResourceDataEntity* QueryNativeMotion("):skeleton.index("void PruneSkeletonCache(")]
        self.assertIn("QueryNativeMotion(", idle)
        for required in ("npc.GetAni(action, phase)", "GetAniVariation",
                         "QueryMotionDataEntity", "gEUseType_None"):
            self.assertIn(required, query)
        for required in ("EvaluateIdle(", "gEAction_Ambient, gEPhase_Loop", "PoseProvenance::Idle"):
            self.assertIn(required, idle)
        for forbidden in ("ContinueRoutine", "sAIPlayAni", "SetTask", "Movement", "OnPreProcess",
                          "UpdatePresentation("):
            self.assertNotIn(forbidden, idle)
            self.assertNotIn(forbidden, query)
        self.assertNotIn('direction("None")', idle)
        self.assertNotIn('direction("None")', query)
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
        source = _testbuild.src_text("npcplayback.cpp")
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
        combat = _testbuild.src_text("combat_melee.cpp")
        contact = combat[combat.index("bool SendHostJudgedMelee("):combat.index("void PlayHitEffect(")]
        self.assertLess(contact.index("CosmeticHit"), contact.index("net::SendHitNpc"))
        self.assertIn("contact.motion", contact)
        self.assertIn("visual=%u", source)

    def test_fallback_scans_processed_npcs_and_bounds_resource_retries(self):
        source = _testbuild.src_text("npcplayback.cpp")
        tick = source[source.index("void Tick()"):]
        self.assertIn("world::ProcessedEntities()", tick)
        self.assertNotIn("Entity::GetNPCs()", tick)
        self.assertLess(tick.index("eEPropertySetType_NPC"), tick.index("Entity npc(entity)"))
        hold = source[source.index("bool HoldUnavailable("):source.index("void ReleaseUnavailable(")]
        self.assertIn("waiting.retry && now - waiting.attempted < 1000", hold)
        self.assertLess(hold.index("waiting.retry &&"), hold.index("BootstrapIdle(entity"))
        self.assertLess(hold.index("provenPoses.find(key)"), hold.index("BootstrapIdle(entity"))

    def test_missing_pose_never_changes_rendering(self):
        source = _testbuild.src_text("npcplayback.cpp")
        self.assertNotIn("EnableRendering(", source)
        self.assertIn("retaining visibility contacts=off", source)
        self.assertIn("PresentationCensus(cleanupNow)", source)
        for counter in ("host=%u", "held=%u", "idle=%u", "native=%u", "unproven=%u",
                        "hidden=%u", "static=%u", "top_reason=%s"):
            self.assertIn(counter, source)
        tick = source[source.index("void Tick()"): ]
        self.assertLess(tick.index("if (!IsCharacter(entity))"), tick.index("Entity npc(entity)"))
        npcs = _testbuild.src_text("npcs.cpp")
        send = npcs[npcs.index("void SendOwned("):npcs.index("const bTObjArray<Entity> all", npcs.index("void SendOwned("))]
        self.assertIn("npcplayback::IsCharacter(instance)", send)

    def test_native_pose_is_preserved_before_creature_stop(self):
        source = _testbuild.src_text("npcplayback.cpp")
        native = source[source.index("bool RememberNativePresentation("):source.index("bool CosmeticHit(")]
        for required in ("GetTargetWeight", "GetPlayTime", "originalActorProcess(",
                         "GETrue, GEFalse, 0.0f", "PoseProvenance::Native"):
            self.assertIn(required, native)
        runtime = _testbuild.src_text("npclease_runtime.cpp")
        stop = runtime[runtime.index("void Stop("):runtime.index("bool Import(")]
        self.assertIn("if (!entity || Player(entity) || !entity->GetPropertySet(eEPropertySetType_ScriptRoutine)) return;", stop)
        self.assertLess(stop.index("RememberNativePresentation(entity)"), stop.index("npc.Routine.FullStop()"))
        imported = runtime[runtime.index("bool Import("):runtime.index("bool Activate(")]
        self.assertLess(imported.index("Stop(entity)"), imported.index("entity->SetToWorldMatrix(pose)"))

    def test_transient_release_unavailable_retains_proven_pose(self):
        source = _testbuild.src_text("npcplayback.cpp")
        release_unavailable = source[source.index("void ReleaseUnavailable("):source.index("bool ReleaseNativePresentation(")]
        self.assertNotIn("provenPoses.erase", release_unavailable)
        release_native = source[source.index("bool ReleaseNativePresentation("):source.index("void UpdatePresentation(")]
        self.assertIn("provenPoses.erase(EntityKey(entity))", release_native)

    def test_force_always_visible_is_scoped_and_detour_lookups_avoid_allocations(self):
        playback = _testbuild.src_text("npcplayback.cpp")
        skeleton = _testbuild.src_text("npcplayback_skeleton.cpp")
        hooks = _testbuild.src_text("npcplayback_hooks.cpp")
        npcs = _testbuild.src_text("npcs.cpp")
        npc_send = _testbuild.src_text("npc_send.cpp")
        ai = _testbuild.src_text("ai.cpp")
        talk = _testbuild.src_text("talk.cpp")

        replay = playback[playback.index("bool Replay("):playback.index("bool ReplayFrozenPose(")]
        self.assertNotIn("SetForceAlwaysVisible(GETrue)", replay)
        update = playback[playback.index("void UpdatePresentation("):playback.index("void PresentationCensus(")]
        self.assertIn("if (!keepVisible) animation->SetForceAlwaysVisible(GEFalse);", update)

        self.assertIn("SetOffscreenCaptureVisible(entity, allowOffscreenCapture);", skeleton)
        self.assertIn("void SyncHostCaptureVisibility(", skeleton)
        self.assertIn("animation->SetForceAlwaysVisible(GEFalse)", skeleton)
        self.assertIn("npcplayback::SyncHostCaptureVisibility(", npcs)
        self.assertIn("npcplayback::SyncHostCaptureVisibility(nullptr, 0);", npc_send)
        self.assertIn("const bool allowOffscreenCapture = needRemotePose && !(r.flags & 1);", npc_send)

        self.assertIn("PropertyTableSlot(", hooks)
        self.assertIn("sourceOptionsEmpty", hooks)
        self.assertIn("originalStateLookup", ai)
        self.assertIn("originalFunctionLookup", ai)
        self.assertIn("std::unordered_map<GuidKey, Hold, GuidHash> holds;", talk)
        self.assertIn("if (!entity || holds.empty()) return nullptr;", talk)

