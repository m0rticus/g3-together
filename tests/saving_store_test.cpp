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
static Bytes BinaryHero(Key const& key, float px, float py, float pz)
{
    auto addSec = [](Bytes& body, uint32_t type, Bytes const& secPayload) {
        Put(body, type, 4);
        Put(body, 0, 4);
        Put(body, secPayload.size(), 4);
        Put(body, Crc(secPayload), 4);
        body.insert(body.end(), secPayload.begin(), secPayload.end());
    };
    Bytes idSec(key.world.begin(), key.world.end());
    idSec.insert(idSec.end(), key.save.begin(), key.save.end());
    Put(idSec, key.player, 8);
    Bytes poseSec;
    PutF32(poseSec, px); PutF32(poseSec, py); PutF32(poseSec, pz);
    PutF32(poseSec, 0.f); PutF32(poseSec, 0.f); PutF32(poseSec, 0.f); PutF32(poseSec, 1.f);
    Bytes stateSec(24, 0);
    Bytes countZero(4, 0);
    Bytes sections;
    addSec(sections, HeroSecIdentity, idSec);
    addSec(sections, HeroSecPose, poseSec);
    addSec(sections, HeroSecState, stateSec);
    addSec(sections, HeroSecAttributes, countZero);
    addSec(sections, HeroSecInventory, countZero);
    addSec(sections, HeroSecSpells, countZero);
    addSec(sections, HeroSecQuickSlots, countZero);

    Bytes out(HeroMagic, HeroMagic + 8);
    Put(out, HeroSchemaVersion, 4);
    Put(out, HeroMinReaderVersion, 4);
    Put(out, HeroHeaderSize, 4);
    Put(out, sections.size(), 4);
    Put(out, 0, 4); // record CRC placeholder
    Put(out, 0, 4); // flags
    Put(out, 7, 4); // sectionCount
    Put(out, 0, 8); // reserved
    Put(out, 0, 4); // header CRC placeholder
    out.insert(out.end(), sections.begin(), sections.end());
    PutLeAt(out.data() + HeroHeaderCrcOffset, Crc32Zeroed(out.data(), HeroHeaderCrcOffset, HeroRecordCrcOffset, 4), 4);
    PutLeAt(out.data() + HeroRecordCrcOffset, Crc32Zeroed(out.data(), out.size(), HeroRecordCrcOffset, 4), 4);
    return out;
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
    // Retired lease payloads are opaque: only container framing/CRC remain authoritative.
    for (uint32_t version : {1u, 2u, 7u}) {
        for (Bytes const& ignored : {Bytes{}, Bytes{'n','p','c','l','e','a','s','e','1',' ','!'},
                                    Bytes(20 + 261, 1), Bytes(20 + 262, 2)}) {
            Save old = save;
            Set(old, {256, version, 0, ignored});
            Bytes container;
            Save loaded;
            assert(Encode(old, container) && Decode(container, loaded));
            assert(Find(loaded, 256)->bytes == ignored);
            container.back() ^= 0x80;
            assert(!Decode(container, loaded)); // Do not weaken outer checksums.
        }
    }
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
    assert(decoded.schemaVersion == Version && decoded.minReaderVersion == MinReaderVersion);
    assert(std::memcmp(encoded.data(), "G3TSAVE\0", 8) == 0);
    const Bytes previous = encoded;
    auto swappedOwner = encoded;
    swappedOwner[96] = 12; // Existing client account: semantic checks alone would allow this header corruption.
    assert(!Decode(swappedOwner, decoded));
    for (size_t n : {size_t(0), size_t(4), size_t(95), size_t(111), encoded.size() - 1}) {
        Bytes cut(encoded.begin(), encoded.begin() + n);
        Save out = decoded;
        assert(!Decode(cut, out) && out.blocks.size() == decoded.blocks.size());
    }
    assert(Modern(Bytes{'G','3','T','S'})); // Damaged modern save never falls through to native migration.
    for (size_t at : {size_t(8), size_t(12), size_t(16), size_t(20), size_t(24), size_t(28),
                      size_t(32), size_t(96), size_t(104), size_t(108), encoded.size() - 1}) {
        auto corrupt = encoded;
        corrupt[at] ^= 0xff;
        Save out;
        assert(!Decode(corrupt, out));
    }
    auto trailing = encoded;
    trailing.push_back(0);
    assert(!Decode(trailing, decoded));

    // Forward-compatible optional section (even below Extension=256) and minor-compatible future header.
    {
        Save withOptional = save;
        withOptional.blocks.push_back({7, 2, 0, {11, 22, 33, 44}, SectionOptional});
        Bytes optEncoded;
        Save optDecoded;
        assert(Encode(withOptional, optEncoded) && Decode(optEncoded, optDecoded));
        assert(Find(optDecoded, 7) && Find(optDecoded, 7)->bytes == Bytes({11, 22, 33, 44}));
        assert(Find(optDecoded, 7)->flags == SectionOptional);

        // Simulate minor-compatible schemaVersion=5, minReaderVersion=4 with an extended 120-byte header.
        Bytes future = optEncoded;
        PutLeAt(future.data() + 8, 5, 4);   // schemaVersion = 5
        PutLeAt(future.data() + 12, 4, 4);  // minReaderVersion = 4
        PutLeAt(future.data() + 16, 120, 4); // headerSize = 120
        future.insert(future.begin() + 112, 8, 0xaa);
        PutLeAt(future.data() + 108, Crc32Zeroed(future.data(), 108, 24, 4), 4);
        PutLeAt(future.data() + 24, ContainerCrc(future), 4);
        Save futureDecoded;
        assert(Decode(future, futureDecoded));
        assert(futureDecoded.schemaVersion == 5 && futureDecoded.minReaderVersion == 4);
        assert(Find(futureDecoded, 7) && Find(futureDecoded, 7)->bytes == Bytes({11, 22, 33, 44}));

        // Breaking minReaderVersion = 5 must be rejected by v4 reader.
        auto breaking = future;
        PutLeAt(breaking.data() + 12, 5, 4);
        PutLeAt(breaking.data() + 108, Crc32Zeroed(breaking.data(), 108, 24, 4), 4);
        PutLeAt(breaking.data() + 24, ContainerCrc(breaking), 4);
        assert(!Decode(breaking, futureDecoded));

        // Unknown required section (flags=0, type < Extension) must be rejected.
        Save badRequired = save;
        badRequired.blocks.push_back({7, 1, 0, {1, 2}, SectionRequired});
        Bytes badOut;
        assert(!Encode(badRequired, badOut));
    }

    // Backward-compatible readers for legacy G3TSAVE3 and G3TSAVE2 binary containers.
    auto makeLegacyContainer = [](char const* magic8, uint32_t ver, Save const& s) {
        Bytes payload;
        for (auto const& b : s.blocks) {
            Put(payload, b.type, 4); Put(payload, b.version, 4); Put(payload, b.account, 8);
            Put(payload, b.bytes.size(), 4); Put(payload, Crc(b.bytes), 4);
            payload.insert(payload.end(), b.bytes.begin(), b.bytes.end());
        }
        Bytes result(magic8, magic8 + 8);
        Put(result, ver, 4); Put(result, payload.size(), 4); Put(result, 0, 4);
        result.insert(result.end(), s.key.world.begin(), s.key.world.end());
        result.insert(result.end(), s.key.save.begin(), s.key.save.end());
        Put(result, s.key.player, 8); Put(result, s.blocks.size(), 4);
        result.insert(result.end(), payload.begin(), payload.end());
        const uint32_t crc = LegacyContainerCrc(result);
        PutLeAt(result.data() + 16, crc, 4);
        return result;
    };
    {
        const Bytes v3Bytes = makeLegacyContainer("G3TSAVE3", 3, save);
        Save v3Loaded;
        assert(Modern(v3Bytes) && Decode(v3Bytes, v3Loaded));
        assert(v3Loaded.schemaVersion == 3 && v3Loaded.minReaderVersion == 3);
        assert(Exact(v3Loaded.key, host) && v3Loaded.blocks.size() == save.blocks.size());
        Bytes upgraded;
        assert(Encode(v3Loaded, upgraded) && std::memcmp(upgraded.data(), "G3TSAVE\0", 8) == 0);

        Save v2Save{host, {{NativeHeader, 1, 0, Bytes(16, 1)}, {NativeWorld, 1, 0, Bytes(32, 2)},
                           {Character, 4, 11, Hero(host)}}};
        const Bytes v2Bytes = makeLegacyContainer("G3TSAVE2", 2, v2Save);
        Save v2Loaded;
        assert(Modern(v2Bytes) && Decode(v2Bytes, v2Loaded));
        assert(v2Loaded.schemaVersion == 2 && v2Loaded.minReaderVersion == 2);
        assert(Exact(v2Loaded.key, host) && Find(v2Loaded, Character, 11)->bytes == v2Save.blocks[2].bytes);
    }
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
    RecordPose textPose;
    assert(HeroPose(absent, rebound, textPose) && Exact(rebound, next) && ValidRecordPose(textPose) &&
           textPose.position[0] == 1.f && textPose.position[1] == 2.f && textPose.position[2] == 3.f);
    const Bytes binHero = BinaryHero(next, -12500.5f, 340.25f, 8900.f);
    RecordPose binPose;
    Key binKey;
    assert(HeroPose(binHero, binKey, binPose) && Exact(binKey, next) && ValidRecordPose(binPose) &&
           binPose.position[0] == -12500.5f && binPose.position[1] == 340.25f && binPose.position[2] == 8900.f);
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
