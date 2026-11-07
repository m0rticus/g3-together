from pathlib import Path
import os
import unittest

from test_hud_notices import read_rva
ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


class NpcLeaseRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime = (ROOT / "src" / "npclease_runtime.cpp").read_text(encoding="utf-8")

    def test_suppressed_npcs_cannot_be_leased_or_adopted(self):
        eligible = section(self.runtime, "char const* EligibilityReason(", "bool ValidDiscovery(")
        receive = section(self.runtime, "void Receive(uint8_t", "GEBool __fastcall Quest(")
        self.assertIn("npcremoval::Suppressed(entity)", eligible)
        self.assertIn("npcremoval::Suppressed(note.guid)", receive)

    def test_discovery_requires_native_template_and_host_epoch(self):
        discovery = section(self.runtime, "char const* DiscoveryReason(", "bool Processed(")
        self.assertIn("proxy.GetEntity()", discovery)
        self.assertIn("eEPropertySetType_NPC", discovery)
        self.assertIn("EligibilityReason(prototype, state.name, true)", discovery)
        host = section(self.runtime, "void ReceiveHost(", "void ReceiveClient(")
        self.assertIn("note.incarnation == 1 && ValidDiscovery(note.state)", host)
        self.assertIn("++authoritySerial", host)
        self.assertNotIn("CreateEntity", host)
        self.assertLess(host.index("PublishGuard(key, entry)"), host.index("Stop(native)"))
        self.assertLess(host.index("Stop(native)"), host.index("Send(key, entry, Kind::Offer)"))

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

    def test_expired_owners_never_resume_without_authorization(self):
        client = section(self.runtime, "void TickClient(", "\n}\n\nbool LocalAuthority(")
        readiness = section(self.runtime, "ClientReadiness Readiness(", "bool CanResumePresentation(")
        self.assertIn("Reached(now, entry.localDeadline)", readiness)
        self.assertIn("readiness.LossReason()", client)
        self.assertLess(client.index("entry.window.Freeze()"), client.index("Send(item.first, entry, Kind::Cut)"))
        self.assertNotIn("Kind::Commit", client)

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

    def test_committed_collection_is_fenced_and_never_restarts_expired_owner(self):
        receive = section(self.runtime, "void ReceiveClient(", "void Receive(uint8_t")
        commit = section(receive, "if (note.kind == Kind::Commit) {", "} else if (note.kind == Kind::Renew)")
        self.assertLess(commit.index("entry.window.Commit"), commit.index("EnableProcessing(GETrue)"))
        self.assertLess(commit.index("PublishGuard(key, entry)"), commit.index("EnableProcessing(GETrue)"))
        self.assertIn("world::QueueEntityProcessing(entity)", commit)
        self.assertIn("entry.nativeActivated = false", commit)
        guard = section(self.runtime, "void PublishGuard(", "AuthorityGuard ReadGuard(")
        self.assertIn("entry.window.Active(entry.lease.epoch", guard)
        self.assertIn("guard.processingAllowed && entry.nativeActivated", guard)
        client = section(self.runtime, "void TickClient(", "\n}\n\nbool LocalAuthority(")
        self.assertLess(client.index("readiness.LossReason()"), client.index("Activate(entity"))
        self.assertIn("readiness.processed && readiness.processable", client)
        self.assertIn("entry.nativeActivated && CaptureNative", client)
        readiness = section(self.runtime, "ClientReadiness Readiness(", "bool CanResumePresentation(")
        self.assertIn("AwaitingInitialCollection(entry.nativeActivated", readiness)
        self.assertIn("LeaseProcessingRange", readiness)

    def test_entity_queue_and_client_range_preserve_native_hero_membership(self):
        world = (ROOT / "src" / "world.cpp").read_text(encoding="utf-8")
        queue = section(world, "bool QueueEntityProcessing(", "// Extra ranges")
        self.assertIn("admin.AddDirtyProcessingRangeEntity(entity, GETrue)", queue)
        self.assertIn("MarkProcessingDirty(&admin)", queue)
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

    def test_hits_only_observe_owner_and_publish_reliable_state(self):
        note = section(self.runtime, "void NoteHit(", "char const* UnavailabilityReason(")
        for mutation in ("entry->active =", "entry->lease.phase =", "entry->window.Freeze",
                         "Stop(", "Kind::Cut", "Import("):
            self.assertNotIn(mutation, note)
        self.assertIn("entry->lastHit = ", note)
        client = section(self.runtime, "void TickClient(", "\n}\n\nbool LocalAuthority(")
        event = section(client, "const bool event =", "if (captured && (event")
        self.assertIn("state.hp != entry.lease.state.hp", event)
        self.assertIn('Diagnostic("state-event"', client)
        combat = (ROOT / "src" / "combat.cpp").read_text(encoding="utf-8")
        self.assertIn('npclease::NoteHit(recipient.GetInstance(), attacker, "AddHitPoints"', combat)
        self.assertIn("npclease::NoteHit(victim.GetInstance(), attacker, Judgements[which]", combat)

    def test_failure_diagnostics_precede_stop_and_distinguish_causes(self):
        client = section(self.runtime, "void TickClient(", "\n}\n\nbool LocalAuthority(")
        cut = section(client, "if (lost ||", "const bool event =")
        self.assertLess(cut.index('ReadinessDiagnostic("frozen"'), cut.index("Stop(entity)"))
        self.assertIn('ReadinessDiagnostic("state-unavailable"', client)
        diagnostic = section(self.runtime, "void ReadinessDiagnostic(", "void CommitDeath(")
        for field in ("guid=%s", "epoch=%u", "processed=%u", "actor=%u", "remaining_ms=%d",
                      "last_hit_age_ms=%u", "task=%s", "state=%s", "e0=%u", "e8=%u",
                      "entity_enabled=%u", "collecting=%u", "hero_cm=%.0f", "camera_cm=%.0f"):
            self.assertIn(field, diagnostic)
        self.assertIn('ReadinessDiagnostic("owner"', client)
        self.assertIn("now - entry.ownerReportAt >= 1000", client)

    def test_kill_ledger_precedes_native_effects(self):
        kill = section(self.runtime, "GEInt GE_STDCALL Kill(", "bool Install()")
        self.assertLess(kill.index("CommitEffect(entry->lease.state, NativeDeath)"),
                        kill.index("const GEInt result = originalKill("))
        self.assertIn("final.dead = 1; final.hp = 0", kill)
        self.assertIn("Kind::State", kill)

    def test_late_pre_handoff_samples_cannot_roll_back_the_cut(self):
        receive = section(self.runtime, "void ReceiveClient(", "void Receive(uint8_t")
        self.assertIn("entry.presentationFloor = note.echo", receive)
        self.assertIn("entry.hasPresentationFloor = true", receive)
        controls = section(self.runtime, "Message Control(", "bool Send(")
        self.assertIn("kind == Kind::Commit && entry.lease.owner == 1", controls)
        self.assertIn("note.echo = net::Now()", controls)
        floor = section(self.runtime, "bool IgnoreHostSnapshot(uint8_t const identity[16],", "uint8_t RemoteOwner(")
        self.assertIn("entry.hasPresentationFloor && !Newer(captured, entry.presentationFloor)", floor)
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        playback = (ROOT / "src" / "npcplayback.cpp").read_text(encoding="utf-8")
        self.assertIn("npclease::IgnoreHostSnapshot(records[i].guid, captured)", npcs)
        self.assertIn("npclease::IgnoreHostSnapshot(record.guid, record.sent)", playback)

    def test_host_commit_is_conditional_on_native_restart(self):
        host = section(self.runtime, "void TickHost(", "void TickClient(")
        frozen = section(host, "if (entry.lease.phase == Phase::Frozen)", "const auto prior")
        self.assertLess(frozen.index("Import(entity, entry.lease.state)"), frozen.index("HandBack("))
        self.assertLess(frozen.index("if (Activate(entity"), frozen.index("Kind::Commit"))
        self.assertIn('Diagnostic("host-held"', frozen)

    def test_expansion_does_not_enable_unportable_roles(self):
        species = section(self.runtime, "char const* SpeciesName(", "NativeClass ClassOf(Entity&")
        for covered in ("gESpecies_Troll", "gESpecies_Minecrawler", "gESpecies_Gargoyle", "gESpecies_Ogre"):
            self.assertIn(covered, species)
        for excluded in ("case gESpecies_Human:", "case gESpecies_Orc:", "case gESpecies_Goblin:",
                         ):
            self.assertNotIn(excluded, species)
        for covered in ("gESpecies_Chicken", "gESpecies_Cow", "gESpecies_Pig"):
            self.assertIn(covered, species)
        role = section(self.runtime, "bool ExcludedRole(", "bool Eligible(")
        for guard in ("GetTradeEnabled()", "GetTeachEnabled()", "GetPartyEnabled()",
                      "gEPartyMemberType_None", "GetMembers().GetCount()"):
            self.assertIn(guard, role)

    def test_discovered_ordinary_requires_host_absence_and_story_catalog(self):
        host = section(self.runtime, "void ReceiveHost(", "void ReceiveClient(")
        self.assertLess(host.index("else if (native)"), host.index("ValidDiscovery(note.state)"))
        self.assertIn("HostUnknown(native != nullptr, npcs::HostKnows(note.guid), entry.hasBaseline)", host)
        self.assertIn("MayDelegate(ClassOf(baseline)", host)
        self.assertIn("entry.adopted && !native", host)
        catalog = section(self.runtime, "bool StoryCatalog()", "bool GenericName(")
        for reference in ("GetQuests()", "GetInfos()", "GetDestinationEntity()", "GetDeliveryEntities()",
                          "GetOwner()", "GetEntity1()", "GetEntity2()", "GetCondPlayerPartyMember()"):
            self.assertIn(reference, catalog)
        client = section(self.runtime, "void TickClient(", "\n}\n\nbool LocalAuthority(")
        self.assertIn("const auto* all = world::ProcessedEntities()", client)
        self.assertNotIn("Entity::GetNPCs()", client)
        diagnostic = section(self.runtime, "char const* UnavailabilityReason(", "bool Fenced(")
        self.assertIn("initialization pending", diagnostic)
        self.assertIn("installation failed", diagnostic)
        self.assertIn("npclease: admission-refused guid=%s name=%s reason=%s", self.runtime)
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        known = section(npcs, "bool HostKnows(", "namespace {")
        for ledger in ("lastSent", "incarnations", "corpses", "absences", "deadOnHost",
                       "journal::RecordedDeath(identity)"):
            self.assertIn(ledger, known)
        journal = (ROOT / "src" / "journal.cpp").read_text(encoding="utf-8")
        deathQuery = section(journal, "bool RecordedDeath(", "unsigned Write(")
        self.assertIn("deaths.count", deathQuery)
        for mutation in ("deaths[", "Kill(", "CountQuest(", "SendDeath("):
            self.assertNotIn(mutation, deathQuery)
