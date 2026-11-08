// SPDX-License-Identifier: GPL-3.0-or-later
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main

int main()
{
    ENetHost fixture{};
    ENetPeer connections[3]{};
    ENetChannel channels[3][ChannelCount]{};
    fixture.peers = connections;
    fixture.peerCount = 3;
    host = &fixture;
    isHost = connected = true;
    localId = 1;
    localSteamAccount = 76561198011111111ULL;
    transportAccounts[0] = 76561198012345678ULL;
    transportAccounts[1] = transportAccounts[0];
    for (unsigned i = 0; i < 3; ++i) {
        auto& peer = connections[i];
        peer.channels = channels[i];
        peer.channelCount = ChannelCount;
        peer.address.port = i == 0 ? 12 : 13;
        peer.state = ENET_PEER_STATE_CONNECTED;
        peer.mtu = Mtu;
        peer.outgoingCommands.sentinel.next = peer.outgoingCommands.sentinel.previous = &peer.outgoingCommands.sentinel;
        peer.outgoingSendReliableCommands.sentinel.next = peer.outgoingSendReliableCommands.sentinel.previous =
            &peer.outgoingSendReliableCommands.sentinel;
        ResetBudget(&peer);
    }
    Hello hello{};
    hello.type = MsgHello;
    hello.version = ProtocolVersion;
    strcpy_s(hello.name, "Spoofed Owner");
    Transmission join{{}, ENET_PACKET_FLAG_RELIABLE, ChannelReliable, nullptr};
    join.bytes.resize(sizeof(hello));
    std::memcpy(join.bytes.data(), &hello, sizeof(hello));
    Deliver(&connections[0], join);
    assert(PeerAccount(2) == transportAccounts[0]);
    assert(std::string(PeerName(2)) == "Steam Player One"); // Hello cannot authorize a label/migration.
    assert(transmissions.size() == 1 && transmissions.back().bytes[0] == MsgWelcome);
    Deliver(&connections[1], join); // Same authenticated SteamID on a second connection.
    assert(!connections[1].data && !peers[3]);
    assert(transmissions.size() == 2 && transmissions.back().bytes[0] == MsgRefused);
    transportAccounts[1] = 18446744073709551615ULL;
    Deliver(&connections[2], join);
    assert(PeerAccount(3) == transportAccounts[1] && connections[2].data);
    assert(std::string(PeerName(3)) == "Steam Player Two");

    unsigned captures = 0;
    SetCharacterHandler(nullptr);
    static unsigned receivedCharacters = 0;
    SetCharacterHandler([](uint8_t from, CharacterNote const& note, std::vector<uint8_t> const& bytes) {
        assert(from == 2 && note.account == 76561198012345678ULL && bytes == std::vector<uint8_t>{7});
        ++receivedCharacters;
    });
    CharacterMsg record{};
    record.type = MsgCharacter;
    record.note.purpose = CharacterCapture;
    record.note.account = transportAccounts[0];
    record.note.epoch = 1;
    std::memset(record.note.world, 'a', 32);
    std::memset(record.note.save, 'b', 32);
    record.note.length = 1;
    record.note.crc = saving::store::Crc(std::vector<uint8_t>{7});
    Transmission upload{{}, ENET_PACKET_FLAG_RELIABLE, ChannelReliable, nullptr};
    upload.bytes.resize(sizeof(record) + 1);
    std::memcpy(upload.bytes.data(), &record, sizeof(record));
    upload.bytes.back() = 7;
    Deliver(&connections[0], upload);
    captures = receivedCharacters;
    assert(captures == 1);
    Deliver(&connections[2], upload); // Claimed account differs from connection authentication.
    assert(receivedCharacters == captures);
    transportAccounts[0] = 76561198187654321ULL; // Relay port reused after transport departure.
    assert(!PeerAccount(2));
    Deliver(&connections[0], upload);
    assert(receivedCharacters == captures);
}
