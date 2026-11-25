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
        fakePeers[i].address.port = static_cast<unsigned short>(i + 12);
        peerAccounts[i + 2] = i + 12;
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
    assert(terminal.channel == ChannelDefinitions && (terminal.flags & ENET_PACKET_FLAG_RELIABLE));
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
                const bool fullFrame = (transmission.bytes[2] & playerwire::FullFlag) != 0;
                assert(transmission.bytes.size() < (fullFrame ? 112u : 100u));
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

    // Player metadata may beat the reliable world-entry ACK on another channel.
    PlayerState entryState = currentLocal;
    entryState.flags = 1;
    entryState.sequence += 10;
    entryState.position[0] = 100;
    playerSenders[2][1] = {};
    assert(SendPlayerState(peers[2], entryState));
    const auto entrySender = playerSenders[2][1];
    const auto entryKeyframe = transmissions.back();
    transmissions.clear();
    isHost = false;
    localId = 2;
    inWorld = false;
    remotes[1] = {};
    playerReceivers[1] = {};
    connected = false;
    Deliver(server, entryKeyframe);
    assert(!playerReceivers[1].HasKeyframe() && !remotes[1].latestTicks);
    connected = true;
    Deliver(&fakePeers[1], entryKeyframe);
    assert(!playerReceivers[1].HasKeyframe() && !remotes[1].latestTicks);
    auto untrustedMetadata = entryKeyframe;
    untrustedMetadata.flags = 0;
    Deliver(server, untrustedMetadata);
    assert(!playerReceivers[1].HasKeyframe());
    Deliver(server, entryKeyframe);
    assert(playerReceivers[1].HasKeyframe() && !inWorld);
    assert(remotes[1].latest.sequence == entryState.sequence);
    assert(remotes[1].preEntryStates == 1 && remotes[1].acceptedStates == 1);
    SetInWorld(2, true);
    assert(playerReceivers[1].HasKeyframe());
    isHost = true;
    localId = 1;
    playerSenders[2][1] = entrySender; // the fixture shares both endpoints' storage
    ++entryState.sequence;
    entryState.position[0] = 110;
    assert(SendPlayerState(peers[2], entryState));
    const auto entryHot = transmissions.back();
    assert(!(entryHot.flags & ENET_PACKET_FLAG_RELIABLE));
    transmissions.clear();
    isHost = false;
    localId = 2;
    Deliver(server, entryHot);
    assert(remotes[1].latest.position[0] == 110 && remotes[1].acceptedStates == 2);
    assert(remotes[1].metadataWaits == 0 && transmissions.empty());
    // Closing a world still invalidates the previous world's metadata.
    SetInWorld(2, false);
    assert(!playerReceivers[1].HasKeyframe() && !remotes[1].latestTicks);
    Deliver(server, entryHot);
    assert(!remotes[1].latestTicks && remotes[1].metadataWaits == 1);
    SetInWorld(2, true);
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
    // Only reliable story/door/sector restoration handshakes bypass closed gameplay gates.
    static unsigned storyMessages = 0, doorMessages = 0, sectorMessages = 0;
    SetStateHandler([](uint8_t from, uint8_t kind, StateEntry const*, int) {
        assert(from == 1 || from == 2);
        assert(kind & story::policy::Snapshot);
        ++storyMessages;
    });
    SetWorldStateHandler([](uint8_t from, WorldState const& state) {
        assert((from == 1 || from == 2) && (state.kind == 10 || state.kind == 2));
        if (state.kind == 10) ++doorMessages;
        else ++sectorMessages;
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
    WorldState sectorReq{};
    sectorReq.kind = 2; sectorReq.npcClass = 0x80;
    assert(SendWorldStateTo(1, sectorReq));
    const auto sectorQuery = transmissions.back();
    transmissions.clear();
    isHost = true;
    localId = 1;
    clientInWorld[2] = false;
    Deliver(peers[2], query);
    Deliver(peers[2], doorQuery);
    Deliver(peers[2], sectorQuery);
    assert(storyMessages == 1 && doorMessages == 1 && sectorMessages == 1);
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
    WorldState sectorEntry{};
    sectorEntry.kind = 2; sectorEntry.npcClass = 0x40;
    strcpy_s(sectorEntry.routine, "G3_Myrtana_01_Ardea_NPC_02");
    sectorEntry.enclave = 1;
    assert(SendWorldStateTo(2, sectorEntry));
    assert(transmissions.size() == 5);
    isHost = false;
    localId = 2;
    for (auto const& transmission : transmissions) Deliver(server, transmission);
    assert(storyMessages == 4 && doorMessages == 2 && sectorMessages == 2);
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

    // MsgHitResult accepts both 68-byte knockback-enriched replies and legacy 34-byte replies (knockbackValid=0).
    static_assert(saving::Protocol == 81, "Build 81 protocol version must remain 81");
    static_assert(LegacyHitResultMsgSize == 34 && sizeof(HitResultMsg) == 68, "HitResultMsg wire layout");
    static unsigned hitResultsReceived = 0;
    static HitResult lastHitResult{};
    SetHitResultHandler([](HitResult const& res) {
        ++hitResultsReceived;
        lastHitResult = res;
    });
    isHost = true;
    localId = 1;
    clientInWorld[2] = true;
    HitResult fullResult{};
    fullResult.guid[0] = 0xAB;
    fullResult.sequence = 77;
    fullResult.incarnation = 5;
    fullResult.accepted = 1;
    fullResult.knockbackValid = 1;
    fullResult.hostReaction = HitStumble;
    fullResult.hitDurationMs = 615;
    fullResult.moveDurationMs = 615;
    fullResult.originPos[0] = 1000.0f;
    fullResult.goalPos[0] = 1083.8f;
    fullResult.stepCm = 83.8f;
    SendHitResult(2, fullResult);
    assert(transmissions.size() == 1 && transmissions[0].bytes.size() == 68 && transmissions[0].channel == ChannelCombat);
    const auto fullHitPacket = transmissions[0];
    transmissions.clear();
    isHost = false;
    localId = 2;
    connected = inWorld = true;
    Deliver(server, fullHitPacket);
    assert(hitResultsReceived == 1 && lastHitResult.sequence == 77 && lastHitResult.knockbackValid == 1);
    assert(lastHitResult.hostReaction == HitStumble && std::fabs(lastHitResult.stepCm - 83.8f) < 0.01f);

    auto legacyHitPacket = fullHitPacket;
    legacyHitPacket.bytes.resize(LegacyHitResultMsgSize);
    lastHitResult = {};
    Deliver(server, legacyHitPacket);
    assert(hitResultsReceived == 2 && lastHitResult.sequence == 77 && lastHitResult.accepted == 1);
    assert(lastHitResult.knockbackValid == 0 && lastHitResult.hostReaction == 0 && lastHitResult.stepCm == 0.0f);

    for (size_t badSize : {size_t(33), size_t(35), size_t(67), size_t(69)}) {
        auto malformed = fullHitPacket;
        malformed.bytes.resize(badSize, 0);
        Deliver(server, malformed);
        assert(hitResultsReceived == 2);
    }
    SetHitResultHandler(nullptr);
    transmissions.clear();

    // Unjoined ENetPeers (spoofed peer->data != active peers[slot]) must be rejected by PeerSlotOf,
    // and client handlers must ignore server frames while connected == false.
    ENetPeer unjoinedPeer = fakePeers[0];
    unjoinedPeer.data = reinterpret_cast<void*>(uintptr_t(2)); // claims slot 2, but peers[2] == &fakePeers[0]
    isHost = true;
    localId = 1;
    clientInWorld[2] = clientInWorld[3] = true;
    static unsigned forgedDialogs = 0;
    SetDialogLineHandler([](uint8_t, DialogLineNote const&) { ++forgedDialogs; });
    DialogLineMsg dlg{MsgDialogLine, 2, {}};
    dlg.note.kind = DialogLineStart;
    strcpy_s(dlg.note.text, "Info_Test");
    Transmission dlgFrame{{}, ENET_PACKET_FLAG_RELIABLE, ChannelReliable, &unjoinedPeer};
    dlgFrame.bytes.resize(sizeof(dlg));
    std::memcpy(dlgFrame.bytes.data(), &dlg, sizeof(dlg));
    Deliver(&unjoinedPeer, dlgFrame);
    assert(forgedDialogs == 0 && transmissions.empty());

    isHost = false;
    localId = 2;
    connected = false;
    inWorld = true;
    dlg.from = 1;
    std::memcpy(dlgFrame.bytes.data(), &dlg, sizeof(dlg));
    Deliver(server, dlgFrame);
    assert(forgedDialogs == 0);
    connected = true;
    Deliver(server, dlgFrame);
    assert(forgedDialogs == 1);
    SetDialogLineHandler(nullptr);

    // Client NPC stream & motion-name messages from non-server peers MUST be rejected,
    // and host MsgPoseAck MUST require reliable delivery.
    static unsigned motionNamesReceived = 0;
    SetNpcHandlers(nullptr, [](uint32_t, char const*) { ++motionNamesReceived; });
    MotionNameMsg motionMsg{MsgMotionName, HashName("Hum_Test_Ani"), {}};
    strcpy_s(motionMsg.name, "Hum_Test_Ani");
    Transmission motionFrame{{}, ENET_PACKET_FLAG_RELIABLE, ChannelDefinitions, &fakePeers[1]};
    motionFrame.bytes.resize(sizeof(motionMsg));
    std::memcpy(motionFrame.bytes.data(), &motionMsg, sizeof(motionMsg));
    Deliver(&fakePeers[1], motionFrame);
    assert(motionNamesReceived == 0);
    motionFrame.peer = server;
    Deliver(server, motionFrame);
    assert(motionNamesReceived == 1);
    SetNpcHandlers(nullptr, nullptr);

    isHost = true;
    localId = 1;
    worldEpoch = 17;
    boneGeneration = 3;
    PoseAckMsg poseAck{MsgPoseAck, worldEpoch, boneGeneration, {}, 1, 1};
    poseAck.guid[0] = 0x42;
    const uint64_t rejectedBefore = npcRejected;
    Transmission ackFrame{{}, 0, ChannelState, peers[2]};
    ackFrame.bytes.resize(sizeof(poseAck));
    std::memcpy(ackFrame.bytes.data(), &poseAck, sizeof(poseAck));
    Deliver(peers[2], ackFrame); // unreliable MsgPoseAck -> rejected before cache lookup
    assert(npcRejected == rejectedBefore + 1);

    // Reliable NPC back-slot equipment replication (MsgNpcEquipment) obeys world gate, server identity,
    // reliability, and non-zero incarnation invariants.
    static unsigned equipmentMessages = 0;
    static NpcEquipmentNote lastEquipment{};
    SetNpcEquipmentHandler([](NpcEquipmentNote const& note) {
        ++equipmentMessages;
        lastEquipment = note;
    });
    NpcEquipmentNote equip{};
    equip.guid[0] = 0x55;
    equip.incarnation = 4;
    equip.backSlots[0] = HashName("It_Shield_01");
    equip.backSlots[1] = HashName("It_1H_Sword_01");
    equip.backSlots[2] = HashName("It_Bow_01");
    equip.backSlots[3] = HashName("It_Crossbow_01");
    transmissions.clear();
    clientInWorld[2] = false;
    assert(!SendNpcEquipmentTo(2, equip) && transmissions.empty());
    clientInWorld[2] = true;
    assert(SendNpcEquipmentTo(2, equip) && transmissions.size() == 1);
    const auto equipFrame = transmissions[0];
    assert(equipFrame.channel == ChannelReliable && (equipFrame.flags & ENET_PACKET_FLAG_RELIABLE));
    assert(equipFrame.bytes.size() == sizeof(NpcEquipmentMsg) && equipFrame.bytes[0] == MsgNpcEquipment);
    transmissions.clear();
    isHost = false;
    localId = 2;
    connected = inWorld = true;
    Deliver(&fakePeers[1], equipFrame); // non-server peer refused
    assert(equipmentMessages == 0);
    auto unreliableEquip = equipFrame;
    unreliableEquip.flags = 0;
    Deliver(server, unreliableEquip); // unreliable refused
    assert(equipmentMessages == 0);
    auto zeroIncEquip = equipFrame;
    NpcEquipmentMsg zeroIncMsg{};
    std::memcpy(&zeroIncMsg, zeroIncEquip.bytes.data(), sizeof(zeroIncMsg));
    zeroIncMsg.note.incarnation = 0;
    std::memcpy(zeroIncEquip.bytes.data(), &zeroIncMsg, sizeof(zeroIncMsg));
    Deliver(server, zeroIncEquip);
    assert(equipmentMessages == 0);
    inWorld = false;
    Deliver(server, equipFrame); // gated out before world admission
    assert(equipmentMessages == 0);
    inWorld = true;
    Deliver(server, equipFrame);
    assert(equipmentMessages == 1 && lastEquipment.guid[0] == 0x55 && lastEquipment.incarnation == 4);
    assert(lastEquipment.backSlots[2] == equip.backSlots[2] && lastEquipment.backSlots[3] == equip.backSlots[3]);
    isHost = true;
    localId = 1;
    Deliver(peers[2], equipFrame); // client cannot publish NPC back equipment to host
    assert(equipmentMessages == 1);
    SetNpcEquipmentHandler(nullptr);

    // Event-driven MsgPlayerEquipment sync: immediate send on change, join/rejoin/load resync, pre-entry caching, and peer relay.
    {
        auto equipTransmissions = [&]() {
            std::vector<Transmission> out;
            for (auto const& t : transmissions)
                if (!t.bytes.empty() && t.bytes[0] == MsgPlayerEquipment) out.push_back(t);
            return out;
        };
        transmissions.clear();
        isHost = true;
        localId = 1;
        worldEpoch = 99;
        clientInWorld[2] = false;
        clientInWorld[3] = false;
        havePendingLocalEquip = false;
        haveCurrentEquip = false;
        ClearAllEquipTables();
        remotes[2] = {};
        remotes[3] = {};

        static unsigned equipEvents = 0;
        static PlayerEquipment lastEquip{};
        SetPlayerEquipmentHandler([](PlayerEquipment const& eq, uint32_t, bool) {
            ++equipEvents;
            lastEquip = eq;
        });

        // Host equips a sword + shield on back, bow + arrow, armor + helmet before client 2 enters world.
        PlayerEquipment hostEq{};
        hostEq.world = 42;
        hostEq.SetSlot(player_equip::SlotBackRight, "It_1H_Sword_01", 2);
        hostEq.SetSlot(player_equip::SlotBackLeft, "It_Shield_01", 0);
        hostEq.SetSlot(player_equip::SlotBow, "It_Bow_Long_01", 0);
        hostEq.SetSlot(player_equip::SlotAmmo, "Arrow", 0);
        hostEq.SetSlot(player_equip::SlotBody, "Body_Paladin", 0);
        hostEq.SetSlot(player_equip::SlotHelmet, "It_Helmet_01", 0);
        UpdateLocalEquipment(hostEq, true);
        Service(currentLocal, false);
        assert(equipTransmissions().empty()); // peer 2 not in world yet

        // Peer 2 joins/enters world: host immediately sends cached host equipment reliably on ChannelState.
        transmissions.clear();
        SetInWorld(2, true);
        auto sent = equipTransmissions();
        assert(sent.size() == 1);
        const auto hostToClient2 = sent[0];
        assert(hostToClient2.peer == peers[2] && hostToClient2.channel == ChannelState);
        assert((hostToClient2.flags & ENET_PACKET_FLAG_RELIABLE) != 0);
        assert(hostToClient2.bytes[0] == MsgPlayerEquipment);
        transmissions.clear();

        // Client 2 receives it even just before its own inWorld flips true (pre-entry cache), and keeps it on SetInWorld(true).
        isHost = false;
        localId = 2;
        connected = true;
        inWorld = false;
        Deliver(server, hostToClient2);
        assert(equipEvents == 1 && remotes[1].hasEquip);
        assert(std::strcmp(lastEquip.slots[player_equip::SlotBackRight].name, "It_1H_Sword_01") == 0);
        assert(lastEquip.slots[player_equip::SlotBackRight].quality == 2);
        assert(std::strcmp(lastEquip.slots[player_equip::SlotBow].name, "It_Bow_Long_01") == 0);
        SetInWorld(2, true);
        assert(remotes[1].hasEquip);

        // Client 2 equips a 2H axe and sends its equipment; arrives at host pre-entry or in-world and relays to peer 3.
        PlayerEquipment client2Eq{};
        client2Eq.world = 42;
        client2Eq.SetSlot(player_equip::SlotBackRight, "It_2H_Axe_01", 4);
        UpdateLocalEquipment(client2Eq, true);
        Service(currentLocal, false);
        sent = equipTransmissions();
        assert(sent.size() == 1);
        const auto client2Pkt = sent[0];
        // Unchanged second tick sends 0 MsgPlayerEquipment packets (no polling!).
        transmissions.clear();
        Service(currentLocal, false);
        assert(equipTransmissions().empty());

        // Host receives client 2's packet before peer 3 enters, then peer 3 enters and receives both host and client 2!
        isHost = true;
        localId = 1;
        UpdateLocalEquipment(hostEq, true);
        Service(currentLocal, false);
        clientInWorld[2] = true;
        clientInWorld[3] = false;
        remotes[2] = {};
        Deliver(peers[2], client2Pkt);
        assert(equipEvents == 2 && remotes[2].hasEquip);
        assert(std::strcmp(remotes[2].latestEquip.slots[player_equip::SlotBackRight].name, "It_2H_Axe_01") == 0);
        transmissions.clear();

        SetInWorld(3, true);
        auto forPeer3 = equipTransmissions();
        assert(forPeer3.size() == 2 && forPeer3[0].peer == peers[3] && forPeer3[1].peer == peers[3]); // host + client 2 delivered to peer 3
        transmissions.clear();

        // World load / epoch bump resets equipment baseline and triggers immediate resync to all in-world peers.
        worldEpoch = 100;
        ClearAllEquipTables();
        SetInWorld(2, false);
        assert(!remotes[2].hasEquip);
        SetInWorld(2, true);
        Service(currentLocal, false);
        sent = equipTransmissions();
        assert(sent.size() == 2 && sent[0].peer == peers[2] && sent[1].peer == peers[3]);

        // Stale world epoch or unreliable packet is ignored.
        isHost = false;
        localId = 2;
        auto badEpoch = hostToClient2; // epoch 99 != 100
        Deliver(server, badEpoch);
        assert(equipEvents == 2);
        auto unreliable = sent[0];
        unreliable.flags = 0;
        Deliver(server, unreliable);
        assert(equipEvents == 2);

        SetPlayerEquipmentHandler(nullptr);
    }

    host = nullptr;
}
