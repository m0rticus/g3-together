// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_store.h"
#include <cassert>
#include <fstream>
#include <iterator>

using namespace g3t::saving;
using namespace g3t::saving::store;

static WriteStage stopAt = WriteStage::Opened;
static bool Proceed(WriteStage stage) { return stage != stopAt; }
static Bytes Read(std::string const& path)
{
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
static Bytes Hero(Key const& key)
{
    const std::string text = "G3THERO 4\nidentity " + key.world + " " + key.save + " " +
        std::to_string(key.player) + "\npose 1 2 3 0 0 0 1\ndowned 0\nend 0\n";
    return Bytes(text.begin(), text.end());
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const Key host{std::string(32, 'a'), std::string(32, 'b'), 11};
    Save save{host, {{NativeHeader, 1, 0, Bytes(65, 1)}, {NativeWorld, 1, 0, Bytes(2048, 2)},
                    {Journal, 1, 0, {}}, {Character, 4, 11, Hero(host)},
                    {Character, 4, 12, Hero({host.world, host.save, 12})}, {256, 7, 0, {3,4,5}}}};
    Bytes encoded;
    assert(Encode(save, encoded));
    assert(Modern(encoded));
    Save decoded;
    assert(Decode(encoded, decoded));
    assert(Exact(decoded.key, host) && decoded.blocks.size() == save.blocks.size());
    assert(Find(decoded, Character, 12)->bytes == save.blocks[4].bytes);
    assert(!Find(decoded, Character, 13)); // Account identity, not display name/network slot.
    assert(Find(decoded, 256)->version == 7);
    assert(!Find(decoded, Personal, 12)); // Earlier G3TSAVE3 remains a valid native/default personal-state migration.
    Bytes personal, player;
    assert(g3t::saving::personal::Encode({}, personal));
    assert(PackPlayer(save.blocks[4].bytes, personal, player));
    assert(player.size() == 20 + save.blocks[4].bytes.size() + personal.size());
    Bytes own{9}, state{8};
    assert(UnpackPlayer(player, own, state) && own == save.blocks[4].bytes && state == personal);
    for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), player.size() - 1}) {
        auto damaged = player;
        damaged[offset] ^= 0xff;
        Bytes oldHero = own, oldState = state;
        assert(!UnpackPlayer(damaged, own, state) && own == oldHero && state == oldState);
    }
    auto extraPlayer = player;
    extraPlayer.push_back(0);
    assert(!UnpackPlayer(extraPlayer, own, state));
    auto withPersonal = save;
    withPersonal.blocks.push_back({Personal, 1, 12, personal});
    Bytes revised;
    assert(Encode(withPersonal, revised) && Decode(revised, decoded));
    assert(Find(decoded, Personal, 12)->bytes == personal);
    auto orphanPersonal = withPersonal;
    orphanPersonal.blocks.back().account = 13;
    assert(!Encode(orphanPersonal, revised)); // Personal data must have the same account's character.
    orphanPersonal = withPersonal;
    orphanPersonal.blocks.back().version = 2;
    assert(!Encode(orphanPersonal, revised));
    orphanPersonal = withPersonal;
    orphanPersonal.blocks.back().bytes[0] ^= 0xff;
    assert(!Encode(orphanPersonal, revised));
    assert(Decode(encoded, decoded));
    Bytes roundTrip;
    assert(Encode(decoded, roundTrip) && roundTrip == encoded); // Unknown extension preserved exactly.
    const Bytes previous = encoded;
    auto swappedOwner = encoded;
    swappedOwner[84] = 12; // Existing client account: semantic checks alone would allow this header corruption.
    assert(!Decode(swappedOwner, decoded));
    for (size_t n : {size_t(0), size_t(4), size_t(95), encoded.size() - 1}) {
        Bytes cut(encoded.begin(), encoded.begin() + n);
        Save out = decoded;
        assert(!Decode(cut, out) && out.blocks.size() == decoded.blocks.size());
    }
    assert(Modern(Bytes{'G','3','T','S'})); // Damaged modern save never falls through to native migration.
    for (size_t at : {size_t(8), size_t(12), size_t(16), size_t(20), size_t(84), size_t(92), encoded.size() - 1}) {
        auto corrupt = encoded;
        corrupt[at] ^= 0xff;
        Save out;
        assert(!Decode(corrupt, out));
    }
    auto trailing = encoded;
    trailing.push_back(0);
    assert(!Decode(trailing, decoded));
    auto invalid = save;
    invalid.blocks.push_back(invalid.blocks.back());
    assert(!Encode(invalid, encoded)); // Duplicate extension.
    invalid = save;
    invalid.blocks[4].account = 13;
    assert(!Encode(invalid, encoded)); // Embedded character identity disagrees with its block.
    invalid = save;
    invalid.blocks[4].version = 3;
    assert(!Encode(invalid, encoded)); // Old/incomplete record cannot masquerade as V4.
    invalid = save;
    invalid.blocks[5].bytes.resize(ExtensionLimit + 1);
    assert(!Encode(invalid, encoded));
    invalid = save;
    invalid.blocks[0].bytes.clear();
    assert(!Encode(invalid, encoded));
    invalid = save;
    invalid.blocks.erase(invalid.blocks.begin() + 3);
    assert(!Encode(invalid, encoded)); // No host-own character.
    auto absent = save.blocks[4].bytes;
    const Key next{host.world, std::string(32, 'c'), 12};
    assert(Rebind(absent, next));
    Key rebound;
    assert(HeroKey(absent, rebound) && Exact(rebound, next));
    assert(!Rebind(absent, {host.world, next.save, 13}));
    assert(!Rebind(absent, {std::string(32, 'd'), next.save, 12}));
    assert(Crc(Bytes{'1','2','3','4','5','6','7','8','9'}) == 0xcbf43926);
    const std::string legacyHeader = "G3T 2\ncheckpoint " + host.world + " " + host.save + " 11 65 1 2048 2\n";
    const std::string legacyEnd = "checkpoint_end " + host.save + "\n";
    const std::string legacyText = legacyHeader + "legacy G3T_Sandbox\nG3T 1\ndowned 0\n" + legacyEnd;
    LegacyManifest legacy;
    assert(ParseLegacy(Bytes(legacyText.begin(), legacyText.end()), legacy));
    assert(Exact(legacy.key, host) && legacy.size[0] == 65 && legacy.crc[1] == 2);
    for (auto text : {legacyHeader, legacyText + "trailing\n", legacyText + legacyEnd,
                      legacyHeader + "legacy ../outside\n" + legacyEnd,
                      legacyHeader + "checkpoint_end " + next.save + "\n"}) {
        assert(!ParseLegacy(Bytes(text.begin(), text.end()), legacy));
        assert(Exact(legacy.key, host));
    }
    auto badLegacy = legacyText;
    badLegacy.insert(badLegacy.find('\n', 6), " extra");
    assert(!ParseLegacy(Bytes(badLegacy.begin(), badLegacy.end()), legacy));

    const std::string path = argv[1];
    assert(Atomic(path, previous));
    save.blocks[1].bytes[0] = 9;
    Bytes replacement;
    assert(Encode(save, replacement) && replacement != previous);
    for (auto stage : {WriteStage::Opened, WriteStage::Written, WriteStage::Flushed, WriteStage::Closed}) {
        stopAt = stage;
        assert(!Atomic(path, replacement, Proceed));
        assert(Read(path) == previous); // Failed/interrupted replacement preserves the previous complete unit.
        assert(Decode(Read(path), decoded));
    }
    { std::ofstream interrupted(path + ".tmp", std::ios::binary); interrupted << "G3TS"; }
    assert(Read(path) == previous && Decode(Read(path), decoded)); // Orphan crash temp is never loaded.
    assert(Atomic(path, replacement));
    assert(Read(path) == replacement && Decode(Read(path), decoded));
    // A partially replaced native menu pair is only a cache: recover exact bytes from the committed unit.
    assert(Atomic(path + ".native", Find(decoded, NativeWorld)->bytes));
    assert(Read(path + ".native") == save.blocks[1].bytes);

    CaptureRound round;
    assert(!round.AllReady() && !round.Accept(2, 1, 12, 3));
    round.active = true; round.epoch = 3; round.started = UINT32_MAX - 500;
    round.members[2] = {1, 0, 12, false};
    round.members[3] = {9, 0, 13, false};
    assert(!round.AllReady());
    assert(!round.Accept(2, 2, 12, 3)); // Reused peer generation.
    assert(!round.Accept(2, 1, 13, 3)); // Wrong account.
    assert(!round.Accept(2, 1, 12, 2)); // Old world.
    assert(!round.Accept(4, 1, 12, 3)); // Not a participant.
    assert(round.Accept(2, 1, 12, 3));
    assert(round.Accept(2, 1, 12, 3)); // Duplicate response is idempotent.
    assert(!round.AllReady()); // Lost second response prevents native saving.
    assert(!round.Expired(9000) && round.Expired(10000)); // Timeout across clock wrap.
    assert(round.Accept(3, 9, 13, 3) && round.AllReady()); // Retry recovers.
    round = {};
    assert(!round.Accept(2, 1, 12, 3)); // Cancellation rejects late responses.
}
