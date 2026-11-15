// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the in-memory ENet and application stubs while exercising net.cpp.
#define main bone_transport_fixture_main
#include "npcwire_bones_transport_test.cpp"
#undef main

#include <cmath>

std::vector<NpcRecord> receivedRecords;
unsigned releasedRecords = 0;
int CaptureRecords(NpcRecord* records, int count, uint32_t, uint32_t, uint8_t, uint32_t)
{
    receivedRecords.insert(receivedRecords.end(), records, records + count);
    return count;
}
int CaptureReleases(uint8_t, uint32_t, uint8_t*, int count) { releasedRecords += count; return count; }

NpcRecord Record(unsigned number)
{
    NpcRecord record{};
    record.guid[0] = static_cast<uint8_t>(number);
    record.guid[1] = static_cast<uint8_t>(number >> 8);
    record.position[0] = 100.25f + number;
    record.position[1] = -10.5f;
    record.position[2] = 5.75f;
    record.rotation[3] = 1.0f;
    record.sent = 1000 + number;
    record.hitPoints = 77;
    record.hitPointsMax = 100;
    record.movementMode = 2;
    record.heldRight = 123;
    record.heldLeft = 456;
    record.combatTarget = 0x12345678;
    record.hitPlayer = 2;
    record.hitSequence = 0x3456;
    record.partyPlayer = 1;
    record.partyType = 3;
    record.waiting = 1;
    record.guidePlayer = 2;
    record.aiMode = 5;
    record.action = 9;
    record.attackReason = 7;
    record.damageType = 3;
    record.damageAmount = 32;
    record.damageMultiplier = 1.25f;
    record.incarnation = 7;
    record.contact = 1;
    record.attitude = 2;
    record.motion = 888;
    record.aniState = 4;
    record.motions[0].active = 1;
    record.motions[0].name = 55;
    record.motions[0].instance = 66;
    record.motions[0].phase = 0.3f;
    record.motions[0].weight = 0.7f;
    record.motions[0].speed = 1.5f;
    record.motions[0].intro = 200.0f;
    record.motions[0].extro = 300.0f;
    record.motions[0].blend = -42;
    for (unsigned i = 0; i < 7; ++i) record.motions[0].options[i] = 100 + i;
    return record;
}

void Handshake(ENetPeer& peer, NpcRecord const& record)
{
    isHost = true;
    transmissions.clear();
    SendNpcs(&record, 1, false);
    assert(transmissions.size() == 3);
    assert(transmissions[0].bytes[0] == MsgControlId);
    assert(transmissions[1].bytes[0] == MsgControlBaseline);
    assert(transmissions[2].bytes[0] == MsgControlBatch);
    assert(transmissions[2].channel == ChannelDefinitions &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto id = transmissions[0], baseline = transmissions[1], dependent = transmissions[2];
    transmissions.clear();
    isHost = false;
    Deliver(&peer, id);
    Deliver(&peer, baseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto ack = transmissions[0];
    transmissions.clear();
    Deliver(&peer, dependent);
    assert(receivedRecords.size() >= 1);
    transmissions.clear();
    isHost = true;
    Deliver(&peer, ack);
    assert(transmissions.empty());
    receivedRecords.clear();
}

int main()
{
    ENetHost fakeHost{};
    ENetPeer fakePeers[2]{};
    ENetPeer& fakePeer = fakePeers[0];
    ENetChannel channels[2][ChannelCount]{};
    fakePeer.channels = channels[0];
    fakePeer.data = reinterpret_cast<void*>(static_cast<uintptr_t>(2));
    fakePeers[1].channels = channels[1];
    fakePeers[1].data = reinterpret_cast<void*>(static_cast<uintptr_t>(3));
    fakeHost.peers = &fakePeer;
    host = &fakeHost;
    peers[2] = &fakePeer;
    fakePeer.address.port = 12;
    fakePeers[1].address.port = 13;
    peerAccounts[2] = 12;
    peerAccounts[3] = 13;
    server = &fakePeer;
    target.host = fakePeer.address.host;
    worldEpoch = 0x1234;
    isHost = true;
    localId = 1;
    inWorld = true;
    clientInWorld[2] = true;
    ResetNpcReplication();
    SetNpcHandlers(CaptureRecords, nullptr);
    SetNpcPoseHandler(GotPose);
    SetNpcReleaseHandler(CaptureReleases);

    // Old wire versions receive a refusal, never a welcome.
    Hello oldHello{};
    oldHello.type = MsgHello;
    std::memcpy(oldHello.name, "old client", 11);
    Transmission oldHandshake;
    oldHandshake.bytes.resize(sizeof(oldHello));
    oldHandshake.flags = ENET_PACKET_FLAG_RELIABLE;
    fakePeers[1].data = nullptr;
    RefusedMsg refusal{};
    for (uint32_t ver : {72u, 73u, 76u, 77u}) {
        oldHello.version = ver;
        std::memcpy(oldHandshake.bytes.data(), &oldHello, sizeof(oldHello));
        transmissions.clear();
        Deliver(&fakePeers[1], oldHandshake);
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgRefused);
        std::memcpy(&refusal, transmissions[0].bytes.data(), sizeof(refusal));
        char expected[32];
        std::snprintf(expected, sizeof(expected), "protocol %u, host 79", ver);
        assert(std::strstr(refusal.reason, expected) != nullptr);
        assert(!fakePeers[1].data && !peers[3]);
    }
    assert(!fakePeers[1].data && !peers[3]);
    fakePeers[1].data = reinterpret_cast<void*>(static_cast<uintptr_t>(3));
    transmissions.clear();

    NpcRecord records[MaxNpcsPerMessage];
    records[0] = Record(1);
    Handshake(fakePeer, records[0]);
    records[0].position[0] += 12.25f;
    records[0].sent = 3333;
    SendNpcs(records, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto firstCompact = transmissions[0];
    assert(firstCompact.bytes.size() == sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader) +
                                        sizeof(npcwire::WireControl) + sizeof(npcwire::WireMotionTick));
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, firstCompact);
    assert(receivedRecords.size() == 1);
    const auto& decoded = receivedRecords.back();
    assert(decoded.sent == 3333 && decoded.hitPoints == 77 && decoded.combatTarget == 0x12345678);
    assert(decoded.heldRight == 123 && decoded.heldLeft == 456 && decoded.hitSequence == 0x3456);
    assert(decoded.partyPlayer == 1 && decoded.partyType == 3 && decoded.waiting == 1 &&
           decoded.guidePlayer == 2 && decoded.aiMode == 5 && decoded.action == 9 &&
           decoded.attackReason == 7 && decoded.damageType == 3 && decoded.damageAmount == 32 &&
           decoded.damageMultiplier == 1.25f && decoded.hitPlayer == 2 && decoded.contact == 1 &&
           decoded.attitude == 2 && decoded.motion == 888 && decoded.aniState == 4 && decoded.incarnation == 7);
    assert(decoded.motions[0].speed == 1.5f && decoded.motions[0].intro == 200.0f &&
           decoded.motions[0].extro == 300.0f && decoded.motions[0].blend == -42);
    for (unsigned i = 0; i < 7; ++i) assert(decoded.motions[0].options[i] == 100 + i);
    assert(std::fabs(decoded.position[0] - records[0].position[0]) <= 0.5f);
    assert(std::fabs(decoded.motions[0].weight - records[0].motions[0].weight) <= 1.0f / 255);

    for (unsigned i = 1; i < MaxNpcsPerMessage; ++i) {
        records[i] = Record(i + 1);
        if (i & 1u) { records[i].rotation[0] = 0.1f; records[i].rotation[3] = 0.9f; }
        Handshake(fakePeer, records[i]);
    }
    isHost = true;
    transmissions.clear();
    SendNpcs(records, MaxNpcsPerMessage, false);
    assert(transmissions.size() >= 2);
    for (auto const& packet : transmissions) {
        assert(packet.bytes[0] == MsgControlBatch);
        assert(packet.bytes.size() <= static_cast<size_t>(NpcDatagramBudget));
        assert(!(packet.flags & ENET_PACKET_FLAG_RELIABLE));
    }
    const auto batches = transmissions;
    transmissions.clear();
    receivedRecords.clear();
    isHost = false;
    for (auto const& packet : batches) Deliver(&fakePeer, packet);
    assert(receivedRecords.size() == MaxNpcsPerMessage);
    for (unsigned i = 0; i < MaxNpcsPerMessage; ++i) {
        assert(std::memcmp(receivedRecords[i].guid, records[i].guid, 16) == 0);
        assert(receivedRecords[i].sent == records[i].sent);
        if (i & 1u)
            assert(std::memcmp(receivedRecords[i].rotation, records[i].rotation, sizeof(records[i].rotation)) == 0);
    }
    for (auto const& packet : batches) Deliver(&fakePeer, packet);
    assert(receivedRecords.size() == MaxNpcsPerMessage);

    auto sendHostBatch = [&](NpcRecord const* recs, int n, bool reliable = false) {
        isHost = true;
        transmissions.clear();
        SendNpcs(recs, n, reliable);
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
        return transmissions[0];
    };
    auto deliverClient = [&](Transmission const& pkt) {
        transmissions.clear();
        isHost = false;
        Deliver(&fakePeer, pkt);
    };

    // Reordering a mixed batch must not discard the still-new NPC merely
    // because another NPC already arrived in a newer packet.
    const auto mixedOlder = sendHostBatch(records, 2);
    const auto oneNewer = sendHostBatch(records, 1);
    receivedRecords.clear();
    deliverClient(oneNewer);
    Deliver(&fakePeer, mixedOlder);
    assert(receivedRecords.size() == 2);
    assert(receivedRecords[0].guid[0] == records[0].guid[0] &&
           receivedRecords[1].guid[0] == records[1].guid[0]);

    // Scalar changes stay compact and exact against the named baseline.
    records[0].hitPoints = 60;
    records[0].combatTarget = 0x87654321;
    records[0].action = 8;
    const auto scalarPacket = sendHostBatch(records, 1);
    deliverClient(scalarPacket);
    assert(receivedRecords.back().hitPoints == 60 && receivedRecords.back().combatTarget == 0x87654321 &&
           receivedRecords.back().action == 8);

    // Reliable publication is bit-exact without repeating descriptor tables.
    records[0].position[0] += 0.123456f;
    records[0].motions[0].phase = 0.123456f;
    records[0].motions[0].weight = 0.654321f;
    const auto exactPacket = sendHostBatch(records, 1, true);
    assert(exactPacket.channel == ChannelDefinitions && (exactPacket.flags & ENET_PACKET_FLAG_RELIABLE));
    deliverClient(exactPacket);
    assert(std::memcmp(&receivedRecords.back(), &records[0], sizeof(NpcRecord)) == 0);

    // A changed immutable motion descriptor uses a relative motion override,
    // without replacing the named baseline or life.
    records[0].motions[0].speed = 1.6f;
    const auto changedCompact = sendHostBatch(records, 1);
    const auto key = NpcKey(records[0].guid);
    const uint16_t token = controlStates.at(key).nextBaseline;
    deliverClient(changedCompact);
    assert(receivedRecords.back().hitPoints == 60 && receivedRecords.back().motions[0].speed == 1.6f);

    // Extended active weight uses exact compact motion ticks without churning baselines.
    records[0].motions[0].weight = 2.0f;
    Transmission lastWeight{};
    for (int i = 0; i < 10; ++i) {
        lastWeight = sendHostBatch(records, 1);
        assert(controlStates.at(key).nextBaseline == token);
    }
    deliverClient(lastWeight);
    assert(receivedRecords.back().motions[0].weight == 2.0f);
    records[0].motions[0].weight = 0.7f;
    records[0].position[0] += 40000.0f;
    (void)sendHostBatch(records, 1);
    assert(controlStates.at(key).nextBaseline == token);

    // Client rejoin loses its tables: the compact packet requests the ID and
    // the host resends the ID, baseline and latest gameplay record reliably.
    NpcRecord fresh = records[1];
    const auto early = sendHostBatch(&fresh, 1);
    transmissions.clear();
    isHost = false;
    SetInWorld(2, false);
    SetInWorld(2, true);
    const size_t beforeUnknown = receivedRecords.size();
    Deliver(&fakePeer, early);
    assert(receivedRecords.size() == beforeUnknown && transmissions.size() == 1 &&
           transmissions[0].bytes[0] == MsgControlSignal);
    const auto request = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, request);
    assert(transmissions.size() == 3 && transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline && transmissions[2].bytes[0] == MsgNpcs &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto resentId = transmissions[0], resentBaseline = transmissions[1], resentGameplay = transmissions[2];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, resentId);
    Deliver(&fakePeer, resentBaseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    Deliver(&fakePeer, resentGameplay);
    assert(receivedRecords.size() == beforeUnknown + 1);
    assert(std::memcmp(&receivedRecords.back(), &fresh, sizeof(NpcRecord)) == 0);
    const auto resyncAck = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, resyncAck);
    SendNpcs(&fresh, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto recovered = transmissions[0];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, recovered);
    assert(receivedRecords.size() == beforeUnknown + 2);

    // Reliable life transitions use compact control after setup is queued,
    // and precede reliable release on the ordered definitions channel.
    isHost = true;
    fresh.flags = 1;
    fresh.hitPoints = 0;
    transmissions.clear();
    SendNpcs(&fresh, 1, true);
    SendNpcRelease(fresh.guid, 1);
    Transmission terminal, release;
    Transmission setupId, setupBaseline;
    bool haveSetupId = false, haveSetupBaseline = false;
    for (const auto& packet : transmissions) {
        if (packet.bytes[0] == MsgControlId) { setupId = packet; haveSetupId = true; }
        if (packet.bytes[0] == MsgControlBaseline) { setupBaseline = packet; haveSetupBaseline = true; }
        if (packet.bytes[0] == MsgControlBatch) terminal = packet;
        if (packet.bytes[0] == MsgNpcRelease) release = packet;
    }
    assert(!terminal.bytes.empty() && !release.bytes.empty());
    assert(terminal.channel == ChannelDefinitions && release.channel == ChannelDefinitions &&
           (terminal.flags & ENET_PACKET_FLAG_RELIABLE) &&
           (release.flags & ENET_PACKET_FLAG_RELIABLE));
    transmissions.clear();
    isHost = false;
    Transmission terminalAck;
    if (haveSetupId && haveSetupBaseline) {
        Deliver(&fakePeer, setupId);
        Deliver(&fakePeer, setupBaseline);
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
        terminalAck = transmissions[0];
        transmissions.clear();
    }
    Deliver(&fakePeer, terminal);
    assert(receivedRecords.back().flags == 1 && receivedRecords.back().hitPoints == 0);
    Deliver(&fakePeer, release);
    assert(releasedRecords == 1);
    const size_t terminalCount = receivedRecords.size();
    Deliver(&fakePeer, recovered);
    assert(receivedRecords.size() == terminalCount);
    if (!terminalAck.bytes.empty()) {
        isHost = true;
        transmissions.clear();
        Deliver(&fakePeer, terminalAck);
        transmissions.clear();
    }

    // Both dictionaries must be acknowledged before a posed update combines
    // compressed control and the exact 24-byte bone transforms.
    NpcRecord posed = Record(99);
    Handshake(fakePeer, posed);
    pose::Bone poseBones[80]{};
    for (int i = 0; i < 80; ++i) {
        poseBones[i].name = static_cast<uint32_t>(i + 1);
        for (int j = 0; j < 12; ++j) poseBones[i].values[j] = static_cast<int16_t>(i * 13 + j - 1000);
    }
    isHost = true;
    transmissions.clear();
    SendPosedNpc(posed, poseBones, 80, pose::Facial);
    assert(transmissions.size() == 2 && transmissions[0].bytes[0] == MsgBoneDictionary &&
           (transmissions[1].bytes[0] == MsgCompactPosedNpc ||
            transmissions[1].bytes[0] == MsgControlPose));
    assert(transmissions[1].channel == ChannelDefinitions &&
           (transmissions[1].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto boneDefinition = transmissions[0], coldPose = transmissions[1];
    bonewire::Dictionary hostBones = std::move(boneLayouts);
    boneLayouts.Clear();
    receivedBoneGeneration = 0;
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, boneDefinition);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    const auto boneAck = transmissions[0];
    transmissions.clear();
    Deliver(&fakePeer, coldPose);
    assert(applied == 1 && appliedBones.size() == 80);
    bonewire::Dictionary clientBones = std::move(boneLayouts);
    boneLayouts = std::move(hostBones);
    isHost = true;
    Deliver(&fakePeer, boneAck);
    transmissions.clear();
    SendPosedNpc(posed, poseBones, 80, pose::Facial);
    assert(transmissions.size() == 1 &&
           (transmissions[0].bytes[0] == MsgCompactPosedNpc ||
            transmissions[0].bytes[0] == MsgControlPose));
    const auto warmPose = transmissions[0];
    hostBones = std::move(boneLayouts);
    boneLayouts = std::move(clientBones);
    isHost = false;
    Deliver(&fakePeer, warmPose);
    assert(applied == 2 && appliedFlags == pose::Facial && appliedBones.size() == 80);
    for (int i = 0; i < 80; ++i) {
        assert(appliedBones[i].name == poseBones[i].name);
        for (int j = 0; j < 12; ++j) assert(appliedBones[i].values[j] == poseBones[i].values[j]);
    }

    clientBones = std::move(boneLayouts);
    boneLayouts = std::move(hostBones);
    isHost = true;
    transmissions.clear();
    SendPosedNpc(posed, poseBones, 80, pose::Facial);
    assert(transmissions.size() == 1 &&
           (transmissions[0].bytes[0] == MsgCompactPosedNpc ||
            transmissions[0].bytes[0] == MsgControlPose));
    const auto earlyPose = transmissions[0];
    hostBones = std::move(boneLayouts);
    boneLayouts.Clear();
    receivedBoneGeneration = 0;
    requestedBoneEpoch = 0;
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, earlyPose);
    assert(applied == 2 && transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneRequest);
    const auto missingBonesRequest = transmissions[0];
    transmissions.clear();
    isHost = true;
    boneLayouts = std::move(hostBones);
    Deliver(&fakePeer, missingBonesRequest);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneDictionary);
    const auto resentBones = transmissions[0];
    hostBones = std::move(boneLayouts);
    boneLayouts.Clear();
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, resentBones);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    transmissions.clear();
    Transmission badPose = earlyPose;
    badPose.bytes.pop_back();
    Deliver(&fakePeer, badPose);
    assert(applied == 2);
    Deliver(&fakePeer, earlyPose);
    assert(applied == 3 && appliedBones.size() == 80);

    clientBones = std::move(boneLayouts);
    boneLayouts = std::move(hostBones);
    isHost = true;
    peers[3] = &fakePeers[1];
    clientInWorld[3] = true;
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    bool readyBatch = false, newIdForPeer = false, newBaselineForPeer = false, dependentForPeer = false;
    for (auto const& packet : transmissions) {
        if (packet.peer == &fakePeer && packet.bytes[0] == MsgControlBatch) readyBatch = true;
        if (packet.peer == &fakePeers[1]) {
            newIdForPeer |= packet.bytes[0] == MsgControlId;
            newBaselineForPeer |= packet.bytes[0] == MsgControlBaseline;
            dependentForPeer |= packet.bytes[0] == MsgControlBatch;
        }
    }
    assert(readyBatch && newIdForPeer && newBaselineForPeer && dependentForPeer);
    Drop(3);

    // ACKs for a previous offer of the same baseline cannot acknowledge a
    // later resync/rejoin offer to the same peer.
    ForgetControlPeer(2);
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 3);
    const auto oldId = transmissions[0], oldBaseline = transmissions[1];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, oldId);
    Deliver(&fakePeer, oldBaseline);
    assert(transmissions.size() == 1);
    const auto oldAck = transmissions[0];
    isHost = true;
    ForgetControlPeer(2);
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 3);
    const auto offeredId = transmissions[0], offeredBaseline = transmissions[1];
    transmissions.clear();
    Deliver(&fakePeer, oldAck);
    assert(controlAcknowledged[2].empty());
    isHost = false;
    Deliver(&fakePeer, offeredId);
    Deliver(&fakePeer, offeredBaseline);
    assert(transmissions.size() == 1);
    const auto offeredAck = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, offeredAck);
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto missingBaseline = transmissions[0];

    // An installed ID without its baseline requests recovery; a malformed
    // update must not consume the sequence needed by the corrected packet.
    isHost = false;
    controlReceiver = npcwire::Context{};
    assert(controlReceiver.Reset(worldEpoch, receivedControlGeneration));
    uint16_t installedId = 0;
    assert(npcwire::DecodeId(controlReceiver, offeredId.bytes.data() + 1,
                             offeredId.bytes.size() - 1, installedId));
    transmissions.clear();
    const size_t beforeMissing = receivedRecords.size();
    Deliver(&fakePeer, missingBaseline);
    assert(receivedRecords.size() == beforeMissing && transmissions.size() == 1 &&
           transmissions[0].bytes[0] == MsgControlSignal);
    const auto missingRequest = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, missingRequest);
    assert(transmissions.size() == 3 && transmissions[2].bytes[0] == MsgNpcs &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto resentControlId = transmissions[0], resentControlBaseline = transmissions[1];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, resentControlId);
    Deliver(&fakePeer, resentControlBaseline);
    assert(transmissions.size() == 1);
    const auto freshAck = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, freshAck);
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto complete = transmissions[0];
    Transmission truncated = complete;
    truncated.bytes.pop_back();
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, truncated);
    assert(receivedRecords.size() == beforeMissing);
    Deliver(&fakePeer, complete);
    assert(receivedRecords.size() == beforeMissing + 1);
    Deliver(&fakePeer, missingBaseline);
    assert(receivedRecords.size() == beforeMissing + 1);

    // Same-world host reset creates a new stream and IDs. Delayed old
    // definitions, ACKs and compact records cannot contaminate the new stream.
    isHost = true;
    const uint32_t previousGeneration = controlGeneration;
    ResetNpcReplication();
    assert(controlGeneration > previousGeneration);
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 3 && transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline &&
           transmissions[2].bytes[0] == MsgControlBatch);
    assert(transmissions[2].channel == ChannelDefinitions &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto nextId = transmissions[0], nextBaseline = transmissions[1], nextDependent = transmissions[2];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, nextId);
    Deliver(&fakePeer, nextBaseline);
    assert(transmissions.size() == 1 && receivedControlGeneration > previousGeneration);
    const auto nextAck = transmissions[0];
    transmissions.clear();
    Deliver(&fakePeer, oldId);
    Deliver(&fakePeer, oldBaseline);
    Deliver(&fakePeer, complete);
    assert(transmissions.empty());
    assert(receivedRecords.size() == beforeMissing + 1);
    isHost = true;
    Deliver(&fakePeer, oldAck);
    assert(controlAcknowledged[2].empty());
    Deliver(&fakePeer, nextAck);
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto nextCompact = transmissions[0];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, nextCompact);
    assert(receivedRecords.size() == beforeMissing + 2);

    // Measure real application payloads for the sender's 48-record chunks.
    isHost = true;
    ResetNpcReplication();
    std::vector<NpcRecord> population;
    for (unsigned i = 0; i < 100; ++i) {
        population.push_back(Record(i + 200));
        Handshake(fakePeer, population.back());
    }
    const unsigned counts[] = {20, 50, 100};
    const size_t expectedControlBytes[] = {581, 1473, 2905};
    const size_t expectedLegacyControlBytes[] = {11990, 29975, 59950};
    for (unsigned c = 0; c < 3; ++c) {
        transmissions.clear();
        for (unsigned offset = 0; offset < counts[c]; offset += MaxNpcsPerMessage) {
            const unsigned size = std::min<unsigned>(MaxNpcsPerMessage, counts[c] - offset);
            SendNpcs(population.data() + offset, static_cast<int>(size), false);
        }
        size_t bytes = 0;
        for (auto const& packet : transmissions) {
            assert(packet.bytes[0] == MsgControlBatch && packet.bytes.size() <= NpcDatagramBudget);
            assert(!(packet.flags & ENET_PACKET_FLAG_RELIABLE));
            bytes += packet.bytes.size();
        }
        assert(bytes == expectedControlBytes[c]);
        assert((counts[c] / 2) * (sizeof(NpcsHeader) + 2 * sizeof(NpcRecord)) == expectedLegacyControlBytes[c]);
    }

    // ENet otherwise promotes the snapshot itself to reliable at sequence
    // wrap. Checkpoint the ordered channel first, then send it unreliably.
    fakePeer.channels[ChannelNpcs].outgoingUnreliableSequenceNumber = UINT16_MAX;
    transmissions.clear();
    SendNpcs(population.data(), 1, false);
    assert(transmissions.size() == 2 && transmissions[0].bytes.empty() &&
           transmissions[0].channel == ChannelNpcs &&
           (transmissions[0].flags & ENET_PACKET_FLAG_RELIABLE));
    assert(transmissions[1].bytes[0] == MsgControlBatch &&
           !(transmissions[1].flags & ENET_PACKET_FLAG_RELIABLE));
    assert(fakePeer.channels[ChannelNpcs].outgoingUnreliableSequenceNumber == 1);

    isHost = true;
    SetInWorld(2, false);
    const uint32_t beforeRejoin = controlGeneration;
    SetInWorld(2, true);
    assert(controlGeneration > beforeRejoin);
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 3 &&
           transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline &&
           transmissions[2].bytes[0] == MsgControlBatch);
    assert(transmissions[2].channel == ChannelDefinitions &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    Deliver(&fakePeer, nextAck);
    assert(controlAcknowledged[2].empty());

    // A round trip longer than the retry interval must still acknowledge the
    // original offer; retries of that offer keep its serial.
    const auto slowId = transmissions[0], slowBaseline = transmissions[1], slowDependent = transmissions[2];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, slowId);
    Deliver(&fakePeer, slowBaseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto slowAck = transmissions[0];
    transmissions.clear();
    Deliver(&fakePeer, slowDependent);
    transmissions.clear();
    isHost = true;
    fakeTime += 1001;
    SendNpcs(&posed, 1, false);
    // A successful ID/baseline queue is retained independently; ordinary
    // sample retries do not enqueue either definition again while ACK is withheld.
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    assert(transmissions[0].channel == ChannelDefinitions &&
           (transmissions[0].flags & ENET_PACKET_FLAG_RELIABLE));
    transmissions.clear();
    Deliver(&fakePeer, slowAck);
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);

    // Withhold setup delivery while changing the descriptor every sample.
    // The named baseline stays pinned and each sample uses a compact override.
    const auto descriptorKey = NpcKey(posed.guid);
    const uint16_t descriptorToken = controlStates.at(descriptorKey).nextBaseline;
    NpcRecord changing = posed;
    changing.motions[0].speed = 1.6f;
    changing.sent++;
    transmissions.clear();
    SendNpcs(&changing, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    assert(controlStates.at(descriptorKey).nextBaseline == descriptorToken);
    transmissions.clear();
    fakeTime += 2100;
    for (unsigned i = 0; i < 4; ++i) {
        changing.motions[0].speed = 1.61f + i * 0.01f;
        changing.sent++;
        changing.position[0] += 1.0f;
        SendNpcs(&changing, 1, false);
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
        assert(controlStates.at(descriptorKey).nextBaseline == descriptorToken);
        transmissions.clear();
    }

    // A failed reliable enqueue leaves the whole transaction retryable. Once
    // successful, later samples enqueue neither definition again.
    NpcRecord partial = Record(102);
    failSend = true;
    transmissions.clear();
    SendNpcs(&partial, 1, false);
    assert(transmissions.empty());
    failSend = false;
    transmissions.clear();
    SendNpcs(&partial, 1, false);
    assert(transmissions.size() == 3);
    transmissions.clear();
    SendNpcs(&partial, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);

    // Exhaustion fails closed, without recycling a control ID or token.
    const auto stateKey = NpcKey(posed.guid);
    controlStates.at(stateKey).nextBaseline = UINT16_MAX;
    posed.motions[0].speed += 0.25f;
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 &&
           (transmissions[0].bytes[0] == MsgControlBatch ||
            transmissions[0].bytes[0] == MsgNpcs));
    nextControlId = UINT16_MAX;
    NpcRecord exhausted = Record(101);
    transmissions.clear();
    SendNpcs(&exhausted, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgNpcs && controlIdExhaustedLogged);

    // Real pose handlers: ACK references survive a lost intermediate packet;
    // resync requests force keys; essential records precede their own pose.
    nextControlId = 0;
    ResetNpcReplication();
    NpcRecord streamRecord = Record(900);
    pose::Bone streamBones[4]{};
    for (int i = 0; i < 4; ++i) {
        streamBones[i].name = static_cast<uint32_t>(i + 100);
        streamBones[i].values[0] = streamBones[i].values[4] = streamBones[i].values[8] = 4096;
        streamBones[i].values[1] = 123; // shear is not projected to a rotation
    }
    auto clientReceive = [&](std::vector<Transmission> const& packets) {
        transmissions.clear();
        isHost = false;
        for (auto const& packet : packets) Deliver(&fakePeer, packet);
        auto responses = transmissions;
        transmissions.clear();
        isHost = true;
        for (auto const& response : responses) Deliver(&fakePeer, response);
    };
    transmissions.clear();
    assert(SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33));
    auto streamBootstrap = transmissions;
    clientReceive(streamBootstrap);
    fakeTime += 33;
    streamRecord.sent = fakeTime;
    transmissions.clear();
    assert(SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33));
    auto streamKey = transmissions;
    clientReceive(streamKey);
    assert(appliedInterval == 33);
    fakeTime += 33; streamRecord.sent = fakeTime; ++streamBones[0].values[9];
    transmissions.clear();
    assert(SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33));
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlPose);
    const auto lostDelta = transmissions.front();
    const size_t boneStart = sizeof(NpcsHeader) + 2 + BoneWire16(lostDelta.bytes.data() + sizeof(NpcsHeader)) + 2;
    assert(bonewire::ReadPose32(lostDelta.bytes.data() + boneStart + 2) != 0);
    fakeTime += 33; streamRecord.sent = fakeTime; ++streamBones[0].values[9];
    transmissions.clear();
    SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33);
    auto survivingDelta = transmissions;
    clientReceive(survivingDelta);
    assert(appliedBones[0].values[9] == streamBones[0].values[9]);
    const unsigned afterSurviving = applied;
    isHost = false;
    Deliver(&fakePeer, lostDelta);
    assert(applied == afterSurviving);
    decodedPoses.clear();
    isHost = true;
    fakeTime += 33; streamRecord.sent = fakeTime; ++streamBones[0].values[9];
    transmissions.clear();
    SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33);
    auto missingReference = transmissions;
    clientReceive(missingReference);
    assert(bandwidth.poseReferenceMisses > 0 && applied == afterSurviving);
    fakeTime += 33; streamRecord.sent = fakeTime;
    transmissions.clear();
    SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, false, 33);
    auto recoveredKey = transmissions;
    clientReceive(recoveredKey);
    assert(applied == afterSurviving + 1 && appliedBones[0].values[9] == streamBones[0].values[9]);

    ++streamRecord.incarnation; streamRecord.flags |= 1; streamRecord.hitPoints = 0;
    fakeTime += 33; streamRecord.sent = fakeTime;
    transmissions.clear();
    assert(SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, true, 16));
    auto deathPackets = transmissions;
    assert(!deathPackets.empty());
    bool reliablePose = false;
    for (const auto& packet : deathPackets) {
        assert(packet.channel == ChannelDefinitions &&
               (packet.flags & ENET_PACKET_FLAG_RELIABLE));
        if (packet.bytes[0] == MsgControlPose ||
            packet.bytes[0] == MsgCompactPosedNpc ||
            packet.bytes[0] == MsgPosedNpc)
            reliablePose = true;
    }
    assert(reliablePose);
    clientReceive(deathPackets);
    assert(applied == afterSurviving + 2);
    failSend = true;
    assert(!SendPosedNpcTo(2, streamRecord, streamBones, 4, 0, true, 16));
    failSend = false;

    // Even a single identity bone has a valid tiny packed-key envelope.
    NpcRecord one = Record(901);
    streamBones[0].values[1] = streamBones[0].values[9] = 0;
    transmissions.clear();
    SendPosedNpcTo(2, one, streamBones, 1, 0, false, 100);
    auto oneBootstrap = transmissions;
    clientReceive(oneBootstrap);
    transmissions.clear();
    ++fakeTime; one.sent = fakeTime;
    SendPosedNpcTo(2, one, streamBones, 1, 0, false, 100);
    auto onePose = transmissions;
    clientReceive(onePose);
    assert(appliedBones.size() == 1 && appliedInterval == 100);

    // The real sender/receiver uses mode 4 and canonical decoded ACK anchors.
    NpcRecord rigidRecord = Record(902);
    pose::Bone rigidBones[80]{};
    unsigned rigidPackets = 0;
    for (unsigned frame = 0; frame < 32; ++frame) {
        fakeTime += 33; rigidRecord.sent = fakeTime;
        for (int i = 0; i < 80; ++i) {
            const double x = std::sin(frame * .21 + i * .43) * .8;
            const double y = std::sin(frame * .31 + i * .27) * .7;
            const double z = std::sin(frame * .17 + i * .19) * .6;
            const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
            const double cz = std::cos(z), sz = std::sin(z);
            pose::Transform transform{};
            transform.name = static_cast<uint32_t>(1000 + i);
            const double matrix[9] = {cz*cy, sz*cy, -sy, cz*sy*sx-sz*cx, sz*sy*sx+cz*cx,
                cy*sx, cz*sy*cx+sz*sx, sz*sy*cx-cz*sx, cy*cx};
            for (int k = 0; k < 9; ++k) transform.values[k] = static_cast<float>(matrix[k]);
            transform.values[10] = static_cast<float>(i * 1.25);
            assert(pose::Encode(transform, rigidBones[i]));
        }
        transmissions.clear();
        assert(SendPosedNpcTo(2, rigidRecord, rigidBones, 80, 0, false, 33));
        auto packets = transmissions;
        for (auto const& packet : packets) if (packet.bytes[0] == MsgControlPose) {
            const size_t start = sizeof(NpcsHeader) + 2 +
                BoneWire16(packet.bytes.data() + sizeof(NpcsHeader)) + 2;
            if (packet.bytes[start + 6] == 4) ++rigidPackets;
            if (frame == 2) {
                auto invalidInterval = packet;
                invalidInterval.bytes[start - 1] = 0;
                const unsigned before = applied;
                isHost = false;
                Deliver(&fakePeer, invalidInterval);
                assert(applied == before);
                isHost = true;
            }
        }
        if (frame % 7 == 6) continue;
        clientReceive(packets);
        assert(appliedBones.size() == 80);
        for (int i = 0; i < 80; ++i)
            for (int k = 0; k < 12; ++k)
                assert(std::abs(int(appliedBones[i].values[k]) - rigidBones[i].values[k]) <= (k < 9 ? 2 : 0));
    }
    assert(rigidPackets);

    // Real reliable contact handlers accept host-judged missing-life intent,
    // reject malformed intent, deduplicate, and report send failure honestly.
    unsigned contacts = 0;
    static unsigned* contactCount = nullptr;
    contactCount = &contacts;
    SetHitHandlers([](uint8_t from, uint8_t const*, int32_t delta, uint8_t const*, uint8_t const*,
                      uint8_t outcome, uint32_t seen, uint8_t reaction, uint32_t sequence,
                      uint32_t incarnation, MeleeContact const& contact) {
        assert(from == 2 && delta == 0 && outcome == HitUnjudged && !seen &&
               reaction == HitNoReaction && sequence && !incarnation &&
               contact.melee == HostJudgedMelee && contact.motion == 2082 &&
               contact.focus[0] == 8 && contact.target[0] == 9);
        ++*contactCount;
    }, nullptr);
    isHost = false;
    connected = true;
    localId = 2;
    MeleeContact contact;
    contact.melee = HostJudgedMelee;
    contact.motion = 2082;
    contact.focus[0] = 8;
    contact.target[0] = 9;
    uint8_t weapon[16] = {};
    transmissions.clear();
    assert(SendHitNpc(1, rigidRecord.guid, 0, weapon, nullptr, HitUnjudged, 0, HitNoReaction, 0, contact));
    assert(transmissions.size() == 1 && transmissions[0].channel == ChannelCombat &&
           (transmissions[0].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto intent = transmissions[0];
    auto malformed = intent;
    HitNpcMsg invalid{};
    std::memcpy(&invalid, malformed.bytes.data(), sizeof(invalid));
    invalid.delta = -1;
    std::memcpy(malformed.bytes.data(), &invalid, sizeof(invalid));
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, malformed);
    assert(contacts == 0);
    Deliver(&fakePeer, intent);
    assert(contacts == 1);
    Deliver(&fakePeer, intent);
    assert(contacts == 1);
    isHost = false;
    failSend = true;
    assert(!SendHitNpc(1, rigidRecord.guid, 0, weapon, nullptr, HitUnjudged, 0, HitNoReaction, 0, contact));
    failSend = false;
    assert(transmissions.empty());
    assert(!SendHitNpc(1, rigidRecord.guid, -1, weapon, nullptr, HitUnjudged, 0, HitNoReaction, 0, contact));
    contact.melee = PredictedMelee;
    assert(!SendHitNpc(1, rigidRecord.guid, -1, weapon, nullptr, HitUnjudged, 0, HitNoReaction, 0, contact));
    assert(transmissions.empty());

    // Protocol 79: cross-channel reordered MsgControlPose (ChannelPoses overtaking ChannelDefinitions)
    // must NOT count as npcRejected or log decode errors, must preserve PoseEssentialFlag contact edges,
    // and must accept stretched (>128 ms) wire intervals.
    isHost = true;
    localId = 1;
    fakeTime += 33;
    rigidRecord.sent = fakeTime;
    rigidRecord.contact = 1;
    const uint8_t quietWire = npclease::EncodePoseIntervalWire(1000);
    assert(npclease::DecodePoseIntervalWire(quietWire) >= 1000u);
    transmissions.clear();
    assert(SendPosedNpcTo(2, rigidRecord, rigidBones, 80, 0, true, quietWire));
    const auto delayedEssential = transmissions;
    fakeTime += 33;
    rigidRecord.sent = fakeTime;
    rigidRecord.contact = 0;
    transmissions.clear();
    assert(SendPosedNpcTo(2, rigidRecord, rigidBones, 80, 0, false, quietWire));
    const auto fasterUnreliable = transmissions;
    clientReceive(fasterUnreliable);
    const unsigned rejectedBefore = npcRejected;
    const unsigned appliedBeforeEssential = applied;
    size_t decodeErrsBefore = 0;
    for (auto const& l : logs)
        if (l.find("compact NPC control decode failed") != std::string::npos) ++decodeErrsBefore;
    clientReceive(delayedEssential);
    size_t decodeErrsAfter = 0;
    for (auto const& l : logs)
        if (l.find("compact NPC control decode failed") != std::string::npos) ++decodeErrsAfter;
    assert(npcRejected == rejectedBefore);
    assert(decodeErrsAfter == decodeErrsBefore);
    assert(applied == appliedBeforeEssential + 1 && appliedEssential && appliedInterval == quietWire);

    // Forced same-baseline resync allocates a fresh offer serial (rejecting any late pre-resync ACK)
    // and only commits the resync cursor once ID + baseline offers actually enqueue.
    ResetNpcReplication();
    NpcRecord resyncRecord = Record(903);
    transmissions.clear();
    assert(SendPosedNpcTo(2, resyncRecord, streamBones, 1, 0, false, 33));
    const auto initialBootstrap = transmissions;
    transmissions.clear();
    isHost = false;
    for (auto const& packet : initialBootstrap) Deliver(&fakePeer, packet);
    Transmission firstControlAck{};
    for (auto const& response : transmissions) {
        if (response.bytes[0] == MsgControlSignal) firstControlAck = response;
        isHost = true;
        Deliver(&fakePeer, response);
        isHost = false;
    }
    assert(!firstControlAck.bytes.empty());
    isHost = true;
    assert(controlAcknowledged[2].size() == 1);
    const uint16_t resyncControlId = controlAcknowledged[2].begin()->first;
    // Simulate client losing its control receiver and requesting a resync while host Send fails.
    controlReceiver = npcwire::Context{};
    receivedControlGeneration = 0;
    requestedControlId = 0;
    fakeTime += 33;
    resyncRecord.sent = fakeTime;
    transmissions.clear();
    assert(SendPosedNpcTo(2, resyncRecord, streamBones, 1, 0, false, 33));
    const auto compactSample = transmissions.front();
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, compactSample);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto resyncSignal = transmissions.front();
    transmissions.clear();
    isHost = true;
    failSend = true;
    Deliver(&fakePeer, resyncSignal);
    failSend = false;
    assert(controlAcknowledged[2].empty());
    // Because the offer failed to enqueue, CommitResync did not suppress a retry of the same resync.
    transmissions.clear();
    Deliver(&fakePeer, resyncSignal);
    assert(transmissions.size() >= 2 &&
           transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline);
    const auto reofferedPackets = transmissions;
    // A late copy of the pre-resync ACK (old serial) must be rejected and not satisfy the new offer.
    const unsigned rejectedBeforeStaleAck = npcRejected;
    Deliver(&fakePeer, firstControlAck);
    assert(npcRejected == rejectedBeforeStaleAck + 1);
    assert(!controlAcknowledged[2].count(resyncControlId));
    clientReceive(reofferedPackets);
    assert(controlAcknowledged[2].count(resyncControlId) == 1);

    return 0;
}
