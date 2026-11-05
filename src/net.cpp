// SPDX-License-Identifier: GPL-3.0-or-later
#include <enet/enet.h>

#include "net.h"

#include "log.h"
#include "parity.h"
#include "replication.h"
#include "steam.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstddef>

namespace g3t::net {
namespace {
constexpr uint16_t ProtocolVersion = 71;  // host-only NPC state with complete rendered bone poses on two alternating pose channels, NPC attitude to the players
enum : uint8_t { MsgHello = 1, MsgWelcome = 2, MsgState = 3, MsgLeave = 4, MsgNpcs = 5, MsgMotionName = 6, MsgHitNpc = 7, MsgHitPlayer = 8, MsgStates = 9, MsgTalk = 10, MsgItem = 11, MsgContents = 12, MsgContainerTx = 13, MsgTxResult = 14, MsgWatch = 15, MsgKilled = 16, MsgRefused = 17, MsgDeath = 18, MsgCampaign = 19, MsgFileChunk = 20, MsgFileDone = 21, MsgClock = 22, MsgWorldState = 23, MsgSummon = 24, MsgShot = 25, MsgPlayerSpell = 26, MsgNpcSpell = 27, MsgEnclave = 28, MsgWorld = 29, MsgDialogLine = 30, MsgGoods = 31 };
// UDP payload per datagram (ENet default 1392 = up to 1420-byte IP packets): VPNs such as Hamachi carry about 1400, so
// stay well below (C, re/findings/hamachi-net-c.md, 21.10.). ENet splits larger messages into fragments of this size.
constexpr enet_uint32 Mtu = 1200;
enum : uint8_t { ChannelState = 0, ChannelReliable = 1, ChannelNpcs = 2, ChannelCombat = 3, ChannelPoses = 4, ChannelPosesAlt = 5, ChannelCount = 6 };
enum : uint8_t { MsgNpcRelease = 36, MsgPosedNpc = 39 };

#pragma pack(push, 1)
struct Hello { uint8_t type; uint16_t version; char name[24]; uint64_t parity[parity::ItemCount]; };
struct RefusedMsg { uint8_t type; char reason[160]; };
struct CampaignMsg { uint8_t type; uint8_t kind; char name[64]; };
struct ClockMsg { uint8_t type; ClockState state; };
struct WorldStateMsg { uint8_t type; WorldState state; };
struct FileChunkHeader { uint8_t type; uint8_t file; uint32_t total; uint32_t offset; uint16_t length; };
struct FileDoneMsg { uint8_t type; char name[64]; uint32_t size[FileCount]; uint32_t crc[FileCount]; };
struct Welcome { uint8_t type; uint8_t id; };
struct State { uint8_t type; PlayerState state; };
struct Leave { uint8_t type; uint8_t id; };
struct NpcsHeader {
    uint8_t type;
    uint32_t sequence, sent, epoch, generation, publication;
    uint8_t count, owner;
};
struct NpcReleaseHeader {
    uint8_t type;
    uint32_t sequence, epoch, generation, publication;
    uint8_t count, owner;
};
struct MotionNameMsg { uint8_t type; uint32_t hash; char name[96]; };
// to: the player who applies it (the NPC's owner, the hit player); from: who sent it (set by the host).
struct HitNpcMsg { uint8_t type; uint8_t guid[16]; int32_t delta; uint8_t weapon[16]; uint8_t to; uint8_t from; uint8_t npc[16]; uint8_t outcome; uint32_t seen; uint8_t reaction; uint32_t epoch, sequence, incarnation; };
enum : uint8_t { MsgHitResult = 38 };
struct HitResultMsg { uint8_t type; HitResult result; };
struct HitPlayerMsg { uint8_t type; int32_t delta; uint8_t attacker[16]; uint8_t attackerPlayer; uint8_t to; uint8_t meansToKill; };
struct StatesHeader { uint8_t type; uint8_t kind; uint8_t count; };
struct TalkMsg { uint8_t type; uint8_t begin; uint8_t guid[16]; };
struct ItemMsg { uint8_t type; ItemNote note; };
struct ContentsHeader { uint8_t type; uint8_t guid[16]; uint8_t count; };
// What a player can trade (trade.cpp): its goods and its gold. player: set by the host to the sender.
struct GoodsHeader { uint8_t type; uint8_t player; uint64_t epoch; uint32_t serial; int32_t gold; uint8_t count; };
struct TxHeader { uint8_t type; uint8_t guid[16]; uint32_t seq; uint8_t count; };
struct TxResultMsg { uint8_t type; uint8_t guid[16]; uint32_t seq; uint8_t ok; };
struct WatchMsg { uint8_t type; uint8_t guid[16]; };
struct KilledMsg { uint8_t type; uint8_t guid[16]; uint8_t to; uint32_t epoch, incarnation; };
struct DeathMsg { uint8_t type; uint8_t guid[16]; uint8_t by; };
struct SummonMsg { uint8_t type; uint8_t from; SummonNote note; };
struct ShotMsg { uint8_t type; uint8_t from; ShotNote note; };
enum : uint8_t { MsgRagdoll = 37 };
struct RagdollMsg { uint8_t type; uint8_t from; RagdollNote note; };
struct DialogLineMsg { uint8_t type; uint8_t from; DialogLineNote note; };
enum : uint8_t { MsgOffer = 32, MsgOfferResult = 33 };  // offer.cpp (A); 31 is B's MsgGoods
struct OfferMsg { uint8_t type; OfferNote note; };  // reliable fragmentation, like goods snapshots
constexpr size_t OfferHeaderSize = offsetof(OfferMsg, note) + offsetof(OfferNote, entries);
struct OfferResultMsg { uint8_t type; OfferResultNote note; };
struct PlayerSpellMsg { uint8_t type; uint8_t from; uint8_t to; char script[32]; };
enum : uint8_t { MsgRansack = 34, MsgWitness = 35 };  // ransack.cpp, witness.cpp
struct RansackMsg { uint8_t type; uint8_t from; uint8_t to; RansackNote note; };
struct WitnessMsg { uint8_t type; uint8_t from; uint8_t to; WitnessNote note; };
struct NpcSpellMsg { uint8_t type; uint8_t from; uint8_t to; char script[32]; uint8_t target[16]; int32_t args; };
struct EnclaveMsg { uint8_t type; EnclaveNote note; };
struct WorldMsg { uint8_t type; WorldNote note; };
#pragma pack(pop)
static_assert(sizeof(NpcsHeader) == 23, "NPC datagram budget must match its wire header");

ENetHost* host = nullptr;
ENetPeer* server = nullptr;  // client side: the connection to the host
ENetAddress target{};        // client side: where to (re)connect
uint32_t lastConnectAttempt = 0;
uint32_t startedAt = 0;      // client: when it began to connect (enet_time_get)
bool everConnected = false;  // client: an ENet connection to the host came up once (owner 24.10.: "host not reachable")
bool isHost = false;
bool enetInitialized = false;
char startupError[256] = {};
uint8_t localId = 0;
bool connected = false;
uint32_t welcomes = 0;  // client side: welcomes so far (WelcomeSerial), never sent
bool refused = false;  // client side: the host refused us; do not reconnect
char refusalReason[160] = {};  // client side: the host's reason, for the screen (persist)
uint16_t helloVersion = 0;     // QA (stage W7): the next hello claims this protocol version, 0: ours
char localName[24] = {};
uint32_t sequence = 0;
uint32_t lastSend = 0;
int sendRate = 60;  // Hz of the player state; npcs.cpp uses the same setting (config send_rate)
Remote remotes[MaxPlayers] = {};
uint32_t joins = 0;
uint32_t npcSequence = 0;
uint32_t npcPublication = 0, worldEpoch = 0;
std::unordered_map<std::string, replication::Sequence> npcOrigins[MaxPlayers];
std::unordered_map<std::string, replication::ReplicaOrder> npcPublications;
unsigned npcAccepted = 0, npcRejected = 0, npcStale = 0;
unsigned poseSent = 0, poseReceived = 0, poseSendFailed = 0;
NpcHandler npcHandler = nullptr;
NpcPoseHandler npcPoseHandler = nullptr;
PlayerStateHandler playerStateHandler = nullptr;
NpcReleaseHandler npcReleaseHandler = nullptr;
MotionNameHandler motionNameHandler = nullptr;
HitNpcHandler hitNpcHandler = nullptr;
HitResultHandler hitResultHandler = nullptr;
uint32_t hitSequence = 0;
replication::Sequence hitClaims[MaxPlayers];
HitPlayerHandler hitPlayerHandler = nullptr;
StateHandler stateHandler = nullptr;
TalkHandler talkHandler = nullptr;
ItemHandler itemHandler = nullptr;
ContentsHandler contentsHandler = nullptr;
GoodsHandler goodsHandler = nullptr;
ContainerTxHandler txHandler = nullptr;
TxResultHandler txResultHandler = nullptr;
WatchHandler watchHandler = nullptr;
KilledHandler killedHandler = nullptr;
DeathHandler deathHandler = nullptr;
CampaignHandler campaignHandler = nullptr;
ClockHandler clockHandler = nullptr;
WorldStateHandler worldStateHandler = nullptr;
FileChunkHandler fileChunkHandler = nullptr;
FileDoneHandler fileDoneHandler = nullptr;
ENetPeer* peers[MaxPlayers] = {};  // host side: peer for each client id
SummonHandler summonHandler = nullptr;
ShotHandler shotHandler = nullptr;
RagdollHandler ragdollHandler = nullptr;
DialogLineHandler dialogLineHandler = nullptr;
OfferHandler offerHandler = nullptr;
OfferResultHandler offerResultHandler = nullptr;
PlayerSpellHandler playerSpellHandler = nullptr;
RansackHandler ransackHandler = nullptr;
WitnessHandler witnessHandler = nullptr;
NpcSpellHandler npcSpellHandler = nullptr;
EnclaveHandler enclaveHandler = nullptr;
WorldHandler worldHandler = nullptr;
bool inWorld = false;                  // client side: this machine is in the host's world (persist.cpp)
bool clientInWorld[MaxPlayers] = {};   // host side: that client is in the host's world
uint32_t generations[MaxPlayers] = {}; // host side: counts the connections a slot was given (PeerGeneration)
unsigned gatedIn = 0, gatedOut = 0;

// What passes the world gate: joining, the save transfer and the world messages. Motion names wait for the world too
// (Fable's review 22.10. 1.8: they were open to a peer that never said Hello); the host resends them all on entry.
bool Gated(uint8_t type)
{
    switch (type) {
    case MsgHello: case MsgWelcome: case MsgRefused: case MsgLeave: case MsgFileChunk: case MsgFileDone: case MsgWorld:
        return false;
    default:
        return true;
    }
}

bool Open(ENetPeer* peer)
{
    if (!isHost) return inWorld;
    const uintptr_t id = reinterpret_cast<uintptr_t>(peer->data);
    return id > 1 && id < MaxPlayers && clientInWorld[id];
}

constexpr enet_uint16 PoseEpochLimit = 0xff00;
constexpr enet_uint32 PoseRestMs = 3000;
struct PoseRoute {
    bool alt = false;
    enet_uint32 checkpointed[2] = {};
};
PoseRoute poseRoutes[MaxPlayers];

bool Send(ENetPeer* peer, void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (size && Gated(static_cast<uint8_t const*>(data)[0]) && !Open(peer)) { ++gatedOut; return false; }
    if (channel == ChannelPoses && !reliable) {
        PoseRoute& route = poseRoutes[peer - host->peers];
        uint8_t active = route.alt ? ChannelPosesAlt : ChannelPoses;
        if (peer->channels[active].outgoingUnreliableSequenceNumber >= PoseEpochLimit) {
            const uint8_t next = route.alt ? ChannelPoses : ChannelPosesAlt;
            const enet_uint32 now = enet_time_get() | 1u;
            if (route.checkpointed[next - ChannelPoses] && now - route.checkpointed[next - ChannelPoses] < PoseRestMs) return false;
            // Only an empty reliable packet resets the 16-bit unreliable epoch, and the receiver holds every later packet
            // of that channel until it arrives. Reset the exhausted channel while the stream already runs on the other.
            ENetPacket* checkpoint = enet_packet_create(nullptr, 0, ENET_PACKET_FLAG_RELIABLE);
            if (!checkpoint) { Log("net: pose sequence checkpoint allocation failed"); return false; }
            if (enet_peer_send(peer, active, checkpoint) < 0) {
                enet_packet_destroy(checkpoint);
                Log("net: pose sequence checkpoint could not be queued");
                return false;
            }
            route.checkpointed[active - ChannelPoses] = now;
            route.alt = !route.alt;
            active = next;
            Logf("net: pose sequence checkpoint queued; the pose stream moved to channel %u", static_cast<unsigned>(active));
        }
        channel = active;
    }
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE
        : size > Mtu ? ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT : 0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (!packet) return false;
    if (enet_peer_send(peer, channel, packet) < 0) { enet_packet_destroy(packet); return false; }
    return true;
}

// A player's state from the network (Fable's review 22.10. 1.2, 1.6): its strings may lack their terminator and its floats
// may be anything; the puppet code feeds them to %s, strstr, Template(...) and MoveTo.
bool Sane(PlayerState& state)
{
    state.name[sizeof(state.name) - 1] = 0;
    state.animation[sizeof(state.animation) - 1] = 0;
    state.heldRight[sizeof(state.heldRight) - 1] = 0;
    state.heldLeft[sizeof(state.heldLeft) - 1] = 0;
    state.shape[sizeof(state.shape) - 1] = 0;
    state.armor[sizeof(state.armor) - 1] = 0;
    state.helmet[sizeof(state.helmet) - 1] = 0;
    state.look[sizeof(state.look) - 1] = 0;
    if (state.gold < 0) state.gold = 0;
    for (float v : state.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    for (float v : state.rotation) if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
    return true;
}

void Store(PlayerState const& state)
{
    if (state.id == 0 || state.id >= MaxPlayers || state.id == localId) return;
    Remote& r = remotes[state.id];
    if (r.latestTicks && static_cast<int32_t>(state.sequence - r.latest.sequence) <= 0) return;
    const bool reset = !r.latestTicks || r.latest.world != state.world || std::strcmp(r.latest.shape, state.shape) ||
                       std::strcmp(r.latest.look, state.look);
    r.previous = r.latestTicks ? r.latest : state;
    r.previousTicks = r.latestTicks ? r.latestTicks : enet_time_get();
    r.latest = state;
    r.latestTicks = enet_time_get();
    r.connected = true;
    if (playerStateHandler) playerStateHandler(state, r.latestTicks, reset);
}

void Drop(uint8_t id)
{
    if (id == 0 || id >= MaxPlayers) return;
    remotes[id] = Remote{};
    peers[id] = nullptr;
    clientInWorld[id] = false;
    npcOrigins[id].clear();
    hitClaims[id] = replication::Sequence{};
}

bool Sane(NpcRecord const& record)
{
    if (!record.incarnation || record.hitPlayer >= MaxPlayers || record.guidePlayer >= MaxPlayers ||
        record.partyType > 8 || record.waiting > 1 || record.presentationReady > 1 || record.absent > 1 || record.contact > 1 || record.attitude > 8 ||
        record.aiMode > 11 || record.attackReason > 21 || record.damageType > 7 ||
        record.damageAmount < -1000000 || record.damageAmount > 1000000 ||
        !std::isfinite(record.damageMultiplier) || std::fabs(record.damageMultiplier) > 1000.0f) return false;
    for (auto const& motion : record.motions) {
        if ((motion.active & ~3u) || (motion.active && !(motion.active & 1u)) ||
            !std::isfinite(motion.phase) || motion.phase < 0.0f || motion.phase > 1.0f ||
            !std::isfinite(motion.speed) || std::fabs(motion.speed) > 100.0f ||
            !std::isfinite(motion.weight) || motion.weight < 0.0f || motion.weight > 10.0f ||
            !std::isfinite(motion.intro) || motion.intro < 0.0f || motion.intro > 100000.0f ||
            !std::isfinite(motion.extro) || motion.extro < 0.0f || motion.extro > 100000.0f ||
            (motion.active && (!motion.name || !motion.instance))) return false;
    }
    for (float v : record.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    float length = 0.0f;
    for (float v : record.rotation) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
        length += v * v;
    }
    return length > 0.01f && record.hitPoints >= -1000000 && record.hitPoints <= 1000000 &&
           record.hitPointsMax >= 1 && record.hitPointsMax <= 1000000 && record.partyPlayer < MaxPlayers;
}

std::string NpcKey(uint8_t const* guid) { return std::string(reinterpret_cast<char const*>(guid), 16); }

void OnReceive(ENetPeer* peer, ENetPacket* packet)
{
    if (isHost && (peer->address.host != target.host || !steam::IsPeerRelay(peer->address.port))) return;
    if (packet->dataLength < 1) return;
    const uint8_t type = packet->data[0];
    if (type == MsgPosedNpc) ++poseReceived;
    if (Gated(type) && !Open(peer)) { ++gatedIn; return; }  // from another world (a client still loading, or the host before a load)
    if (type == MsgHello && isHost && packet->dataLength >= 3) {
        if (peer->data) { Log("net: a second hello from a player who joined already; ignored"); return; }  // it took a new slot each time
        Hello hello{};
        std::memcpy(&hello, packet->data, packet->dataLength < sizeof(hello) ? packet->dataLength : sizeof(hello));
        hello.name[sizeof(hello.name) - 1] = '\0';
        RefusedMsg refusal{MsgRefused, {}};
        if (hello.version != ProtocolVersion || packet->dataLength != sizeof(Hello)) {
            Format(refusal.reason, "different G3 Together version (protocol %u, host %u)", hello.version, ProtocolVersion);
        } else {
            uint64_t const* ours = parity::Local();
            for (int i = 0; i != parity::ItemCount; ++i)
                if (hello.parity[i] != ours[i]) {
                    strcat_s(refusal.reason, refusal.reason[0] ? ", " : "install differs from the host: ");
                    strcat_s(refusal.reason, parity::ItemName(i));
                    // Which option, the client cannot tell from a hash: the host's values (the host on Hard, the client on Medium,
                    // playtest 27.10.); the reason is cut at its 160 characters.
                    if (i == parity::Options) {
                        strncat_s(refusal.reason, " (host: ", _TRUNCATE);
                        strncat_s(refusal.reason, parity::OptionsText(), _TRUNCATE);
                        strncat_s(refusal.reason, ")", _TRUNCATE);
                    }
                }
        }
        if (refusal.reason[0]) {
            Logf("net: refused %s: %s", hello.name, refusal.reason);
            Send(peer, &refusal, sizeof(refusal), ChannelReliable, true);
            enet_peer_disconnect_later(peer, 1);
            return;
        }
        uint8_t id = 0;
        for (uint8_t i = 2; i < MaxPlayers && !id; ++i) if (!peers[i]) id = i;
        if (!id) { enet_peer_disconnect(peer, 2); return; }
        peers[id] = peer;
        ++generations[id];
        peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
        remotes[id].connected = false;  // until it is in the host's world (SetInWorld)
        clientInWorld[id] = false;
        strncpy_s(remotes[id].latest.name, hello.name, _TRUNCATE);
        const Welcome welcome{MsgWelcome, id};
        Send(peer, &welcome, sizeof(welcome), ChannelReliable, true);
        Logf("net: %s joined as player %u", hello.name, id);
    } else if (type == MsgRefused && !isHost && packet->dataLength == sizeof(RefusedMsg)) {
        RefusedMsg refusal;
        std::memcpy(&refusal, packet->data, sizeof(refusal));
        refusal.reason[sizeof(refusal.reason) - 1] = '\0';
        refused = true;
        strncpy_s(refusalReason, refusal.reason, _TRUNCATE);
        Logf("net: REFUSED by host: %s", refusal.reason);
    } else if (type == MsgWelcome && !isHost && packet->dataLength == sizeof(Welcome)) {
        Welcome welcome;
        std::memcpy(&welcome, packet->data, sizeof(welcome));
        if (welcome.id < 2 || welcome.id >= MaxPlayers) { Logf("net: welcome with player id %u; ignored", welcome.id); return; }
        localId = welcome.id;
        connected = true;
        inWorld = false;  // until the host's world is known to be this one (persist.cpp)
        ++welcomes;
        ResetNpcReplication();  // a new host connection has a fresh publication stream, even in the same campaign
        Logf("net: welcomed as player %u (welcome %u)", localId, welcomes);
    } else if (type == MsgState && packet->dataLength == sizeof(State)) {
        State message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (!Sane(message.state)) return;
        if (isHost) {
            // The host decides who sent it; a client cannot speak for another player.
            message.state.id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            Store(message.state);
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.state.id)
                    Send(peers[i], &message, sizeof(message), ChannelState, (packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0);
        } else {
            Store(message.state);
        }
    } else if ((type == MsgNpcs || type == MsgPosedNpc) && packet->dataLength >= sizeof(NpcsHeader)) {
        if (isHost) { ++npcRejected; return; }
        NpcsHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (!header.count || header.count > MaxNpcsPerMessage || header.epoch != worldEpoch) {
            ++npcRejected;
            return;
        }
        uint16_t boneCount = 0;
        uint8_t poseFlags = 0;
        const pose::Bone* bones = nullptr;
        const size_t recordsSize = sizeof(header) + header.count * sizeof(NpcRecord);
        if (type == MsgPosedNpc) {
            if (header.count != 1 || packet->dataLength < recordsSize + sizeof(boneCount) + sizeof(poseFlags)) { ++npcRejected; return; }
            std::memcpy(&boneCount, packet->data + recordsSize, sizeof(boneCount));
            std::memcpy(&poseFlags, packet->data + recordsSize + sizeof(boneCount), sizeof(poseFlags));
            if (!boneCount || boneCount > pose::MaxBones ||
                (poseFlags & ~pose::Facial) ||
                packet->dataLength != recordsSize + sizeof(boneCount) + sizeof(poseFlags) + boneCount * sizeof(pose::Bone)) {
                ++npcRejected;
                return;
            }
            bones = reinterpret_cast<pose::Bone const*>(packet->data + recordsSize + sizeof(boneCount) + sizeof(poseFlags));
            for (int i = 0; i < boneCount; ++i) {
                if (!bones[i].name) { ++npcRejected; return; }
                for (int k = 0; k < i; ++k)
                    if (bones[k].name == bones[i].name) { ++npcRejected; return; }
            }
        } else if (packet->dataLength != recordsSize) { ++npcRejected; return; }
        NpcRecord records[MaxNpcsPerMessage];
        std::memcpy(records, packet->data + sizeof(header), header.count * sizeof(NpcRecord));
        if (isHost) {
            header.owner = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (header.owner < 2 || header.owner >= MaxPlayers) return;
            header.generation = generations[header.owner];
        } else if (header.owner != 1) {
            ++npcRejected;
            return;
        }
        int count = 0;
        for (int i = 0; i < header.count; ++i) {
            NpcRecord const& record = records[i];
            if (!Sane(record)) { ++npcRejected; continue; }
            const std::string key = NpcKey(record.guid);
            const bool newer = isHost ? npcOrigins[header.owner][key].Accept(header.sequence)
                                     : npcPublications[key].Snapshot(header.owner, header.publication);
            if (!newer) { ++npcStale; continue; }
            if (bones && npcPoseHandler)
                npcPoseHandler(record, bones, boneCount, poseFlags, (packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0);
            records[count++] = record;
        }
        if (!count) return;
        const int accepted = npcHandler ? npcHandler(records, count, enet_time_get(), header.sent, header.owner, header.generation) : 0;
        if (accepted < 0 || accepted > count) { Log("net: invalid accepted NPC count; publication refused"); return; }
        npcRejected += count - accepted;
        npcAccepted += accepted;
        if (isHost && accepted) {
            // Adjudicate first: an update refused by the host must never become truth on a third client.
            header.count = static_cast<uint8_t>(accepted);
            header.publication = ++npcPublication;
            uint8_t buffer[sizeof(header) + MaxNpcsPerMessage * sizeof(NpcRecord)];
            std::memcpy(buffer, &header, sizeof(header));
            std::memcpy(buffer + sizeof(header), records, accepted * sizeof(NpcRecord));
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != header.owner)
                    Send(peers[i], buffer, sizeof(header) + accepted * sizeof(NpcRecord), ChannelNpcs,
                         (packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0);
        }
    } else if (type == MsgNpcRelease && packet->dataLength >= sizeof(NpcReleaseHeader)) {
        if (isHost) { ++npcRejected; return; }
        NpcReleaseHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (!header.count || header.count > MaxNpcsPerMessage ||
            packet->dataLength != sizeof(header) + header.count * 16u || header.epoch != worldEpoch) {
            ++npcRejected;
            return;
        }
        if (isHost) {
            header.owner = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (header.owner < 2 || header.owner >= MaxPlayers) return;
            header.generation = generations[header.owner];
        } else if (header.owner != 1) {
            return;
        }
        uint8_t guids[MaxNpcsPerMessage * 16];
        int count = 0;
        for (int i = 0; i < header.count; ++i) {
            uint8_t const* guid = packet->data + sizeof(header) + i * 16;
            const std::string key = NpcKey(guid);
            const bool newer = isHost ? npcOrigins[header.owner][key].Accept(header.sequence)
                                     : npcPublications[key].Release(header.owner, header.publication);
            if (!newer) { ++npcStale; continue; }
            std::memcpy(guids + count++ * 16, guid, 16);
        }
        const int accepted = count && npcReleaseHandler ? npcReleaseHandler(header.owner, header.generation, guids, count) : 0;
        if (accepted < 0 || accepted > count) { Log("net: invalid accepted release count; publication refused"); return; }
        npcRejected += count - accepted;
        if (isHost && accepted) {
            header.count = static_cast<uint8_t>(accepted);
            header.publication = ++npcPublication;
            uint8_t buffer[sizeof(header) + MaxNpcsPerMessage * 16];
            std::memcpy(buffer, &header, sizeof(header));
            std::memcpy(buffer + sizeof(header), guids, accepted * 16u);
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != header.owner) Send(peers[i], buffer, sizeof(header) + accepted * 16u, ChannelNpcs, true);
        }
    } else if (type == MsgMotionName && packet->dataLength == sizeof(MotionNameMsg)) {
        if (isHost) { Log("net: client NPC motion dictionary publication refused"); return; }
        MotionNameMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.name[sizeof(message.name) - 1] = '\0';
        if (message.hash != HashName(message.name)) { Log("net: NPC motion dictionary hash mismatch"); return; }
        if (motionNameHandler) motionNameHandler(message.hash, message.name);
    } else if (type == MsgHitNpc && packet->dataLength == sizeof(HitNpcMsg)) {
        if (!isHost) { Log("net: client NPC hit publication refused"); return; }
        HitNpcMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        }
        static const uint8_t zero[16] = {};
        if (message.from < 2 || message.from >= MaxPlayers || message.to != 1 || message.delta >= 0 || message.delta < -1000000 ||
            message.epoch != worldEpoch || !message.sequence || !message.incarnation || message.reaction > HitSitKnockDown ||
            message.outcome > HitDeath ||
            std::memcmp(message.npc, zero, 16)) { Log("net: invalid or stale NPC hit rejected"); return; }
        if (!hitClaims[message.from].Accept(message.sequence)) {
            Log("net: repeated NPC hit ignored; its original reliable result remains authoritative");
            return;
        }
        // The sender's destination is only a hint: gameplay resolves the current authority before applying or relaying.
        if (hitNpcHandler && message.from)
            hitNpcHandler(message.from, message.guid, message.delta, message.weapon, message.npc, message.outcome, message.seen,
                          message.reaction, message.sequence, message.incarnation);
    } else if (type == MsgHitResult && packet->dataLength == sizeof(HitResultMsg)) {
        if (isHost) return;
        HitResultMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.result.epoch == worldEpoch && message.result.accepted <= 1 && hitResultHandler)
            hitResultHandler(message.result);
    } else if (type == MsgHitPlayer && packet->dataLength == sizeof(HitPlayerMsg)) {
        HitPlayerMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.delta < -1000000 || message.delta > 0) return;  // damage only, no overflow
        if (isHost && message.attackerPlayer) message.attackerPlayer = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (isHost && message.to > 1 && message.to < MaxPlayers) {  // for another client: never the host's hero, even if it just left
            if (peers[message.to]) Send(peers[message.to], &message, sizeof(message), ChannelCombat, true);
            return;
        }
        if (hitPlayerHandler) hitPlayerHandler(message.delta, message.attacker, message.attackerPlayer, message.meansToKill != 0);
    } else if (type == MsgStates && packet->dataLength >= sizeof(StatesHeader)) {
        StatesHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (header.count <= MaxStatesPerMessage && packet->dataLength == sizeof(header) + header.count * sizeof(StateEntry) &&
            stateHandler && from) {
            StateEntry entries[MaxStatesPerMessage];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(StateEntry));
            for (int i = 0; i < header.count; ++i) entries[i].name[sizeof(entries[i].name) - 1] = 0;
            stateHandler(from, header.kind, entries, header.count);
        }
    } else if (type == MsgTalk && isHost && packet->dataLength == sizeof(TalkMsg)) {
        TalkMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (talkHandler && from) talkHandler(from, message.begin != 0, message.guid);
    } else if (type == MsgSummon && packet->dataLength == sizeof(SummonMsg)) {
        SummonMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.spellItem[sizeof(message.note.spellItem) - 1] = 0;
        message.note.spawn[sizeof(message.note.spawn) - 1] = 0;
        if (message.note.epoch != worldEpoch || message.note.ownerPlayer >= MaxPlayers) return;
        if (isHost) {  // from a client: the caster is that client; pass it on to the others
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.note.op == SummonCreature) { Log("net: client NPC spawn publication refused"); return; }
            if (message.note.op == SummonCreatureRequest) message.note.ownerPlayer = message.from;
            else for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
        }
        if (summonHandler && message.from) summonHandler(message.from, message.note);
    } else if (type == MsgEnclave && packet->dataLength == sizeof(EnclaveMsg)) {
        EnclaveMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.name[sizeof(message.note.name) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (enclaveHandler && from) enclaveHandler(from, message.note);
    } else if (type == MsgPlayerSpell && packet->dataLength == sizeof(PlayerSpellMsg)) {
        PlayerSpellMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.script[sizeof(message.script) - 1] = 0;
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.to > 1 && message.to < MaxPlayers && peers[message.to]) {
                Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return;
            }
        }
        if (playerSpellHandler && message.from && message.to == localId) playerSpellHandler(message.from, message.script);
    } else if (type == MsgRansack && packet->dataLength == sizeof(RansackMsg)) {
        RansackMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.to > 1 && message.to < MaxPlayers && peers[message.to]) {
                Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return;
            }
        }
        if (ransackHandler && message.from && message.to == localId) ransackHandler(message.from, message.note);
    } else if (type == MsgWitness && packet->dataLength == sizeof(WitnessMsg)) {
        WitnessMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.to > 1 && message.to < MaxPlayers) {
                if (peers[message.to]) Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return;
            }
            if (message.to == 0)  // to everybody: the other clients too, and the host itself below
                for (uint8_t i = 2; i < MaxPlayers; ++i)
                    if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
        }
        if (witnessHandler && message.from && (message.to == 0 || message.to == localId)) witnessHandler(message.from, message.note);
    } else if (type == MsgNpcSpell && packet->dataLength == sizeof(NpcSpellMsg)) {
        NpcSpellMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.script[sizeof(message.script) - 1] = 0;
        if (isHost) {
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.to > 1 && message.to < MaxPlayers && peers[message.to]) {
                Send(peers[message.to], &message, sizeof(message), ChannelReliable, true);
                return;
            }
        }
        if (npcSpellHandler && message.from && message.to == localId) npcSpellHandler(message.from, message.script, message.target, message.args);
    } else if (type == MsgShot && packet->dataLength == sizeof(ShotMsg)) {
        ShotMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        static const uint8_t none[16] = {};
        if (isHost && std::memcmp(message.note.caster, none, 16)) { Log("net: client NPC projectile publication refused"); return; }
        message.note.arrow[sizeof(message.note.arrow) - 1] = 0;
        message.note.spellItem[sizeof(message.note.spellItem) - 1] = 0;
        message.note.spell[sizeof(message.note.spell) - 1] = 0;
        if (message.note.epoch != worldEpoch || message.note.targetPlayer >= MaxPlayers) {
            Log("net: stale or invalid projectile refused");
            return;
        }
        if (isHost) {  // from a client: the shooter is that client; pass it on to the others
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), ChannelCombat, true);
        }
        if (shotHandler && message.from) shotHandler(message.from, message.note);
    } else if (type == MsgRagdoll && packet->dataLength == sizeof(RagdollMsg)) {
        if (isHost) { Log("net: client NPC force publication refused"); return; }
        RagdollMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.note.epoch != worldEpoch || !message.note.sequence || !message.note.incarnation) return;
        for (float v : message.note.at) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return;
        for (float v : message.note.force) if (!std::isfinite(v) || std::fabs(v) > 1e9f) return;
        if (isHost) {  // from a client: pass it on to the others
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), ChannelCombat, true);
        }
        if (ragdollHandler) ragdollHandler(message.note);
    } else if (type == MsgDialogLine && packet->dataLength == sizeof(DialogLineMsg)) {
        DialogLineMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.sample[sizeof(message.note.sample) - 1] = 0;
        if (isHost) {  // from a client: pass it on to the others
            message.from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.from) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
        }
        if (dialogLineHandler && message.from) dialogLineHandler(message.from, message.note);
    } else if (type == MsgOffer && packet->dataLength >= OfferHeaderSize) {
        OfferMsg message{};
        std::memcpy(&message, packet->data, OfferHeaderSize);
        if (message.note.count > MaxOfferEntries ||
            packet->dataLength != OfferHeaderSize + message.note.count * sizeof(OfferEntry)) return;
        std::memcpy(&message, packet->data, packet->dataLength);
        for (auto& entry : message.note.entries) entry.name[sizeof(entry.name) - 1] = 0;
        if (isHost) {
            message.note.buyer = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));  // who sent it
            if (message.note.seller != 1) {  // another client's goods: on to it
                const uint8_t to = message.note.seller;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.buyer) Send(peers[to], &message, packet->dataLength, ChannelReliable, true);
                return;
            }
        } else if (message.note.seller != localId) {
            return;
        }
        if (offerHandler && message.note.buyer) offerHandler(message.note);
    } else if (type == MsgOfferResult && packet->dataLength == sizeof(OfferResultMsg)) {
        OfferResultMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (isHost) {
            message.note.seller = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (message.note.buyer != 1) {
                const uint8_t to = message.note.buyer;
                if (to > 1 && to < MaxPlayers && peers[to] && to != message.note.seller) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
                return;
            }
        } else if (message.note.buyer != localId) {
            return;
        }
        if (offerResultHandler && message.note.seller) offerResultHandler(message.note);
    } else if (type == MsgItem && packet->dataLength == sizeof(ItemMsg)) {
        ItemMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.name[sizeof(message.note.name) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (itemHandler && from) itemHandler(from, message.note);
    } else if (type == MsgGoods && packet->dataLength >= sizeof(GoodsHeader)) {
        GoodsHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (header.count > MaxGoodsEntries || packet->dataLength != sizeof(header) + header.count * sizeof(GoodsEntry) || header.gold < 0)
            return;
        if (isHost) {
            // The host decides who sent it and passes it on to the others.
            header.player = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (!header.player) return;
            std::memcpy(packet->data, &header, sizeof(header));
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != header.player) Send(peers[i], packet->data, packet->dataLength, ChannelReliable, true);
        }
        if (header.player == 0 || header.player >= MaxPlayers || header.player == localId || !goodsHandler) return;
        GoodsEntry entries[MaxGoodsEntries];
        if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(GoodsEntry));
        for (int i = 0; i < header.count; ++i) {
            entries[i].name[sizeof(entries[i].name) - 1] = 0;
            if (entries[i].amount <= 0 || entries[i].amount > 1000000 || entries[i].quality >= 1024) return;
        }
        goodsHandler(header.player, header.epoch, header.serial, header.gold, entries, header.count);
    } else if (type == MsgContents && !isHost && packet->dataLength >= sizeof(ContentsHeader)) {
        ContentsHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (header.count <= MaxContentEntries && packet->dataLength == sizeof(header) + header.count * sizeof(ContentEntry) &&
            contentsHandler) {
            ContentEntry entries[MaxContentEntries];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(ContentEntry));
            for (int i = 0; i < header.count; ++i) entries[i].name[sizeof(entries[i].name) - 1] = 0;
            contentsHandler(header.guid, entries, header.count);
        }
    } else if (type == MsgContainerTx && isHost && packet->dataLength >= sizeof(TxHeader)) {
        TxHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (header.count <= MaxContentEntries && packet->dataLength == sizeof(header) + header.count * sizeof(ContentEntry) &&
            txHandler && from) {
            ContentEntry entries[MaxContentEntries];
            if (header.count) std::memcpy(entries, packet->data + sizeof(header), header.count * sizeof(ContentEntry));
            for (int i = 0; i < header.count; ++i) entries[i].name[sizeof(entries[i].name) - 1] = 0;
            txHandler(from, header.guid, header.seq, entries, header.count);
        }
    } else if (type == MsgTxResult && !isHost && packet->dataLength == sizeof(TxResultMsg)) {
        TxResultMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (txResultHandler) txResultHandler(message.guid, message.seq, message.ok != 0);
    } else if (type == MsgWatch && isHost && packet->dataLength == sizeof(WatchMsg)) {
        WatchMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        const uint8_t from = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (watchHandler && from) watchHandler(from, message.guid);
    } else if (type == MsgFileChunk && !isHost && packet->dataLength >= sizeof(FileChunkHeader)) {
        FileChunkHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (packet->dataLength == sizeof(header) + header.length && fileChunkHandler)
            fileChunkHandler(header.file, header.total, header.offset, packet->data + sizeof(header), header.length);
    } else if (type == MsgFileDone && !isHost && packet->dataLength == sizeof(FileDoneMsg)) {
        FileDoneMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.name[sizeof(message.name) - 1] = 0;
        if (fileDoneHandler) fileDoneHandler(message.name, message.size, message.crc);
    } else if (type == MsgWorldState && packet->dataLength == sizeof(WorldStateMsg)) {
        WorldStateMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.state.routine[sizeof(message.state.routine) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (worldStateHandler && from) worldStateHandler(from, message.state);
    } else if (type == MsgClock && packet->dataLength == sizeof(ClockMsg)) {
        ClockMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (clockHandler) clockHandler(message.state);
    } else if (type == MsgCampaign && !isHost && packet->dataLength == sizeof(CampaignMsg)) {
        CampaignMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.name[sizeof(message.name) - 1] = 0;
        if (campaignHandler) campaignHandler(message.kind, message.name);
    } else if (type == MsgDeath && !isHost && packet->dataLength == sizeof(DeathMsg)) {
        DeathMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (deathHandler && message.by && message.by < MaxPlayers) deathHandler(message.by, message.guid);
    } else if (type == MsgKilled && packet->dataLength == sizeof(KilledMsg)) {
        if (isHost) return;
        KilledMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.to != localId || message.epoch != worldEpoch || !message.incarnation) return;
        if (killedHandler) killedHandler(message.guid, message.incarnation);
    } else if (type == MsgLeave && !isHost && packet->dataLength == sizeof(Leave)) {
        Leave leave;
        std::memcpy(&leave, packet->data, sizeof(leave));
        Logf("net: player %u left", leave.id);
        Drop(leave.id);
    } else if (type == MsgWorld && packet->dataLength == sizeof(WorldMsg)) {
        WorldMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.note.campaign[sizeof(message.note.campaign) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (!isHost && message.note.op == WorldHost && worldEpoch != message.note.epoch) {
            ResetNpcReplication();
            worldEpoch = message.note.epoch;
        }
        if (worldHandler && from) worldHandler(from, message.note);
    }
}
}

uint32_t HashWorld(char const* name)
{
    uint32_t hash = 2166136261u;
    for (; *name; ++name) hash = (hash ^ static_cast<uint8_t>(*name)) * 16777619u;
    return hash;
}

uint32_t Now() { return enet_time_get(); }
uint8_t LocalId() { return localId; }
bool IsHost() { return isHost; }
uint32_t JoinCount() { return joins; }

void SetInWorld(uint8_t id, bool in)
{
    if (!isHost) {
        if (inWorld == in) return;
        inWorld = in;
        if (!in) for (uint8_t i = 0; i < MaxPlayers; ++i) remotes[i] = Remote{};  // the others' puppets are of that world
        Logf("net: this player is %s the host's world", in ? "in" : "out of");
        return;
    }
    if (id < 2 || id >= MaxPlayers || !peers[id] || clientInWorld[id] == in) return;
    clientInWorld[id] = in;
    if (in) {
        ++joins;  // like a join for everything that sends a newcomer the whole state
    } else {
        Remote cleared{};
        strncpy_s(cleared.latest.name, remotes[id].latest.name, _TRUNCATE);
        remotes[id] = cleared;  // its puppet goes; it comes back with the first state from the host's world
        npcOrigins[id].clear();
    }
    Logf("net: player %u (%s) is %s the host's world", id, remotes[id].latest.name, in ? "in" : "out of");
}

bool InWorld(uint8_t id) { return isHost ? (id == 1 || (id < MaxPlayers && clientInWorld[id])) : inWorld; }
bool Joined() { return connected; }
char const* RefusalReason() { return refusalReason; }
uint32_t ConnectingFor() { return isHost || everConnected || !host ? 0 : enet_time_get() - startedAt; }
void ClaimVersionForTest(uint16_t version) { helloVersion = version; }
void PrimePoseWrapForTest()
{
    if (!isHost) return;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) peers[i]->channels[ChannelPoses].outgoingUnreliableSequenceNumber = 0xfffe;
}
uint32_t WelcomeSerial() { return welcomes; }
void AdvanceWelcomeSerialForTest() { ++welcomes; }
bool PeerJoined(uint8_t id) { return isHost && id > 1 && id < MaxPlayers && peers[id]; }
uint32_t PeerGeneration(uint8_t id) { return id < MaxPlayers ? generations[id] : 0; }
char const* PeerName(uint8_t id) { return id < MaxPlayers ? remotes[id].latest.name : ""; }
void GateCounts(unsigned& in, unsigned& out) { in = gatedIn; out = gatedOut; }
void SetWorldHandler(WorldHandler handler) { worldHandler = handler; }

void SendWorld(WorldNote const& note, uint8_t to)
{
    if (!host) return;
    if (isHost && note.op == WorldHost && worldEpoch != note.epoch) {
        ResetNpcReplication();
        worldEpoch = note.epoch;
    }
    const WorldMsg message{MsgWorld, note};
    if (isHost) {
        if (to > 1 && to < MaxPlayers && peers[to]) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    } else if (server && connected) {
        Send(server, &message, sizeof(message), ChannelReliable, true);
    }
}

uint32_t HashName(char const* name) { return HashWorld(name); }

uint32_t NextDeedId()
{
    static uint32_t last = 0;
    return ++last;
}

void SetNpcHandlers(NpcHandler npcs, MotionNameHandler names)
{
    npcHandler = npcs;
    motionNameHandler = names;
}

void SetNpcPoseHandler(NpcPoseHandler handler) { npcPoseHandler = handler; }

void SetPlayerStateHandler(PlayerStateHandler handler) { playerStateHandler = handler; }
void SetSendRate(int hz) { sendRate = hz < 20 ? 20 : hz > 60 ? 60 : hz; }

void ResetNpcReplication()
{
    for (auto& origins : npcOrigins) origins.clear();
    npcPublications.clear();
}

void NpcReplicationCounts(unsigned& accepted, unsigned& rejected, unsigned& stale)
{
    accepted = npcAccepted;
    rejected = npcRejected;
    stale = npcStale;
}

void NpcPoseCounts(unsigned& sent, unsigned& received, unsigned& failed)
{
    sent = poseSent; received = poseReceived; failed = poseSendFailed;
}

void SendNpcs(NpcRecord const* records, int count, bool reliable)
{
    if (!host || !isHost || count <= 0 || count > MaxNpcsPerMessage) return;
    static uint8_t buffer[sizeof(NpcsHeader) + MaxNpcsPerMessage * sizeof(NpcRecord)];
    const uint8_t owner = isHost ? uint8_t(1) : localId;
    const NpcsHeader header{MsgNpcs, ++npcSequence, enet_time_get(), worldEpoch, isHost ? 1u : 0u,
                            isHost ? ++npcPublication : 0u, static_cast<uint8_t>(count), owner};
    std::memcpy(buffer, &header, sizeof(header));
    for (int i = 0; i < count; ++i) {
        NpcRecord record = records[i];
        if (!record.sent) record.sent = header.sent;
        std::memcpy(buffer + sizeof(header) + i * sizeof(record), &record, sizeof(record));
    }
    if (!isHost) {
        if (server && connected) Send(server, buffer, sizeof(header) + count * sizeof(NpcRecord), ChannelNpcs, reliable);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], buffer, sizeof(header) + count * sizeof(NpcRecord), ChannelNpcs, reliable);
}

void SendPosedNpc(NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags)
{
    if (!host || !isHost || !bones || count <= 0 || count > pose::MaxBones || (poseFlags & ~pose::Facial)) return;
    uint8_t buffer[sizeof(NpcsHeader) + sizeof(NpcRecord) + sizeof(uint16_t) + sizeof(poseFlags) + pose::MaxBones * sizeof(pose::Bone)];
    const NpcsHeader header{MsgPosedNpc, ++npcSequence, enet_time_get(), worldEpoch, 1, ++npcPublication, 1, 1};
    const uint16_t boneCount = static_cast<uint16_t>(count);
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), &record, sizeof(record));
    std::memcpy(buffer + sizeof(header) + sizeof(record), &boneCount, sizeof(boneCount));
    std::memcpy(buffer + sizeof(header) + sizeof(record) + sizeof(boneCount), &poseFlags, sizeof(poseFlags));
    std::memcpy(buffer + sizeof(header) + sizeof(record) + sizeof(boneCount) + sizeof(poseFlags), bones, count * sizeof(*bones));
    const size_t size = sizeof(header) + sizeof(record) + sizeof(boneCount) + sizeof(poseFlags) + count * sizeof(*bones);
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) {
            if (Send(peers[i], buffer, size, ChannelPoses, false)) ++poseSent;
            else ++poseSendFailed;
        }
}

void SendNpcRelease(uint8_t const* guids, int count)
{
    if (!host || !isHost || !guids || count <= 0 || count > MaxNpcsPerMessage) return;
    uint8_t buffer[sizeof(NpcReleaseHeader) + MaxNpcsPerMessage * 16];
    const NpcReleaseHeader header{MsgNpcRelease, ++npcSequence, worldEpoch, isHost ? 1u : 0u,
                                  isHost ? ++npcPublication : 0u, static_cast<uint8_t>(count), localId};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), guids, count * 16u);
    const size_t size = sizeof(header) + count * 16u;
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i]) Send(peers[i], buffer, size, ChannelNpcs, true);
    } else if (server && connected) {
        Send(server, buffer, size, ChannelNpcs, true);
    }
}

void SetNpcReleaseHandler(NpcReleaseHandler handler) { npcReleaseHandler = handler; }

void SetHitHandlers(HitNpcHandler npc, HitPlayerHandler player)
{
    hitNpcHandler = npc;
    hitPlayerHandler = player;
}

uint32_t SendHitNpc(uint8_t to, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16], uint8_t outcome,
                    uint32_t seen, uint8_t reaction, uint32_t incarnation)
{
    if (!host || isHost || !server || !connected || !inWorld || to != 1) {
        Log("net: NPC hit not sent: no ready authoritative host");
        return 0;
    }
    if (++hitSequence == 0) ++hitSequence;
    HitNpcMsg message{MsgHitNpc, {}, delta, {}, to, localId, {}, outcome, seen, reaction, worldEpoch, hitSequence, incarnation};
    std::memcpy(message.guid, guid, 16);
    std::memcpy(message.weapon, weapon, 16);
    if (npc) std::memcpy(message.npc, npc, 16);
    if (server && connected) Send(server, &message, sizeof(message), ChannelCombat, true);
    return hitSequence;
}

void SetHitResultHandler(HitResultHandler handler) { hitResultHandler = handler; }

void SendHitResult(uint8_t to, HitResult const& result)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to]) return;
    HitResultMsg message{MsgHitResult, result};
    message.result.epoch = worldEpoch;
    message.result.processedAt = enet_time_get();
    Send(peers[to], &message, sizeof(message), ChannelCombat, true);
}
void SendHitPlayer(uint8_t playerId, int32_t delta, uint8_t const attacker[16], uint8_t attackerPlayer, bool meansToKill)
{
    if (!host || playerId == localId || playerId >= MaxPlayers) return;
    HitPlayerMsg message{MsgHitPlayer, delta, {}, attackerPlayer, playerId, static_cast<uint8_t>(meansToKill ? 1 : 0)};
    std::memcpy(message.attacker, attacker, 16);
    if (!isHost) { if (server && connected) Send(server, &message, sizeof(message), ChannelCombat, true); return; }
    if (peers[playerId]) Send(peers[playerId], &message, sizeof(message), ChannelCombat, true);
}

void SetStateHandler(StateHandler handler) { stateHandler = handler; }

void SetTalkHandler(TalkHandler handler) { talkHandler = handler; }

void SetItemHandler(ItemHandler handler) { itemHandler = handler; }

void SetKilledHandler(KilledHandler handler) { killedHandler = handler; }

void SetDeathHandler(DeathHandler handler) { deathHandler = handler; }

void SetCampaignHandler(CampaignHandler handler) { campaignHandler = handler; }

void SetClockHandler(ClockHandler handler) { clockHandler = handler; }

void SetWorldStateHandler(WorldStateHandler handler) { worldStateHandler = handler; }

void SendWorldState(WorldState const& state, uint8_t except)
{
    if (!host) return;
    const WorldStateMsg message{MsgWorldState, state};
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != except) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendClock(ClockState const& state)
{
    if (!host) return;
    const ClockMsg message{MsgClock, state};
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SetFileHandlers(FileChunkHandler chunk, FileDoneHandler done)
{
    fileChunkHandler = chunk;
    fileDoneHandler = done;
}

bool SendFileChunk(uint8_t to, uint8_t file, uint32_t total, uint32_t offset, void const* data, uint16_t length)
{
    if (!host || !isHost || to >= MaxPlayers || !peers[to]) return false;
    static uint8_t buffer[sizeof(FileChunkHeader) + MaxChunk];
    if (length > MaxChunk) return false;
    const FileChunkHeader header{MsgFileChunk, file, total, offset, length};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), data, length);
    Send(peers[to], buffer, sizeof(header) + length, ChannelReliable, true);
    return true;
}

bool SendFileDone(uint8_t to, char const* name, uint32_t const size[FileCount], uint32_t const crc[FileCount])
{
    if (!host || !isHost || to >= MaxPlayers || !peers[to]) return false;
    FileDoneMsg message{MsgFileDone, {}, {}, {}};
    strncpy_s(message.name, name, _TRUNCATE);
    std::memcpy(message.size, size, sizeof(message.size));
    std::memcpy(message.crc, crc, sizeof(message.crc));
    Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    return true;
}

void SendCampaign(uint8_t kind, char const* name)
{
    if (!host || !isHost) return;
    CampaignMsg message{MsgCampaign, kind, {}};
    strncpy_s(message.name, name, _TRUNCATE);
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendDeath(uint8_t const guid[16], uint8_t by, uint8_t except)
{
    if (!host || !isHost) return;
    DeathMsg message{MsgDeath, {}, by};
    std::memcpy(message.guid, guid, 16);
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != except) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendKilled(uint8_t to, uint8_t const guid[16], uint32_t incarnation)
{
    if (!host || !isHost || to <= 1 || to >= MaxPlayers || !incarnation) return;
    KilledMsg message{MsgKilled, {}, to, worldEpoch, incarnation};
    std::memcpy(message.guid, guid, 16);
    if (peers[to]) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

bool SendGoods(uint64_t epoch, uint32_t serial, int32_t gold, GoodsEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxGoodsEntries || gold < 0) return false;
    uint8_t buffer[sizeof(GoodsHeader) + MaxGoodsEntries * sizeof(GoodsEntry)];
    const GoodsHeader header{MsgGoods, localId, epoch, serial, gold, static_cast<uint8_t>(count)};
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(GoodsEntry));
    const size_t size = sizeof(header) + count * sizeof(GoodsEntry);
    if (!isHost) {
        if (!server || !connected) return false;
        Send(server, buffer, size, ChannelReliable, true);
        return true;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], buffer, size, ChannelReliable, true);
    return true;
}

void SetGoodsHandler(GoodsHandler goods) { goodsHandler = goods; }

void SetContainerHandlers(ContentsHandler contents, ContainerTxHandler tx, TxResultHandler result, WatchHandler watch)
{
    contentsHandler = contents;
    txHandler = tx;
    txResultHandler = result;
    watchHandler = watch;
}

bool SendContents(uint8_t const guid[16], ContentEntry const* entries, int count)
{
    if (!host || !isHost || count < 0 || count > MaxContentEntries) return false;
    uint8_t buffer[sizeof(ContentsHeader) + MaxContentEntries * sizeof(ContentEntry)];
    ContentsHeader header{MsgContents, {}, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
    return true;
}

bool SendContainerTx(uint8_t const guid[16], uint32_t seq, ContentEntry const* entries, int count)
{
    if (!host || isHost || !server || !connected || count < 0 || count > MaxContentEntries) return false;
    uint8_t buffer[sizeof(TxHeader) + MaxContentEntries * sizeof(ContentEntry)];
    TxHeader header{MsgContainerTx, {}, seq, static_cast<uint8_t>(count)};
    std::memcpy(header.guid, guid, 16);
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(ContentEntry));
    Send(server, buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
    return true;
}

void SendContainerTxResult(uint8_t to, uint8_t const guid[16], uint32_t seq, bool ok)
{
    if (!host || !isHost || to >= MaxPlayers || !peers[to]) return;
    TxResultMsg message{MsgTxResult, {}, seq, static_cast<uint8_t>(ok ? 1 : 0)};
    std::memcpy(message.guid, guid, 16);
    Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

void SendWatch(uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    WatchMsg message{MsgWatch, {}};
    std::memcpy(message.guid, guid, 16);
    Send(server, &message, sizeof(message), ChannelReliable, true);
}

void SendItem(ItemNote const& note, uint8_t to)
{
    if (!host) return;
    ItemMsg message{MsgItem, note};
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && (to == 0 || to == i)) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SetSummonHandler(SummonHandler handler) { summonHandler = handler; }
void SetShotHandler(ShotHandler handler) { shotHandler = handler; }
void SetRagdollHandler(RagdollHandler handler) { ragdollHandler = handler; }
void SetDialogLineHandler(DialogLineHandler handler) { dialogLineHandler = handler; }
void SetOfferHandlers(OfferHandler offer, OfferResultHandler result)
{
    offerHandler = offer;
    offerResultHandler = result;
}
void SetPlayerSpellHandler(PlayerSpellHandler handler) { playerSpellHandler = handler; }
void SetRansackHandler(RansackHandler handler) { ransackHandler = handler; }
void SetWitnessHandler(WitnessHandler handler) { witnessHandler = handler; }
void SetNpcSpellHandler(NpcSpellHandler handler) { npcSpellHandler = handler; }

void SendNpcSpell(uint8_t to, char const* script, uint8_t const target[16], int32_t args)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    NpcSpellMsg message{MsgNpcSpell, localId, to, {}, {}, args};
    strncpy_s(message.script, script, _TRUNCATE);
    std::memcpy(message.target, target, 16);
    if (!isHost) { if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true); return; }
    if (peers[to]) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

void SetEnclaveHandler(EnclaveHandler handler) { enclaveHandler = handler; }

void SendEnclave(EnclaveNote const& note, uint8_t to)
{
    if (!host) return;
    const EnclaveMsg message{MsgEnclave, note};
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && (to == 0 || to == i)) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendPlayerSpell(uint8_t to, char const* script)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    PlayerSpellMsg message{MsgPlayerSpell, localId, to, {}};
    strncpy_s(message.script, script, _TRUNCATE);
    if (!isHost) { if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true); return; }
    if (peers[to]) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

void SendRansack(uint8_t to, RansackNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const RansackMsg message{MsgRansack, localId, to, note};
    if (!isHost) { if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true); return; }
    if (peers[to]) Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

void SendWitness(uint8_t to, WitnessNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const WitnessMsg message{MsgWitness, localId, to, note};
    if (!isHost) { if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true); return; }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && (to == 0 || to == i)) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendShot(ShotNote const& note)
{
    if (!host) return;
    ShotMsg message{MsgShot, localId, note};
    message.note.epoch = worldEpoch;
    if (!message.note.sent) message.note.sent = enet_time_get();
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelCombat, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelCombat, true);
}

void SendRagdoll(RagdollNote const& note)
{
    if (!host || !isHost) return;
    RagdollMsg message{MsgRagdoll, localId, note};
    message.note.epoch = worldEpoch;
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelCombat, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelCombat, true);
}

void SendDialogLine(DialogLineNote const& note)
{
    if (!host) return;
    const DialogLineMsg message{MsgDialogLine, localId, note};
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

bool SendOffer(OfferNote const& note)
{
    if (!host || note.seller == 0 || note.seller >= MaxPlayers || note.seller == localId) return false;
    OfferMsg message{MsgOffer, note};
    message.note.buyer = localId;
    if (note.count > MaxOfferEntries) return false;
    const size_t size = OfferHeaderSize + note.count * sizeof(OfferEntry);
    if (!isHost) {
        if (!server || !connected) return false;
        Send(server, &message, size, ChannelReliable, true);
        return true;
    }
    if (!peers[note.seller] || !InWorld(note.seller)) return false;
    Send(peers[note.seller], &message, size, ChannelReliable, true);
    return true;
}

void SendOfferResult(OfferResultNote const& note)
{
    if (!host) return;
    OfferResultMsg message{MsgOfferResult, note};
    message.note.seller = localId;
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    if (note.buyer > 1 && note.buyer < MaxPlayers && peers[note.buyer]) Send(peers[note.buyer], &message, sizeof(message), ChannelReliable, true);
}

void SendSummon(SummonNote const& note)
{
    if (!host) return;
    SummonMsg message{MsgSummon, localId, note};
    message.note.epoch = worldEpoch;
    if (!isHost) {
        if (server && connected) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}

void SendTalk(bool begin, uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    TalkMsg message{MsgTalk, static_cast<uint8_t>(begin ? 1 : 0), {}};
    std::memcpy(message.guid, guid, 16);
    Send(server, &message, sizeof(message), ChannelReliable, true);
}

void SendStates(uint8_t kind, StateEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxStatesPerMessage || (count == 0 && isHost)) return;
    uint8_t buffer[sizeof(StatesHeader) + MaxStatesPerMessage * sizeof(StateEntry)];
    const StatesHeader header{MsgStates, kind, static_cast<uint8_t>(count)};
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(StateEntry));
    const size_t size = sizeof(header) + count * sizeof(StateEntry);
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i]) Send(peers[i], buffer, size, ChannelReliable, true);
    } else if (server && connected) {
        Send(server, buffer, size, ChannelReliable, true);
    }
}

void SendMotionName(uint32_t hash, char const* name)
{
    if (!host || !isHost) return;
    MotionNameMsg message{MsgMotionName, hash, {}};
    strncpy_s(message.name, name, _TRUNCATE);
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelReliable, true);
}
bool Connected() { return isHost ? connected : connected && inWorld; }
Remote const* Remotes() { return remotes; }

void Stop()
{
    // Tell the others right away; otherwise a closed game only shows up after the long peer timeout.
    if (host) {
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
        } else if (server) {
            enet_peer_disconnect_now(server, 0);
        }
        enet_host_flush(host);
    }
    steam::Stop();  // passes the goodbyes on before closing ENet
    if (host) enet_host_destroy(host);
    host = nullptr;
    server = nullptr;
    connected = inWorld = false;
    localId = 0;
    ResetNpcReplication();
    for (uint8_t i = 0; i < MaxPlayers; ++i) Drop(i);
    if (enetInitialized) {
        enet_deinitialize();
        enetInitialized = false;
    }
}

char const* StartupError() { return startupError; }

namespace {
bool StartFailed(char const* reason)
{
    strncpy_s(startupError, reason, _TRUNCATE);
    Logf("net: Steam-only connection failed: %s", startupError);
    Stop();
    return false;
}
}

bool Start(bool asHost)
{
    isHost = asHost;
    ResetNpcReplication();
    worldEpoch = npcPublication = npcSequence = 0;
    startupError[0] = '\0';
    if (enet_initialize() != 0) return StartFailed("ENet initialization failed");
    enetInitialized = true;
    target = ENetAddress{};
    if (enet_address_set_host_ip(&target, "127.0.0.1") != 0) return StartFailed("loopback address unavailable");
    host = enet_host_create(&target, isHost ? MaxPlayers - 1 : 1, ChannelCount, 0, 0);
    if (!host) return StartFailed("loopback ENet socket could not be created");
    host->mtu = Mtu;
    constexpr int PoseBurstBuffer = 4 * 1024 * 1024;
    if (enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, PoseBurstBuffer) < 0 ||
        enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, PoseBurstBuffer) < 0)
        return StartFailed("cannot configure the owned ENet socket for canonical pose bursts");
    if (!steam::Start(isHost, isHost ? host->address.port : 0)) return StartFailed(steam::Error());
    strcpy_s(localName, steam::PlayerName());
    Logf("net: player name from Steam: %s", localName);
    if (isHost) {
        localId = 1;
        connected = true;
        Logf("net: Steam-only host relay on 127.0.0.1:%u", host->address.port);
    } else {
        target.port = steam::RelayPort();
        if (!target.port) return StartFailed("Steam relay socket is unavailable");
        server = enet_host_connect(host, &target, ChannelCount, 0);
        if (!server) return StartFailed("connection to the Steam relay could not be created");
        lastConnectAttempt = enet_time_get();
        startedAt = enet_time_get();
        Logf("net: connecting over Steam (relay 127.0.0.1:%u): pending", target.port);
    }
    return true;
}

namespace {
void Service(PlayerState const& local, bool haveLocal)
{
    ENetEvent event;
    // At most 512 events a frame: a flood must not stall the game (Fable's review 22.10. 1.9); the rest waits for the next.
    for (int events = 0; events < 512 && enet_host_service(host, &event, 0) > 0; ++events) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (isHost && (event.peer->address.host != target.host || !steam::IsPeerRelay(event.peer->address.port))) {
                Log("net: rejected an ENet connection outside the linked Steam relays");
                enet_peer_reset(event.peer);
                break;
            }
            // World loads and the intro video stop the engine loop (and so this service call) for
            // many seconds; the default ENet timeouts dropped a loading player after about 9 s.
            enet_peer_timeout(event.peer, 0, 60000, 180000);
            // The pose stream is fixed-rate and already tolerates loss; ENet's RTT-jitter throttle only drops more
            // frames and shrinks the reliable window, so keep it at full rate (deceleration 0).
            enet_peer_throttle_configure(event.peer, 5000, 0, 0);
            // A new connection has not said hello yet. ENet hands out the peer of a player who left before, and its old
            // player id made the hello guard refuse every rejoin (stageU1, 22.10.).
            event.peer->data = nullptr;
            poseRoutes[event.peer - host->peers] = PoseRoute{};
            if (!isHost) {
                everConnected = true;
                Hello hello{MsgHello, helloVersion ? helloVersion : ProtocolVersion, {}, {}};
                strncpy_s(hello.name, localName, _TRUNCATE);
                std::memcpy(hello.parity, parity::Local(), sizeof(hello.parity));
                Send(event.peer, &hello, sizeof(hello), ChannelReliable, true);
                Log("net: connected, hello sent");
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            OnReceive(event.peer, event.packet);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (isHost) {
                const uint8_t id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(event.peer->data));
                Logf("net: player %u disconnected", id);
                Drop(id);
                event.peer->data = nullptr;
                const Leave leave{MsgLeave, id};
                for (uint8_t i = 2; i < MaxPlayers; ++i) if (peers[i]) Send(peers[i], &leave, sizeof(leave), ChannelReliable, true);
            } else {
                Log("net: disconnected from host; will reconnect");
                connected = false;
                inWorld = false;
                server = nullptr;
                for (uint8_t i = 0; i < MaxPlayers; ++i) Drop(i);
            }
            break;
        default:
            break;
        }
    }
    const uint32_t now = enet_time_get();
    if (!isHost && !server && !refused && now - lastConnectAttempt > 3000) {
        lastConnectAttempt = now;
        server = enet_host_connect(host, &target, ChannelCount, 0);
        Log(server ? "net: reconnecting" : "net: reconnect failed");
    }
    if (!connected || !haveLocal || now - lastSend < std::max<uint32_t>(8, 1000u / static_cast<uint32_t>(sendRate) - 1)) return;
    lastSend = now;
    State message{MsgState, local};
    message.state.id = localId;
    message.state.sequence = ++sequence;
    message.state.sent = now;
    static uint8_t previousFlags = 0;
    const bool reliable = message.state.flags != previousFlags;
    previousFlags = message.state.flags;
    strncpy_s(message.state.name, localName, _TRUNCATE);
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i) if (peers[i]) Send(peers[i], &message, sizeof(message), ChannelState, reliable);
    } else if (server) {
        Send(server, &message, sizeof(message), ChannelState, reliable);
    }
    enet_host_flush(host);
}
}

// Round trip and packet loss of every connection (Stufe 1, owner 27.10.: how much of the delay is the connection, Hamachi or
// Steam). ENet measures both itself; every 5 s one line per peer. Over Steam ENet runs through the relay, so this is the
// whole way.
bool rttLog = true;  // QA negative control: rttlogoff
enet_uint32 lastRttLog = 0;
void RttLine(ENetPeer* peer, char const* who, char* out, size_t size)
{
    auto const& poses = peer->channels[ChannelPoses];
    auto const& alt = peer->channels[ChannelPosesAlt];
    Format(out, size, "net: rtt=%u ms (+-%u) loss=%.1f%% sent=%u lost=%u throttle=%u/%u to %s "
                     "pose_send=%u/%u,%u/%u pose_recv=%u/%u,%u/%u waiting_bytes=%u",
           peer->roundTripTime, peer->roundTripTimeVariance,
           peer->packetLoss * 100.0 / ENET_PEER_PACKET_LOSS_SCALE, peer->packetsSent, peer->packetsLost,
           peer->packetThrottle, ENET_PEER_PACKET_THROTTLE_SCALE, who,
           poses.outgoingReliableSequenceNumber, poses.outgoingUnreliableSequenceNumber,
           alt.outgoingReliableSequenceNumber, alt.outgoingUnreliableSequenceNumber,
           poses.incomingReliableSequenceNumber, poses.incomingUnreliableSequenceNumber,
           alt.incomingReliableSequenceNumber, alt.incomingUnreliableSequenceNumber,
           static_cast<unsigned>(peer->totalWaitingData));
}
void RttLog()
{
    if (!rttLog) return;
    const enet_uint32 now = enet_time_get();
    if (now - lastRttLog < 5000) return;
    lastRttLog = now;
    char line[384];
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peers[i]->state == ENET_PEER_STATE_CONNECTED) { RttLine(peers[i], PeerName(i), line, sizeof(line)); Log(line); }
    } else if (server && connected) {
        RttLine(server, "host", line, sizeof(line));
        Log(line);
    }
}

void Tick(PlayerState const& local, bool haveLocal)
{
    if (!host) return;
    RttLog();
    steam::Pump();  // Steam -> ENet before the service
    Service(local, haveLocal);
    steam::Pump();  // what ENet sent in this frame goes out now, not a frame later
}

// QA: NETSTATS lines, one per connection, now; rttlogoff stops the 5-s lines.
void Stats(void (*log)(char const*))
{
    char line[384];
    int n = 0;
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peers[i]->state == ENET_PEER_STATE_CONNECTED) { RttLine(peers[i], PeerName(i), line, sizeof(line)); log(line + 5); ++n; }
    } else if (server && connected) {
        RttLine(server, "host", line, sizeof(line));
        log(line + 5);
        ++n;
    }
    Format(line, "NETSTATS peers=%d rtt_log=%d", n, rttLog ? 1 : 0);
    log(line);
}
void RttLogOff(void (*log)(char const*)) { rttLog = false; log("SWITCH rttlogoff ok=1"); }

uint32_t RoundTrip(uint8_t player)
{
    ENetPeer* peer = isHost ? (player < MaxPlayers ? peers[player] : nullptr) : (connected ? server : nullptr);
    return peer && peer->state == ENET_PEER_STATE_CONNECTED ? peer->roundTripTime : 0;
}

// QA, client (stage W1): leave the host and connect again at once, as a player whose connection dropped and came back.
void Reconnect(void (*log)(char const*))
{
    if (isHost || !server || !connected) { log("REFUSED reconnect: a connected client only"); return; }
    enet_peer_disconnect_now(server, 0);
    connected = false;
    inWorld = false;
    for (uint8_t i = 0; i < MaxPlayers; ++i) Drop(i);
    server = enet_host_connect(host, &target, ChannelCount, 0);
    lastConnectAttempt = enet_time_get();
    Log("net: QA reconnect: left the host and connecting again");
    log(server ? "RECONNECT ok=1" : "RECONNECT ok=0");
}
}
