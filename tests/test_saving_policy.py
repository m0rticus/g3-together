# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SAVING_POLICY_TARGET = _testbuild.register_target(
    "saving-policy",
    [_testbuild.unit(
        "tests/saving_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

COOP_ENVELOPE_TARGET = _testbuild.register_target(
    "saving-coop-envelope",
    [_testbuild.inline_unit(
        "saving_coop_envelope",
        r'''
#include "saving_store.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> MakeHeroRecord(std::string const& world, std::string const& save, uint64_t player, unsigned level, int xp)
{
    std::string text = "G3THERO 4\n";
    text += "identity " + world + " " + save + " " + std::to_string(player) + "\n";
    text += "pose 100 200 300 0 0 0 1\n";
    text += "downed 0\n";
    text += "level " + std::to_string(level) + " " + std::to_string(level) + "\n";
    text += "xp " + std::to_string(xp) + "\nlp 10 0\n";
    text += "quick_end 0 0\nend 0\n";
    return std::vector<uint8_t>(text.begin(), text.end());
}
} // namespace

int main()
{
    using namespace g3t::saving;
    const std::string world = "0123456789abcdef0123456789abcdef";
    const std::string save1 = "fedcba9876543210fedcba9876543210";
    const std::string save2 = "11112222333344445555666677778888";
    const uint64_t hostId = 76561198000000001ULL;
    const uint64_t clientId = 76561198000000002ULL;

    // Fallback empty G3TPERS1 personal state packs cleanly with Player 2's G3THERO 4 character record.
    std::vector<uint8_t> emptyPersonal;
    assert(personal::Encode({}, emptyPersonal));
    assert(emptyPersonal.size() == 12);

    const auto hostHero = MakeHeroRecord(world, save1, hostId, 5, 4500);
    const auto clientHero = MakeHeroRecord(world, save1, clientId, 2, 1700);
    std::vector<uint8_t> envelope;
    assert(store::PackPlayer(clientHero, emptyPersonal, envelope));

    std::vector<uint8_t> unpackedHero, unpackedPersonal;
    assert(store::UnpackPlayer(envelope, unpackedHero, unpackedPersonal));
    assert(unpackedHero == clientHero);
    assert(unpackedPersonal == emptyPersonal);

    store::Save manifest;
    manifest.key = {world, save1, hostId};
    store::Set(manifest, {store::NativeHeader, 1, 0, {1, 2, 3, 4}});
    store::Set(manifest, {store::NativeWorld, 1, 0, {5, 6, 7, 8, 9}});
    store::Set(manifest, {store::Character, 4, hostId, hostHero});
    store::Set(manifest, {store::Personal, 1, hostId, emptyPersonal});
    store::Set(manifest, {store::Character, 4, clientId, unpackedHero});
    store::Set(manifest, {store::Personal, 1, clientId, unpackedPersonal});
    store::Set(manifest, {store::Journal, 1, 0, {'d', 'o', 'w', 'n', 'e', 'd', ' ', '0', '\n'}});

    std::vector<uint8_t> sidecar;
    assert(store::Encode(manifest, sidecar));
    store::Save loaded;
    assert(store::Decode(sidecar, loaded));
    auto const* p1 = store::Find(loaded, store::Character, hostId);
    auto const* p2 = store::Find(loaded, store::Character, clientId);
    assert(p1 && p2);
    assert(p1->bytes == hostHero);
    assert(p2->bytes == clientHero);

    // Offline Player 2 character rebinds cleanly onto save2 when host saves solo later.
    std::vector<uint8_t> rebound = p2->bytes;
    assert(store::Rebind(rebound, {world, save2, clientId}));
    Key reboundKey;
    assert(store::HeroKey(rebound, reboundKey));
    assert(Exact(reboundKey, {world, save2, clientId}));
    return 0;
}
''',
        includes=[ROOT / "src"],
    )],
)


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
        cls.persist = "\n".join(
            _testbuild.src_text(name)
            for name in ("persist.cpp", "persist_hooks.cpp", "persist_storage.cpp", "persist_client.cpp")
        )
        cls.net = _testbuild.src_text("net.cpp")
        cls.hero = "\n".join(
            _testbuild.src_text(name)
            for name in ("hero.cpp", "hero_codec.cpp")
        )

    def test_production_record_policy_executes_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = SAVING_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
        self.assertIn("isHost ? PeerAccount(from) : steam::PlayerId()", self.net)
        self.assertIn("note.account != authenticated", self.net)
        account = function_body(self.net, r"\buint64_t\s+PeerAccount\s*\(")
        auth_helper = function_body(self.net, r"\bbool\s+ReadAuthenticatedPeerIdentity\s*\(")
        self.assertIn("ReadAuthenticatedPeerIdentity(peers[id], account, name, generation)", account)
        self.assertIn("steam::PeerIdentity(peer->address.port, account, name, generation)", auth_helper)
        self.assertIn("steam::DirectPeerIdentity(peer->address.host, peer->address.port, account, name, generation)", auth_helper)
        self.assertIn("account == peerAccounts[id] && generation == peerRelayGenerations[id]", account)
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

    def test_host_load_retires_live_world_and_drains_pvs_before_native_load(self):
        load = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertIn("if (worldCleanup) worldCleanup();", load)
        self.assertIn("clock::OnWorldChange();", load)
        self.assertLess(load.index("if (worldCleanup) worldCleanup();"),
                        load.index("originalLoad(self, name)", load.index('CancelSave("canceled-load")')))

    def test_character_validation_and_restore_use_in_memory_records_and_door_admission(self):
        validate = function_body(self.persist, r"\bbool\s+ValidateCharacter\s*\(")
        self.assertIn("hero::ValidateRecordMemory(bytes.data(), bytes.size(), key)", validate)
        self.assertNotIn(".validate", validate)
        apply_fn = function_body(self.persist, r"\bbool\s+ApplyCharacter\s*\(")
        self.assertIn("hero::LoadRecordFromMemory(bytes.data(), bytes.size(), key)", apply_fn)
        self.assertNotIn(".restore", apply_fn)
        on_world = function_body(self.persist, r"\bvoid\s+OnWorld\s*\(")
        ready_branch = on_world.split("if (note.op == net::WorldReady)", 1)[1].split("if (note.op != net::WorldHost)", 1)[0]
        self.assertIn("!story::AdmissionReady() || !doors::AdmissionReady()", ready_branch)

    def test_coop_save_load_and_rejoin_never_abort_on_personal_containers(self):
        with tempfile.TemporaryDirectory() as folder:
            result = COOP_ENVELOPE_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        readiness = function_body(self.persist, r"\bchar const\*\s+SaveReadiness\s*\(")
        self.assertIn("net::IsHost() ? NativeSaveReason() : ClientSaveReason()", readiness)
        client_reason = function_body(self.persist, r"\bchar const\*\s+ClientSaveReason\s*\(")
        self.assertNotIn("IsSaveAllowed()", client_reason)
        self.assertNotIn("GetIsRunning()", client_reason)
        self.assertIn("self.IsDead() && !downed::Held()", client_reason)

        save_now = function_body(self.persist, r"\bvoid\s+CALLBACK\s+SaveNow\s*\(")
        self.assertIn("CapturePersonalWithFallback(steam::PlayerId(), state)", save_now)
        self.assertNotIn("if (!containers::CapturePersonal(state)) { CancelSave", save_now)

        capture_player = function_body(self.persist, r"\bbool\s+CapturePlayer\s*\(")
        self.assertIn("CapturePersonalWithFallback(key.player, state)", capture_player)
        self.assertNotIn('reason = "personal-record"', capture_player)

        tick = function_body(self.persist, r"\bvoid\s+Tick\s*\(")
        self.assertNotIn('!containers::RestorePersonal(receivedPersonal) ? "personal-record"', tick)
        self.assertNotIn('SaveFailure("personal-record")', tick)

        stacks_fn = function_body(self.hero, r"\bstd::vector<Stack>\s+Stacks\s*\(")
        self.assertIn("self.Inventory.GetQuality(i) & QualityMask", stacks_fn)
        quicks_fn = function_body(self.hero, r"\bstd::vector<Quick>\s+QuickSlotsOf\s*\(")
        self.assertIn("self.Inventory.GetQuality(i) & QualityMask", quicks_fn)


if __name__ == "__main__":
    unittest.main()
