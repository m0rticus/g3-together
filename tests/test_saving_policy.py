import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source, signature):
    opening = -1
    for match in re.finditer(signature, source):
        candidate = source.find("{", match.end())
        if candidate >= 0 and ";" not in source[match.end():candidate]:
            opening = candidate
            break
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if not depth:
                return source[opening + 1:index]
    return None


class SavingPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.persist = (ROOT / "src" / "persist.cpp").read_text(encoding="utf-8")
        cls.net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        cls.hero = (ROOT / "src" / "hero.cpp").read_text(encoding="utf-8")

    def test_production_pairing_rules_execute_without_engine(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required for engine-free production policy fixture")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "saving-policy.exe"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "saving_policy_test.cpp"), "-o", str(executable)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)

    def test_save_controls_are_welcomed_direction_checked_and_gate_exempt(self):
        body = function_body(self.net, r"\bbool\s+Gated\s*\(\s*uint8_t\s+type\s*\)")
        self.assertIn("case MsgSave:", body)
        self.assertIn("peers[from] != peer", self.net)
        self.assertIn("(isHost && !clientOp) || (!isHost && !hostOp)", self.net)
        self.assertIn("saving::Protocol", self.net)
        self.assertIn("MsgFileChunk && !isHost", self.net)
        self.assertIn("MsgFileDone && !isHost", self.net)

    def test_incompatible_protocol_is_refused_before_welcome(self):
        receive = function_body(self.net, r"\bvoid\s+OnReceive\s*\(")
        mismatch = receive.index("hello.version != ProtocolVersion")
        refusal = receive.index("different G3 Together version (protocol %u, host %u)")
        refused = receive.index("enet_peer_disconnect_later(peer, 1)")
        welcome = receive.index("const Welcome welcome")
        self.assertLess(mismatch, refusal)
        self.assertLess(refusal, refused)
        self.assertLess(refused, welcome)

    def test_client_save_requests_host_and_never_calls_native_save(self):
        body = function_body(self.persist, r"\bbool\s+__stdcall\s+SaveDetour\s*\(")
        self.assertIsNotNone(body)
        self.assertIn("SafeName(name->GetText())", body)
        self.assertIn("net::SaveRequest", body)
        self.assertIn("net::SendSave(1, note)", body)
        self.assertIn("if (!pairedEnabled) return originalSave && originalSave(self, name)", body)
        self.assertEqual(body.count("originalSave("), 1)  # Only disabled/partial hooks pass through.
        self.assertIn("return false;", body)  # queued != native success
        timer = function_body(self.persist, r"\bvoid\s+CALLBACK\s+SaveNow\s*\(")
        self.assertIn("processDepth || nativeLoadDepth || nativeSaveDepth", timer)
        self.assertIn("barrier.AllReady()", timer)
        self.assertIn("remote::DespawnAll", timer)
        self.assertIn("originalSave(", timer)

    def test_record_selection_never_refreshes_an_old_checkpoint(self):
        body = function_body(self.persist, r"\bbool\s+ChooseCharacter\s*\(")
        self.assertIn("saving::Select(Exists(exact), Exists(latest)", body)
        self.assertIn("hero::ReadRecordKey", body)
        self.assertIn("saving::SameCharacter", body)
        self.assertIn("saving::Exact", body)
        self.assertIn("ClaimLegacy", body)
        self.assertNotIn("FindFirstFile", self.persist)
        self.assertNotIn("24ull", self.persist)
        self.assertNotIn("SnapshotWrittenInWelcome", self.persist)

    def test_manifest_binds_native_files_and_is_published_last(self):
        verify = function_body(self.persist, r"\bbool\s+Verify\s*\(")
        self.assertIn("manifest.size[f] != data[f].size()", verify)
        self.assertIn("manifest.crc[f] != Crc32", verify)
        store = function_body(self.persist, r"\bbool\s+StoreBundle\s*\(")
        self.assertIn("f < net::FileCount", store)
        self.assertIn("WriteAtomic(Path(slot, f)", store)
        atomic = function_body(self.persist, r"\bbool\s+WriteAtomic\s*\(")
        self.assertIn('path + ".tmp"', atomic)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", atomic)
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertLess(load.index("ReadBundle("), load.index("const bool hudOk = originalLoad("))
        self.assertLess(load.index("SafeName("), load.index("Path(slot"))

    def test_commit_stores_same_world_without_loading_or_recapturing_character(self):
        body = function_body(self.persist, r"\bvoid\s+OnFileDone\s*\(")
        self.assertIn("incomingWelcome != net::WelcomeSerial()", body)
        self.assertIn("incomingNote.epoch != hostWorld.epoch", body)
        self.assertIn("prepared.welcome == net::WelcomeSerial()", body)
        self.assertIn("saving::Exact(key, found)", body)
        self.assertIn("StoreBundle(name, incoming)", body)
        self.assertIn("const char* alias = prepared.note.name", body)
        self.assertIn("StoreBundle(alias, incoming)", body)
        self.assertIn("net::SaveStored", body)
        self.assertNotIn("hero::SaveRecord", body)
        self.assertNotIn("originalLoad(", body)

    def test_missing_and_removed_characters_never_open_gate_with_host_hero(self):
        body = function_body(self.persist, r"\bvoid\s+WaitForRecord\s*\(")
        self.assertNotIn("NoCampaignHero", body)
        self.assertNotIn("GetFileAttributesA", body)
        self.assertIn("ApplyCharacter()", body)
        evaluate = function_body(self.persist, r"\bvoid\s+Evaluate\s*\(")
        self.assertIn("!ownCharacterReady", evaluate)
        self.assertIn("!pendingRecord.empty()", evaluate)
        self.assertIn("!waitingImport.empty()", evaluate)
        self.assertIn("net::WorldWaiting", evaluate)

    def test_failed_or_reused_connections_cannot_ack_a_different_checkpoint(self):
        poll = function_body(self.persist, r"\bvoid\s+PollSave\s*\(")
        self.assertIn("net::PeerGeneration(i) != barrier.members[i].generation", poll)
        self.assertIn("PrepareLimit", poll)
        self.assertIn("WriteLimit", poll)
        self.assertIn("TransferLimit", poll)
        self.assertIn("barrier.AllStored()", poll)
        self.assertIn("aliasPublished", poll)

    def test_native_load_uses_lower_result_not_only_masking_hud_boolean(self):
        body = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("hudOk && lowerLoadCalls == 1 && lowerLoadGood", body)
        lower = function_body(self.persist, r"\bbool\s+__fastcall\s+WorldLoadDetour\s*\(")
        self.assertIn("lowerLoadGood = lowerLoadGood && ok", lower)
        self.assertIn("nativeLoadDepth", lower)
        self.assertLess(lower.index("if (pairedEnabled && !observed)"), lower.index("originalWorldLoad("))
        self.assertIn("use HUD.LoadGame", lower)
        self.assertIn("ownCharacterReady = false", body)

    def test_reload_uses_world_incarnation_not_only_last_stored_checkpoint(self):
        evaluate = function_body(self.persist, r"\bvoid\s+Evaluate\s*\(")
        self.assertIn("saving::WorldMatches(hostWorld.epoch, synced, loadedEpoch", evaluate)
        commit = function_body(self.persist, r"\bvoid\s+OnFileDone\s*\(")
        self.assertNotIn("loadedEpoch =", commit)
        settled = function_body(self.persist, r"\bvoid\s+Settled\s*\(")
        self.assertIn("loadedEpoch = hostWorld.epoch", settled)

    def test_character_seeding_load_retains_bound_host_import(self):
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("waitingEpoch == hostWorld.epoch", load)
        self.assertIn("waitingWelcome == net::WelcomeSerial()", load)
        self.assertIn("waitingImport = deferredImport", load)
        self.assertIn("!loadingHostCheckpoint", load)

    def test_reserved_names_match_windows_case_rules(self):
        start = function_body(self.persist, r"\bbool\s+StartSave\s*\(")
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn('saving::Prefix(name, "G3T_")', start)
        self.assertIn('saving::Prefix(slot, "G3T_")', load)
        self.assertIn('saving::Prefix(slot, "G3T_host_")', load)

    def test_valid_alias_repairs_the_complete_canonical_bundle_before_load(self):
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("ReadBundle(canonical, stored, canonicalData)", load)
        self.assertIn("saving::Exact(stored.key, manifest.key)", load)
        self.assertIn("canonicalData[2] == data[2]", load)
        self.assertLess(load.index("StoreBundle(canonical, data)"), load.index("const bool hudOk = originalLoad("))
        self.assertIn('fopen_s(&journalFile, Path(slot, 2).c_str(), "r")', load)

    def test_transient_publication_retries_without_source_file_edit(self):
        wait = function_body(self.persist, r"\bvoid\s+WaitForRecord\s*\(")
        self.assertIn("now - nextRestoreTry", wait)
        self.assertIn("ApplyResult::PublicationPending ? 5000 : 30000", wait)
        self.assertIn("Exists(refusedRecord)", wait)

    def test_all_supported_native_save_routes_use_coordinator(self):
        body = function_body(self.persist, r"\bbool\s+__fastcall\s+WorldSaveDetour\s*\(")
        self.assertIn("if (!pairedEnabled) return originalWorldSave", body)
        self.assertIn("if (nativeSaveDepth)", body)
        self.assertIn("originalWorldSave(self, name, image, metadata, size)", body)
        self.assertIn("SaveDetour(nullptr, &name)", body)
        ready = function_body(self.persist, r"\bbool\s+NativeSaveAllowed\s*\(")
        self.assertLess(ready.index("gCInfoManager_PS::GetInstance()"), ready.index("session.IsSaveAllowed()"))
        self.assertIn("session.GetPlayer() != session.GetHero()", ready)
        self.assertIn("downed::Held()", ready)

    def test_legacy_migration_is_content_bound_and_originals_are_preserved(self):
        legacy = function_body(self.persist, r"\bbool\s+AdoptLegacy\s*\(")
        self.assertIn("LegacyId(data)", legacy)
        self.assertIn("StoreBundle(canonical, data)", legacy)
        self.assertNotIn("StoreBundle(slot,", legacy)
        self.assertNotIn("DeleteFile", legacy)
        claim = function_body(self.persist, r"\bbool\s+ClaimLegacy\s*\(")
        self.assertIn("steam::PlayerId()", claim)
        self.assertIn("== owner", claim)

    def test_mandatory_legacy_migration_never_uses_permissive_diagnostic_load(self):
        body = function_body(self.persist, r"\bApplyResult\s+ApplyCharacter\s*\(")
        self.assertIn("hero::LoadLegacyRecordStrict(pendingRecord.c_str())", body)
        self.assertNotIn("hero::LoadRecord(pendingRecord.c_str())", body)
        strict = function_body(self.hero, r"\bbool\s+LoadLegacyRecordStrict\s*\(")
        self.assertIsNotNone(strict)

    def test_saved_xp_is_not_lifted_to_another_players_total(self):
        xp = (ROOT / "src" / "xpshare.cpp").read_text(encoding="utf-8")
        self.assertNotIn("LiftTo", xp)
        self.assertIn("Gain(static_cast<int>(gain))", xp)
        self.assertIn("waiting = 0", function_body(xp, r"\bvoid\s+OnWorldChange\s*\("))

    def test_trades_cannot_cross_character_checkpoint_capture(self):
        trade = (ROOT / "src" / "trade.cpp").read_text(encoding="utf-8")
        offer = (ROOT / "src" / "offer.cpp").read_text(encoding="utf-8")
        self.assertIn("persist::CheckpointBusy()", function_body(trade, r"\bbool\s+BuildExchange\s*\("))
        self.assertIn("persist::CheckpointBusy()", function_body(offer, r"\bbool\s+Apply\s*\("))
        self.assertIn("persist::CheckpointBusy()", function_body(offer, r"\bbool\s+Reserve\s*\("))

    def test_real_native_hooks_have_unique_named_resolution_and_safe_partial_fallback(self):
        resolve = function_body(self.persist, r"\bbool\s+ResolveNative\s*\(")
        install = function_body(self.persist, r"\bvoid\s+Install\s*\(")
        self.assertIn("saving::native::FindUnique", resolve)
        self.assertIn("matches == 1", resolve)
        self.assertIn("GetProcAddress(game, fn.symbol)", resolve)
        self.assertIn("saving::native::Links", resolve)
        self.assertIn('persist: unresolved %s', resolve)
        self.assertIn("pairedEnabled = originalSave && originalLoad && originalWorldSave && originalWorldLoad", install)
        self.assertIn("partial hooks pass through, paired saves off", install)
        self.assertIn("persist: save and load hooked", install)
        native = (ROOT / "src" / "saving_native.h").read_text(encoding="utf-8")
        self.assertIn("constexpr size_t Stolen = 6", native)
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("if (!pairedEnabled) return originalLoad", load)
        self.assertLess(load.index("self != FileManager(true)"), load.index("const bool hudOk = originalLoad("))

    def test_transfer_total_is_bound_and_import_lifetime_is_bounded(self):
        chunk = function_body(self.persist, r"\bvoid\s+OnFileChunk\s*\(")
        self.assertIn("saving::ChunkFits(incomingTotal[file]", chunk)
        self.assertIn("if (incomingFailed) return", chunk)
        done = function_body(self.persist, r"\bvoid\s+OnFileDone\s*\(")
        self.assertIn("incomingTotal[f] != size[f]", done)
        tick = function_body(self.persist, r"\bvoid\s+Tick\s*\(")
        self.assertIn("now - incomingAt > TransferLimit", tick)
        self.assertIn("incomingWelcome != net::WelcomeSerial()", tick)

    def test_successful_store_retains_prepare_until_final_result(self):
        done = function_body(self.persist, r"\bvoid\s+OnFileDone\s*\(")
        self.assertNotIn("prepared = Prepared{}", done)
        save = function_body(self.persist, r"\bvoid\s+OnSave\s*\(")
        self.assertIn("saving::PreparedMatches", save)
        self.assertIn("note.op == net::SaveCancel || note.op == net::SaveResult", save)
        self.assertIn("prepared = Prepared{}", save)

    def test_host_overlay_is_gated_and_missing_host_record_does_not_schedule_client_import(self):
        pending = function_body(self.persist, r"\bbool\s+HeroPending\s*\(")
        self.assertNotIn("!net::IsHost()", pending)
        self.assertIn("pairedEnabled", pending)
        host = function_body(self.persist, r"\bvoid\s+HostTick\s*\(")
        self.assertLess(host.index("!pendingRecord.empty()"), host.index("Tell(i)"))
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("if (!net::IsHost())", load)

    def test_legacy_and_bound_records_preserve_downed_latch_and_verify_final_stats(self):
        load = function_body(self.hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn("saving::ParseRecordDowned(text, version, haveDowned, savedDowned)", load)
        self.assertIn('downed::Restore(savedDowned != 0, "hero-record")', load)
        self.assertIn("restored character stats do not match the record", load)
        save = function_body(self.hero, r"\bbool\s+SaveRecordImpl\s*\(")
        self.assertIn('fprintf(file, "downed %d\\n"', save)
        self.assertNotIn('else ok = ok && fprintf(file, "downed', save)

    def test_complete_character_preflight_precedes_native_load_and_never_mutates_hero(self):
        choose = function_body(self.persist, r"\bbool\s+ChooseCharacter\s*\(")
        self.assertIn("hero::ValidateRecord(pendingRecord.c_str(), pendingKey)", choose)
        self.assertIn("hero::ValidateLegacyRecord(pendingRecord.c_str())", choose)
        load = function_body(self.hero, r"\bbool\s+LoadRecordImpl\s*\(")
        preflight = load.index("if (validateOnly) {")
        self.assertLess(load.index("declared != wanted.size()"), preflight)
        self.assertLess(load.index("ValidateContents(wanted, inlineSpells, inlineQuick, !validateOnly)"), preflight)
        self.assertLess(preflight, load.index("downed::Restore("))
        self.assertLess(preflight, load.index("DeleteStack("))
        self.assertIn("if (!validateOnly && !Ready(hero))", load)

    def test_final_character_comparison_is_read_only_and_downed_aware(self):
        match = function_body(self.hero, r"\bbool\s+AttributesMatch\s*\(")
        self.assertIn('held && value.tag == "HP" ? 1 : value.base', match)
        self.assertNotIn("Set", match)
        load = function_body(self.hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertEqual(load.count("ApplyAttributes(*memory, attrs)"), 1)
        self.assertLess(load.index("ApplySpellAndQuick("), load.index("AttributesMatch(*memory, attrs, downed::Held())"))

    def test_prepare_capture_lifetime_covers_all_host_phases(self):
        self.assertIn("PreparedLimit = PrepareLimit + WriteLimit + TransferLimit", self.persist)
        tick = function_body(self.persist, r"\bvoid\s+Tick\s*\(")
        self.assertIn("now - prepared.since > PreparedLimit", tick)


if __name__ == "__main__":
    unittest.main()
