from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPCPRESENCE_POLICY_TARGET = _testbuild.register_target(
    "npcpresence-policy",
    [
        _testbuild.unit(
            "tests/npcpresence_policy_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcPresencePolicyTests(unittest.TestCase):
    def test_presence_policy_and_reject_wire_roundtrip(self):
        result = NPCPRESENCE_POLICY_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_client_presence_reconciles_reversibly_without_disabling_processing(self):
        identity = _testbuild.src_text("npc_identity.cpp")
        npcs = _testbuild.src_text("npcs.cpp")
        playback = _testbuild.src_text("npcplayback.cpp")

        reconcile_entities = identity[identity.index("void ReconcileClientEntities()"):
                                      identity.index("namespace detail {")]
        self.assertLess(reconcile_entities.index("detail::ReconcileClientPresence()"),
                        reconcile_entities.index("identityCensusAt && now - identityCensusAt < 250"))

        hide = identity[identity.index("void HideClientTree("):identity.index("npcpresence::Facts PresenceFacts(")]
        restore = identity[identity.index("void RestoreHiddenTree("):identity.index("void HideClientTree(")]
        for flag_call in ("EnableRendering(", "EnablePicking(", "EnableCollision("):
            self.assertIn(flag_call, hide)
            self.assertIn(flag_call, restore)
        self.assertNotIn("DisableProcessing(", hide)
        self.assertIn("ClearLocalReferencesToDeadMirror(entity, nullptr, false, true)", hide)

        presence_facts = identity[identity.index("npcpresence::Facts PresenceFacts("):
                                  identity.index("bool RedundantClientEntity(")]
        self.assertIn("facts.clientDialogPartner = talk::TalkingWithHero(entity)", presence_facts)
        self.assertIn("facts.clientGuideActive =", presence_facts)
        self.assertIn("guide::ClientHolding(entity)", presence_facts)
        self.assertIn("npclease::PreviouslyVetted(guid)", presence_facts)

        tick = npcs[npcs.index("void Tick()"):]
        self.assertLess(tick.index("PassiveMirrorTick()"), tick.index("detail::ReconcileClientPresence()"))
        self.assertLess(tick.index("CorpseTick("), tick.index("detail::ReconcileClientPresence()"))

        self.assertIn("state.root.rendering = true;", hide)
        companions = _testbuild.src_text("npc_companions.cpp")
        reset_stream = companions[companions.index("void ResetMirrorForNewStream("):
                                  companions.index("void PackMotionFromRecord(")]
        self.assertIn("const bool keepVisiblePlacement = sameOwner && m.havePresentation && m.entity.GetInstance() != nullptr", reset_stream)
        self.assertIn("m.havePresentation = keepVisiblePlacement;", reset_stream)
        passive = npcs[npcs.index("void PassiveMirrorTick()"):npcs.index("bool MirrorFightsHero()")]
        self.assertIn("!IsNpc(live.GetInstance())", passive)
        self.assertIn("npcplayback::SetEntityPose(m.entity.GetInstance(), body::Pose(", passive)

        blow = npcs[npcs.index("void TryPlayerBlow("):npcs.index("void LogClientDeath(")]
        self.assertIn("!ClientPresent(victim)", blow)

        playback_tick = playback[playback.index("void Tick()"):]
        self.assertIn("npcs::ClientPresent(npc.GetInstance())", playback_tick)

    def test_despawn_and_unloaded_deaths_prevent_client_ghosts(self):
        respawn = _testbuild.src_text("respawn.cpp")
        send = _testbuild.src_text("npc_send.cpp")
        companions = _testbuild.src_text("npc_companions.cpp")
        journal = _testbuild.src_text("journal.cpp")

        detour = respawn[respawn.index("GEInt GE_STDCALL Detour("):respawn.index("void Tick()")]
        self.assertIn("if (!net::Connected()) return original(spu, self, other, args);", detour)
        self.assertIn("!net::IsHost() || npcs::IsMirrored(instance)", detour)

        despawn = send[send.index("void Despawn(eCEntity* entity)"):send.index("uint32_t Incarnation(")]
        self.assertIn("absences[key] = record", despawn)
        self.assertIn("corpses.erase(key)", despawn)
        self.assertIn("npcpresence::VoidDespawnX", despawn)
        self.assertIn("npcsend::SanitizeCapturedRecord(record)", despawn)

        host_tick = send[send.index("void HostTick("):send.index("} // namespace g3t::npcs::detail")]
        self.assertIn("for (auto const& entry : absences) net::SendNpcs(&entry.second, 1, true)", host_tick)
        self.assertIn("journal::SyncToClients()", host_tick)

        admit = companions[companions.index("bool AdmitIncomingNpc("):]
        self.assertIn("npcpresence::IsVoidDespawnPosition(record.position)", admit)
        self.assertIn("LeaseReleaseMirror(record.guid)", admit)
        self.assertIn("absences[key] = record", admit)
        self.assertIn("npclease::RelinquishToHost(live", admit)

        on_death = journal[journal.index("void OnDeath("):journal.index("void Apply(")]
        self.assertIn("if (!net::IsHost() && key.size() == 16)", on_death)
        world_change = journal[journal.index("void OnWorldChange()"):journal.index("bool Pending(")]
        self.assertIn("if (!net::IsHost()) deaths.clear()", world_change)


if __name__ == "__main__":
    unittest.main()
