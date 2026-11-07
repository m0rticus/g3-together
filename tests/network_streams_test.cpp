// SPDX-License-Identifier: GPL-3.0-or-later
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main

unsigned removalMessages = 0;
NpcRemovalNote receivedRemoval{};

int main()
{
    ENetHost fakeHost{};
    ENetPeer fakePeers[2]{};
    ENetChannel channels[2][ChannelCount]{};
    fakeHost.peers = fakePeers;
    fakeHost.peerCount = 2;
    host = &fakeHost;
    for (unsigned i = 0; i < 2; ++i) {
        fakePeers[i].channels = channels[i];
        fakePeers[i].channelCount = ChannelCount;
        fakePeers[i].state = ENET_PEER_STATE_CONNECTED;
        fakePeers[i].mtu = Mtu;
        fakePeers[i].packetThrottle = ENET_PEER_DEFAULT_PACKET_THROTTLE;
        fakePeers[i].data = reinterpret_cast<void*>(uintptr_t(i + 2));
        fakePeers[i].outgoingCommands.sentinel.next = &fakePeers[i].outgoingCommands.sentinel;
        fakePeers[i].outgoingCommands.sentinel.previous = &fakePeers[i].outgoingCommands.sentinel;
        fakePeers[i].outgoingSendReliableCommands.sentinel.next = &fakePeers[i].outgoingSendReliableCommands.sentinel;
        fakePeers[i].outgoingSendReliableCommands.sentinel.previous = &fakePeers[i].outgoingSendReliableCommands.sentinel;
        peers[i + 2] = &fakePeers[i];
        clientInWorld[i + 2] = true;
        generations[i + 2] = 1;
        ResetBudget(&fakePeers[i]);
    }
    server = &fakePeers[0];
    connected = inWorld = isHost = true;
    localId = 1;

    const uint32_t previousEpoch = worldEpoch;
    worldEpoch = 17;
    SetNpcRemovalHandler([](NpcRemovalNote const& removal) {
        ++removalMessages;
        receivedRemoval = removal;
    });
    NpcRemovalNote removal{};
    removal.guid[0] = 77;
    removal.epoch = worldEpoch;
    removal.world = 42;
    removal.incarnation = 3;
    clientInWorld[2] = false;
    assert(!SendNpcRemovalTo(2, removal) && transmissions.empty());
    clientInWorld[2] = true;
    failSend = true;
    assert(!SendNpcRemovalTo(2, removal) && transmissions.empty());
    failSend = false;
    assert(SendNpcRemovalTo(2, removal) && transmissions.size() == 1);
    const auto terminal = transmissions[0];
    assert(terminal.bytes.size() == 29 && terminal.bytes[0] == MsgNpcRemoval);
    assert(terminal.channel == ChannelNpcs && (terminal.flags & ENET_PACKET_FLAG_RELIABLE));
    transmissions.clear();
    isHost = false;
    localId = 2;
    Deliver(server, terminal);
    assert(removalMessages == 1 && receivedRemoval.guid[0] == 77 && receivedRemoval.incarnation == 3);
    Deliver(&fakePeers[1], terminal);
    assert(removalMessages == 1);
    auto unreliableRemoval = terminal;
    unreliableRemoval.flags = 0;
    Deliver(server, unreliableRemoval);
    assert(removalMessages == 1);
    auto staleRemoval = terminal;
    NpcRemovalMsg staleMessage{};
    std::memcpy(&staleMessage, staleRemoval.bytes.data(), sizeof(staleMessage));
    --staleMessage.note.epoch;
    std::memcpy(staleRemoval.bytes.data(), &staleMessage, sizeof(staleMessage));
    Deliver(server, staleRemoval);
    assert(removalMessages == 1);
    inWorld = false;
    Deliver(server, terminal);
    assert(removalMessages == 1);
    inWorld = true;
    auto oversizedRemoval = terminal;
    oversizedRemoval.bytes.push_back(0);
    Deliver(server, oversizedRemoval);
    assert(removalMessages == 1);
    isHost = true;
    localId = 1;
    worldEpoch = previousEpoch;
    SetNpcRemovalHandler(nullptr);

    PlayerState local{};
    local.flags = 1;
    local.world = 42;
    local.rotation[3] = 1;
    local.focusNpc = 123;
    local.targetNpc = 456;
    std::memcpy(local.animation, "Walk", 5);
    currentLocal = {};
    haveCurrentLocal = false;
    playerCadence = {};
    unsigned continuousStates = 0;
    for (unsigned elapsed = 0; elapsed < 10000; ++elapsed) {
        fakeTime = 5000 + elapsed;
        Service(local, true);
        unsigned statesThisFrame = 0;
        for (auto const& transmission : transmissions) {
            assert(transmission.channel == ChannelState);
            if (transmission.bytes[0] == MsgState) {
                ++statesThisFrame;
                assert(transmission.bytes.size() < 100);
            } else {
                assert(transmission.bytes[0] == MsgPlayerActivity);
                assert(transmission.bytes.size() == sizeof(ActivityMsg));
            }
        }
        continuousStates += statesThisFrame;
        transmissions.clear();
    }
    // Initial full plus exactly scheduled 30 Hz, once per outgoing peer.
    assert(continuousStates == 600);
    ++local.downedSequence;
    local.flags = 9;
    Service(local, true);
    unsigned immediateStates = 0, heartbeats = 0;
    for (auto const& transmission : transmissions) {
        assert(transmission.flags & ENET_PACKET_FLAG_RELIABLE);
        if (transmission.bytes[0] == MsgState) ++immediateStates;
        else if (transmission.bytes[0] == MsgPlayerActivity) ++heartbeats;
    }
    assert(immediateStates == 2 && heartbeats == 2);
    for (auto const& transmission : transmissions) assert(transmission.flags & ENET_PACKET_FLAG_RELIABLE);
    Transmission hostState;
    for (auto const& transmission : transmissions)
        if (transmission.bytes[0] == MsgState) { hostState = transmission; break; }
    transmissions.clear();
    isHost = false;
    localId = 2;
    Deliver(server, hostState); // metadata delta without its earlier baseline requests recovery.
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgPlayerResync);
    const auto request = transmissions[0];
    transmissions.clear();
    isHost = true;
    localId = 1;
    Deliver(peers[2], request);
    ++fakeTime;
    ++currentLocal.sequence;
    assert(SendPlayerState(peers[2], currentLocal));
    assert(transmissions.size() == 1 && (transmissions[0].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto recovered = transmissions[0];
    transmissions.clear();
    isHost = false;
    localId = 2;
    Deliver(server, recovered);
    assert(remotes[1].latest.focusNpc == 123 && remotes[1].latest.targetNpc == 456);
    assert(remotes[1].latest.downedSequence == local.downedSequence && remotes[1].latest.flags == 9);
    const uint32_t acceptedSequence = remotes[1].latest.sequence;
    Deliver(server, hostState);
    assert(remotes[1].latest.sequence == acceptedSequence);
    isHost = true;
    localId = 1;
    transmissions.clear();

    // A full story reply targets only the asking peer and never fragments.
    StateEntry entries[MaxStatesPerMessage]{};
    SendStatesTo(2, StateEvents, entries, MaxStatesPerMessage);
    assert(transmissions.size() == 4);
    for (auto const& transmission : transmissions) {
        assert(transmission.peer == peers[2] && transmission.channel == ChannelReliable);
        assert(transmission.bytes.size() <= 1168);
        assert(transmission.flags & ENET_PACKET_FLAG_RELIABLE);
    }
    transmissions.clear();

    SaveNote note{};
    note.op = SaveCommit;
    note.epoch = 42;
    std::memset(note.world, 'a', 32);
    std::memset(note.save, 'b', 32);
    const std::string name = saving::Slot(note.save);
    std::vector<uint8_t> files[FileCount];
    const uint32_t sizes[FileCount]{10000, 4000, 1000};
    const uint32_t crcs[FileCount]{1, 2, 3};
    for (int f = 0; f < FileCount; ++f) files[f].resize(sizes[f], static_cast<uint8_t>(f));
    ResetBudget(peers[2]);
    assert(QueueFileTransfer(2, note, name.c_str(), files, sizes, crcs));
    assert(FileTransferBusy(2) && transmissions.empty());
    for (auto const& file : files) assert(file.empty());

    // Failed ENet admission cannot consume cursor or tokens.
    failSend = true;
    PumpFileTransfers();
    assert(!fileTransfers[2].prefixed && fileTransfers[2].offset == 0);
    assert(transmissions.empty());
    failSend = false;
    PumpFileTransfers();
    assert(transmissions[0].bytes[0] == MsgSave);
    assert(transmissions.size() <= 5);
    for (auto const& transmission : transmissions)
        assert(transmission.channel == ChannelBulk && transmission.bytes.size() <= 1168);
    uint32_t received[FileCount]{};
    bool prefixed = false, done = false;
    unsigned frames = 0;
    while (!done && frames++ < 1000) {
        for (auto const& transmission : transmissions) {
            if (transmission.bytes[0] == MsgSave) {
                assert(!prefixed);
                prefixed = true;
            } else if (transmission.bytes[0] == MsgFileChunk) {
                assert(prefixed && !done);
                FileChunkHeader chunk{};
                std::memcpy(&chunk, transmission.bytes.data(), sizeof(chunk));
                assert(chunk.offset == received[chunk.file]);
                assert(chunk.total == sizes[chunk.file] && chunk.length <= MaxChunk);
                received[chunk.file] += chunk.length;
            } else if (transmission.bytes[0] == MsgFileDone) {
                for (int f = 0; f < FileCount; ++f) assert(received[f] == sizes[f]);
                done = true;
            } else {
                assert(false);
            }
        }
        transmissions.clear();
        fakeTime += 34;
        UpdateBudgets(fakeTime);
        PumpFileTransfers();
    }
    assert(done && !FileTransferBusy(2));

    // Congestion pauses bulk, but never refuses a reliable hit/result.
    BudgetTelemetry congested;
    congested.throttle = 16;
    peerBudgets[0].Advance(fakeTime, congested);
    assert(peerBudgets[0].Pressure() == BudgetPressure::Critical);
    files[0].resize(1); files[1].resize(1); files[2].resize(1);
    const uint32_t tiny[FileCount]{1, 1, 1};
    assert(QueueFileTransfer(2, note, name.c_str(), files, tiny, crcs));
    PumpFileTransfers();
    assert(transmissions.empty() && FileTransferBusy(2));
    const HitResultMsg hit{MsgHitResult, {}};
    assert(Send(peers[2], &hit, sizeof(hit), ChannelCombat, true));
    assert(transmissions.size() == 1 && transmissions[0].channel == ChannelCombat);
    transmissions.clear();
    ++generations[2];
    PumpFileTransfers();
    assert(!FileTransferBusy(2) && transmissions.empty());

    remotes[2].connected = true;
    remotes[2].latestTicks = fakeTime;
    remotes[2].latest.focusNpc = 123;
    remotes[2].latest.targetNpc = 456;
    assert(RemoteFocusNpc(2) == 123 && RemoteTargetNpc(2) == 456 && RemoteStateAgeMs(2) == 0);
    fakeTime += 1000;
    assert(RemoteFocusNpc(2) == 0 && RemoteTargetNpc(2) == 0);
    clientInWorld[2] = false;
    assert(RemoteStateAgeMs(2) == UINT32_MAX);
    host = nullptr;
}
