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
    assert(transmissions[2].bytes[0] == MsgNpcs);
    const auto id = transmissions[0], baseline = transmissions[1];
    transmissions.clear();
    isHost = false;
    Deliver(&peer, id);
    Deliver(&peer, baseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto ack = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&peer, ack);
    assert(transmissions.empty());
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

    // An actual protocol-72 hello receives a refusal, never a welcome.
    Hello oldHello{};
    oldHello.type = MsgHello;
    oldHello.version = 72;
    std::memcpy(oldHello.name, "old client", 11);
    Transmission oldHandshake;
    oldHandshake.bytes.resize(sizeof(oldHello));
    std::memcpy(oldHandshake.bytes.data(), &oldHello, sizeof(oldHello));
    oldHandshake.flags = ENET_PACKET_FLAG_RELIABLE;
    fakePeers[1].data = nullptr;
    transmissions.clear();
    Deliver(&fakePeers[1], oldHandshake);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgRefused);
    RefusedMsg refusal{};
    std::memcpy(&refusal, transmissions[0].bytes.data(), sizeof(refusal));
    assert(std::strstr(refusal.reason, "protocol 72, host 73") != nullptr);
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

    // Reordering a mixed batch must not discard the still-new NPC merely
    // because another NPC already arrived in a newer packet.
    isHost = true;
    transmissions.clear();
    SendNpcs(records, 2, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto mixedOlder = transmissions[0];
    transmissions.clear();
    SendNpcs(records, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto oneNewer = transmissions[0];
    transmissions.clear();
    receivedRecords.clear();
    isHost = false;
    Deliver(&fakePeer, oneNewer);
    Deliver(&fakePeer, mixedOlder);
    assert(receivedRecords.size() == 2);
    assert(receivedRecords[0].guid[0] == records[0].guid[0] &&
           receivedRecords[1].guid[0] == records[1].guid[0]);

    // A changed immutable field starts a fresh baseline; fallback stays exact until its ACK.
    isHost = true;
    transmissions.clear();
    records[0].hitPoints = 60;
    SendNpcs(records, 1, false);
    assert(transmissions.size() == 3 && transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline && transmissions[2].bytes[0] == MsgNpcs);
    const auto newId = transmissions[0], newBaseline = transmissions[1], changedFull = transmissions[2];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, changedFull);
    assert(receivedRecords.back().hitPoints == 60);
    Deliver(&fakePeer, newId);
    Deliver(&fakePeer, newBaseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto newAck = transmissions[0];
    transmissions.clear();
    isHost = true;
    Deliver(&fakePeer, newAck);
    const auto key = NpcKey(records[0].guid);
    const uint16_t token = controlStates.at(key).nextBaseline;

    // Extended active weight uses exact full fallback without churning baselines.
    records[0].motions[0].weight = 2.0f;
    for (int i = 0; i < 10; ++i) {
        transmissions.clear();
        SendNpcs(records, 1, false);
        assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgNpcs);
        assert(controlStates.at(key).nextBaseline == token);
    }
    records[0].motions[0].weight = 0.7f;
    records[0].position[0] += 40000.0f;
    transmissions.clear();
    SendNpcs(records, 1, false);
    assert(controlStates.at(key).nextBaseline == token + 1);
    assert(transmissions.size() == 3 && transmissions[2].bytes[0] == MsgNpcs);

    // Client rejoin loses its tables: the compact packet requests the ID and
    // the host resends the current generation's ID and baseline reliably.
    NpcRecord fresh = records[1];
    transmissions.clear();
    SendNpcs(&fresh, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);
    const auto early = transmissions[0];
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
    assert(transmissions.size() == 2 && transmissions[0].bytes[0] == MsgControlId &&
           transmissions[1].bytes[0] == MsgControlBaseline);
    const auto resentId = transmissions[0], resentBaseline = transmissions[1];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, resentId);
    Deliver(&fakePeer, resentBaseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
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
    assert(receivedRecords.size() == beforeUnknown + 1);

    // Reliable life transitions remain full records and precede reliable release.
    isHost = true;
    fresh.flags = 1;
    fresh.hitPoints = 0;
    transmissions.clear();
    SendNpcs(&fresh, 1, true);
    SendNpcRelease(fresh.guid, 1);
    assert(transmissions.size() == 4 && transmissions[2].bytes[0] == MsgNpcs &&
           (transmissions[2].flags & ENET_PACKET_FLAG_RELIABLE));
    assert(transmissions[3].bytes[0] == MsgNpcRelease && (transmissions[3].flags & ENET_PACKET_FLAG_RELIABLE));
    const auto terminal = transmissions[2], release = transmissions[3];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, terminal);
    assert(receivedRecords.back().flags == 1 && receivedRecords.back().hitPoints == 0);
    Deliver(&fakePeer, release);
    assert(releasedRecords == 1);
    const size_t terminalCount = receivedRecords.size();
    Deliver(&fakePeer, recovered);
    assert(receivedRecords.size() == terminalCount);

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
           transmissions[1].bytes[0] == MsgPosedNpc);
    const auto boneDefinition = transmissions[0], fullPose = transmissions[1];
    bonewire::Dictionary hostBones = std::move(boneLayouts);
    boneLayouts.Clear();
    receivedBoneGeneration = 0;
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, boneDefinition);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    const auto boneAck = transmissions[0];
    transmissions.clear();
    Deliver(&fakePeer, fullPose);
    assert(applied == 1 && appliedBones.size() == 80);
    bonewire::Dictionary clientBones = std::move(boneLayouts);
    boneLayouts = std::move(hostBones);
    isHost = true;
    Deliver(&fakePeer, boneAck);
    transmissions.clear();
    SendPosedNpc(posed, poseBones, 80, pose::Facial);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlPose);
    const auto compactPose = transmissions[0];
    assert(compactPose.bytes.size() == sizeof(NpcsHeader) + 2 + sizeof(npcwire::WireBatchHeader) +
           sizeof(npcwire::WireControl) + sizeof(npcwire::WireMotionTick) + sizeof(BonePosePrefix) + 80 * 24);
    assert(fullPose.bytes.size() - compactPose.bytes.size() == 861);
    hostBones = std::move(boneLayouts);
    boneLayouts = std::move(clientBones);
    isHost = false;
    Deliver(&fakePeer, compactPose);
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
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlPose);
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
    bool readyBatch = false, newIdForPeer = false, newBaselineForPeer = false, fullForPeer = false;
    for (auto const& packet : transmissions) {
        if (packet.peer == &fakePeer && packet.bytes[0] == MsgControlBatch) readyBatch = true;
        if (packet.peer == &fakePeers[1]) {
            newIdForPeer |= packet.bytes[0] == MsgControlId;
            newBaselineForPeer |= packet.bytes[0] == MsgControlBaseline;
            fullForPeer |= packet.bytes[0] == MsgNpcs;
        }
    }
    assert(readyBatch && newIdForPeer && newBaselineForPeer && fullForPeer);
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
    assert(transmissions.size() == 2);
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
           transmissions[1].bytes[0] == MsgControlBaseline && transmissions[2].bytes[0] == MsgNpcs);
    const auto nextId = transmissions[0], nextBaseline = transmissions[1];
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
    assert(transmissions.empty() && receivedRecords.size() == beforeMissing + 1);
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
    const size_t expectedLegacyPoseBytes[] = {57080, 142700, 285400};
    const size_t expectedCompactPoseBytes[] = {39860, 99650, 199300};
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
        assert(counts[c] * fullPose.bytes.size() == expectedLegacyPoseBytes[c]);
        assert(counts[c] * compactPose.bytes.size() == expectedCompactPoseBytes[c]);
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
    assert(transmissions.size() == 3 && transmissions[2].bytes[0] == MsgNpcs);
    Deliver(&fakePeer, nextAck);
    assert(controlAcknowledged[2].empty());

    // A round trip longer than the retry interval must still acknowledge the
    // original offer; retries of that offer keep its serial.
    const auto slowId = transmissions[0], slowBaseline = transmissions[1];
    transmissions.clear();
    isHost = false;
    Deliver(&fakePeer, slowId);
    Deliver(&fakePeer, slowBaseline);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlSignal);
    const auto slowAck = transmissions[0];
    transmissions.clear();
    isHost = true;
    fakeTime += 1001;
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 3);
    assert(transmissions[0].bytes == slowId.bytes && transmissions[1].bytes == slowBaseline.bytes);
    transmissions.clear();
    Deliver(&fakePeer, slowAck);
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgControlBatch);

    // Exhaustion fails closed, without recycling a control ID or token.
    const auto stateKey = NpcKey(posed.guid);
    controlStates.at(stateKey).nextBaseline = UINT16_MAX;
    posed.hitPoints--;
    transmissions.clear();
    SendNpcs(&posed, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgNpcs);
    nextControlId = UINT16_MAX;
    NpcRecord exhausted = Record(101);
    transmissions.clear();
    SendNpcs(&exhausted, 1, false);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgNpcs && controlIdExhaustedLogged);
    return 0;
}
