// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/player_identity.h"
#include "../src/saving_store.h"
#include <cassert>

using namespace g3t::identity;
using namespace g3t::saving;
using namespace g3t::saving::store;

static Bytes Hero(Key const& key)
{
    const std::string text = "G3THERO 4\nidentity " + key.world + " " + key.save + " " +
        std::to_string(key.player) + "\npose 1 2 3 0 0 0 1\ndowned 0\nend 0\n";
    return Bytes(text.begin(), text.end());
}
static Companions Reload(Companions const& table)
{
    Companions restored;
    std::istringstream stream(table.Write());
    std::string line;
    while (std::getline(stream, line)) assert(restored.Read(line) == Row::Accepted);
    return restored;
}
int main()
{
    constexpr uint64_t host = 76561198011111111ULL, player = 76561198012345678ULL;
    constexpr uint64_t other = 76561198187654321ULL, high = 18446744073709551615ULL;
    for (uint64_t id : std::initializer_list<uint64_t>{11, 12, 4294967297ULL, player, other, high}) {
        uint64_t parsed = 0;
        assert(SteamId(std::to_string(id), parsed) && parsed == id);
        assert(Admit(id, host, {}) && !Admit(id, host, {id}));
    }
    for (std::string invalid : {"", "0", "-1", "+11", " 11", "11x", "18446744073709551616"}) {
        uint64_t parsed = 99;
        assert(!SteamId(invalid, parsed) && parsed == 99);
    }
    assert(!Admit(0, host, {}) && !Admit(host, host, {}));
    std::string guid;
    assert(Guid(std::string(32, 'a'), guid));
    std::string unused;
    assert(!Guid(std::string(32, 'z'), unused));
    Companions table;
    table.host = host;
    table.candidates = {host, player};
    table.labels = {{host, "Host"}, {player, "Old Name"}};
    assert(table.Read("companion " + Hex(guid) + " Old Name\r\n") == Row::Accepted);
    assert(table.Migrate(guid, {}) == Decision::Bound);
    assert(table.owners.at(guid).account == player && table.owners.at(guid).legacyName.empty());
    assert(table.Migrate(guid, {}) == Decision::Unknown); // exactly once
    table.labels[player] = "New Name";
    auto renamed = Reload(table);
    assert(renamed.owners.at(guid).account == player && renamed.labels.at(player) == "New Name");
    assert(renamed.Write().find("companion64 1 ") != std::string::npos);
    assert(renamed.Read("companion64 1 " + Hex(guid) + " " + std::to_string(other)) == Row::Invalid);

    Companions legacy;
    legacy.host = host;
    legacy.candidates = {host, player, other};
    legacy.labels = {{host, "Old Name"}, {player, "Old Name"}};
    assert(legacy.Read("companion " + Hex(guid) + " Old Name") == Row::Accepted);
    assert(legacy.Migrate(guid, {}) == Decision::Unknown); // unidentified absent stored account
    legacy.labels[other] = "Old Name";
    assert(legacy.Migrate(guid, {}) == Decision::Ambiguous);
    assert(Reload(legacy).owners.at(guid).legacyName == "Old Name");
    assert(!legacy.owners.at(guid).account);
    assert(legacy.Migrate(guid, {{"Old Name", 0}}) == Decision::Ambiguous); // conflicting host settings
    assert(legacy.Migrate(guid, {{"Old Name", host}}) == Decision::Ambiguous); // host stays native
    assert(legacy.Migrate(guid, {{"Old Name", 999}}) == Decision::Ambiguous);
    legacy.labels[player] = "Renamed";
    legacy.labels[other] = "Also Renamed";
    assert(legacy.Migrate(guid, {}) == Decision::Unknown); // no orphan guessing
    assert(legacy.Migrate(guid, {{"Old Name", player}}) == Decision::Explicit);
    assert(Reload(legacy).owners.at(guid).account == player);
    Companions tagged;
    tagged.candidates = {player};
    assert(tagged.Read("companion " + Hex(guid) + " Name#1234 ") == Row::Accepted);
    assert(Reload(tagged).owners.at(guid).legacyName == "Name#1234 ");
    assert(tagged.Migrate(guid, {{"Name#1234 ", player}}) == Decision::Explicit);

    Companions summons;
    assert(summons.Read("summon64 1 " + Hex(guid) + " " + std::to_string(high)) == Row::Accepted);
    assert(Reload(summons).owners.at(guid).account == high && Reload(summons).owners.at(guid).summoned);
    for (std::string invalid : std::initializer_list<std::string>{"companion64 2 " + Hex(guid) + " 11",
                                "companion64 1 " + Hex(guid) + " -1",
                                "companion64 1 bad 11", "identitylabel 2 11 Name",
                                "companion " + Hex(guid) + " "}) {
        Companions invalidTable;
        assert(invalidTable.Read(invalid) == Row::Invalid);
    }
    // Existing v3 container/v4 hero/v1 personal/envelope framing works with all 64 bits.
    for (uint64_t account : {player, high}) {
        const Key key{std::string(32, 'a'), std::string(32, 'b'), host};
        Bytes personal;
        assert(g3t::saving::personal::Encode({}, personal));
        const auto character = Hero({key.world, key.save, account});
        Save save{key, {{NativeHeader, 1, 0, {1}}, {NativeWorld, 1, 0, {2}},
                       {Journal, 1, 0, {}}, {Character, 4, host, Hero(key)},
                       {Character, 4, account, character}, {Personal, 1, account, personal}}};
        Bytes encoded, envelope, decodedHero, decodedPersonal;
        Save decoded;
        assert(Encode(save, encoded) && Decode(encoded, decoded));
        assert(Find(decoded, Character, account)->bytes == character);
        assert(Find(decoded, Personal, account)->bytes == personal);
        assert(!Find(decoded, Character, static_cast<uint32_t>(account)));
        assert(PackPlayer(character, personal, envelope) && UnpackPlayer(envelope, decodedHero, decodedPersonal));
        Key actual;
        assert(HeroKey(decodedHero, actual) && actual.player == account && decodedPersonal == personal);
        assert(Rebind(decodedHero, {key.world, std::string(32, 'c'), account}));
        assert(!Rebind(decodedHero, {key.world, std::string(32, 'c'), other}));
        auto old = save;
        old.blocks.pop_back(); // build-57/earlier containers without optional Personal blocks still load
        assert(Encode(old, encoded) && Decode(encoded, decoded));
    }
}
