from pathlib import Path
import os
import re
import sys
import unittest

test_dir = str(Path(__file__).resolve().parent)
if test_dir not in sys.path:
    sys.path.insert(0, test_dir)
from test_hud_notices import read_rva
ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


class NpcLeaseRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host_source = (ROOT / "src" / "npclease_runtime.cpp").read_text(encoding="utf-8")
        cls.client_source = (ROOT / "src" / "npclease_client.cpp").read_text(encoding="utf-8")
        cls.runtime = cls.host_source + "\n" + cls.client_source
        cls.policy = (ROOT / "src" / "npcown_policy.h").read_text(encoding="utf-8")

    def test_split_translation_units_are_bounded(self):
        self.assertLess(len(self.host_source.splitlines()), 880)
        self.assertLess(len(self.client_source.splitlines()), 800)
        self.assertIn("void ReceiveHost(", self.host_source)
        self.assertIn("void TickClient(", self.client_source)
        client_header = (ROOT / "src" / "npclease_client.h").read_text(encoding="utf-8")
        self.assertNotIn("<Game.h>", client_header)

    def test_review_regressions_preserved_across_split(self):
        guard = section(self.host_source, "void PublishGuard(", "void SyncGuardHostClaims(")
        self.assertIn("guard.hostClaimed = !net::IsHost() && HostWins(key)", guard)
        for start, end in (("bool LocalAuthority(", "bool ProcessingAuthority("),
                           ("bool ProcessingAuthority(", "void NoteHit(")):
            body = section(self.host_source, start, end)
            self.assertIn("ReadGuard(entity)", body)
            self.assertNotIn("HostWins(", body)
            self.assertNotIn("HostStreamClaimed(", body)

        death = section(self.host_source, "void CommitDeath(", "void ReceiveHost(")
        self.assertIn("if (!entity) return", death)
        self.assertIn("if (NativeReady(entity))", death)
        self.assertIn("npc.DamageReceiver.HitPoints = 0", death)
        self.assertIn("npc.Routine.AIMode = gEAIMode_Dead", death)
        self.assertIn("journal::LeaseDeath(entity", death)
        host = section(self.host_source, "void ReceiveHost(", "void Receive(uint8_t")
        self.assertIn("const uint8_t priorEffects = entry.lease.state.effects", host)
        self.assertIn("entry.lease.state.effects |= priorEffects", host)
        self.assertLess(host.index("entry.lease.state.effects |= priorEffects"),
                        host.index("CommitDeath(key, entry)"))
        self.assertIn("value.effects |= entry.lease.state.effects", host)
        self.assertIn("npcremoval::SuppressedUnloaded(note.guid, baseline.name, baseline.position)", host)

        capture = section(self.host_source, "bool CaptureNative(", "void Stop(")
        relinquish = self.client_source[self.client_source.index("void RelinquishToHost("):]
        self.assertIn("if (allowRoleTransition) role.summonOrFollower = false", capture)
        self.assertIn("CaptureNative(entity, state, true)", relinquish)
        self.assertIn("state.effects |= entry->lease.state.effects", relinquish)
        self.assertIn("if (!entry->confirmed || !captured) ReleaseLocal", relinquish)
        self.assertIn("CutToHost(key, *entry, entity, &state, reason, net::Now())", relinquish)

        maintain = section(self.client_source, "void MaintainOwners(", "void ScanClient(")
        self.assertIn("facts.sharedWithLowerClient = SharedWithOtherClient(entity)", maintain)
        self.assertIn("!SharedWithOtherClient(entity)", maintain)
        self.assertIn("entry.lease.phase = Phase::Host", maintain)
        self.assertIn("entry.lease.phase != Phase::Frozen || entry.lease.owner != from", host)
        self.assertIn("entry.lease.phase = Phase::Dormant", host)
        self.assertIn("from >= entry.lease.owner", host)

    def test_suppressed_npcs_cannot_be_leased_or_adopted(self):
        role = section(self.runtime, "npcown::RoleFacts RoleOf(", "char const* EligibilityReason(")
        eligible = section(self.runtime, "char const* EligibilityReason(", "bool Eligible(")
        exclusion = section(self.policy, "inline char const* Exclusion(", "// ---- the decision")
        receive = section(self.runtime, "void Receive(uint8_t", "GEBool __fastcall Quest(")
        self.assertIn("role.removed = npcremoval::Suppressed(entity)", role)
        self.assertIn("npcown::Exclusion(RoleOf(entity, prototype))", eligible)
        self.assertIn('if (role.removed) return "authored removal"', exclusion)
        self.assertIn("npcremoval::Suppressed(note.guid)", receive)

    def test_discovery_requires_native_template_and_host_epoch(self):
        discovery = section(self.runtime, "char const* DiscoveryReason(", "bool Processed(")
        self.assertIn("proxy.GetEntity()", discovery)
        self.assertIn("eEPropertySetType_NPC", discovery)
        self.assertIn("EligibilityReason(prototype, state.name, true)", discovery)
        host = section(self.runtime, "void ReceiveHost(", "void Receive(uint8_t")
        adopt = section(host, "if (note.kind == Kind::Request || note.kind == Kind::Adopt)", "if (found == entries.end())")
        self.assertIn("DiscoveryReason(baseline)", adopt)
        self.assertIn("native ? npcs::Incarnation(note.guid) : note.incarnation", adopt)
        self.assertIn("++authoritySerial, now, baseline", adopt)
        self.assertNotIn("note.incarnation == 1", adopt)
        self.assertNotIn("CreateEntity", host)
        self.assertLess(adopt.index("PublishGuard(key, entry)"), adopt.index("Stop(native)"))
        self.assertLess(adopt.index("Stop(native)"), adopt.index("Send(key, entry, Kind::Offer)"))

    def test_activation_imports_before_native_restart(self):
        activate = section(self.runtime, "bool Activate(", "Interest InterestOf(")
        self.assertLess(activate.index("PublishGuard("), activate.index("npcs::LeaseReleaseMirror("))
        self.assertLess(activate.index("Import(entity"), activate.index("npc.Routine.ContinueRoutine()"))
        self.assertLess(activate.index("entry.nativeActivated = true"), activate.index("ReleaseNativePresentation"))
        self.assertLess(activate.index("ReleaseNativePresentation"), activate.index("npc.Routine.ContinueRoutine()"))
        self.assertNotIn("Kill", activate)
        self.assertNotIn("GiveXP", activate)
        self.assertNotIn("CreateItems", activate)

    def test_save_capture_is_empty_and_restore_discards_old_authority(self):
        capture = section(self.runtime, "SaveOverlay Capture()", "bool Restore(")
        restore = section(self.runtime, "bool Restore(", "bool WriteSnapshot(")
        self.assertIn("return {};", capture)
        self.assertNotIn("entries", capture)
        self.assertIn("OnWorldDeactivate();", restore)
        self.assertIn("return true;", restore)
        for operation in ("EncodeOverlay", "Import(", "PublishGuard(", "entries[", "return false"):
            self.assertNotIn(operation, restore)
        write = section(self.runtime, "bool WriteSnapshot(", "bool ReadSnapshot(")
        self.assertNotIn("fwrite", write)
        self.assertNotIn("EncodeSaveText", write)

    def test_compact_state_uses_canonical_strings_not_client_identity(self):
        receive = section(self.runtime, "void Receive(uint8_t", "GEBool __fastcall Quest(")
        self.assertIn("!found->second.hasBaseline", receive)
        self.assertIn("hydrated.state.name, canonical.name", receive)
        self.assertIn("hydrated.state.routine, canonical.routine", receive)
        self.assertLess(receive.index("hydrated.state.name"), receive.index("Valid(hydrated)"))
        send = section(self.runtime, "bool Send(", "void Diagnostic(")
        self.assertIn("kind = Kind::StateFull", send)

    def test_departed_owner_controls_do_not_reach_reused_slot(self):
        current = section(self.runtime, "bool CurrentPeer(", "bool Send(")
        self.assertIn("PeerCurrent(net::PeerJoined(player), net::PeerGeneration(player), generation)", current)
        send = section(self.runtime, "bool Send(", "void Diagnostic(")
        self.assertIn("net::IsHost() && !CurrentPeer(entry.lease.owner, entry.lease.generation)", send)
        host = section(self.host_source, "void TickHost(", "\n}\nusing namespace detail;")
        self.assertIn("const bool ownerGone = !CurrentPeer(entry.lease.owner, entry.lease.generation)", host)
        self.assertIn("if (CurrentPeer(client, generation))", host)
        self.assertIn("if (CurrentPeer(previousOwner, previousGeneration))", host)
        self.assertIn("CurrentPeer(client, entry.previousGeneration)", host)

    def test_live_dormant_pending_leases_never_refuse_or_mutate_on_save(self):
        persist = (ROOT / "src" / "persist.cpp").read_text(encoding="utf-8")
        i18n = (ROOT / "src" / "i18n.cpp").read_text(encoding="utf-8")
        self.assertNotIn("npclease::", persist)
        self.assertNotIn("npc-lease-active", persist + i18n)
        self.assertNotIn("NPC lease active", persist + i18n)
        self.assertNotIn('Register("npclease"', persist)
        self.assertIn("if (block.type == LegacyNpcLeaseExtension) continue;", persist)
        self.assertIn("processDepth || nativeLoadDepth || nativeSaveDepth", persist)
        self.assertIn('return "downed"', persist)

    def test_ownership_does_not_expire_or_depend_on_processing(self):
        client = section(self.client_source, "void TickClient(", "\n}\nusing namespace detail;")
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        self.assertIn("MaintainOwners(now)", client)
        for body in (client, maintain):
            self.assertNotIn("conservative lease expired", body)
            self.assertNotIn("processingQueued", body)
        release_policy = section(self.policy, "inline char const* Release(", "// ---- processing registration")
        for allowed in ("Exclusion(facts.role)", "facts.hostPresenting", "facts.hostCommit", "facts.hostRecallDue"):
            self.assertIn(allowed, release_policy)
        for ignored in ("leaseExpired", "hostSilent", "checkpointBusy", "paused", "registrationFailed",
                        "processingListsMissing", "outsideRing", "disconnected"):
            self.assertNotIn(ignored, release_policy)
        self.assertIn("facts.hostPresenting = HostWins(key)", maintain)
        self.assertNotIn("terminalDeath", maintain)
        self.assertIn("npcown::Release(facts)", maintain)
        self.assertIn("if (facts.hostPresenting || !entry.confirmed || !captured)", maintain)
        self.assertIn("ReleaseLocal(key, entry, entity, true)", maintain)
        self.assertIn("CutToHost(key, entry, entity, &state, release, now)", maintain)
        release = section(self.runtime, "void ReleaseLocal(", "void ReceiveClient(")
        takeover = section(self.runtime, "char const* TakeOver(", "void CutToHost(")
        cut = section(self.runtime, "void CutToHost(", "bool SharedWithOtherClient(")
        clear = r"\bentry\.local\s*=\s*false\b"
        for body in (release, takeover, cut):
            self.assertEqual(len(re.findall(clear, body)), 1)
        self.assertEqual(len(re.findall(clear, self.runtime)), 3)
        release_branch = section(maintain, "if (char const* release = npcown::Release(facts))", "ObserveReaction(")
        # Besides the policy release, ownership ends only when the engine's reloaded instance cannot be restored or the
        # revision counter is exhausted (both fall back to the host stream; nothing is frozen).
        self.assertEqual(maintain.count("ReleaseLocal(key, entry, entity,"),
                         release_branch.count("ReleaseLocal(key, entry, entity,") + 2,
                         "MaintainOwners releases outside npcown::Release and the two documented fallbacks")
        self.assertIn("reloaded instance could not be restored", maintain)
        self.assertIn("revision exhausted", maintain)

    def test_owner_readiness_uses_native_processing_not_interaction_roi(self):
        processed = section(self.runtime, "bool Processed(", "bool NativeReady(")
        self.assertIn("world::ProcessedEntities()", processed)
        self.assertIn("entity->IsProcessingDisabled()", processed)
        self.assertIn("entity->IsEnabled()", processed)
        self.assertNotIn("Entity::GetNPCs()", processed)
        readiness = section(self.runtime, "ClientReadiness Readiness(", "bool CanResumePresentation(")
        self.assertIn("Processed(entity)", readiness)
        self.assertNotIn("busy", readiness)
        self.assertNotIn("AIMode", readiness)
        self.assertNotIn("HitPoints", readiness)
        world = (ROOT / "src" / "world.cpp").read_text(encoding="utf-8")
        self.assertIn("GetEntitiesInProcessingRange()", section(world, "bTSmallArray<eCEntity*> const* Processed()", "\n}\n\nbTSmallArray"))

    def test_takeover_is_fenced_before_registration_and_handshake(self):
        takeover = section(self.runtime, "char const* TakeOver(", "void CutToHost(")
        activate = section(self.runtime, "bool Activate(", "Interest InterestOf(")
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        self.assertLess(takeover.index("Activate(entity, entry, key.data())"),
                        takeover.index("RegisterProcessing(key, entry, entity)"))
        self.assertNotRegex(takeover, r"\bif\s*\([^;{}]*RegisterProcessing\(")
        self.assertLess(activate.index("PublishGuard("), activate.index("npcs::LeaseReleaseMirror("))
        self.assertLess(activate.index("npcs::LeaseReleaseMirror("), activate.index("Import(entity"))
        self.assertLess(activate.index("Import(entity"), activate.index("ReleaseNativePresentation"))
        self.assertLess(activate.index("ReleaseNativePresentation"), activate.index("ContinueRoutine()"))
        self.assertNotIn("Send(", takeover)
        self.assertIn("Send(key, entry, Kind::Adopt)", maintain)
        registration = section(self.runtime, "void RegisterProcessing(", "char const* TakeOver(")
        self.assertIn("queued = world::QueueEntityProcessing(entity, &queueWhy)", registration)
        self.assertIn("npcown::Classify(processedBefore, processableBefore, queued, enabled)", registration)
        for operation in ("entry.local = false", "Stop(", "Kind::Cut"):
            self.assertNotIn(operation, registration)
        receive = section(self.client_source, "void ReceiveClient(", "// Which native path")
        offer = section(receive, "if (note.kind == Kind::Offer)", "const auto found = entries.find(key)")
        self.assertIn("!it->second.local", offer)
        self.assertIn("entry.lease.epoch = note.epoch", offer)
        self.assertIn("Send(key, entry, Kind::Prepared)", offer)
        self.assertNotIn("Import(", offer)
        self.assertNotIn("Stop(", offer)
        host_commit = section(receive, "if (note.kind == Kind::Commit && note.owner == 1", "if (!Exact(note, entry))")
        self.assertIn("Newer(note.epoch, entry.lease.epoch)", host_commit)
        self.assertIn("ReleaseLocal(key, entry, entity, !wasCut)", host_commit)
        commit = section(receive, "if (note.kind == Kind::Commit) {", "} else if (note.kind == Kind::Renew)")
        self.assertIn("entry.confirmed = true", commit)
        self.assertIn("Send(key, entry, Kind::Ack)", commit)
        guard = section(self.runtime, "void PublishGuard(", "AuthorityGuard ReadGuard(")
        self.assertIn("entry.local && entry.active", guard)
        self.assertIn("guard.persistent = entry.local", guard)
        self.assertIn("guard.processingAllowed && entry.nativeActivated", guard)
        self.assertNotIn("entry.window.Active(", guard)
        self.assertNotIn("Reached(", guard)

    def test_entity_queue_and_client_range_preserve_native_hero_membership(self):
        world = (ROOT / "src" / "world.cpp").read_text(encoding="utf-8")
        queue = section(world, "bool QueueEntityProcessing(", "// Extra ranges")
        self.assertIn("admin.AddDirtyProcessingRangeEntity(entity, GETrue)", queue)
        self.assertLess(queue.index("MarkProcessingDirty(&admin, why)"),
                        queue.index("admin.AddDirtyProcessingRangeEntity(entity, GETrue)"))
        self.assertIn('if (why) *why = "queued"', queue)
        for reason in ("no entity", "retirement gate paused", "shutting down", "scene administration unavailable"):
            self.assertIn(f'*why = "{reason}"', queue)
        dirty = section(world, "bool MarkProcessingDirty(", "void RequestProcessingUpdate(")
        self.assertIn('*why = "engine ROI refresh signature mismatch"', dirty)
        for destructive in ("ClearProcessingRangeList", "FourceFullROIUpdate()", "EnterProcessingRange(",
                            "ExitProcessingRange(", "GetEntitiesInProcessingRange"):
            self.assertNotIn(destructive, queue)
        membership = section(world, "ProcessingMembership ProcessingOf(", "namespace {\nbool MarkProcessingDirty")
        self.assertIn("GetEntitiesInProcessingRange()", membership)
        self.assertIn("GetProcessableEntities()", membership)
        cull = section(world, "bool NpcIntersectsAt(", "bool __fastcall NpcIntersects(")
        self.assertIn("if (!net::IsHost()) return npclease::ProcessingAuthority(entity)", cull)
        self.assertLess(cull.index("npcremoval::ExcludeFromProcessing"), cull.index("ProcessingAuthority"))
        tick = section(world, "void Tick()", "void OnWorldDeactivate()")
        self.assertLess(tick.index("HookElastic()"), tick.index("if (!net::IsHost()) return"))
        permit = section(self.runtime, "bool ProcessingAuthority(", "void NoteHit(")
        self.assertIn("ReadGuard(entity).Processing(net::Now())", permit)
        self.assertIn("LeaseProcessingRange", permit)
        self.assertIn("!npcs::RedundantClientEntity(entity)", permit)
        self.assertIn("!npcremoval::Suppressed(entity)", permit)

    def test_native_restart_retires_zero_speed_playback_instances(self):
        playback = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        release = section(playback, "bool ReleaseNativePresentation(", "void UpdatePresentation(")
        self.assertIn("Passive(entity)", release)
        self.assertIn("actor->StopMotion(slot, 0.0f)", release)
        self.assertIn("provenPoses.erase(EntityKey(entity))", release)
        self.assertIn("!animation->GetRagDollEnabled()", release)
        self.assertNotIn("SetPlaySpeed", release)

    def test_supported_native_dirty_queue_and_both_range_calls(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("G3T_GAME_DLL required for read-only Engine.dll byte verification")
        engine = Path(game).parent / "Engine.dll"
        data = engine.read_bytes()
        self.assertEqual(read_rva(data, 0x2544a0, 7), bytes.fromhex("8d81e0000000c3"))
        self.assertEqual(read_rva(data, 0x7a270, 7), bytes.fromhex("8d81e8000000c3"))
        self.assertEqual(read_rva(data, 0x23155d, 7), bytes.fromhex("8b7c24108b4f3c"))
        self.assertEqual(read_rva(data, 0x23157f, 9), bytes.fromhex("8d97e8000000528bcd"))
        self.assertEqual(read_rva(data, 0x230854, 9), bytes.fromhex("8d87e8000000508bcd"))
        self.assertEqual(read_rva(data, 0x231b88, 16),
                         bytes.fromhex("8d4c240c518d8ed8000000e818040000"))
        self.assertEqual(read_rva(data, 0x230c14, 9), bytes.fromhex("80bdfd000000017462"))

    def test_hits_only_observe_owner_and_death_state_requires_confirmation(self):
        note = section(self.runtime, "void NoteHit(", "char const* UnavailabilityReason(")
        for mutation in ("entry->active =", "entry->lease.phase =", "entry->window.Freeze",
                         "Stop(", "Kind::Cut", "Import(", "HitPoints", "entry->lease.state.hp =",
                         "SetHitPoints"):
            self.assertNotIn(mutation, note)
        self.assertIn("entry->lastHit = ", note)
        self.assertIn("if (after < before) ++ownTotals.hitsApplied", note)
        self.assertIn("npcown::Event::Hit", note)
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        event = section(maintain, "const bool event =", "if (captured && (event")
        self.assertIn("state.hp != entry.lease.state.hp", event)
        self.assertIn('Diagnostic("state-event"', maintain)
        kill = section(self.runtime, "GEInt GE_STDCALL Kill(", "bool Install()")
        self.assertIn("if (entry->confirmed && Send(Key(victim), *entry, Kind::State))", kill)
        self.assertIn("Send(key, entry, Kind::Adopt)", maintain)
        control = section(self.client_source, "Message Control(", "bool Exact(")
        self.assertIn("note.state = entry.lease.state", control)
        combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        self.assertIn('npclease::NoteHit(recipient.GetInstance(), attacker, "AddHitPoints"', combat)
        self.assertIn("npclease::NoteHit(victim.GetInstance(), attacker, Judgements[which]", combat)

    def test_release_diagnostics_precede_stop_and_distinguish_causes(self):
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        cut = section(self.runtime, "void CutToHost(", "bool SharedWithOtherClient(")
        self.assertIn("npcown::Release(facts)", maintain)
        self.assertIn("CutToHost(key, entry, entity, &state, release, now)", maintain)
        self.assertLess(cut.index('Diagnostic("frozen", key, entry, why)'), cut.index("Stop(entity)"))
        self.assertLess(cut.index("OwnEventAlways("), cut.index("Stop(entity)"))
        self.assertIn('"reason=%s awaiting host hand-back', cut)
        self.assertIn('ReadinessDiagnostic("state-unavailable"', maintain)
        diagnostic = section(self.client_source, "void ReadinessDiagnostic(", "void ObserveProcessing(")
        for field in ("guid=%s", "epoch=%u", "processed=%u", "actor=%u", "local=%u", "confirmed=%u",
                      "last_hit_age_ms=%u", "task=%s", "state=%s", "e0=%u", "e8=%u",
                      "entity_enabled=%u", "suspension=%s", "hero_cm=%.0f", "camera_cm=%.0f"):
            self.assertIn(field, diagnostic)

    def test_kill_ledger_precedes_native_effects(self):
        kill = section(self.runtime, "GEInt GE_STDCALL Kill(", "bool Install()")
        self.assertLess(kill.index("CommitEffect(entry->lease.state, NativeDeath)"),
                        kill.index("const GEInt result = originalKill("))
        self.assertLess(kill.index("ownerDeath = victim"), kill.index("const GEInt result = originalKill("))
        self.assertLess(kill.index("const GEInt result = originalKill("), kill.index("ownerDeath = before"))
        self.assertIn("entry->lease.state.effects & NativeDeath", kill)
        self.assertIn("++repeatedDeaths", kill)
        self.assertIn("final.dead = 1; final.hp = 0", kill)
        self.assertIn("final.effects |= NativeDeath", kill)
        self.assertIn("if (entry->confirmed && Send(Key(victim), *entry, Kind::State))", kill)
        quest = section(self.runtime, "GEBool __fastcall Quest(", "GEInt GE_STDCALL Kill(")
        self.assertIn("ownerDeath == &victim) return GEFalse", quest)
        death = section(self.runtime, "void CommitDeath(", "void ReceiveHost(")
        self.assertIn("entry.lease.state.effects & NativeDeath", death)
        self.assertIn("entry.lease.state.effects & QuestDeath", death)
        self.assertIn("journal::LeaseDeath(entity, entry.lease.owner)", death)
        self.assertIn("CommitEffect(entry.lease.state, QuestDeath)", death)
        host = section(self.runtime, "void ReceiveHost(", "void Receive(uint8_t")
        self.assertIn("CommitDeath(key, entry)", host)

    def test_late_pre_handoff_samples_cannot_roll_back_the_cut(self):
        receive = section(self.client_source, "void ReceiveClient(", "// Which native path")
        self.assertIn("entry.presentationFloor = note.echo", receive)
        self.assertIn("entry.hasPresentationFloor = true", receive)
        controls = section(self.client_source, "Message Control(", "bool Exact(")
        self.assertIn("kind == Kind::Commit && entry.lease.owner == 1", controls)
        self.assertIn("note.echo = net::Now()", controls)
        floor = section(self.runtime, "bool IgnoreHostSnapshot(uint8_t const identity[16],", "uint8_t RemoteOwner(")
        self.assertIn("entry.hasPresentationFloor && !Newer(captured, entry.presentationFloor)", floor)
        self.assertIn("if (entry.local) return false", floor)
        self.assertIn("!LocalOwner(identity)", floor)
        self.assertIn("found->second.local && found->second.active", floor)
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        playback = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        self.assertIn("npclease::IgnoreHostSnapshot(records[i].guid, captured)", npcs)
        self.assertIn("npclease::IgnoreHostSnapshot(record.guid, record.sent)", playback)

    def test_host_commit_is_conditional_on_native_restart(self):
        host = section(self.host_source, "void TickHost(", "\n}\nusing namespace detail;")
        frozen = section(host, "if (entry.lease.phase == Phase::Frozen)", "const auto prior")
        self.assertLess(frozen.index("Import(entity, entry.lease.state)"), frozen.index("HandBack("))
        self.assertLess(frozen.index("if (Activate(entity"), frozen.index("Kind::Commit"))
        self.assertIn('Diagnostic("host-held"', frozen)

    def test_role_admission_excludes_only_unsupported_roles(self):
        exclusion = section(self.policy, "inline char const* Exclusion(", "// ---- the decision")
        self.assertEqual(set(re.findall(r"\brole\.(\w+)", exclusion)),
                         {"hero", "remotePuppet", "summonOrFollower", "itemOrSpell",
                          "duplicateIdentity", "removed", "complete"})
        self.assertIn("return nullptr", exclusion)
        role = section(self.runtime, "npcown::RoleFacts RoleOf(", "char const* EligibilityReason(")
        for guard in ("GetHero()", "remote::ControlledBody()", "eEPropertySetType_PlayerMemory",
                      "remote::IsPuppet(entity)", "npcs::RedundantClientEntity(entity)",
                      "npcremoval::Suppressed(entity)", "role.complete =", "GetCreator()",
                      "eEPropertySetType_Magic", "eEPropertySetType_Item", "FollowerRole(entity)"):
            self.assertIn(guard, role)
        follower = section(self.runtime, "bool FollowerRole(", "npcown::RoleFacts RoleOf(")
        for guard in ("GetOwner().GetInstance()", "GetPartyLeader().GetInstance()", "npc.Party.Waiting",
                      "gEPartyMemberType_None", "GetMembers().GetCount()"):
            self.assertIn(guard, follower)
        for obsolete in ("SpeciesName", "ClassOf(", "StoryCatalog", "GetTradeEnabled()", "GetTeachEnabled()"):
            self.assertNotIn(obsolete, role + follower)

    def test_adopt_uses_client_baseline_for_all_classes(self):
        host = section(self.runtime, "void ReceiveHost(", "void Receive(uint8_t")
        adopt = section(host, "if (note.kind == Kind::Request || note.kind == Kind::Adopt)", "if (found == entries.end())")
        self.assertIn("const Checkpoint baseline = note.state", adopt)
        self.assertIn("EligibilityReason(native)", adopt)
        self.assertIn("std::memcmp(&native->GetTemplateID().GetCore(), baseline.templateId, 16)", adopt)
        self.assertIn("else if (auto const* why = DiscoveryReason(baseline))", adopt)
        self.assertIn("!baseline.dead && Entity(native).IsDead()", adopt)
        self.assertIn("!baseline.dead && journal::RecordedDeath(note.guid)", adopt)
        self.assertIn("entry.interest.Delegate(interest, now)", adopt)
        self.assertIn("!native && !entry.adopted", adopt)
        for obsolete in ("StoryCatalog", "HostUnknown", "MayDelegate", "HostKnows", "ClassOf(", "SpeciesName"):
            self.assertNotIn(obsolete, adopt)
        scan = section(self.runtime, "void ScanClient(", "void TickClient(")
        self.assertIn("const auto* all = world::ProcessedEntities()", scan)
        self.assertNotIn("Entity::GetNPCs()", scan)
        diagnostic = section(self.runtime, "char const* UnavailabilityReason(", "bool Fenced(")
        self.assertIn("initializing", diagnostic)
        self.assertIn("installation failed", diagnostic)
        self.assertIn("EligibilityReason(entity)", diagnostic)
        self.assertIn("npclease: admission-refused guid=%s name=%s reason=%s", self.runtime)
        journal = (ROOT / "src" / "journal.cpp").read_text(encoding="utf-8")
        deathQuery = section(journal, "bool RecordedDeath(", "unsigned Write(")
        self.assertIn("deaths.count", deathQuery)
        for mutation in ("deaths[", "Kill(", "CountQuest(", "SendDeath("):
            self.assertNotIn(mutation, deathQuery)

    def test_client_first_takeover_is_immediate_and_independent_of_handshake(self):
        scan = section(self.runtime, "void ScanClient(", "void TickClient(")
        takeover = section(self.runtime, "char const* TakeOver(", "void CutToHost(")
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        decision = section(self.policy, "inline Decision Decide(", "// ---- acquisition timing")
        self.assertIn("npcown::StreamSettled(now, sessionReadyAt, npcs::HostStreamSeen())", scan)
        self.assertIn("facts.settled = settled", scan)
        self.assertIn("facts.cooldownHeld = found != entries.end() && npcown::CooldownHeld(", scan)
        self.assertIn("if (!facts.settled)", decision)
        self.assertIn("if (facts.cooldownHeld)", decision)
        self.assertLess(scan.index("npcown::Decide(facts)"), scan.index("TakeOver(entity, key, now)"))
        self.assertLess(scan.index("takeovers >= 8"), scan.index("++takeovers"))
        self.assertLess(scan.index("++takeovers"), scan.index("TakeOver(entity, key, now)"))
        for obsolete in ("dwell", "lastRequest", "Kind::Adopt", "Send("):
            self.assertFalse(obsolete in scan, f"ScanClient unexpectedly contains {obsolete}")
        # The only timer in the scan is the back-off after a FAILED takeover (a permanent fence failure must not retry at 10 Hz).
        self.assertIn("entries[key].retryAt = (now ? now : 1) + 2000", scan)
        self.assertLess(scan.index("TakeOver(entity, key, now)"), scan.index("entries[key].retryAt ="))
        self.assertEqual(scan.count("Reached("), 1)
        self.assertNotRegex(scan, r"\bif\s*\([^)]*\b(?:now\s*-|unavailableAt)")
        for operation in ("QueueEntityProcessing(", "RegisterProcessing("):
            self.assertNotIn(operation, scan)
            self.assertNotRegex(scan, rf"\bif\s*\([^;{{}}]*{re.escape(operation)}")
        self.assertNotIn("Send(", takeover)
        self.assertLess(maintain.index("entry.lease.state = state"), maintain.index("Send(key, entry, Kind::Adopt)"))

    def test_review_hardening_keeps_owners_alive_and_hand_back_loss_free(self):
        host = section(self.host_source, "void ReceiveHost(", "void Receive(uint8_t")
        client = section(self.client_source, "void ReceiveClient(", "// Which native path")
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        interest = section(self.host_source, "Interest InterestOf(", "bool CurrentPeer(")
        capture = section(self.runtime, "bool CaptureNative(", "void Stop(")
        shared = section(self.runtime, "bool SharedWithOtherClient(", "// Reaction logging")
        # A live client keeps its epoch through host pauses/checkpoints: the deadline renews regardless of Running().
        renew = section(host, "entry.lease.AcceptState(", "} else if (note.kind == Kind::Cut)")
        self.assertLess(renew.index("entry.lease.deadline = now + LeaseMs + LeaseSafetyMs"),
                        renew.index("if (Running() && !persist::CheckpointBusy()) Send(key, entry, Kind::Renew)"))
        # The host re-offers a cut NPC the same client reclaimed (no obsolete-checkpoint hand-back).
        self.assertIn("entry.lease.phase != Phase::Frozen || entry.lease.owner != from", host)
        self.assertIn("entry.lease.phase = Phase::Dormant", host)
        # A Commit(owner=1) is accepted even after the client already yielded to the host stream (no endless retransmits).
        self.assertIn("(entry.cut || entry.local || entry.lease.phase == Phase::Host)", client)
        # Local terminal death cannot overrule host authority; one stray record still cannot revoke an established owner.
        self.assertNotIn("terminalDeath", maintain)
        self.assertIn("facts.hostPresenting = HostWins(key)", maintain)
        wins = section(self.host_source, "bool HostWins(", "bool Running(")
        self.assertIn("npcs::HostStreamRecords(key.data()) >= 2", wins)
        # Native dialogue is busy and blocks a forced recall cut.
        self.assertIn("gEAIMode_Talk", capture)
        self.assertIn("npc.Routine.AIMode != gEAIMode_Talk", maintain)
        # A reloaded instance waits for native readiness without dropping ownership.
        reload = section(maintain, "if (entry.ownedEntity != entity && !HostWins(key))", "ObserveProcessing(")
        self.assertLess(reload.index("if (!NativeReady(entity)) continue;"), reload.index("Activate(entity, entry, key.data())"))
        # One owner per NPC: only LOWER-numbered clients within reach take precedence, on both machines.
        self.assertIn("i < net::LocalId()", shared)
        self.assertIn("i < client", interest)

    def test_stale_host_state_cannot_authorize_takeover_or_native_death(self):
        takeover = section(self.runtime, "char const* TakeOver(", "void CutToHost(")
        scan = section(self.runtime, "void ScanClient(", "void TickClient(")
        maintain = section(self.runtime, "void MaintainOwners(", "void ScanClient(")
        kill = section(self.runtime, "GEInt GE_STDCALL Kill(", "bool Install(")
        self.assertLess(takeover.index("HostStreamClaimed"), takeover.index("CaptureNative"))
        self.assertLess(takeover.index("AcquisitionTimely"), takeover.index("Activate("))
        self.assertIn("facts.hostPresenting = npcs::HostStreamClaimed(key.data())", scan)
        self.assertIn("facts.hostStreamUncertain = streamUncertain", scan)
        self.assertIn("facts.hostReleasePending = npcs::HostReleasePending(key.data())", scan)
        self.assertIn("!npcs::HostStreamClaimed(key.data())", maintain)
        self.assertIn("entry.ownedEntity != entity && !HostWins(key)", maintain)
        self.assertLess(kill.index("HostWins(Key(victim))"), kill.index("return originalKill"))
        self.assertIn("native death refused", kill)
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        claim = section(npcs, "bool HostStreamClaimed(", "uint32_t HostStreamDelay(")
        self.assertNotIn("PresentationFresh", claim)
        self.assertNotIn("latestTicks", claim)
        self.assertIn("npclease::ReconcileLife(localDeath, m.killed, (shown.flags & 1) != 0)", npcs)
        self.assertIn("NPCRECONCILE", npcs)
