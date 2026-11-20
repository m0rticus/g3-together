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

HERO_RESTORE_TARGET = _testbuild.register_target(
    "hero-restore-stacks",
    [
        _testbuild.unit("src/hero_codec.cpp", std="c++17", warnings="strict"),
        _testbuild.inline_unit(
            "hero_restore_stacks",
            r'''
#include "hero_codec.h"
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

namespace {
using g3t::hero::codec::ParsedRecord;
using g3t::hero::codec::RestoreItemStack;
using g3t::hero::codec::RestoreStackOrder;
using g3t::hero::codec::SameStacks;
using g3t::hero::codec::Schema;
using g3t::hero::codec::Stack;

// Mirrors Game.dll gCInventoryStackList::CreateItems (0x100c3700), MustSplitBeforeLinking (0x100c0550),
// gCInventoryStack::Split (0x100c09b0), and gCInventory_PS::EquipStackToSlot (0x100b80e0).
struct FakeNativeInventory {
    std::vector<Stack> stacks;
    std::vector<int> quickSlots;

    static bool MustSplitBeforeLinking(int slot)
    {
        return slot > 0 && slot != 14; // gESlot_Ammo (14) returns false unless RightHand.
    }

    int StackCount() const { return static_cast<int>(stacks.size()); }
    int GetAmount(int index) const { return stacks[static_cast<size_t>(index)].amount; }

    int CreateItems(Stack const& item)
    {
        for (size_t i = 0; i < stacks.size(); ++i) {
            Stack& existing = stacks[i];
            if (existing.id != item.id || existing.quality != item.quality || existing.category != item.category) continue;
            const bool linked = existing.slot != 0;
            const bool hasQuick = quickSlots[i] >= 0;
            if ((!linked && !hasQuick) || !MustSplitBeforeLinking(existing.slot)) {
                existing.amount += item.amount;
                return static_cast<int>(i);
            }
        }
        Stack created = item;
        created.slot = 0;
        stacks.push_back(created);
        quickSlots.push_back(-1);
        return static_cast<int>(stacks.size() - 1);
    }

    int SplitStack(int index, int splitAmount)
    {
        if (index < 0 || static_cast<size_t>(index) >= stacks.size()) return -1;
        Stack& existing = stacks[static_cast<size_t>(index)];
        if (splitAmount <= 0 || splitAmount >= existing.amount) return -1;
        existing.amount -= splitAmount;
        Stack tail = existing;
        tail.amount = splitAmount;
        tail.slot = 0;
        stacks.push_back(tail);
        quickSlots.push_back(-1);
        return static_cast<int>(stacks.size() - 1);
    }

    bool EquipStackToSlot(int index, int slot)
    {
        if (index < 0 || static_cast<size_t>(index) >= stacks.size() || slot <= 0) return false;
        for (Stack& other : stacks) {
            if (other.slot == slot) other.slot = 0;
        }
        stacks[static_cast<size_t>(index)].slot = slot;
        return true;
    }
};

std::string MakeFullRecord(g3t::saving::Key const& key, std::vector<Stack> const& stacks)
{
    size_t tagCount = 0;
    char const* const* tags = g3t::hero::codec::AttributeTags(tagCount);
    std::string text = "G3THERO 4\n";
    text += "identity " + key.world + " " + key.save + " " + std::to_string(key.player) + "\n";
    text += "pose 100 200 300 0 0 0 1\ndowned 0\nlevel 4 4\nxp 2500\nlp 10 0\n";
    for (size_t i = 0; i < tagCount; ++i) {
        std::string const tag = tags[i];
        if (tag == "HP" || tag == "MP" || tag == "SP") text += "attr " + tag + " 100 0 100 0\n";
        else text += "attr " + tag + " 100 0\n";
    }
    for (Stack const& s : stacks) {
        text += "stack " + s.id + " " + std::to_string(s.amount) + " " + std::to_string(s.quality) + " " +
                std::to_string(s.slot) + " " + std::to_string(s.category);
        if (s.category == 10) text += " " + std::to_string(s.learned);
        text += "\n";
    }
    text += "quick_end 0 0\nend " + std::to_string(stacks.size()) + "\n";
    return text;
}
} // namespace

int main()
{
    const g3t::saving::Key key{"07c4959c878d0258fcad8b2d2af97409", "e0037c47a774b7683e887822e481ad11", 76561198068674551ULL};
    const std::string bow = "2c77e863bf84f54ea4342bfdeab8eb72";
    const std::string arrow = "582bdbe87e22c541a1041212ab0ef911";
    const std::string ring = "91ab440123cd55ef6789ab0123cd45ef";
    const std::string potion = "aa11bb22cc33dd44ee55ff6677889900";
    const std::string perk = "ff00ee11dd22cc33bb44aa5599668877";
    Schema schema;
    schema.minSlot = 0;
    schema.maxSlot = 15;
    schema.categoryCount = 16;
    schema.skillCategory = 10;
    schema.spellbookCategory = 9;

    // Saved stacks sorted by (id, slot): slot=0 bow precedes slot=7 bow, just as in G3_World_01_0372.g3t.
    const std::vector<Stack> wanted = {
        {bow, 1, 0, 0, 2, -1},
        {bow, 1, 0, 7, 2, -1},
        {arrow, 25, 0, 0, 3, -1},
        {arrow, 50, 0, 14, 3, -1},
        {ring, 1, 0, 12, 5, -1},
        {ring, 1, 0, 13, 5, -1},
        {potion, 5, 0, 0, 4, -1},
        {potion, 3, 0, 0, 4, -1},
        {perk, 1, 0, 0, 10, 1},
    };
    const std::string raw = MakeFullRecord(key, wanted);
    ParsedRecord parsed;
    assert(g3t::hero::codec::ParseBoundRecord(raw.data(), raw.size(), key, schema, parsed) == nullptr);

    // Without RestoreStackOrder + RestoreItemStack, CreateItems merges slot=0 + slot=7 bow into {slot=7, amount=2}.
    FakeNativeInventory broken;
    for (Stack const& s : parsed.stacks) {
        if (s.category == schema.skillCategory) { broken.stacks.push_back(s); broken.quickSlots.push_back(-1); continue; }
        const int idx = broken.CreateItems(s);
        if (s.slot != 0) broken.EquipStackToSlot(idx, s.slot);
    }
    assert(!SameStacks(broken.stacks, parsed.stacks));

    // With RestoreStackOrder + RestoreItemStack, skills, equipped items, split ammo, twin rings, and split consumables match.
    FakeNativeInventory restored;
    const auto order = RestoreStackOrder(parsed.stacks, schema.skillCategory, 0);
    assert(order.size() == parsed.stacks.size());
    assert(parsed.stacks[order[0]].category == schema.skillCategory);
    for (size_t idx : order) {
        Stack const& s = parsed.stacks[idx];
        if (s.category == schema.skillCategory) { restored.stacks.push_back(s); restored.quickSlots.push_back(-1); continue; }
        assert(RestoreItemStack(s, 0, restored) == nullptr);
    }
    assert(restored.stacks.size() == parsed.stacks.size());
    assert(SameStacks(restored.stacks, parsed.stacks));
    return 0;
}
''',
            includes=[ROOT / "src"],
            std="c++17",
            warnings="strict",
        ),
    ],
)

HERO_CODEC_V5_TARGET = _testbuild.register_target(
    "hero-codec-v5",
    [
        _testbuild.unit("src/hero_codec.cpp", std="c++17", warnings="strict"),
        _testbuild.inline_unit(
            "hero_codec_v5",
            r'''
#include "hero_codec.h"
#include "saving_store.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

int main()
{
    using namespace g3t::hero::codec;
    using namespace g3t::saving;

    const Key hostKey{"07c4959c878d0258fcad8b2d2af97409", "e0037c47a774b7683e887822e481ad11", 76561198068674551ULL};
    const Key clientKey{"07c4959c878d0258fcad8b2d2af97409", "e0037c47a774b7683e887822e481ad11", 76561198273277479ULL};
    Schema schema{0, 15, 16, 10, 9};

    ParsedRecord rec;
    rec.key = hostKey;
    rec.pose = {{88320.2f, 5127.7f, -10964.0f}, {0.0f, 1.0f, 0.0f, -0.008f}};
    rec.downed = 1;
    rec.level = 12;
    rec.levelMax = 14;
    rec.xp = 15400;
    rec.lpAttribs = 25;
    rec.lpPerks = 3;
    size_t tagCount = 0;
    char const* const* tags = AttributeTags(tagCount);
    for (size_t i = 0; i < tagCount; ++i) {
        RecordAttribute a;
        a.tag = tags[i];
        a.base = 100 + static_cast<int32_t>(i);
        a.modifier = 5;
        if (StatTag(a.tag)) { a.baseMaximum = 200; a.maximumModifier = 10; }
        rec.attrs.push_back(a);
    }
    const std::string sword = "11112222333344445555666677778888";
    const std::string spell = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string perk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    rec.stacks = {
        {sword, 1, 0, 1, 2, -1},
        {spell, 1, 0, 0, 9, -1},
        {perk, 1, 0, 0, 10, 1},
    };
    rec.spells = {spell};
    rec.quicks = {{2, sword, 0}};
    rec.extraSections = {{300, HeroSectionOptional, {9, 8, 7, 6}}};

    std::vector<uint8_t> bin;
    assert(EncodeRecord(rec, schema, bin) == nullptr);
    assert(bin.size() >= HeroHeaderSize && std::memcmp(bin.data(), "G3THERO\0", 8) == 0);

    Key inspectedKey;
    uint32_t inspectedVer = 0;
    assert(store::HeroKey(bin, inspectedKey, &inspectedVer));
    assert(inspectedVer == 5 && Exact(inspectedKey, hostKey));

    ParsedRecord parsed;
    assert(ParseBoundRecord(reinterpret_cast<char const*>(bin.data()), bin.size(), hostKey, schema, parsed) == nullptr);
    assert(parsed.schemaVersion == 5 && parsed.minReaderVersion == 5);
    assert(parsed.downed == 1 && parsed.level == 12 && parsed.xp == 15400);
    assert(parsed.stacks.size() == 3 && parsed.spells.size() == 1 && parsed.quicks.size() == 1);
    assert(parsed.extraSections.size() == 1 && parsed.extraSections[0].type == 300 &&
           parsed.extraSections[0].bytes == std::vector<uint8_t>({9, 8, 7, 6}));

    const std::string nextSave = "fedcba9876543210fedcba9876543210";
    std::vector<uint8_t> rebound = bin;
    assert(store::Rebind(rebound, {hostKey.world, nextSave, hostKey.player}));
    ParsedRecord reboundParsed;
    assert(ParseBoundRecord(reinterpret_cast<char const*>(rebound.data()), rebound.size(),
                            {hostKey.world, nextSave, hostKey.player}, schema, reboundParsed) == nullptr);
    assert(reboundParsed.extraSections.size() == 1 &&
           reboundParsed.extraSections[0].bytes == std::vector<uint8_t>({9, 8, 7, 6}));

    std::vector<uint8_t> future = bin;
    PutLeAt(future.data() + 8, 6, 4);
    PutLeAt(future.data() + 12, 5, 4);
    PutLeAt(future.data() + 16, 56, 4);
    future.insert(future.begin() + 48, 8, 0x55);
    PutLeAt(future.data() + 44, Crc32Zeroed(future.data(), 44, 24, 4), 4);
    PutLeAt(future.data() + 24, Crc32Zeroed(future.data(), future.size(), 24, 4), 4);
    ParsedRecord futureParsed;
    assert(ParseBoundRecord(reinterpret_cast<char const*>(future.data()), future.size(), hostKey, schema, futureParsed) == nullptr);
    assert(futureParsed.schemaVersion == 6 && futureParsed.minReaderVersion == 5);

    auto breaking = future;
    PutLeAt(breaking.data() + 12, 6, 4);
    PutLeAt(breaking.data() + 44, Crc32Zeroed(breaking.data(), 44, 24, 4), 4);
    PutLeAt(breaking.data() + 24, Crc32Zeroed(breaking.data(), breaking.size(), 24, 4), 4);
    assert(ParseBoundRecord(reinterpret_cast<char const*>(breaking.data()), breaking.size(), hostKey, schema, futureParsed) != nullptr);

    for (size_t offset : {size_t(8), size_t(24), size_t(44), size_t(52), bin.size() - 1}) {
        auto corrupt = bin;
        corrupt[offset] ^= 0x80;
        assert(ParseBoundRecord(reinterpret_cast<char const*>(corrupt.data()), corrupt.size(), hostKey, schema, futureParsed) != nullptr);
        assert(!store::HeroKey(corrupt, inspectedKey));
    }
    auto truncated = bin;
    truncated.pop_back();
    assert(ParseBoundRecord(reinterpret_cast<char const*>(truncated.data()), truncated.size(), hostKey, schema, futureParsed) != nullptr);

    std::string legacyHeroText = "G3THERO 4\nidentity " + clientKey.world + " " + clientKey.save + " " +
        std::to_string(clientKey.player) + "\npose 100 200 300 0 0 0 1\ndowned 0\nlevel 2 2\nxp 100\nlp 0 0\n";
    for (size_t i = 0; i < tagCount; ++i) {
        std::string const tag = tags[i];
        legacyHeroText += StatTag(tag) ? "attr " + tag + " 50 0 100 0\n" : "attr " + tag + " 50 0\n";
    }
    legacyHeroText += "quick_end 0 0\nend 0\n";
    std::vector<uint8_t> legacyHero(legacyHeroText.begin(), legacyHeroText.end());

    store::Save mixed;
    mixed.key = hostKey;
    store::Set(mixed, {store::NativeHeader, 1, 0, {1, 2, 3}});
    store::Set(mixed, {store::NativeWorld, 1, 0, {4, 5, 6}});
    store::Set(mixed, {store::Journal, 1, 0, {'d','o','w','n','e','d',' ','0','\n'}});
    store::Set(mixed, {store::Character, 5, hostKey.player, bin});
    store::Set(mixed, {store::Character, 4, clientKey.player, legacyHero});
    std::vector<uint8_t> containerBytes;
    assert(store::Encode(mixed, containerBytes));
    store::Save decodedMixed;
    assert(store::Decode(containerBytes, decodedMixed));
    assert(store::Find(decodedMixed, store::Character, hostKey.player)->version == 5);
    assert(store::Find(decodedMixed, store::Character, clientKey.player)->version == 4);

    ParsedRecord legacyParsed;
    assert(ParseBoundRecord(reinterpret_cast<char const*>(legacyHero.data()), legacyHero.size(), clientKey, schema, legacyParsed) == nullptr);
    assert(legacyParsed.schemaVersion == 4 && legacyParsed.minReaderVersion == 4);
    return 0;
}
''',
            includes=[ROOT / "src"],
            std="c++17",
            warnings="strict",
        ),
    ],
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
        self.assertIn("if (wasActive) QueueReply(from, Note(net::SavePrepare, savingManifest));", controls)
        start = function_body(self.persist, r"\bbool\s+StartSave\s*\(")
        self.assertLess(start.index("SetTimer(saveWindow, SaveTimer, 30, &SaveNow)"), start.index("operation.active = true;"))
        native_ready = function_body(self.persist, r"\bbool\s+NativeReady\s*\(")
        self.assertIn("rejoinHooks = false;", native_ready)

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
        self.assertIn('publishMarker = path + ".publish"', deletion)
        self.assertIn("DeleteFileA(publishMarker.c_str())", deletion)
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
        spells_fn = function_body(self.hero, r"\bstd::vector<std::string>\s+LearnedSpellsOf\s*\(")
        self.assertIn("self.Inventory.GetAmount(i) <= 0 && self.Inventory.GetSlotFromStack(i) == gESlot_None", spells_fn)
        quicks_fn = function_body(self.hero, r"\bstd::vector<Quick>\s+QuickSlotsOf\s*\(")
        self.assertIn("self.Inventory.GetQuality(i) & QualityMask", quicks_fn)
        self.assertIn("self.Inventory.GetAmount(i) <= 0 && self.Inventory.GetSlotFromStack(i) == gESlot_None", quicks_fn)

    def test_duplicate_stacks_and_load_lifecycle_regressions(self):
        with tempfile.TemporaryDirectory() as folder:
            result = HERO_RESTORE_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        load_hero = function_body(self.hero, r"\bbool\s+LoadRecordImpl\s*\(")
        rollback_hero = function_body(self.hero, r"\bvoid\s+RollbackHeroSnapshot\s*\(")
        for fn in (load_hero, rollback_hero):
            self.assertIn("codec::RestoreStackOrder(", fn)
            self.assertIn("codec::RestoreItemStack(", fn)
        self.assertLess(load_hero.index("saving::PoseRestored(pose, actual)"),
                        load_hero.index('downed::Restore(savedDowned != 0, "hero-record")'))
        prep_hero = function_body(self.hero, r"\bchar const\*\s+PrepareInventoryForSnapshot\s*\(")
        skill_hero = function_body(self.hero, r"\bchar const\*\s+ApplySkillStack\s*\(")
        self.assertIn("RemoveStack(", prep_hero)
        self.assertIn("RemoveStack(", skill_hero)
        capture_hero = function_body(self.hero, r"\bbool\s+CaptureHeroSnapshot\s*\(")
        self.assertIn("ReadNormalizedPose(hero)", capture_hero)
        quick_apply = function_body(self.hero, r"\bbool\s+ApplySpellAndQuick\s*\(")
        self.assertLess(
            quick_apply.index("AssignStackToQuickSlot(i, -1)"),
            quick_apply.index("for (Quick const& q : wanted)"),
        )

        load_detour = function_body(self.persist, r"\bbool\s+__stdcall\s+LoadDetour\s*\(")
        self.assertNotIn('SaveFailure("character-record")', load_detour)
        tick = function_body(self.persist, r"\bvoid\s+Tick\s*\(")
        self.assertNotIn("SaveFailure(refusal)", tick)
        self.assertIn('ClientRestoreFailure("character-record")', tick)
        self.assertIn("for (uint8_t i = 2; i < net::MaxPlayers; ++i) clients[i].nextTry = 0;", tick)
        new_epoch = function_body(self.persist, r"\bvoid\s+NewEpoch\s*\(")
        self.assertIn("clients[i].nextTry = 0", new_epoch)
        apply_fn = function_body(self.persist, r"\bbool\s+ApplyCharacter\s*\(")
        self.assertIn("combat::OnWorldChange();", apply_fn)
        self.assertIn("xpshare::OnWorldChange();", apply_fn)
        on_world = function_body(self.persist, r"\bvoid\s+OnWorld\s*\(")
        self.assertIn("combat::OnWorldChange(); xpshare::OnWorldChange();", on_world)

    def test_versioned_hero_record_and_pose_restoration(self):
        with tempfile.TemporaryDirectory() as folder:
            result = HERO_CODEC_V5_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        load_hero = function_body(self.hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn("if (net::IsHost())", load_hero)
        self.assertLess(load_hero.index("if (net::IsHost())"),
                        load_hero.index("self.MoveTo(body::Pose(*hero, pose.position, pose.rotation))"))
        self.assertIn("saving::PoseRestored(pose, actual)", load_hero)
        self.assertNotIn("dx * dx + dy * dy + dz * dz > 1.0f ||", load_hero)

        save_now = function_body(self.persist, r"\bvoid\s+CALLBACK\s+SaveNow\s*\(")
        self.assertIn("saving::store::HeroKey(hero, heroKey, &heroVersion)", save_now)
        on_char = function_body(self.persist, r"\bvoid\s+OnCharacter\s*\(")
        self.assertIn("saving::store::HeroKey(hero, embedded, &heroVersion)", on_char)

        host_tick = function_body(self.persist, r"\bvoid\s+HostTick\s*\(")
        self.assertIn("xpshare::PreparePeerAdmissionOrigin(i, generation, record != nullptr);", host_tick)
        start_save = function_body(self.persist, r"\bbool\s+StartSave\s*\(")
        self.assertIn("xpshare::BeginSaveCapture();", start_save)
        finish_save = function_body(self.persist, r"\bvoid\s+FinishSave\s*\(")
        self.assertIn("xpshare::CommitSaveCapture();", finish_save)
        self.assertIn("xpshare::CancelSaveCapture();", finish_save)
        apply_fn = function_body(self.persist, r"\bbool\s+ApplyCharacter\s*\(")
        self.assertIn("xpshare::OnClientRecordRestored(true);", apply_fn)


if __name__ == "__main__":
    unittest.main()
