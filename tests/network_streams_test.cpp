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

    // A local load/timeout cancellation uses the existing opcode and must reach the host coordinator.
    static unsigned cancellations = 0;
    SetSaveHandler([](uint8_t from, SaveNote const& value) {
        assert(from == 2 && value.op == SaveCancel && !value.ok && value.epoch == 42);
        assert(std::strcmp(value.name, "canceled-load") == 0);
        ++cancellations;
    });
    SaveNote cancel{};
    cancel.epoch = 42;
    std::memset(cancel.world, 'a', 32);
    std::memset(cancel.save, 'b', 32);
    cancel.op = SaveCancel;
    cancel.ok = 0;
    std::memcpy(cancel.name, "canceled-load", sizeof("canceled-load"));
    SaveMsg cancellation{MsgSave, cancel};
    Transmission cancelPacket;
    cancelPacket.bytes.resize(sizeof(cancellation));
    std::memcpy(cancelPacket.bytes.data(), &cancellation, sizeof(cancellation));
    cancelPacket.channel = ChannelReliable;
    cancelPacket.flags = ENET_PACKET_FLAG_RELIABLE;
    Deliver(peers[2], cancelPacket);
    assert(cancellations == 1);
    cancel.world[0] = 0; // The first new-game request has no world ID until host Prepare arrives.
    cancellation.note = cancel;
    std::memcpy(cancelPacket.bytes.data(), &cancellation, sizeof(cancellation));
    Deliver(peers[2], cancelPacket);
    assert(cancellations == 2);
    cancelPacket.flags = 0;
    Deliver(peers[2], cancelPacket);
    assert(cancellations == 2); // Control messages, like records, must be reliable.
    cancelPacket.flags = ENET_PACKET_FLAG_RELIABLE;
    static unsigned saveRequests = 0;
    SetSaveHandler([](uint8_t from, SaveNote const& value) {
        assert(from == 2 && value.op == SaveRequest);
        ++saveRequests;
    });
    cancellation.note.op = SaveRequest;
    cancellation.note.ok = 1;
    std::memset(cancellation.note.world, '0', 32);
    std::memcpy(cancelPacket.bytes.data(), &cancellation, sizeof(cancellation));
    Deliver(peers[2], cancelPacket);
    assert(saveRequests == 1);
    cancellation.note.save[0] = 'z';
    std::memcpy(cancelPacket.bytes.data(), &cancellation, sizeof(cancellation));
    Deliver(peers[2], cancelPacket);
    assert(saveRequests == 1); // Request tokens also require the normal identity syntax.
    cancellation.note.save[0] = 'b';
    for (uint8_t retired : {uint8_t(4), uint8_t(5), uint8_t(8)}) {
        cancellation.note.op = retired;
        std::memcpy(cancelPacket.bytes.data(), &cancellation, sizeof(cancellation));
        Deliver(peers[2], cancelPacket);
        assert(saveRequests == 1);
    }
    SetSaveHandler(nullptr);

    // Small character records bypass admission on channel 1, never bulk pacing.
    fakePeers[0].address.port = 12;
    fakePeers[1].address.port = 13;
    connected = true;
    clientInWorld[2] = false;
    inWorld = false;
    static unsigned characters = 0;
    SetCharacterHandler([](uint8_t from, CharacterNote const& value, std::vector<uint8_t> const& bytes) {
        assert(from == 1 || from == 2);
        assert(value.account == 12 && bytes.size() == 6906);
        ++characters;
    });
    std::vector<uint8_t> character(6906, 'h');
    CharacterNote characterNote{};
    characterNote.purpose = CharacterCapture;
    characterNote.epoch = 42;
    characterNote.account = 12;
    characterNote.length = static_cast<uint32_t>(character.size());
    characterNote.crc = saving::store::Crc(character);
    std::memset(characterNote.world, 'a', 32);
    std::memset(characterNote.save, 'b', 32);
    isHost = false;
    localId = 2;
    failSend = true;
    assert(!SendCharacter(1, characterNote, character));
    assert(transmissions.empty());
    failSend = false;
    assert(SendCharacter(1, characterNote, character));
    assert(transmissions.size() == 1 && transmissions[0].channel == ChannelReliable);
    const auto upload = transmissions[0];
    assert(upload.bytes.size() == 6906 + sizeof(CharacterMsg));
    assert(upload.flags & ENET_PACKET_FLAG_RELIABLE);
    transmissions.clear();
    isHost = true;
    localId = 1;
    Deliver(peers[2], upload);
    assert(characters == 1 && PeerAccount(2) == 12 && PeerAccount(3) == 13);
    Deliver(peers[3], upload); // Cannot claim account 12 on account 13's transport.
    assert(characters == 1);
    auto damaged = upload;
    damaged.bytes.back() ^= 1;
    Deliver(peers[2], damaged);
    assert(characters == 1);
    auto unreliable = upload;
    unreliable.flags = 0;
    Deliver(peers[2], unreliable);
    assert(characters == 1);
    auto oversized = upload;
    oversized.bytes.push_back(1);
    Deliver(peers[2], oversized);
    assert(characters == 1);
    characterNote.purpose = CharacterRestore;
    BudgetTelemetry congested;
    congested.throttle = 16;
    peerBudgets[0].Advance(fakeTime, congested);
    assert(peerBudgets[0].Pressure() == BudgetPressure::Critical);
    assert(SendCharacter(2, characterNote, character)); // No bandwidth credit gate.
    const auto restore = transmissions.back();
    assert(restore.channel == ChannelReliable);
    transmissions.clear();
    isHost = false;
    localId = 2;
    Deliver(server, restore);
    assert(characters == 2);
    Deliver(&fakePeers[1], restore);
    assert(characters == 2); // Only the welcomed server can restore.
    Deliver(server, upload);
    assert(characters == 2); // Host cannot send a client capture response.
    SetCharacterHandler(nullptr);
    // Only reliable story/door restoration handshakes bypass closed gameplay gates.
    static unsigned storyMessages = 0, doorMessages = 0;
    SetStateHandler([](uint8_t from, uint8_t kind, StateEntry const*, int) {
        assert(from == 1 || from == 2);
        assert(kind & story::policy::Snapshot);
        ++storyMessages;
    });
    SetWorldStateHandler([](uint8_t from, WorldState const& state) {
        assert((from == 1 || from == 2) && state.kind == 10);
        ++doorMessages;
    });
    inWorld = false;
    transmissions.clear();
    assert(!SendStates(StateQuests, entries, 1));
    assert(transmissions.empty());
    assert(SendStates(StateQuests | story::policy::Snapshot, nullptr, 0));
    const auto query = transmissions.back();
    transmissions.clear();
    WorldState door{};
    door.kind = 10; door.npcClass = 0x80; door.guid[0] = 1;
    assert(SendWorldStateTo(1, door));
    const auto doorQuery = transmissions.back();
    transmissions.clear();
    isHost = true;
    localId = 1;
    clientInWorld[2] = false;
    Deliver(peers[2], query);
    Deliver(peers[2], doorQuery);
    assert(storyMessages == 1 && doorMessages == 1);
    auto lostReliability = query;
    lostReliability.flags = 0;
    Deliver(peers[2], lostReliability);
    assert(storyMessages == 1);
    failSend = true;
    assert(!SendStatesTo(2, StateQuests | story::policy::Snapshot, nullptr, 0));
    failSend = false;
    assert(SendStatesTo(2, StateQuests | story::policy::Snapshot, nullptr, 0));
    assert(SendStatesTo(2, StateQuests | story::policy::Snapshot, entries, 1));
    assert(SendStatesTo(2, StateQuests | story::policy::Snapshot | story::policy::End, nullptr, 0));
    door.npcClass = 0x43;
    assert(SendWorldStateTo(2, door));
    assert(transmissions.size() == 4);
    isHost = false;
    localId = 2;
    for (auto const& transmission : transmissions) Deliver(server, transmission);
    assert(storyMessages == 4 && doorMessages == 2);
    auto rawDelta = transmissions[1];
    rawDelta.bytes[1] = StateQuests;
    Deliver(server, rawDelta);
    assert(storyMessages == 4);
    transmissions.clear();
    isHost = true; localId = 1;
    assert(SendStatesTo(2, StateQuests | story::policy::Snapshot | story::policy::Catchup, entries, 1));
    assert(!SendStatesTo(2, StateQuests | story::policy::Snapshot | story::policy::Catchup, nullptr, 0));
    const auto catchup = transmissions.back();
    isHost = false; localId = 2;
    Deliver(server, catchup);
    assert(storyMessages == 5); // Authoritative reward-free cutover is delivered before the admission ACK.
    isHost = true; localId = 1;
    Deliver(peers[2], catchup);
    assert(storyMessages == 5); // Clients cannot forge bootstrap data or catch-up.
    transmissions.clear();
    WorldNote admissionAck{};
    admissionAck.op = WorldReady; admissionAck.kind = KindSaved; admissionAck.epoch = 7;
    SendWorld(admissionAck, 2);
    const auto ack = transmissions.back();
    static unsigned admitted = 0;
    SetWorldHandler([](uint8_t from, WorldNote const& note) {
        assert(from == 1 && note.op == WorldReady && note.epoch == 7);
        ++admitted;
    });
    isHost = false; localId = 2;
    Deliver(server, ack);
    assert(admitted == 1);
    auto unsafeAck = ack;
    unsafeAck.flags = 0;
    Deliver(server, unsafeAck);
    Deliver(&fakePeers[1], ack);
    assert(admitted == 1);
    SetWorldHandler(nullptr);
    SetStateHandler(nullptr); SetWorldStateHandler(nullptr);
    transmissions.clear();
    isHost = true;
    localId = 1;
    clientInWorld[2] = true;

    remotes[2].connected = true;
    remotes[2].latestTicks = fakeTime;
    remotes[2].latest.focusNpc = 123;
    remotes[2].latest.targetNpc = 456;
    assert(RemoteFocusNpc(2) == 123 && RemoteTargetNpc(2) == 456 && RemoteStateAgeMs(2) == 0);
    fakeTime += 1000;
    assert(RemoteFocusNpc(2) == 0 && RemoteTargetNpc(2) == 0);
    clientInWorld[2] = false;
    assert(RemoteStateAgeMs(2) == UINT32_MAX);

    // Live authenticated heartbeat/relay paths reject protected players even without a loaded puppet.
    clientInWorld[2] = clientInWorld[3] = true;
    currentLocal.flags = 1;
    currentLocal.world = 42;
    auto activityFrame = [&](uint8_t player, uint8_t flags, uint32_t packetSequence) {
        const ActivityMsg message{MsgPlayerActivity, player, flags, packetSequence, 42, worldEpoch, 0};
        Transmission frame{{}, ENET_PACKET_FLAG_RELIABLE, ChannelState, peers[player]};
        frame.bytes.resize(sizeof(message));
        std::memcpy(frame.bytes.data(), &message, sizeof(message));
        return frame;
    };
    remotes[3].activity = {};
    const auto openTrade = activityFrame(3, 1 | downed::InTrade, 100);
    Deliver(peers[3], openTrade);
    assert(remotes[3].activity.flags == (1 | downed::InTrade));
    assert(downed::PlayerInteractionFlags(3) == downed::InTrade);
    transmissions.clear();
    static unsigned receivedPlayerHits = 0;
    SetHitHandlers(nullptr, [](int32_t, uint8_t const*, uint8_t, bool) { ++receivedPlayerHits; });
    HitPlayerMsg hit{MsgHitPlayer, -10, {}, 2, 3, 0};
    Transmission hitFrame{{}, ENET_PACKET_FLAG_RELIABLE, ChannelCombat, peers[2]};
    hitFrame.bytes.resize(sizeof(hit));
    std::memcpy(hitFrame.bytes.data(), &hit, sizeof(hit));
    Deliver(peers[2], hitFrame);
    assert(transmissions.empty() && !receivedPlayerHits);
    SendHitPlayer(3, -10, hit.attacker, 1, false);
    assert(transmissions.empty()); // host native/friendly hit is not sent either
    const auto grace = activityFrame(3, 1 | downed::InteractionGrace, 101);
    Deliver(peers[3], grace);
    transmissions.clear();
    Deliver(peers[2], hitFrame);
    assert(transmissions.empty());
    const auto closed = activityFrame(3, 1, 102);
    Deliver(peers[3], closed);
    Deliver(peers[3], openTrade);
    assert(remotes[3].activity.sequence == 102 && !downed::PlayerInteractionFlags(3));
    transmissions.clear();
    Deliver(peers[2], hitFrame);
    assert(transmissions.size() == 1 && transmissions[0].peer == peers[3]);
    transmissions.clear();
    SendHitPlayer(3, -10, hit.attacker, 1, false);
    assert(transmissions.size() == 1);
    transmissions.clear();

    currentLocal.flags = 1 | downed::InDialogue;
    hit.to = 1;
    std::memcpy(hitFrame.bytes.data(), &hit, sizeof(hit));
    Deliver(peers[2], hitFrame);
    assert(!receivedPlayerHits && transmissions.empty()); // host hero's owning side is symmetric
    currentLocal.flags = 1;
    Deliver(peers[2], hitFrame);
    assert(receivedPlayerHits == 1);
    SetHitHandlers(nullptr, nullptr);
    transmissions.clear();
    host = nullptr;
}
