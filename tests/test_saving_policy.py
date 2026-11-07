# SPDX-License-Identifier: GPL-3.0-or-later
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function_body(source, signature):
    for match in re.finditer(signature, source):
        opening = source.find("{", match.end())
        if opening < 0 or ";" in source[match.end():opening]:
            continue
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

    def test_production_record_policy_executes_without_engine(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "saving-policy.exe"
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     str(ROOT / "tests" / "saving_policy_test.cpp"), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)

    def test_no_world_file_transport_or_client_native_operations(self):
        for obsolete in ("QueueFileTransfer", "OnFileChunk", "OnFileDone", "SaveStored",
                         "StoreBundle", "OrderAutoload", "PublishLatest", "Barrier"):
            self.assertNotIn(obsolete, self.persist)
        self.assertNotIn("FileTransfer", self.net)
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertLess(load.index("if (!net::IsHost())"), load.index("originalLoad(self, name)", 100))
        save = function_body(self.persist, r"\bbool\s+__stdcall\s+SaveDetour\s*\(")
        self.assertEqual(save.count("originalSave("), 1)  # Disabled/partial hooks only.
        self.assertIn("net::SaveRequest", save)
        self.assertIn("net::SendSave(1, requested)", save)

    def test_character_sender_is_authenticated_and_gate_exempt(self):
        gated = function_body(self.net, r"\bbool\s+Gated\s*\(")
        self.assertIn("case MsgCharacter:", gated)
        self.assertIn("case MsgSave:", gated)
        self.assertIn("note.account != steam::PeerId(peer->address.port)", self.net)
        self.assertIn("note.account != steam::PlayerId()", self.net)
        self.assertIn("packet->flags & ENET_PACKET_FLAG_RELIABLE", self.net)
        send = function_body(self.net, r"\bbool\s+SendCharacter\s*\(")
        self.assertIn("ChannelReliable, true", send)
        self.assertNotIn("ChannelBulk", send)
        self.assertIn("saving::store::PlayerLimit", send)

    def test_single_host_native_save_is_outside_process(self):
        timer = function_body(self.persist, r"\bvoid\s+CALLBACK\s+SaveNow\s*\(")
        self.assertIn("processDepth || nativeLoadDepth || nativeSaveDepth", timer)
        self.assertIn("AllReady()", timer)
        self.assertEqual(timer.count("originalSave("), 1)
        self.assertIn("lowerSaveCalls != 1 || !lowerSaveGood", timer)
        self.assertIn("operation.Expired(GetTickCount())", timer)
        self.assertLess(timer.index("saving::store::Encode"), timer.index("saving::store::Publish("))
        self.assertIn("if (!nativeSaved)", timer)
        self.assertIn("nativeSavedAt", timer)
        self.assertIn("saving::store::RetryNative", timer)
        self.assertIn("saving::store::ReadNative", timer)
        self.assertNotIn('"disk-write"', timer)
        self.assertNotIn("NewEpoch", timer)
        self.assertNotIn("localKind =", timer)

    def test_native_refusal_matches_verified_predicate_without_combat_cap(self):
        ready = function_body(self.persist, r"\bchar const\*\s+NativeSaveReason\s*\(")
        self.assertIn("saving::NativeSaveRefusal", ready)
        self.assertIn("self.NPC.DefeatedByPlayer", ready)
        self.assertLess(ready.index("if (!infos)"), ready.index("session.IsSaveAllowed()"))
        self.assertNotIn("Combat", ready)
        self.assertNotIn("IsFighting", ready)
        menu = function_body(self.persist, r"\bbool\s+__stdcall\s+MenuNewSlot\s*\(")
        self.assertLess(menu.index("SaveReadiness()"),
                        menu.index("originalNewSlot(self);", menu.index("const unsigned before")))

    def test_pup_entry_and_lower_boolean_remain_authoritative(self):
        install = function_body(self.persist, r"\bvoid\s+Install\s*\(")
        self.assertIn("originalWorldLoad = reinterpret_cast<WorldLoadFn>(resolved[3])", install)
        self.assertIn("HookWorldLoadCall(resolved[1] + 0x2b2, resolved[3])", install)
        self.assertNotIn("ai::JumpTo(resolved[3]", install)
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("hudOk && lowerLoadCalls == 1 && lowerLoadGood", load)
        self.assertLess(load.index("saving::store::Decode"), load.index('CancelSave("canceled-load")'))
        self.assertLess(load.index("Materialize"), load.index("const bool hudOk"))
        self.assertIn('saving::Prefix(slot, "G3T_pending_")', load)
        self.assertIn("hasSidecar && bytes.empty()", load)
        self.assertIn('"publish-incomplete"', load)
        self.assertLess(load.index("story::OnWorldChange()"), load.index("const bool hudOk"))
        self.assertLess(load.index("doors::OnWorldChange()"), load.index("const bool hudOk"))
        self.assertNotIn('saving::Prefix(slot, "G3T_")', load)  # G3T_Sandbox is a normal legacy/native slot.

    def test_records_retries_bound_to_epoch_account_and_generation(self):
        poll = function_body(self.persist, r"\bvoid\s+PollSave\s*\(")
        self.assertIn("operation.Expired(now)", poll)
        self.assertIn("member.generation != net::PeerGeneration(i)", poll)
        self.assertIn("member.account != net::PeerAccount(i)", poll)
        receive = function_body(self.persist, r"\bvoid\s+OnCharacter\s*\(")
        self.assertIn("operation.Accept(", receive)
        self.assertIn("UnpackPlayer(bytes, hero, state)", receive)
        self.assertIn("ValidateCharacter(hero, key)", receive)
        self.assertIn("saving::store::Personal, 1, key.player", receive)
        self.assertIn("if (member.ready) return", receive)
        controls = function_body(self.persist, r"\bvoid\s+OnSave\s*\(")
        self.assertIn("cached.token == note.save", controls)
        self.assertIn("cached.complete", controls)
        self.assertIn("captured.bytes", controls)
        self.assertIn("requests[from].token == note.save", controls)

    def test_plain_native_load_does_not_require_published_steam_account(self):
        legacy = function_body(self.persist, r"\bbool\s+ReadLegacy\s*\(")
        self.assertIn("saving::Id(save.key.world) && saving::Id(save.key.save)", legacy)
        self.assertNotIn("return saving::Valid(save.key)", legacy)
        self.assertIn("PersonalReadyForRestore(receivedPersonal)", self.persist)
        self.assertIn('SaveFailure("personal-hooks")', self.persist)

    def test_slot_deletion_retires_container_and_preserves_recovery(self):
        deletion = function_body(self.persist, r"\bbool\s+__fastcall\s+DeleteDetour\s*\(")
        self.assertIn("nativeSaveDepth", deletion)
        self.assertIn("DeleteFileA(path.c_str())", deletion)
        self.assertIn("Materialize(slot, intact)", deletion)
        self.assertIn("!Exists(Path(slot, 0)) && !Exists(Path(slot, 1))", deletion)

    def test_character_restores_before_admission_and_never_imports_latest(self):
        tick = function_body(self.persist, r"\bvoid\s+Tick\s*\(")
        self.assertIn("ApplyCharacter(receivedRecord", tick)
        self.assertIn("!resetLocalWorld", tick)
        self.assertIn("restoreFailed = true", tick)
        self.assertIn("if (receivedWelcome != seenWelcome)", tick)
        evaluate = function_body(self.persist, r"\bvoid\s+Evaluate\s*\(")
        self.assertIn("ownCharacterReady", evaluate)
        self.assertIn("!needsNewHero && !restoreFailed", evaluate)
        self.assertIn("synced == hostWorld.epoch", evaluate)
        self.assertNotIn("LatestPath", self.persist)
        self.assertNotIn("FindFirstFile", self.persist)
        new_epoch = function_body(self.persist, r"\bvoid\s+NewEpoch\s*\(")
        self.assertIn("notice.kind = net::KindMenu", new_epoch)
        self.assertIn("net::SendWorld(notice, i)", new_epoch)
        host_tick = function_body(self.persist, r"\bvoid\s+HostTick\s*\(")
        self.assertIn("!restoreFailed ? localKind : net::KindMenu", host_tick)
        self.assertIn("if (note.kind == net::KindMenu) continue", host_tick)

    def test_extensions_preserve_unknown_blocks_and_restore_absence(self):
        capture = function_body(self.persist, r"\bbool\s+CaptureExtensions\s*\(")
        self.assertIn("entry.capture(bytes)", capture)
        self.assertIn("saving::store::ExtensionLimit", capture)
        start = function_body(self.persist, r"\bbool\s+StartSave\s*\(")
        self.assertIn("block.type >= saving::store::Extension", start)
        restore = function_body(self.persist, r"\bbool\s+RestoreExtensions\s*\(")
        self.assertIn("block ? block->version : 0", restore)
        self.assertIn("block ? block->bytes : empty", restore)

    def test_hero_backup_is_durable_and_bound(self):
        capture = function_body(self.persist, r"\bbool\s+CaptureCharacter\s*\(")
        self.assertIn("hero::SaveRecord(path.c_str(), key)", capture)
        save = function_body(self.hero, r"\bbool\s+SaveRecordImpl\s*\(")
        self.assertIn("_commit(_fileno(file))", save)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", save)


if __name__ == "__main__":
    unittest.main()
