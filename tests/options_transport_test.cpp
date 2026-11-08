// SPDX-License-Identifier: GPL-3.0-or-later
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main

template<class T> Transmission Packet(T const& message, bool reliable = true)
{
    Transmission packet;
    packet.bytes.resize(sizeof(message));
    std::memcpy(packet.bytes.data(), &message, sizeof(message));
    packet.flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    return packet;
}
unsigned worlds = 0;
void World(uint8_t from, WorldNote const&)
{
    assert(from == 1 && options::observed.active);
    assert(options::observed.host.xp == 150 || options::observed.host.xp == 175);
    ++worlds;
}
int main()
{
    ENetHost fakeHost{};
    ENetPeer fakePeers[2]{};
    ENetChannel channels[2][ChannelCount]{};
    for (int i = 0; i < 2; ++i) fakePeers[i].channels = channels[i];
    fakePeers[0].address.port = 12;
    fakePeers[1].address.port = 13;
    localSteamAccount = 1;
    fakeHost.peers = fakePeers;
    host = &fakeHost;
    isHost = true;
    localId = 1;
    connected = true;
    Hello hello{MsgHello, ProtocolVersion, {}, {}};
    std::strcpy(hello.name, "client Easy");
    // Every build/data identity item still refuses by exact differing name.
    for (unsigned i = 0; i < parity::ItemCount; ++i) {
        auto different = hello;
        different.parity[i] = 1;
        fakePeers[0].data = nullptr;
        transmissions.clear();
        Deliver(&fakePeers[0], Packet(different));
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgRefused);
        RefusedMsg refusal{};
        std::memcpy(&refusal, transmissions[0].bytes.data(), sizeof(refusal));
        assert(std::strstr(refusal.reason, "install differs"));
        assert(std::strstr(refusal.reason, parity::ItemName(i)));
        assert(std::strstr(refusal.reason, "install the host's"));
        assert(!peers[2]);
    }
    options::localOptions.xp = 150;
    std::strcpy(options::localOptions.difficulty, "Hard");
    hostOptions = options::localOptions;
    transmissions.clear();
    Deliver(&fakePeers[0], Packet(hello));
    assert(peers[2] == &fakePeers[0]);
    assert(transmissions.size() == 1 && transmissions[0].bytes.size() == sizeof(Welcome));
    const auto welcome = transmissions[0];
    options::localOptions = {};
    std::strcpy(options::localOptions.difficulty, "Easy");
    isHost = false;
    connected = inWorld = false;
    server = &fakePeers[0];
    const unsigned before = options::adoptions;
    Deliver(&fakePeers[1], welcome);
    assert(options::adoptions == before && !connected); // not the server
    auto unreliable = welcome; unreliable.flags = 0;
    Deliver(&fakePeers[0], unreliable);
    assert(options::adoptions == before && !connected);
    Deliver(&fakePeers[0], welcome);
    assert(connected && !inWorld && options::observed.active);
    assert(options::observed.host.xp == 150);
    assert(std::strcmp(options::localOptions.difficulty, "Easy") == 0);
    worldHandler = &World;
    WorldNote note{}; note.op = WorldHost; note.kind = KindNew; note.epoch = 12;
    HostWorldMsg announcement{MsgWorld, note, hostOptions};
    Deliver(server, Packet(announcement));
    assert(worlds == 1 && worldEpoch == 12);
    announcement.note.epoch = 13; announcement.options.xp = 175;
    Deliver(server, Packet(announcement));
    assert(worlds == 2 && options::observed.host.xp == 175);
    // Malformed snapshot fails atomically and is never admitted or dispatched to persist.
    announcement.note.epoch = 14; announcement.options.npcHp = 0;
    Deliver(server, Packet(announcement));
    assert(worlds == 2 && !connected && !inWorld && refused && !options::observed.active);
    assert(std::strstr(RefusalReason(), "host world balancing"));
    // Legacy world envelopes cannot silently skip host adoption.
    connected = true; refused = false;
    const WorldMsg legacy{MsgWorld, note};
    Deliver(server, Packet(legacy));
    assert(!connected && refused && worlds == 2);
}
