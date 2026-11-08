// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real network handlers with an in-memory ENet send queue.
#include "../src/net.cpp"

#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <vector>

struct Transmission {
    std::vector<uint8_t> bytes;
    enet_uint32 flags;
    uint8_t channel;
    ENetPeer* peer;
};
std::vector<Transmission> transmissions;
std::vector<std::string> logs;
enet_uint32 fakeTime = 5000;
bool failSend = false;
uint64_t transportAccounts[2] = {12, 13};
uint64_t transportGenerations[2] = {};
std::vector<g3t::steam::RelayDrop> relayDrops;
struct RelayClose {
    uint64_t first, second;
    std::string reason;
};
std::vector<RelayClose> relayCloses;
struct ClientEnetSnapshot {
    unsigned state, event;
    uint32_t data, elapsedMs;
    std::string reason;
};
std::vector<ClientEnetSnapshot> clientEnetSnapshots;
g3t::steam::Connection flowSnapshot;
uint64_t localSteamAccount = 12;

extern "C" {
int enet_initialize() { return 0; }
void enet_deinitialize() {}
size_t enet_list_size(ENetList* list)
{
    size_t count = 0;
    for (ENetListNode const* node = list->sentinel.next; node != &list->sentinel; node = node->next) ++count;
    return count;
}
enet_uint32 enet_time_get() { return fakeTime; }
int enet_address_set_host_ip(ENetAddress*, const char*) { return 0; }
int enet_socket_set_option(ENetSocket, ENetSocketOption, int) { return 0; }
ENetPacket* enet_packet_create(const void* data, size_t size, enet_uint32 flags)
{
    auto* packet = new ENetPacket{};
    packet->data = new enet_uint8[size];
    if (size) std::memcpy(packet->data, data, size);
    packet->dataLength = size;
    packet->flags = flags;
    return packet;
}
void enet_packet_destroy(ENetPacket* packet) { delete[] packet->data; delete packet; }
int enet_peer_send(ENetPeer* peer, enet_uint8 channel, ENetPacket* packet)
{
    if (failSend) return -1;
    transmissions.push_back({std::vector<uint8_t>(packet->data, packet->data + packet->dataLength),
                             packet->flags, channel, peer});
    if (channel == g3t::net::ChannelNpcs) {
        auto& sequence = peer->channels[channel].outgoingUnreliableSequenceNumber;
        if (packet->flags & ENET_PACKET_FLAG_RELIABLE) sequence = 0;
        else ++sequence;
    }
    enet_packet_destroy(packet);
    return 0;
}
ENetHost* enet_host_create(const ENetAddress*, size_t, size_t, enet_uint32, enet_uint32) { return nullptr; }
void* enet_malloc(size_t size) { return std::malloc(size); }
void enet_free(void* memory) { std::free(memory); }
void enet_host_compress(ENetHost* host, ENetCompressor const* compressor) { host->compressor = *compressor; }
#include "../thirdparty/enet/compress.c"
void enet_host_destroy(ENetHost*) {}
ENetPeer* enet_host_connect(ENetHost*, const ENetAddress*, size_t, enet_uint32) { return nullptr; }
int enet_host_service(ENetHost*, ENetEvent*, enet_uint32) { return 0; }
void enet_host_flush(ENetHost*) {}
void enet_peer_disconnect(ENetPeer*, enet_uint32) {}
void enet_peer_disconnect_now(ENetPeer*, enet_uint32) {}
void enet_peer_disconnect_later(ENetPeer*, enet_uint32) {}
void enet_peer_reset(ENetPeer*) {}
void enet_peer_timeout(ENetPeer*, enet_uint32, enet_uint32, enet_uint32) {}
void enet_peer_throttle_configure(ENetPeer*, enet_uint32, enet_uint32, enet_uint32) {}
}

namespace g3t {
void Log(char const* text) { logs.emplace_back(text); }
void Logf(char const* format, ...)
{
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    logs.emplace_back(buffer);
}
int FormatV(char* out, size_t size, char const* format, va_list args)
{
    return std::vsnprintf(out, size, format, args);
}
int Format(char* out, size_t size, char const* format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = FormatV(out, size, format, args);
    va_end(args);
    return result;
}
namespace ai { bool ShuttingDown() { return false; } }
namespace movement {
Stage::Stage(char const*) : before(nullptr), started(0) {}
Stage::~Stage() { assert(before == nullptr && started == 0 && beforeCalls == 0); }
}
namespace downed {
uint8_t PlayerInteractionFlags(unsigned player)
{
    if (!player || player >= net::MaxPlayers) return 0;
    if (player == net::LocalId())
        return (net::currentLocal.flags & 1) && !(net::currentLocal.flags & 14)
            ? net::currentLocal.flags & InteractionMask : 0;
    net::Remote const& remote = net::Remotes()[player];
    return PeerInteraction(remote.connected, remote.activity.known, remote.activity.ticks,
                           remote.activity.world, net::currentLocal.world, remote.activity.flags, net::Now());
}
}
namespace steam {
TrafficCounters Traffic() { return {}; }
bool IsPeerRelay(unsigned short) { return true; }
uint64_t PeerId(unsigned short port) { return port == 12 ? transportAccounts[0] : port == 13 ? transportAccounts[1] : 0; }
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24])
{
    account = PeerId(port);
    if (!account) return false;
    strcpy_s(name, port == 12 ? "Steam Player One" : "Steam Player Two");
    return true;
}
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!PeerIdentity(port, account, name)) return false;
    generation = port == 12 ? transportGenerations[0] : transportGenerations[1];
    return true;
}
bool ClaimPeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!PeerIdentity(port, account, name, generation) || account == localSteamAccount) return false;
    relayCloses.erase(std::remove_if(relayCloses.begin(), relayCloses.end(),
                                    [=](RelayClose const& close) {
                                        return close.first == account && close.second == generation;
                                    }),
                      relayCloses.end());
    return true;
}
std::vector<RelayDrop> TakeRelayDrops()
{
    std::vector<RelayDrop> result;
    result.swap(relayDrops);
    return result;
}
void RequestHostRelayClose(uint64_t account, uint64_t generation, char const* reason)
{
    if (account && generation) relayCloses.push_back({account, generation, reason ? reason : ""});
}
uint64_t PlayerId() { return localSteamAccount; }
bool Start(bool, unsigned short) { return false; }
unsigned short RelayPort() { return 0; }
void Pump() {}
void Stop() {}
int Ping() { return -1; }
bool HostLinked() { return false; }
uint64_t HostRelayGeneration() { return 0; }
bool RequestClientDisconnect() { return false; }
void NoteClientEnetState(unsigned state, unsigned event, uint32_t data, uint32_t elapsedMs, char const* reason)
{
    clientEnetSnapshots.push_back({state, event, data, elapsedMs, reason ? reason : ""});
}
Connection Flow() { return flowSnapshot; }
char const* PlayerName() { return ""; }
char const* Error() { return ""; }
}
namespace parity {
uint64_t const* Local() { static uint64_t values[ItemCount] = {}; return values; }
char const* ItemName(int item) {
    static char const* names[] = {"Game.dll", "Script.dll", "Engine.dll", "scripts folder",
                                 "rules archives", "other archives", "options"};
    return names[item];
}
char const* OptionsText() { return ""; }
}
namespace options {
policy::Session observed;
policy::Snapshot localOptions;
unsigned adoptions = 0, restores = 0;
bool Install() { return true; }
policy::Snapshot Capture() { return localOptions; }
bool Adopt(policy::Snapshot const& s, uint32_t epoch) { ++adoptions; return observed.Adopt(localOptions, s, epoch); }
void Restore() { ++restores; observed.Restore(); }
bool WorldAnnounced() { return true; }
}
}

using namespace g3t;
using namespace g3t::net;

unsigned applied = 0;
uint8_t appliedFlags = 0;
uint8_t appliedInterval = 0;
bool appliedEssential = false;
std::vector<pose::Bone> appliedBones;
void GotPose(NpcRecord const&, pose::Bone const* bones, int count, uint8_t flags, bool essential, uint8_t interval)
{
    ++applied;
    appliedFlags = flags;
    appliedInterval = interval;
    appliedEssential = essential;
    appliedBones.assign(bones, bones + count);
}
int GotRecords(NpcRecord*, int count, uint32_t, uint32_t, uint8_t, uint32_t) { return count; }

void Deliver(ENetPeer* peer, Transmission const& transmission)
{
    ENetPacket packet{};
    packet.data = const_cast<enet_uint8*>(transmission.bytes.data());
    packet.dataLength = transmission.bytes.size();
    packet.flags = transmission.flags;
    OnReceive(peer, &packet);
}

int main()
{
    {
        void* context = enet_range_coder_create();
        assert(context);
        uint8_t input[64]{}, encoded[100]{}, flatEncoded[100]{}, restored[64]{};
        ENetBuffer flat{}, segmented[4]{};
        flat.data = input;
        flat.dataLength = sizeof(input);
        segmented[0].data = input;
        segmented[0].dataLength = 32;
        segmented[2].data = input + 32;
        segmented[2].dataLength = 32;
        const auto size = CompressDatagram(context, segmented, 4, sizeof(input), encoded, sizeof(encoded));
        const auto flatSize = enet_range_coder_compress(context, &flat, 1, sizeof(input), flatEncoded, sizeof(flatEncoded));
        assert(size && size == flatSize && !std::memcmp(encoded, flatEncoded, size));
        assert(logs.size() == 1 && logs.back().find("action=skip-empty-preserve-checkpoint") != std::string::npos);
        assert(DecompressDatagram(context, encoded, size, restored, sizeof(restored)) == sizeof(input));
        assert(!std::memcmp(input, restored, sizeof(input)));
        segmented[0].data = nullptr;
        assert(CompressDatagram(context, segmented, 4, sizeof(input), encoded, sizeof(encoded)) == 0);
        assert(logs.back().find("guard: ENet compression null payload") != std::string::npos);
        assert(DecompressDatagram(context, nullptr, 1, restored, sizeof(restored)) == 0);
        assert(logs.back().find("guard: ENet decompression missing") != std::string::npos);
        enet_range_coder_destroy(context);
        logs.clear();
    }
    ENetHost fakeHost{};
    ENetPeer fakePeers[2]{};
    ENetPeer& fakePeer = fakePeers[0];
    ENetChannel channels[2][ChannelCount]{};
    fakePeer.channels = channels[0];
    fakePeer.data = reinterpret_cast<void*>(static_cast<uintptr_t>(2));
    fakePeers[1].channels = channels[1];
    fakePeers[1].data = reinterpret_cast<void*>(static_cast<uintptr_t>(3));
    fakeHost.peers = fakePeers;
    host = &fakeHost;
    peers[2] = &fakePeer;
    fakePeer.address.port = 12;
    fakePeers[1].address.port = 13;
    peerAccounts[2] = 12;
    peerAccounts[3] = 13;
    server = &fakePeer;
    worldEpoch = 0x1234;
    target.host = fakePeer.address.host;
    isHost = true;
    clientInWorld[2] = true;
    ResetNpcReplication();
    SetNpcPoseHandler(GotPose);
    SetNpcHandlers(GotRecords, nullptr);

    constexpr int count = 80;
    pose::Bone input[count]{};
    for (int i = 0; i < count; ++i) {
        input[i].name = static_cast<uint32_t>(100 + i);
        for (int j = 0; j < 12; ++j)
            input[i].values[j] = static_cast<int16_t>(i * 31 + j * 13 - 32768);
    }
    NpcRecord record{};
    record.rotation[3] = 1.0f;
    SendPosedNpc(record, input, count, pose::Facial);
    assert(transmissions.size() == 2);
    const Transmission definition = transmissions[0], fallback = transmissions[1];
    assert(definition.bytes[0] == MsgBoneDictionary && definition.flags == ENET_PACKET_FLAG_RELIABLE);
    assert((fallback.bytes[0] == MsgCompactPosedNpc || fallback.bytes[0] == MsgControlPose) &&
           fallback.flags == ENET_PACKET_FLAG_RELIABLE);
    assert(fallback.channel == ChannelDefinitions);
    transmissions.clear();
    SendPosedNpc(record, input, count, pose::Facial);
    assert(transmissions.size() == 1 &&
           (transmissions[0].bytes[0] == MsgCompactPosedNpc ||
            transmissions[0].bytes[0] == MsgControlPose));

    bonewire::Dictionary hostLayouts = std::move(boneLayouts);
    isHost = false;
    inWorld = true;
    transmissions.clear();
    Deliver(&fakePeer, definition);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    const Transmission ack = transmissions[0];
    transmissions.clear();
    Deliver(&fakePeer, fallback);
    assert(applied == 1 && appliedFlags == pose::Facial && appliedBones.size() == count && !appliedEssential);
    bonewire::Dictionary clientLayouts = std::move(boneLayouts);

    isHost = true;
    boneLayouts = std::move(hostLayouts);
    transmissions.clear();
    Transmission unknownAck = ack;
    unknownAck.bytes[1 + 16] ^= 0x40;
    Deliver(&fakePeer, unknownAck);
    assert(boneAcknowledged[2].empty());
    Deliver(&fakePeer, ack);
    peers[3] = &fakePeers[1];
    clientInWorld[3] = true;
    SendPosedNpc(record, input, count, pose::Facial);
    assert(transmissions.size() == 3 &&
           (transmissions[0].bytes[0] == MsgCompactPosedNpc ||
            transmissions[0].bytes[0] == MsgControlPose));
    assert(transmissions[0].peer == &fakePeer);
    assert(transmissions[1].peer == &fakePeers[1] && transmissions[1].bytes[0] == MsgBoneDictionary);
    assert(transmissions[2].peer == &fakePeers[1] &&
           (transmissions[2].bytes[0] == MsgCompactPosedNpc ||
            transmissions[2].bytes[0] == MsgControlPose));
    Drop(3);
    const Transmission compact = transmissions[0];
    hostLayouts = std::move(boneLayouts);

    // Simulate a lost definition/rejoin and a cross-channel early compact pose.
    isHost = false;
    boneLayouts = std::move(clientLayouts);
    SetInWorld(2, false);
    assert(!boneLayouts.HasLayout(1) && receivedBoneGeneration == 0);
    SetInWorld(2, true);
    transmissions.clear();
    Deliver(&fakePeer, compact);
    assert(applied == 1);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneRequest);
    const Transmission request = transmissions[0];
    assert(request.bytes == std::vector<uint8_t>({MsgBoneRequest, 0x34, 0x12, 0, 0, 1, 0}));
    transmissions.clear();
    Deliver(&fakePeer, compact);
    assert(applied == 1 && transmissions.empty()); // request throttled

    isHost = true;
    boneLayouts = std::move(hostLayouts);
    Deliver(&fakePeer, request);
    assert(transmissions.size() == 1 && transmissions[0].bytes == definition.bytes);
    const Transmission resent = transmissions[0];
    hostLayouts = std::move(boneLayouts);
    isHost = false;
    boneLayouts.Clear();
    transmissions.clear();
    Deliver(&fakePeer, resent);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    transmissions.clear();
    Deliver(&fakePeer, compact);
    assert(applied == 2 && appliedFlags == pose::Facial && appliedBones.size() == count);
    for (int i = 0; i < count; ++i) {
        assert(appliedBones[i].name == input[i].name);
        for (int j = 0; j < 12; ++j) assert(appliedBones[i].values[j] == input[i].values[j]);
    }
    Transmission later = compact;
    uint32_t laterPublication = npcPublication + 1;
    std::memcpy(later.bytes.data() + offsetof(NpcsHeader, publication), &laterPublication, sizeof(laterPublication));
    Transmission malformed = later;
    malformed.bytes.pop_back();
    Deliver(&fakePeer, malformed);
    assert(applied == 2);
    Transmission conflict = resent;
    conflict.bytes.back() ^= 1;
    Deliver(&fakePeer, conflict);
    assert(applied == 2);
    Transmission badFuture = resent;
    badFuture.bytes[1 + 8] += 1;
    std::memset(badFuture.bytes.data() + 1 + bonewire::HeaderBytes + bonewire::CountBytes, 0, 4);
    const uint64_t previousGeneration = receivedBoneGeneration;
    Deliver(&fakePeer, badFuture);
    assert(receivedBoneGeneration == previousGeneration);
    Deliver(&fakePeer, later);
    assert(applied == 3 && appliedBones[1].name == input[1].name);

    const uint64_t oldClientGeneration = receivedBoneGeneration;
    clientLayouts = std::move(boneLayouts);
    isHost = true;
    boneLayouts = std::move(hostLayouts);
    SetInWorld(2, false);
    SetInWorld(2, true);
    // Rejoining rotates the shared dictionaries, invalidating old ACKs.
    assert(!boneLayouts.HasLayout(1) && boneAcknowledged[2].empty() && boneOffered[2].empty());

    // Same epoch, new generation: an early new-ID pose must request by epoch
    // alone, and stale IDs/acks from before reset must not be accepted.
    transmissions.clear();
    SendPosedNpc(record, input, count, pose::Facial);
    assert(transmissions.size() == 2);
    const Transmission newDefinition = transmissions[0];
    assert(newDefinition.bytes[1 + 16] == 2 && newDefinition.bytes[1 + 17] == 0);
    transmissions.clear();
    Deliver(&fakePeer, ack);
    Deliver(&fakePeer, request);
    assert(transmissions.empty() && boneAcknowledged[2].empty());
    boneAcknowledged[2].insert(2); // Model an unreliable pose arriving before its reliable definition.
    SendPosedNpc(record, input, count, pose::Facial);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgCompactPosedNpc);
    const Transmission newCompact = transmissions[0];
    hostLayouts = std::move(boneLayouts);

    isHost = false;
    boneLayouts = std::move(clientLayouts);
    receivedBoneGeneration = oldClientGeneration;
    transmissions.clear();
    Deliver(&fakePeer, newCompact);
    assert(applied == 3 && transmissions.size() == 1);
    const Transmission newRequest = transmissions[0];
    assert(newRequest.bytes == std::vector<uint8_t>({MsgBoneRequest, 0x34, 0x12, 0, 0, 2, 0}));
    clientLayouts = std::move(boneLayouts);
    isHost = true;
    boneLayouts = std::move(hostLayouts);
    transmissions.clear();
    Deliver(&fakePeer, newRequest);
    assert(transmissions.size() == 1 && transmissions[0].bytes == newDefinition.bytes);
    hostLayouts = std::move(boneLayouts);
    isHost = false;
    boneLayouts = std::move(clientLayouts);
    transmissions.clear();
    Deliver(&fakePeer, newDefinition);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneAck);
    transmissions.clear();
    Deliver(&fakePeer, definition);
    assert(transmissions.empty());
    Deliver(&fakePeer, compact);
    assert(applied == 3 && transmissions.size() == 1 && transmissions[0].bytes[0] == MsgBoneRequest);
    transmissions.clear();
    Deliver(&fakePeer, newCompact);
    assert(applied == 4 && appliedBones.size() == count);

    isHost = true;
    boneLayouts = std::move(hostLayouts);
    nextBoneLayout = UINT16_MAX;
    transmissions.clear();
    SendPosedNpc(record, input, count, 0);
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgCompactPosedNpc);
    input[0].name = 9999;
    transmissions.clear();
    SendPosedNpc(record, input, count, 0);
    assert(boneExhaustionLogged && transmissions.size() == 1 && transmissions[0].bytes[0] == MsgPosedNpc);
    const auto rawOrdinary = transmissions[0];
    assert(!(rawOrdinary.flags & ENET_PACKET_FLAG_RELIABLE));
    transmissions.clear();
    assert(SendPosedNpcTo(2, record, input, count, pose::Facial, true, 16));
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgPosedNpc);
    const auto rawEssential = transmissions[0];
    assert(rawEssential.channel == ChannelDefinitions && (rawEssential.flags & ENET_PACKET_FLAG_RELIABLE));
    isHost = false;
    transmissions.clear();
    Deliver(&fakePeer, rawOrdinary);
    assert(!appliedEssential && appliedFlags == 0);
    Deliver(&fakePeer, rawEssential);
    assert(appliedEssential && appliedFlags == pose::Facial);
    isHost = true;
    input[0].name = 100;
    transmissions.clear();
    assert(SendPosedNpcTo(2, record, input, count, pose::Facial, true, 16));
    assert(transmissions.size() == 1 && transmissions[0].bytes[0] == MsgCompactPosedNpc);
    const auto compactEssential = transmissions[0];
    assert(compactEssential.bytes[sizeof(NpcsHeader) + sizeof(NpcRecord)] & PoseEssentialFlag);
    isHost = false;
    transmissions.clear();
    Deliver(&fakePeer, compactEssential);
    assert(appliedEssential && appliedFlags == pose::Facial);
    isHost = true;
    input[0].name = 9999;
    const unsigned failedBefore = poseSendFailed;
    const unsigned logsBefore = boneFallbackFailureLogs;
    failSend = true;
    SendPosedNpc(record, input, count, 0);
    failSend = false;
    assert(poseSendFailed == failedBefore + 1 && boneFallbackFailureLogs == logsBefore + 1);
    input[0].name = 0;
    for (int i = 0; i < 64; ++i) SendPosedNpc(record, input, count, 0);
    assert(boneInvalidLogs == 16);
    auto logged = [](char const* text) {
        return std::any_of(logs.begin(), logs.end(), [text](std::string const& line) {
            return line.find(text) != std::string::npos;
        });
    };
    assert(logged("new bone layout=1 bones=80 definition_bytes=341"));
    assert(logged("pose=full bones=80"));
    assert(logged("pose=compact bones=80"));
    assert(logged("bone layout IDs exhausted"));
    assert(logged("full-hash pose fallback failed"));
    return 0;
}
