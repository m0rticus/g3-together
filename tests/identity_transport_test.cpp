// SPDX-License-Identifier: GPL-3.0-or-later
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main
#include "../src/trade_replay.h"

int main()
{
    assert(trade::replay::SellerEpoch(77, 77));
    assert(!trade::replay::SellerEpoch(77, 78));
    assert(!trade::replay::SellerEpoch(0, 0));
    assert(trade::replay::SellerAccount(true, 12, 12));
    assert(!trade::replay::SellerAccount(true, 12, 13));
    assert(!trade::replay::SellerAccount(true, 12, 0));
    assert(trade::replay::SellerAccount(false, 0, 0));
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
    assert(!connections[0].data && connections[1].data && peers[2] == &connections[1] && !peers[3]);
    assert(transmissions.size() == 2 && transmissions.back().bytes[0] == MsgWelcome);
    assert(PeerGeneration(2) == 2);
    LeavePeer(&connections[0], "late-event", false);
    assert(peers[2] == &connections[1]);
    clientInWorld[2] = true;
    goodsCache[2] = {1, 2};
    npcOrigins[2]["old"] = {};
    steam::RelayDrop drop;
    drop.account = transportAccounts[0];
    drop.port = 13;
    drop.generation = transportGenerations[1];
    strcpy_s(drop.reason, "left the lobby");
    relayDrops.push_back(drop);
    ProcessRelayDrops();
    assert(!PeerJoined(2) && !clientInWorld[2] && goodsCache[2].empty() && npcOrigins[2].empty());
    assert(ClientCount() == 0 && !remotes[2].connected);
    transportGenerations[0] = 1;
    Deliver(&connections[0], join); // Same account returns to its free slot.
    assert(peers[2] == &connections[0] && PeerGeneration(2) == 3 && ClientCount() == 1);
    drop.port = 12; // A delayed drop for the old relay must not evict the replacement.
    drop.generation = 0;
    relayDrops.push_back(drop);
    ProcessRelayDrops();
    assert(peers[2] == &connections[0]);
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

    LeavePeer(&connections[0], "timeout-or-disconnect", false);
    assert(!PeerJoined(2) && relayCloses.size() == 1 && relayCloses[0].second == 1);
    assert(relayCloses[0].first == record.note.account &&
           relayCloses[0].reason == "timeout-or-disconnect");
    transportAccounts[0] = record.note.account;
    Deliver(&connections[0], join);
    assert(PeerJoined(2) && relayCloses.empty()); // An already-authenticated rejoin supersedes the queued close.

    isHost = false;
    connected = inWorld = steamLinked = true;
    hostRelayGeneration = 77;
    server = &connections[0];
    remotes[1].connected = true;
    const unsigned restoresBefore = options::restores;
    drop.generation = 76;
    relayDrops.push_back(drop);
    ProcessRelayDrops();
    assert(connected && server);
    drop.generation = 77;
    relayDrops.push_back(drop);
    ProcessRelayDrops();
    assert(!connected && !inWorld && !steamLinked && !server && !remotes[1].connected);
    assert(options::restores == restoresBefore + 1);

    assert(flowSnapshot.endReason[0] == '\0' && !flowSnapshot.endedImmediately);
    std::strcpy(flowSnapshot.endReason, "mock link ended");
    steamLinked = connected = inWorld = true;
    hostRelayGeneration = 78;
    server = &connections[0];
    startedAt = fakeTime - 37;
    Service(PlayerState{}, false);
    assert(!connected && !inWorld && !steamLinked && !server);
    assert(std::any_of(logs.begin(), logs.end(), [](std::string const& line) {
        return line.find("reason=mock link ended") != std::string::npos &&
               line.find("elapsed_ms=37") != std::string::npos;
    }));

    server = &connections[0];
    connected = true;
    startedAt = fakeTime - 123;
    clientEnetSnapshots.clear();
    Reconnect([](char const* text) { logs.emplace_back(text); });
    assert(!server && clientEnetSnapshots.size() == 1);
    auto const& snapshot = clientEnetSnapshots[0];
    assert(snapshot.state == ENET_PEER_STATE_DISCONNECTED && snapshot.event == ENET_EVENT_TYPE_NONE &&
           snapshot.data == 0 && snapshot.elapsedMs == 123 && snapshot.reason == "ENet QA reconnect refused");
}
