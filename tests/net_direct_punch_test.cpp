// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests for Section 2 Direct UDP hole-punch security, relay fallback, and MsgGoods validation.
#include "../src/net_internal.h"
#include "../src/downed.h"
#include "../src/downed_policy.h"
#include "../src/movement.h"
#include "../src/saving_policy.h"

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// Forward-declare anonymous-namespace test hooks via thin wrappers in net.cpp?
// No: we drive public g3t::net::Tick / Start / Reconnect / Stop and g3t::net::internal::* directly!

namespace {
struct SentDatagram {
    ENetAddress to{};
    std::vector<uint8_t> bytes;
};
struct QueuedPacket {
    ENetPeer* peer = nullptr;
    uint8_t channel = 0;
    enet_uint32 flags = 0;
    std::vector<uint8_t> bytes;
};

enet_uint32 fakeNow = 10000;
std::vector<SentDatagram> socketSent;
std::vector<QueuedPacket> peerSent;
std::deque<ENetEvent> pendingEvents;
std::vector<ENetAddress> connectAddresses;
std::vector<enet_uint32> connectTags;
bool failDirectConnect = false;
ENetHost fakeHostStorage{};
ENetPeer fakePeersStorage[g3t::net::MaxPlayers]{};
ENetChannel fakeChannelsStorage[g3t::net::MaxPlayers][g3t::net::ChannelCount]{};

uint64_t fakeHostGeneration = 0;
std::vector<g3t::steam::DirectOffer> fakeDirectOffers;
struct RegisteredDirectEndpoint {
    uint64_t account = 0;
    uint64_t generation = 0;
    uint32_t host = 0;
    uint16_t port = 0;
    uint32_t expectedTag = 0;
};
std::vector<RegisteredDirectEndpoint> registeredEndpoints;
} // namespace

extern "C" {
int enet_initialize() { return 0; }
void enet_deinitialize() {}
size_t enet_list_size(ENetList* list)
{
    size_t count = 0;
    for (ENetListNode const* node = list->sentinel.next; node != &list->sentinel; node = node->next) ++count;
    return count;
}
enet_uint32 enet_time_get() { return fakeNow; }
int enet_address_set_host_ip(ENetAddress* address, const char*)
{
    if (!address) return -1;
    address->host = 0x0100007fu; // 127.0.0.1
    return 0;
}
int enet_socket_set_option(ENetSocket, ENetSocketOption, int) { return 0; }
int enet_socket_get_address(ENetSocket, ENetAddress* address)
{
    if (!address) return -1;
    address->host = 0;
    address->port = 27777;
    return 0;
}
int enet_socket_send(ENetSocket, const ENetAddress* to, const ENetBuffer* buffers, size_t count)
{
    if (!to || !buffers) return -1;
    std::vector<uint8_t> payload;
    for (size_t i = 0; i < count; ++i) {
        auto const* ptr = static_cast<uint8_t const*>(buffers[i].data);
        payload.insert(payload.end(), ptr, ptr + buffers[i].dataLength);
    }
    socketSent.push_back({*to, std::move(payload)});
    return static_cast<int>(socketSent.back().bytes.size());
}
ENetPacket* enet_packet_create(const void* data, size_t size, enet_uint32 flags)
{
    auto* packet = new ENetPacket{};
    packet->data = new enet_uint8[size ? size : 1];
    if (size && data) std::memcpy(packet->data, data, size);
    packet->dataLength = size;
    packet->flags = flags;
    return packet;
}
void enet_packet_destroy(ENetPacket* packet)
{
    if (!packet) return;
    delete[] packet->data;
    delete packet;
}
int enet_peer_send(ENetPeer* peer, enet_uint8 channel, ENetPacket* packet)
{
    peerSent.push_back({peer, channel, packet->flags,
                        std::vector<uint8_t>(packet->data, packet->data + packet->dataLength)});
    enet_packet_destroy(packet);
    return 0;
}
ENetHost* enet_host_create(const ENetAddress* address, size_t peerCount, size_t, enet_uint32, enet_uint32)
{
    fakeHostStorage = ENetHost{};
    if (address) fakeHostStorage.address = *address;
    fakeHostStorage.socket = 1;
    fakeHostStorage.mtu = g3t::net::Mtu;
    fakeHostStorage.peers = fakePeersStorage;
    fakeHostStorage.peerCount = peerCount;
    auto initList = [](ENetList& list) {
        list.sentinel.next = &list.sentinel;
        list.sentinel.previous = &list.sentinel;
    };
    for (size_t i = 0; i < g3t::net::MaxPlayers; ++i) {
        fakePeersStorage[i] = ENetPeer{};
        for (size_t c = 0; c < g3t::net::ChannelCount; ++c) {
            fakeChannelsStorage[i][c] = ENetChannel{};
            initList(fakeChannelsStorage[i][c].incomingReliableCommands);
            initList(fakeChannelsStorage[i][c].incomingUnreliableCommands);
        }
        fakePeersStorage[i].channels = fakeChannelsStorage[i];
        fakePeersStorage[i].channelCount = g3t::net::ChannelCount;
        fakePeersStorage[i].mtu = g3t::net::Mtu;
        fakePeersStorage[i].packetThrottle = ENET_PEER_DEFAULT_PACKET_THROTTLE;
        initList(fakePeersStorage[i].acknowledgements);
        initList(fakePeersStorage[i].sentReliableCommands);
        initList(fakePeersStorage[i].outgoingCommands);
        initList(fakePeersStorage[i].outgoingSendReliableCommands);
        initList(fakePeersStorage[i].dispatchedCommands);
    }
    return &fakeHostStorage;
}
void enet_host_compress(ENetHost* h, ENetCompressor const* c) { if (h && c) h->compressor = *c; }
void* enet_malloc(size_t size) { return std::malloc(size); }
void enet_free(void* memory) { std::free(memory); }
#include "../thirdparty/enet/compress.c"
void enet_host_destroy(ENetHost*) {}
ENetPeer* enet_host_connect(ENetHost*, const ENetAddress* address, size_t, enet_uint32 data)
{
    connectAddresses.push_back(*address);
    connectTags.push_back(data);
    if (failDirectConnect && address->host != 0x0100007fu) return nullptr;
    fakePeersStorage[0].address = *address;
    fakePeersStorage[0].state = ENET_PEER_STATE_CONNECTING;
    return &fakePeersStorage[0];
}
int enet_host_service(ENetHost*, ENetEvent* event, enet_uint32)
{
    if (pendingEvents.empty()) return 0;
    *event = pendingEvents.front();
    pendingEvents.pop_front();
    return 1;
}
void enet_host_flush(ENetHost*) {}
void enet_peer_disconnect(ENetPeer* peer, enet_uint32) { if (peer) peer->state = ENET_PEER_STATE_DISCONNECTING; }
void enet_peer_disconnect_now(ENetPeer* peer, enet_uint32) { if (peer) peer->state = ENET_PEER_STATE_DISCONNECTED; }
void enet_peer_disconnect_later(ENetPeer* peer, enet_uint32) { if (peer) peer->state = ENET_PEER_STATE_DISCONNECT_LATER; }
void enet_peer_reset(ENetPeer* peer) { if (peer) peer->state = ENET_PEER_STATE_DISCONNECTED; }
void enet_peer_timeout(ENetPeer*, enet_uint32, enet_uint32, enet_uint32) {}
void enet_peer_throttle_configure(ENetPeer*, enet_uint32, enet_uint32, enet_uint32) {}
}

namespace g3t {
void Log(char const*) {}
void Logf(char const*, ...) {}
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
uint8_t PlayerInteractionFlags(unsigned) { return 0; }
}
namespace steam {
TrafficCounters Traffic() { return {}; }
bool IsPeerRelay(unsigned short port) { return port == 30001 || port == 30002; }
uint64_t PeerId(unsigned short port) { return port == 30001 ? 2001ull : port == 30002 ? 2002ull : 0ull; }
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24])
{
    account = PeerId(port);
    if (!account) return false;
    strcpy_s(name, port == 30001 ? "RelayTwo" : "RelayThree");
    return true;
}
bool PeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    if (!PeerIdentity(port, account, name)) return false;
    generation = 77;
    return true;
}
bool ClaimPeerIdentity(unsigned short port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    return PeerIdentity(port, account, name, generation);
}
std::vector<RelayDrop> TakeRelayDrops() { return {}; }
std::vector<DirectOffer> DirectOffers() { return fakeDirectOffers; }
void RegisterDirectPeerEndpoint(uint64_t account, uint64_t generation, uint32_t hostIp, uint16_t port)
{
    if (!hostIp || hostIp == 0xffffffffu || !port) return;
    const uint8_t firstOctet = static_cast<uint8_t>(hostIp & 0xffu);
    if (firstOctet == 0 || firstOctet >= 224) return;
    for (auto const& ep : registeredEndpoints) {
        if (ep.account == account && ep.generation == generation) return;
    }
    registeredEndpoints.push_back({account, generation, hostIp, port, 0});
}
bool DirectPeerIdentity(uint32_t hostIp, uint16_t port, uint64_t& account, char (&name)[24], uint64_t& generation)
{
    for (auto const& ep : registeredEndpoints) {
        if (ep.host == hostIp && ep.port == port) {
            account = ep.account;
            generation = ep.generation;
            strcpy_s(name, "DirectPeer");
            return true;
        }
    }
    return false;
}
bool ClaimDirectPeerIdentity(uint32_t hostIp, uint16_t port, uint32_t connectTag,
                             uint64_t& account, char (&name)[24], uint64_t& generation)
{
    for (auto const& ep : registeredEndpoints) {
        if (ep.host == hostIp && ep.port == port && (!ep.expectedTag || ep.expectedTag == connectTag)) {
            account = ep.account;
            generation = ep.generation;
            strcpy_s(name, "DirectPeer");
            return true;
        }
    }
    return false;
}
void RequestHostRelayClose(uint64_t, uint64_t, char const*) {}
uint64_t PlayerId() { return g3t::net::IsHost() ? 1001ull : 2001ull; }
bool Start(bool, unsigned short) { return true; }
unsigned short RelayPort() { return 29999; }
void Pump() {}
void Stop() {}
int Ping() { return 12; }
bool HostLinked() { return fakeHostGeneration != 0; }
uint64_t HostRelayGeneration() { return fakeHostGeneration; }
bool RequestClientDisconnect() { return false; }
void NoteClientEnetState(unsigned, unsigned, uint32_t, uint32_t, char const*) {}
Connection Flow() { return {}; }
char const* PlayerName() { return g3t::net::IsHost() ? "HostHero" : "ClientHero"; }
char const* Error() { return ""; }
} // namespace steam
namespace parity {
uint64_t const* Local() { static uint64_t values[ItemCount] = {}; return values; }
char const* ItemName(int) { return "Game.dll"; }
char const* OptionsText() { return ""; }
} // namespace parity
namespace options {
bool Install() { return true; }
policy::Snapshot Capture() { return {}; }
bool Adopt(policy::Snapshot const&, uint32_t) { return true; }
void Restore() {}
bool WorldAnnounced() { return true; }
} // namespace options
} // namespace g3t

namespace {
std::vector<uint8_t> MakePunchPacket(uint8_t const cookie[16], uint8_t flags, uint8_t role)
{
    std::vector<uint8_t> pkt(g3t::net::internal::DirectPunchBytes, 0);
    std::memcpy(pkt.data(), "G3TPUNCH", 8);
    std::memcpy(pkt.data() + 8, cookie, 16);
    pkt[24] = flags;
    pkt[25] = role;
    return pkt;
}

int InjectDatagram(ENetAddress from, std::vector<uint8_t>& bytes)
{
    fakeHostStorage.receivedAddress = from;
    fakeHostStorage.receivedData = bytes.data();
    fakeHostStorage.receivedDataLength = bytes.size();
    return g3t::net::internal::InterceptTransportPacket(&fakeHostStorage, nullptr);
}

void TestDirectPunchRoleValidationAndEndpointLock()
{
    using namespace g3t::net;
    using namespace g3t::net::internal;

    fakeNow = 10000;
    registeredEndpoints.clear();
    socketSent.clear();
    fakeDirectOffers.clear();

    g3t::steam::DirectOffer offer{};
    offer.account = 2001ull;
    offer.generation = 77ull;
    for (uint8_t i = 0; i < 16; ++i) offer.cookie[i] = static_cast<uint8_t>(0xA0u + i);
    offer.candidateCount = 1;
    offer.candidates[0] = {0x0201A8C0u, 41000};
    offer.ready = true;
    fakeDirectOffers.push_back(offer);

    assert(Start(true));
    SyncDirectSessions(fakeNow);
    assert(directSessions[0].account == 2001ull);
    assert(!directSessions[0].inboundVerified);

    // 1) Self-reflected / spoofed host-role (role == 1) packet arriving at host MUST be ignored.
    auto selfReflected = MakePunchPacket(offer.cookie, 0x01u, 1u);
    assert(InjectDatagram({0x0909A8C0u, 55555}, selfReflected) == 1);
    assert(!directSessions[0].inboundVerified);
    assert(registeredEndpoints.empty());

    // 1b) Invalid / loopback / link-local / multicast / broadcast / relay-port source addresses MUST be rejected even with a valid client-role probe.
    socketSent.clear();
    auto clientProbe = MakePunchPacket(offer.cookie, 0x03u, 2u);
    for (ENetAddress badAddr : {ENetAddress{0x0100007fu, 41000}, ENetAddress{0x0100007fu, 30001},
                                ENetAddress{0x0201fea9u, 41000}, ENetAddress{0u, 41000},
                                ENetAddress{0xffffffffu, 41000}, ENetAddress{0x010000e0u, 41000},
                                ENetAddress{0x0201A8C0u, 0}}) {
        assert(InjectDatagram(badAddr, clientProbe) == 1);
        assert(!directSessions[0].inboundVerified);
        assert(socketSent.empty());
        assert(registeredEndpoints.empty());
    }

    // 2) Legitimate client-role (role == 2) packet verifies the endpoint and sends ACK (0x02).
    const ENetAddress legitimateAddr{0x0201A8C0u, 41000};
    assert(InjectDatagram(legitimateAddr, clientProbe) == 1);
    assert(directSessions[0].inboundVerified);
    assert(directSessions[0].bidirectionalVerified);
    assert(directSessions[0].verifiedAddress.host == legitimateAddr.host);
    assert(directSessions[0].verifiedAddress.port == legitimateAddr.port);
    assert(socketSent.size() == 1 && socketSent[0].bytes[24] == 0x02u && socketSent[0].bytes[25] == 1u);

    // 2b) Pre-connect first-endpoint pinning: before ClaimAuthenticatedPeerIdentity runs, a replayed probe from
    // another address MUST NOT migrate verifiedAddress away from the first verified endpoint!
    auto preConnectHijack = MakePunchPacket(offer.cookie, 0x03u, 2u);
    assert(InjectDatagram({0x5555A8C0u, 51111}, preConnectHijack) == 1);
    assert(directSessions[0].verifiedAddress.host == legitimateAddr.host);
    assert(directSessions[0].verifiedAddress.port == legitimateAddr.port);

    // Non-punch ENet traffic from verifiedAddress passes intercept (0), while spoofed IP is dropped (1).
    std::vector<uint8_t> enetPayload = {0x01, 0x02, 0x03, 0x04};
    assert(InjectDatagram(legitimateAddr, enetPayload) == 0);
    assert(InjectDatagram({0x6363A8C0u, 49999}, enetPayload) == 1);

    // 3) ClaimAuthenticatedPeerIdentity locks the direct endpoint against post-connect hijacking.
    fakePeersStorage[0].address = legitimateAddr;
    peerConnectData[0] = DirectCookieTag(offer.cookie);
    registeredEndpoints[0].expectedTag = DirectCookieTag(offer.cookie);
    uint64_t claimedAccount = 0, claimedGen = 0;
    char claimedName[24] = {};
    assert(ClaimAuthenticatedPeerIdentity(&fakePeersStorage[0], claimedAccount, claimedName, claimedGen));
    assert(claimedAccount == 2001ull && claimedGen == 77ull);
    assert(directSessions[0].endpointLocked);

    // Attacker replays valid cookie from a new address after lock: MUST NOT migrate verifiedAddress!
    const ENetAddress hijackAddr{0x7777A8C0u, 60001};
    auto hijackProbe = MakePunchPacket(offer.cookie, 0x03u, 2u);
    assert(InjectDatagram(hijackAddr, hijackProbe) == 1);
    assert(directSessions[0].verifiedAddress.host == legitimateAddr.host);
    assert(directSessions[0].verifiedAddress.port == legitimateAddr.port);
    assert(InjectDatagram(hijackAddr, enetPayload) == 1);
    assert(InjectDatagram(legitimateAddr, enetPayload) == 0);

    // 3b) Direct peer replacing an older relay slot in HandleHello keeps endpointLocked set after LeavePeer drops the old peer.
    ENetPeer& oldRelayPeer = fakePeersStorage[1];
    oldRelayPeer.state = ENET_PEER_STATE_CONNECTED;
    oldRelayPeer.address = {0x0100007fu, 30001};
    oldRelayPeer.data = reinterpret_cast<void*>(uintptr_t(2));
    peers[2] = &oldRelayPeer;
    peerAccounts[2] = 2001ull;
    peerRelayGenerations[2] = 77ull;
    fakePeersStorage[0].state = ENET_PEER_STATE_CONNECTED;
    fakePeersStorage[0].data = nullptr;
    HandleHello(&fakePeersStorage[0], true, g3t::saving::Protocol, "DirectPeer", g3t::parity::Local());
    assert(peers[2] == &fakePeersStorage[0]);
    assert(directSessions[0].endpointLocked);

    // 4) LeavePeer unlocks the session endpoint and broadcasts MsgLeave on ChannelState.
    peerSent.clear();
    peers[3] = &fakePeersStorage[2];
    fakePeersStorage[2].state = ENET_PEER_STATE_CONNECTED;
    fakePeersStorage[2].data = reinterpret_cast<void*>(uintptr_t(3));
    LeavePeer(&fakePeersStorage[0], "test-leave", true);
    assert(!directSessions[0].endpointLocked);
    bool leaveOnStateChannel = false;
    for (auto const& pkt : peerSent) {
        if (!pkt.bytes.empty() && pkt.bytes[0] == MsgLeave) {
            assert(pkt.channel == ChannelState);
            assert((pkt.flags & ENET_PACKET_FLAG_RELIABLE) != 0);
            leaveOnStateChannel = true;
        }
    }
    assert(leaveOnStateChannel);
    peers[3] = nullptr;

    Stop();
}

void TestClientDirectFallbackToSteamRelay()
{
    using namespace g3t::net;
    using namespace g3t::net::internal;

    fakeNow = 20000;
    connectAddresses.clear();
    connectTags.clear();
    pendingEvents.clear();
    registeredEndpoints.clear();
    fakeDirectOffers.clear();
    fakeHostGeneration = 88ull;

    g3t::steam::DirectOffer offer{};
    offer.account = 1001ull;
    offer.generation = 88ull;
    for (uint8_t i = 0; i < 16; ++i) offer.cookie[i] = static_cast<uint8_t>(0x10u + i);
    offer.candidateCount = 1;
    offer.candidates[0] = {0x0A000001u, 42000};
    offer.ready = true;
    fakeDirectOffers.push_back(offer);

    // Case A: Direct UDP verified, ENet connects over direct UDP, then times out before welcome -> falls back to Steam relay!
    failDirectConnect = false;
    assert(Start(false));
    PlayerState dummy{};
    SyncDirectSessions(fakeNow);
    auto hostProbe = MakePunchPacket(offer.cookie, 0x03u, 1u);
    const ENetAddress directHostAddr{0x0A000001u, 42000};
    assert(InjectDatagram(directHostAddr, hostProbe) == 1);
    assert(directSessions[0].bidirectionalVerified);

    Tick(dummy, false);
    assert(connectAddresses.size() == 1);
    assert(connectAddresses[0].host == directHostAddr.host && connectAddresses[0].port == directHostAddr.port);
    assert(directSessions[0].connectStarted && directSessions[0].endpointLocked);

    // Simulate ENet connect timeout (ENET_EVENT_TYPE_DISCONNECT before everConnected).
    fakeNow += 5000;
    ENetEvent disconnectEvent{};
    disconnectEvent.type = ENET_EVENT_TYPE_DISCONNECT;
    disconnectEvent.peer = &fakePeersStorage[0];
    disconnectEvent.data = 0;
    pendingEvents.push_back(disconnectEvent);

    Tick(dummy, false);
    assert(directSessions[0].directFailed);
    assert(!directSessions[0].connectStarted);
    assert(!directSessions[0].endpointLocked);
    // Same Tick immediately re-attempts connection via Steam loopback relay (127.0.0.1:29999)!
    assert(connectAddresses.size() == 2);
    assert(connectAddresses[1].host == 0x0100007fu && connectAddresses[1].port == 29999);

    // Once connected over relay, Reconnect stays on relay (since directFailed is sticky) and resets activity state;
    // Stop clears directSessions cleanly.
    connected = true;
    activitySent = true;
    Reconnect([](char const*) {});
    assert(!connected && !activitySent);
    assert(connectAddresses.size() == 3);
    assert(connectAddresses[2].host == 0x0100007fu && connectAddresses[2].port == 29999);
    Stop();
    assert(directSessions[0].account == 0);

    // Case B: Direct UDP verified, but ConnectTransportHost fails immediately -> immediate fallback to Steam relay in same Tick!
    fakeNow = 40000;
    connectAddresses.clear();
    failDirectConnect = true;
    assert(Start(false));
    SyncDirectSessions(fakeNow);
    assert(InjectDatagram(directHostAddr, hostProbe) == 1);
    Tick(dummy, false);
    assert(connectAddresses.size() == 2);
    assert(connectAddresses[0].host == directHostAddr.host);
    assert(connectAddresses[1].host == 0x0100007fu && connectAddresses[1].port == 29999);
    assert(directSessions[0].directFailed);
    failDirectConnect = false;
    Stop();
}

void TestMsgGoodsValidationBeforeRelay()
{
    using namespace g3t::net;
    using namespace g3t::net::internal;

    fakeNow = 60000;
    peerSent.clear();
    pendingEvents.clear();
    fakeDirectOffers.clear();
    fakeHostGeneration = 0;

    assert(Start(true));
    worldEpoch = 5;

    // Attach two authenticated in-world clients on slots 2 and 3.
    for (uint8_t slot = 2; slot <= 3; ++slot) {
        ENetPeer* p = &fakePeersStorage[slot];
        p->state = ENET_PEER_STATE_CONNECTED;
        p->address = {0x0100007fu, static_cast<uint16_t>(slot == 2 ? 30001 : 30002)};
        p->data = reinterpret_cast<void*>(uintptr_t(slot));
        peers[slot] = p;
        peerAccounts[slot] = slot == 2 ? 2001ull : 2002ull;
        peerRelayGenerations[slot] = 77ull;
        clientInWorld[slot] = true;
        ResetBudget(p);
    }

    static unsigned sGoodsHandled = 0;
    sGoodsHandled = 0;
    SetGoodsHandler([](uint8_t, uint64_t, uint32_t, int32_t, GoodsEntry const*, int) { ++sGoodsHandled; });

    auto sendGoodsFromSlot2 = [&](int32_t amount, uint16_t quality, uint32_t serial,
                                  enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE) {
        GoodsHeader hdr{MsgGoods, 2, uint64_t(worldEpoch), serial, 50, 1};
        GoodsEntry entry{};
        strcpy_s(entry.name, "ItMw_1H_Sword_01");
        entry.amount = amount;
        entry.quality = quality;
        std::vector<uint8_t> raw(sizeof(hdr) + sizeof(entry));
        std::memcpy(raw.data(), &hdr, sizeof(hdr));
        std::memcpy(raw.data() + sizeof(hdr), &entry, sizeof(entry));
        ENetEvent ev{};
        ev.type = ENET_EVENT_TYPE_RECEIVE;
        ev.peer = peers[2];
        ev.channelID = ChannelReliable;
        ev.packet = enet_packet_create(raw.data(), raw.size(), flags);
        pendingEvents.push_back(ev);
        PlayerState local{};
        Tick(local, false);
    };

    peerSent.clear();
    // Unreliable MsgGoods MUST be rejected even with valid entries!
    sendGoodsFromSlot2(3, 1, 1, 0);
    assert(sGoodsHandled == 0);
    assert(goodsCache[2].empty());
    for (auto const& pkt : peerSent) assert(pkt.bytes.empty() || pkt.bytes[0] != MsgGoods);

    // Invalid amount (0) -> MUST NOT relay to peer 3 and MUST NOT populate goodsCache[2]!
    sendGoodsFromSlot2(0, 0, 2);
    assert(sGoodsHandled == 0);
    assert(goodsCache[2].empty());
    for (auto const& pkt : peerSent) assert(pkt.bytes.empty() || pkt.bytes[0] != MsgGoods);

    // Invalid quality (>= 1024) -> MUST NOT relay to peer 3 and MUST NOT populate goodsCache[2]!
    sendGoodsFromSlot2(1, 1024, 3);
    assert(sGoodsHandled == 0);
    assert(goodsCache[2].empty());
    for (auto const& pkt : peerSent) assert(pkt.bytes.empty() || pkt.bytes[0] != MsgGoods);

    // Valid entry -> relayed to peer 3 AND cached in goodsCache[2] AND delivered to local handler!
    peerSent.clear();
    sendGoodsFromSlot2(3, 1, 4);
    assert(sGoodsHandled == 1);
    assert(!goodsCache[2].empty());
    unsigned goodsRelays = 0;
    for (auto const& pkt : peerSent) {
        if (!pkt.bytes.empty() && pkt.bytes[0] == MsgGoods && pkt.peer == peers[3]) ++goodsRelays;
    }
    assert(goodsRelays == 1);

    peers[2] = peers[3] = nullptr;
    Stop();
}
} // namespace

int main()
{
    TestDirectPunchRoleValidationAndEndpointLock();
    TestClientDirectFallbackToSteamRelay();
    TestMsgGoodsValidationBeforeRelay();
    return 0;
}
