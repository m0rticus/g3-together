// SPDX-License-Identifier: GPL-3.0-or-later
#include <enet/enet.h>

#include "net.h"

#include "enet_policy.h"
#include "latency.h"
#include "log.h"
#include "npcwire_adapter.h"
#include "npcwire_bones.h"
#include "npcwire_pose.h"
#include "npcwire_control.h"
#include "net_metrics.h"
#include "playerwire.h"
#include "parity.h"
#include "replication.h"
#include "saving_policy.h"
#include "saving_store.h"
#include "steam.h"
#include "ai.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cstddef>
#include "story_policy.h"

namespace g3t::net {
namespace {
constexpr uint16_t ProtocolVersion = saving::Protocol;  // 77: host-managed saves and guarded bone packing
enum : uint8_t { MsgHello = 1, MsgWelcome = 2, MsgState = 3, MsgLeave = 4, MsgNpcs = 5, MsgMotionName = 6, MsgHitNpc = 7, MsgHitPlayer = 8, MsgStates = 9, MsgTalk = 10, MsgItem = 11, MsgContents = 12, MsgContainerTx = 13, MsgTxResult = 14, MsgWatch = 15, MsgKilled = 16, MsgRefused = 17, MsgDeath = 18, MsgCampaign = 19, MsgFileChunk = 20, MsgFileDone = 21, MsgClock = 22, MsgWorldState = 23, MsgSummon = 24, MsgShot = 25, MsgPlayerSpell = 26, MsgNpcSpell = 27, MsgEnclave = 28, MsgWorld = 29, MsgDialogLine = 30, MsgGoods = 31 };
// UDP payload per datagram (ENet default 1392 = up to 1420-byte IP packets): VPNs such as Hamachi carry about 1400, so
// stay well below (C, re/findings/hamachi-net-c.md, 21.10.). ENet splits larger messages into fragments of this size.
constexpr enet_uint32 Mtu = 1200;
enum : uint8_t { ChannelState = 0, ChannelReliable = 1, ChannelNpcs = 2, ChannelCombat = 3, ChannelPoses = 4, ChannelPosesAlt = 5, ChannelBulk = 6, ChannelCount = 7 };
enum : uint8_t { MsgNpcRelease = 36, MsgPosedNpc = 39 };
enum : uint8_t { MsgSave = 40, MsgBoneDictionary = 41, MsgBoneAck = 42, MsgBoneRequest = 43, MsgCompactPosedNpc = 44 };
enum : uint8_t { MsgControlBatch = 45, MsgControlId = 46, MsgControlBaseline = 47,
                 MsgControlSignal = 48, MsgControlPose = 49 };
enum : uint8_t { MsgPlayerActivity = 50 };
enum : uint8_t { MsgPoseAck = 51 };
enum : uint8_t { MsgNpcRemoval = 52 };
constexpr uint8_t MsgCharacter = 53;
constexpr uint8_t MsgPlayerResync = 60;
constexpr uint8_t MsgNpcLease = 61;

#pragma pack(push, 1)
struct Hello { uint8_t type; uint16_t version; char name[24]; uint64_t parity[parity::ItemCount]; };
struct RefusedMsg { uint8_t type; char reason[160]; };
struct ClockMsg { uint8_t type; ClockState state; };
struct WorldStateMsg { uint8_t type; WorldState state; };
struct Welcome { uint8_t type; uint8_t id; };
struct State { uint8_t type; PlayerState state; };
struct ActivityMsg {
    uint8_t type, id, flags;
    uint32_t sequence, world, epoch, downedSequence;
};
struct PlayerResyncMsg { uint8_t type, player; };
struct Leave { uint8_t type; uint8_t id; };
struct NpcsHeader {
    uint8_t type;
    uint32_t sequence, sent, epoch, generation, publication;
    uint8_t count, owner;
};
struct BoneRequestMsg { uint8_t type; uint32_t epoch; uint16_t layoutId; };
struct BonePosePrefix { uint8_t flags, intervalMs; uint16_t layoutId; };
struct PoseAckMsg { uint8_t type; uint32_t epoch; uint64_t generation; uint8_t guid[16]; uint32_t incarnation, token; };
struct NpcReleaseHeader {
    uint8_t type;
    uint32_t sequence, epoch, generation, publication;
    uint8_t count, owner;
};
struct NpcRemovalMsg { uint8_t type; NpcRemovalNote note; };
struct MotionNameMsg { uint8_t type; uint32_t hash; char name[96]; };
// to: the player who applies it (the NPC's owner, the hit player); from: who sent it (set by the host).
struct HitNpcMsg { uint8_t type; uint8_t guid[16]; int32_t delta; uint8_t weapon[16]; uint8_t to; uint8_t from; uint8_t npc[16]; uint8_t outcome; uint32_t seen; uint8_t reaction; uint32_t epoch, sequence, incarnation; MeleeContact contact; };
static_assert(sizeof(HitNpcMsg) == 112, "NPC hit wire layout changed");
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
struct SaveMsg { uint8_t type; SaveNote note; };
struct CharacterMsg { uint8_t type; CharacterNote note; };
#pragma pack(pop)
static_assert(sizeof(NpcsHeader) == 23, "NPC datagram budget must match its wire header");
static_assert(sizeof(SaveMsg) == 122, "save message wire layout changed");
static_assert(sizeof(CharacterMsg) == 88, "character envelope wire layout changed");
static_assert(sizeof(ActivityMsg) == 19, "player activity heartbeat wire layout changed");
static_assert(sizeof(BoneRequestMsg) == 7, "Bone request wire layout changed");
static_assert(sizeof(BonePosePrefix) == 4 && sizeof(PoseAckMsg) == 37, "Pose interval/ack wire layout changed");
static_assert(offsetof(BoneRequestMsg, layoutId) == 5 && offsetof(BonePosePrefix, layoutId) == 2,
              "Bone request or pose field offsets changed");
static_assert(sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader) + sizeof(npcwire::WireControl) +
              12 + npcwire::MotionSlots * 8 + sizeof(npcwire::WireQuaternion) +
              npcwire::MaxMetadataBytes <= NpcDatagramBudget,
              "largest compact NPC record must fit the unfragmented budget");

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
uint32_t lastActivitySend = 0;
uint8_t lastActivityFlags = 0;
uint32_t lastActivityDowned = 0;
bool activitySent = false;
int sendRate = 30;
uint32_t budgetRate = 256 * 1024;
uint32_t relayEnvelopeBytes = 16;
PeerBudget peerBudgets[MaxPlayers];
uint32_t budgetLossAt[MaxPlayers]{}, budgetLastLost[MaxPlayers]{};
playerwire::Encoder playerSenders[MaxPlayers][MaxPlayers];
playerwire::Decoder playerReceivers[MaxPlayers];
uint32_t playerRequestAt[MaxPlayers]{};
PlayerState currentLocal;
bool haveCurrentLocal = false;
playerwire::Cadence playerCadence;
bool forcePlayerFull[MaxPlayers][MaxPlayers]{};
static_assert(1 + playerwire::FixedBytes + 352 <= 1168, "full compact player keyframe must fit ENet without fragmentation");
Remote remotes[MaxPlayers] = {};
uint32_t joins = 0;
uint32_t npcSequence = 0;
uint32_t npcPublication = 0, worldEpoch = 0;
std::unordered_map<std::string, replication::Sequence> npcOrigins[MaxPlayers];
std::unordered_map<std::string, replication::ReplicaOrder> npcPublications;
unsigned npcAccepted = 0, npcRejected = 0, npcStale = 0;
unsigned poseSent = 0, poseReceived = 0, poseSendFailed = 0;
namespace bonewire = npcwire::bones;
bonewire::Dictionary boneLayouts;
std::unordered_map<std::string, bonewire::PoseCache> sentPoses[MaxPlayers], decodedPoses;
// Generation and layout IDs are never reused during this process, even on a
// replication reset. Delayed packets cannot acquire a new layout's identity.
uint64_t boneGeneration = 0, receivedBoneGeneration = 0;
bool boneGenerationExhausted = false;
uint32_t nextBoneLayout = 0;
std::unordered_map<uint16_t, uint32_t> boneOffered[MaxPlayers];
std::unordered_set<uint16_t> boneAcknowledged[MaxPlayers];
uint32_t requestedBoneEpoch = 0, requestedBoneAt = 0;
uint16_t requestedBoneLayout = 0;
bool boneExhaustionLogged = false;
unsigned boneLayoutLogs = 0, boneAckLogs = 0, boneFullPoseLogs = 0, boneCompactPoseLogs = 0;
unsigned boneInvalidLogs = 0, boneRequestLogs = 0;
unsigned boneFallbackFailureLogs = 0, boneDefinitionFailureLogs = 0;
struct ControlState {
    uint16_t id = 0, nextBaseline = 0;
    uint32_t nextAnchor = 0;
    npcwire::BaselineRef current{};
    NpcRecord latest{};
    uint8_t lifeFlags = 0;
};
struct ControlOffer { npcwire::BaselineRef ref{}; uint32_t at = 0, serial = 0; };
npcwire::Context controlSender, controlReceiver;
uint32_t controlGeneration = 0, receivedControlGeneration = 0, nextControlId = 0;
uint32_t nextControlOffer = 0;
bool controlGenerationExhausted = false, controlIdExhaustedLogged = false;
std::unordered_map<std::string, ControlState> controlStates;
std::unordered_map<uint16_t, std::string> controlKeys;
std::unordered_map<uint16_t, ControlOffer> controlOffered[MaxPlayers];
std::unordered_map<uint16_t, npcwire::BaselineRef> controlAcknowledged[MaxPlayers];
uint32_t requestedControlAt = 0, requestedControlGeneration = 0;
uint16_t requestedControlId = 0;
unsigned controlBaselineLogs = 0, controlRequestLogs = 0, controlBatchLogs = 0, controlErrors = 0;
bool SendFullNpc(ENetPeer* peer, NpcRecord const& record, bool reliable);
detail::BandwidthCounters bandwidth;
uint32_t bandwidthSince = 0, lastSentData = 0, lastSentPackets = 0, lastReceivedData = 0, lastReceivedPackets = 0;
NpcHandler npcHandler = nullptr;
NpcLeaseHandler npcLeaseHandler = nullptr;
NpcPoseHandler npcPoseHandler = nullptr;
PlayerStateHandler playerStateHandler = nullptr;
NpcReleaseHandler npcReleaseHandler = nullptr;
NpcRemovalHandler npcRemovalHandler = nullptr;
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
ClockHandler clockHandler = nullptr;
WorldStateHandler worldStateHandler = nullptr;
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
SaveHandler saveHandler = nullptr;
CharacterHandler characterHandler = nullptr;
bool inWorld = false;                  // client side: this machine is in the host's world (persist.cpp)
bool clientInWorld[MaxPlayers] = {};   // host side: that client is in the host's world
uint32_t generations[MaxPlayers] = {}; // host side: counts the connections a slot was given (PeerGeneration)
uint32_t worldEntries = 0;             // client side: local world gate openings
std::vector<uint8_t> goodsCache[MaxPlayers];  // host: validated, sender-stamped latest snapshots
uint8_t goodsReplay[MaxPlayers] = {};        // host: source bits still to queue for each joining recipient
static_assert(MaxPlayers <= 8, "goods replay bitset must cover every player slot");
unsigned gatedIn = 0, gatedOut = 0;
bool haveQueued = false;
uint32_t queuedAt = 0, lastServiceAt = 0;
latency::Samples enqueueDelays, serviceGaps;
unsigned serviceCaps = 0, serviceErrors = 0;
steam::TrafficCounters lastSteamTraffic;
struct BulkRefusal {
    uint32_t at = 0;
    int reason = -1;
};
BulkRefusal bulkRefusals[MaxPlayers];

size_t PeerIndex(ENetPeer const* peer) { return static_cast<size_t>(peer - host->peers); }

uint32_t QueueBytes(ENetList const& list)
{
    uint64_t bytes = 0;
    for (auto* node = list.sentinel.next; node != &list.sentinel; node = node->next)
        bytes += reinterpret_cast<ENetOutgoingCommand const*>(node)->fragmentLength;
    return static_cast<uint32_t>((std::min)(bytes, uint64_t(UINT32_MAX)));
}

BudgetClass SendPriority(uint8_t kind, uint8_t channel, bool reliable)
{
    if (channel == ChannelBulk) return BudgetClass::BulkReliable;
    if (reliable) return BudgetClass::GameplayReliable;
    if (kind == MsgState) return BudgetClass::PlayerState;
    return BudgetClass::NpcActive;
}

void ResetBudget(ENetPeer* peer)
{
    BudgetConfig config;
    config.bytesPerSecond = budgetRate;
    config.mtuBytes = peer->mtu;
    config.envelopeBytes = relayEnvelopeBytes;
    config.checksumBytes = host->checksum ? 4 : 0;
    peerBudgets[PeerIndex(peer)].Reset(enet_time_get(), config);
    budgetLossAt[PeerIndex(peer)] = budgetLastLost[PeerIndex(peer)] = 0;
}

void UpdateBudgets(uint32_t now)
{
    for (size_t i = 0; i < host->peerCount; ++i) {
        ENetPeer const& peer = host->peers[i];
        if (peer.state != ENET_PEER_STATE_CONNECTED) continue;
        BudgetTelemetry sample;
        const uint64_t queued = uint64_t(QueueBytes(peer.outgoingCommands)) + QueueBytes(peer.outgoingSendReliableCommands);
        sample.queuedPayloadBytes = static_cast<uint32_t>((std::min)(queued, uint64_t(UINT32_MAX)));
        sample.reliableInFlightBytes = peer.reliableDataInTransit;
        sample.reliableWindowBytes = (std::max)(peer.mtu, peer.packetThrottle * peer.windowSize / ENET_PEER_PACKET_THROTTLE_SCALE);
        sample.rttMs = peer.roundTripTime;
        sample.throttle = peer.packetThrottle;
        sample.validRtt = peer.lastReceiveTime != 0 && peer.packetsSent >= 32;
        if (peer.packetsLost != budgetLastLost[i]) {
            budgetLastLost[i] = peer.packetsLost;
            budgetLossAt[i] = now;
        }
        sample.packetsSent = peer.packetsSent;
        sample.packetsLost = peer.packetsLost;
        sample.lossPermille = peer.packetsSent >= 32
            ? static_cast<uint32_t>((std::min)(uint64_t(1000), uint64_t(peer.packetsLost) * 1000 / peer.packetsSent)) : 0;
        sample.validLoss = peer.packetsSent >= 32;
        sample.retrySampleAt = budgetLossAt[i];
        peerBudgets[i].Advance(now, sample);
    }
}

// What passes the world gate: joining, small save/character controls and world messages. Motion names wait for the world too
// (Fable's review 22.10. 1.8: they were open to a peer that never said Hello); the host resends them all on entry.
bool Gated(uint8_t type)
{
    switch (type) {
    case MsgHello: case MsgWelcome: case MsgRefused: case MsgLeave:
    case MsgWorld: case MsgSave: case MsgCharacter:
    case MsgPlayerActivity:
        return false;
    default:
        return true;
    }
}

bool StoryHandshake(void const* data, size_t size)
{
    if (size < sizeof(StatesHeader) || static_cast<uint8_t const*>(data)[0] != MsgStates) return false;
    StatesHeader header{};
    std::memcpy(&header, data, sizeof(header));
    const uint8_t kind = header.kind & story::policy::KindMask;
    return (header.kind & story::policy::Snapshot) && kind >= StateQuests && kind <= StateWorldClock &&
           header.count <= MaxStatesPerMessage && size == sizeof(header) + header.count * sizeof(StateEntry) &&
           (!(header.kind & story::policy::End) || !header.count) &&
           (!(header.kind & story::policy::Catchup) || (header.count && !(header.kind & story::policy::End)));
}

bool DoorHandshake(void const* data, size_t size)
{
    if (size != sizeof(WorldStateMsg) || static_cast<uint8_t const*>(data)[0] != MsgWorldState) return false;
    WorldStateMsg message{};
    std::memcpy(&message, data, sizeof(message));
    return message.state.kind == 10 && (message.state.npcClass == 0x80 ||
           ((message.state.npcClass & 0x40) && !(message.state.npcClass & ~0x43)));
}

bool XpHandshake(void const* data, size_t size)
{
    if (size != sizeof(WorldStateMsg) || static_cast<uint8_t const*>(data)[0] != MsgWorldState) return false;
    WorldStateMsg message;
    std::memcpy(&message, data, sizeof(message));
    return message.state.kind == 19 && message.state.dialog >= 1 && message.state.dialog <= 3;
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
    if (!host || !peer || (size && !data)) return false;
    if (size && Gated(static_cast<uint8_t const*>(data)[0]) && !XpHandshake(data, size) &&
        !(reliable && (StoryHandshake(data, size) || DoorHandshake(data, size))) && !Open(peer)) { ++gatedOut; return false; }
    const BudgetClass priority = SendPriority(size ? static_cast<uint8_t const*>(data)[0] : 0, channel, reliable);
    const uint64_t charge = BudgetCharge(size, peer->mtu, relayEnvelopeBytes, host->checksum ? 4 : 0);
    PeerBudget& budget = peerBudgets[PeerIndex(peer)];
    if (priority == BudgetClass::BulkReliable) {
        const auto decision = budget.Preview(priority, charge);
        budget.RecordDecision(decision);
        if (decision != BudgetDecision::Allow) {
            auto& refusal = bulkRefusals[PeerIndex(peer)];
            const uint32_t now = enet_time_get();
            if (!refusal.at || refusal.reason != static_cast<int>(decision) || now - refusal.at >= 5000) {
                refusal = {now | 1u, static_cast<int>(decision)};
                const auto snapshot = budget.Snapshot();
                Logf("net: checkpoint bulk deferred player=%u kind=%u bytes=%zu charge=%llu decision=%s credit=%lld pressure=%u queue=%u in_flight=%u",
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(peer->data)), size ? static_cast<uint8_t const*>(data)[0] : 0,
                     size, charge, decision == BudgetDecision::NeedsChunking ? "NeedsChunking" : "Defer",
                     static_cast<long long>(snapshot.bulkCreditBytes), static_cast<unsigned>(snapshot.pressure),
                     QueueBytes(peer->outgoingCommands) + QueueBytes(peer->outgoingSendReliableCommands), peer->reliableDataInTransit);
            }
            return false;
        }
    }
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
    ENetPacket* packet = enet_packet_create(data, size, PacketFlags(reliable));
    if (!packet) { Log("net: outgoing packet allocation failed"); return false; }
    const int queued = enet_peer_send(peer, channel, packet);
    if (queued < 0) {
        enet_packet_destroy(packet);
        if (priority != BudgetClass::BulkReliable) {
            Log("net: outgoing packet could not be queued");
            return false;
        }
        Logf("net: outgoing bulk packet could not be queued channel=%u bytes=%zu enet=%d", channel, size, queued);
        return false;
    }
    budget.CommitQueued(priority, charge);
    if (!haveQueued) { haveQueued = true; queuedAt = enet_time_get(); }
    if (size) bandwidth.Tx(static_cast<uint8_t const*>(data)[0], size);
    return true;
}

uint8_t DestinationId(ENetPeer const* peer)
{
    return isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
}

bool SendPlayerState(ENetPeer* peer, PlayerState const& state)
{
    const uint8_t to = DestinationId(peer);
    if (!to || to >= MaxPlayers || !state.id || state.id >= MaxPlayers) return false;
    auto& encoder = playerSenders[to][state.id];
    playerwire::Packet encoded;
    const uint32_t now = enet_time_get();
    if (encoder.Plan(state, now, forcePlayerFull[to][state.id], encoded) != playerwire::Status::Ok) {
        Logf("net: local player state refused by compact encoder player=%u", state.id);
        return false;
    }
    std::vector<uint8_t> message;
    message.reserve(1 + encoded.bytes.size());
    message.push_back(MsgState);
    message.insert(message.end(), encoded.bytes.begin(), encoded.bytes.end());
    if (!Send(peer, message.data(), message.size(), ChannelState, encoded.reliable)) return false;
    encoder.Commit(state, now, encoded);
    forcePlayerFull[to][state.id] = false;
    return true;
}

bool SendBoneControl(ENetPeer* peer, uint8_t type, std::vector<uint8_t> const& payload)
{
    std::vector<uint8_t> message;
    message.reserve(1 + payload.size());
    message.push_back(type);
    message.insert(message.end(), payload.begin(), payload.end());
    return Send(peer, message.data(), message.size(), ChannelReliable, true);
}

uint16_t BoneWire16(uint8_t const* data)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

uint64_t BoneWire64(uint8_t const* data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
    return value;
}

void ForgetBonePeer(uint8_t id)
{
    boneOffered[id].clear();
    boneAcknowledged[id].clear();
    sentPoses[id].clear();
}

void ForgetControlPeer(uint8_t id)
{
    controlOffered[id].clear();
    controlAcknowledged[id].clear();
}

bool BoneLog(unsigned& count)
{
    if (count >= 16) return false;
    ++count;
    return true;
}

void RequestBoneLayout(ENetPeer* peer, uint16_t layoutId)
{
    const uint32_t now = enet_time_get();
    if (requestedBoneEpoch == worldEpoch && requestedBoneLayout == layoutId &&
        static_cast<uint32_t>(now - requestedBoneAt) < 1000) return;
    std::vector<uint8_t> request;
    if (bonewire::EncodeRequest(worldEpoch, layoutId, request) == bonewire::Status::Ok &&
        SendBoneControl(peer, MsgBoneRequest, request)) {
        requestedBoneEpoch = worldEpoch;
        requestedBoneLayout = layoutId;
        requestedBoneAt = now;
        if (BoneLog(boneRequestLogs))
            Logf("net: requested unknown bone layout %u epoch %u", static_cast<unsigned>(layoutId), worldEpoch);
    } else if (BoneLog(boneDefinitionFailureLogs)) {
        Logf("net: could not request bone layout %u epoch %u", static_cast<unsigned>(layoutId), worldEpoch);
    }
}

void FlushGoodsReplays()
{
    if (!isHost) return;
    for (uint8_t to = 2; to < MaxPlayers; ++to) {
        if (!peers[to] || !clientInWorld[to]) { goodsReplay[to] = 0; continue; }
        for (uint8_t from = 2; from < MaxPlayers; ++from) {
            const uint8_t bit = static_cast<uint8_t>(1u << from);
            if (!(goodsReplay[to] & bit)) continue;
            auto const& snapshot = goodsCache[from];
            if (!clientInWorld[from] || snapshot.empty() ||
                Send(peers[to], snapshot.data(), snapshot.size(), ChannelReliable, true))
                goodsReplay[to] &= static_cast<uint8_t>(~bit);
        }
    }
}

void SendToServer(void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (!isHost && server && connected) Send(server, data, size, channel, reliable);
}

void SendToPeer(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t to)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    if (to < MaxPlayers && peers[to]) Send(peers[to], data, size, channel, reliable);
}

void SendBroadcast(void const* data, size_t size, uint8_t channel, bool reliable)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i]) Send(peers[i], data, size, channel, reliable);
}

void SendBroadcastExcept(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t except)
{
    if (!isHost) { SendToServer(data, size, channel, reliable); return; }
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && i != except) Send(peers[i], data, size, channel, reliable);
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
    r.activity.Accept(state.sequence, enet_time_get(), state.world, worldEpoch, state.flags, state.downedSequence);
    if (r.latestTicks && static_cast<int32_t>(state.sequence - r.latest.sequence) <= 0) {
        if ((r.latest.flags & 14) != (state.flags & 14))
            Logf("downed: remote stale player=%u packet=%u current=%u flags=%u current_flags=%u source=player-state",
                 state.id, state.sequence, r.latest.sequence, state.flags, r.latest.flags);
        return;
    }
    if (!r.latestTicks || (r.latest.flags & 14) != (state.flags & 14) || r.latest.downedSequence != state.downedSequence)
        Logf("downed: remote player=%u packet=%u downed_sequence=%u flags=%u previous=%u source=player-state",
             state.id, state.sequence, state.downedSequence, state.flags, r.latest.flags);
    const bool reset = !r.latestTicks || r.latest.world != state.world || std::strcmp(r.latest.shape, state.shape) ||
                       std::strcmp(r.latest.look, state.look);
    r.previous = r.latestTicks ? r.latest : state;
    r.previousTicks = r.latestTicks ? r.latestTicks : enet_time_get();
    r.latest = state;
    r.latestTicks = enet_time_get();
    r.connected = true;
    if (playerStateHandler) playerStateHandler(state, r.latestTicks, reset);
}

void ClearPresentation(uint8_t id)
{
    Remote cleared{};
    cleared.activity = remotes[id].activity;
    cleared.connected = remotes[id].connected;
    strncpy_s(cleared.latest.name, remotes[id].latest.name, _TRUNCATE);
    remotes[id] = cleared;
}

void Drop(uint8_t id)
{
    if (id == 0 || id >= MaxPlayers) return;
    ForgetBonePeer(id);
    ForgetControlPeer(id);
    remotes[id] = Remote{};
    playerReceivers[id] = {};
    playerRequestAt[id] = 0;
    for (auto& sender : playerSenders[id]) sender = {};
    for (auto& senders : playerSenders) senders[id] = {};
    for (auto& forced : forcePlayerFull[id]) forced = false;
    for (auto& forced : forcePlayerFull) forced[id] = false;
    peers[id] = nullptr;
    clientInWorld[id] = false;
    goodsCache[id].clear();
    goodsReplay[id] = 0;
    for (auto& pending : goodsReplay) pending &= static_cast<uint8_t>(~(1u << id));
    npcOrigins[id].clear();
    hitClaims[id] = replication::Sequence{};
}

bool Sane(NpcRecord const& record)
{
    if (!record.incarnation || record.hitPlayer >= MaxPlayers || record.guidePlayer >= MaxPlayers ||
        record.partyType > 8 || record.waiting > 1 || record.contact > 1 || record.attitude > 8 ||
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

bool SameControlRef(npcwire::BaselineRef const& a, npcwire::BaselineRef const& b)
{
    return a.id == b.id && a.baseline == b.baseline && a.anchor == b.anchor && a.incarnation == b.incarnation;
}

uint32_t ControlWire32(uint8_t const* data)
{
    return static_cast<uint32_t>(BoneWire16(data)) | (static_cast<uint32_t>(BoneWire16(data + 2)) << 16);
}

bool ControlStream(uint8_t const* data, size_t size, uint8_t kind, npcwire::Stream& stream)
{
    if (!data || size < sizeof(npcwire::WireBatchHeader) || data[0] != 'N' || data[1] != kind ||
        data[2] != 1 || data[3] != 0) return false;
    stream = {ControlWire32(data + 4), ControlWire32(data + 8)};
    return stream.epoch && stream.epoch == worldEpoch && stream.generation;
}

struct PreparedControl {
    npcwire::Control value{};
    npcwire::BaselineRef ref{};
    bool available = false, encodable = false;
};

PreparedControl PrepareControl(NpcRecord const& record)
{
    PreparedControl result;
    const npcwire::Stream stream{worldEpoch, controlGeneration};
    if (!stream.epoch || !stream.generation) return result;
    if (!controlSender.Matches(stream)) {
        controlSender = npcwire::Context{};
        controlStates.clear();
        controlKeys.clear();
        for (uint8_t i = 0; i < MaxPlayers; ++i) ForgetControlPeer(i);
        if (!controlSender.Reset(stream.epoch, stream.generation)) return result;
    }
    npcwire::Control full;
    if (!npcwire::FromNpcRecord(record, 0, 0, true, full)) return result;
    const std::string key = NpcKey(record.guid);
    auto found = controlStates.find(key);
    if (found == controlStates.end()) {
        if (nextControlId >= UINT16_MAX) {
            if (!controlIdExhaustedLogged) {
                controlIdExhaustedLogged = true;
                Log("net: NPC control IDs exhausted; using full-record fallback");
            }
            return result;
        }
        ControlState state;
        state.id = static_cast<uint16_t>(++nextControlId);
        if (!controlSender.AddId(stream, state.id, full.guid)) return result;
        found = controlStates.emplace(key, state).first;
        controlKeys.emplace(state.id, key);
    }
    auto& state = found->second;
    bool needsBaseline = !state.current.id || state.current.incarnation != record.incarnation ||
                         state.lifeFlags != (record.flags & 0x83u);
    npcwire::Control dynamic;
    if (!npcwire::FromNpcRecord(record, state.current.anchor, state.current.baseline, false, dynamic)) return result;
    if (!needsBaseline) {
        auto probe = dynamic;
        for (auto& motion : probe.motions)
            if (motion.active & 1u) { motion.phase = 0.0f; motion.weight = 0.0f; }
        std::vector<uint8_t> ignored;
        needsBaseline = !npcwire::EncodeRecord(controlSender, probe, ignored);
    }
    if (needsBaseline) {
        if (state.nextBaseline == UINT16_MAX || state.nextAnchor == UINT32_MAX) {
            if (BoneLog(controlErrors)) Logf("net: NPC control tokens exhausted for id %u", static_cast<unsigned>(state.id));
            return result;
        }
        const uint16_t token = ++state.nextBaseline;
        const uint32_t anchor = ++state.nextAnchor;
        if (!npcwire::FromNpcRecord(record, anchor, token, true, full) ||
            !controlSender.AdvanceLife(stream, state.id, record.incarnation) ||
            !controlSender.AddBaseline(stream, state.id, token, anchor, full)) {
            if (BoneLog(controlErrors)) Logf("net: NPC control baseline refused for id %u", static_cast<unsigned>(state.id));
            return result;
        }
        state.current = {state.id, token, record.incarnation, anchor};
        state.lifeFlags = record.flags & 0x83u;
        if (BoneLog(controlBaselineLogs))
            Logf("net: NPC control baseline id=%u token=%u anchor=%u",
                 static_cast<unsigned>(state.id), static_cast<unsigned>(token), anchor);
    }
    if (!npcwire::FromNpcRecord(record, state.current.anchor, state.current.baseline, false, dynamic)) return result;
    result.available = true;
    state.latest = record;
    result.ref = state.current;
    result.value = std::move(dynamic);
    std::vector<uint8_t> encoded;
    result.encodable = npcwire::EncodeRecord(controlSender, result.value, encoded);
    return result;
}

bool ControlReady(uint8_t peerId, PreparedControl const& prepared)
{
    const auto ack = controlAcknowledged[peerId].find(prepared.ref.id);
    return prepared.available && prepared.encodable && ack != controlAcknowledged[peerId].end() &&
           SameControlRef(ack->second, prepared.ref);
}

void OfferControl(uint8_t peerId, PreparedControl const& prepared, bool force = false)
{
    if (!prepared.available || !peers[peerId] || !clientInWorld[peerId]) return;
    const auto ack = controlAcknowledged[peerId].find(prepared.ref.id);
    if (!force && ack != controlAcknowledged[peerId].end() && SameControlRef(ack->second, prepared.ref)) return;
    const uint32_t now = enet_time_get();
    const auto offered = controlOffered[peerId].find(prepared.ref.id);
    if (!force && offered != controlOffered[peerId].end() && SameControlRef(offered->second.ref, prepared.ref) &&
        static_cast<uint32_t>(now - offered->second.at) < 1000) return;
    const bool retry = !force && offered != controlOffered[peerId].end() &&
                       SameControlRef(offered->second.ref, prepared.ref);
    if (!retry && nextControlOffer == UINT32_MAX) {
        if (BoneLog(controlErrors)) Log("net: NPC control offer serials exhausted; using full-record fallback");
        return;
    }
    const uint32_t serial = retry ? offered->second.serial : ++nextControlOffer;
    std::vector<uint8_t> id, baseline;
    if (npcwire::EncodeId(controlSender, serial, prepared.ref.id, id) &&
        npcwire::EncodeBaseline(controlSender, serial, prepared.ref.id, prepared.ref.baseline, baseline) &&
        SendBoneControl(peers[peerId], MsgControlId, id) &&
        SendBoneControl(peers[peerId], MsgControlBaseline, baseline)) {
        controlOffered[peerId][prepared.ref.id] = {prepared.ref, now, serial};
    } else if (BoneLog(controlErrors)) {
        Logf("net: could not offer NPC control id=%u to player=%u",
             static_cast<unsigned>(prepared.ref.id), static_cast<unsigned>(peerId));
    }
}

void RequestControl(ENetPeer* peer, npcwire::Stream stream, uint16_t id, uint32_t anchor, uint16_t baseline,
                    npcwire::ResyncReason reason)
{
    if (!id || !stream.epoch || !stream.generation || stream.epoch != worldEpoch) return;
    const uint32_t now = enet_time_get();
    if (requestedControlId == id && requestedControlGeneration == stream.generation &&
        static_cast<uint32_t>(now - requestedControlAt) < 1000) return;
    npcwire::Context future;
    npcwire::Context const* context = &controlReceiver;
    if (!controlReceiver.Matches(stream)) {
        if (!future.Reset(stream.epoch, stream.generation)) return;
        context = &future;
    }
    npcwire::Signal signal;
    signal.kind = npcwire::SignalKind::Resync;
    signal.reference.id = id;
    signal.reference.anchor = anchor;
    signal.reference.baseline = baseline;
    signal.reason = reason;
    std::vector<uint8_t> message;
    if (npcwire::EncodeSignal(*context, npcSequence, signal, message) &&
        SendBoneControl(peer, MsgControlSignal, message)) {
        requestedControlId = id;
        requestedControlGeneration = stream.generation;
        requestedControlAt = now;
        if (BoneLog(controlRequestLogs))
            Logf("net: requested NPC control id=%u reason=%u", static_cast<unsigned>(id), static_cast<unsigned>(reason));
    }
}

bool DecodeControls(ENetPeer* peer, NpcsHeader const& header, uint8_t const* data, size_t size,
                    std::vector<NpcRecord>& records, std::vector<uint8_t>& acceptedBatch)
{
    npcwire::Stream stream;
    if (!ControlStream(data, size, 'C', stream) || stream.generation < receivedControlGeneration ||
        size > npcwire::MaxBatchBytes || BoneWire16(data + 16) != header.count ||
        ControlWire32(data + 12) != header.publication) return false;
    size_t offset = sizeof(npcwire::WireBatchHeader);
    std::unordered_set<uint16_t> seen;
    std::vector<NpcRecord> result;
    std::vector<uint8_t> batch(data, data + sizeof(npcwire::WireBatchHeader));
    for (unsigned i = 0; i < header.count; ++i) {
        if (size - offset < sizeof(npcwire::WireControl)) return false;
        uint8_t const* raw = data + offset;
        const uint16_t id = BoneWire16(raw);
        const uint32_t anchor = ControlWire32(raw + offsetof(npcwire::WireControl, anchor));
        const uint16_t baseline = BoneWire16(raw + offsetof(npcwire::WireControl, baseline));
        size_t length = 0;
        if (!id || !anchor || !baseline || !seen.insert(id).second ||
            !npcwire::RecordBytes(raw, size - offset, length)) return false;
        npcwire::Guid guid;
        if (!controlReceiver.Matches(stream) || !controlReceiver.FindGuid(id, guid)) {
            RequestControl(peer, stream, id, anchor, baseline, npcwire::ResyncReason::UnknownId);
            return false;
        }
        npcwire::Control value;
        if (!npcwire::DecodeRecord(controlReceiver, raw, length, value)) {
            npcwire::Anchor saved;
            RequestControl(peer, stream, id, anchor, baseline,
                           controlReceiver.GetAnchor(id, anchor, saved) ? npcwire::ResyncReason::UnknownBaseline
                                                                       : npcwire::ResyncReason::UnknownAnchor);
            return false;
        }
        NpcRecord record;
        if (!npcwire::ToNpcRecord(value, record) || !Sane(record)) return false;
        const auto previous = npcPublications.find(NpcKey(record.guid));
        if (previous != npcPublications.end() && previous->second.publication.initialized &&
            !npcwire::Newer(header.publication, previous->second.publication.latest)) {
            ++npcStale;
        } else {
            result.push_back(record);
            batch.insert(batch.end(), raw, raw + length);
        }
        offset += length;
    }
    if (offset != size || result.empty()) return false;
    batch[16] = static_cast<uint8_t>(result.size());
    batch[17] = static_cast<uint8_t>(result.size() >> 8);
    records = std::move(result);
    acceptedBatch = std::move(batch);
    return true;
}

void OnReceive(ENetPeer* peer, ENetPacket* packet)
{
    if (packet->dataLength) bandwidth.Rx(packet->data[0], packet->dataLength);
    if (isHost && (peer->address.host != target.host || !steam::IsPeerRelay(peer->address.port))) return;
    if (packet->dataLength < 1) return;
    const uint8_t type = packet->data[0];
    if (type == MsgPosedNpc || type == MsgCompactPosedNpc || type == MsgControlPose) ++poseReceived;
    const bool xpHandshake = XpHandshake(packet->data, packet->dataLength);
    const bool storyHandshake = (packet->flags & ENET_PACKET_FLAG_RELIABLE) &&
                               (StoryHandshake(packet->data, packet->dataLength) || DoorHandshake(packet->data, packet->dataLength));
    if (Gated(type) && !xpHandshake && !storyHandshake && !Open(peer)) { ++gatedIn; return; }
    if (type == MsgNpcLease) {
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if ((isHost && (from < 2 || from >= MaxPlayers || peers[from] != peer)) ||
            (!isHost && (!connected || peer != server)) ||
            !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        npclease::Message note;
        if (!npclease::DecodeMessage(packet->data + 1, packet->dataLength - 1, note) ||
            note.world != worldEpoch) {
            Log("net: invalid NPC lease identity/state refused");
            return;
        }
        if (npcLeaseHandler) npcLeaseHandler(from, note);
        return;
    }
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
        for (auto& sender : playerSenders[id]) sender = {};
        for (auto& forced : forcePlayerFull[id]) forced = false;
        ++generations[id];
        peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
        remotes[id].connected = true; // joined; activity and presentation have separate readiness
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
        activitySent = false;
        ++welcomes;
        ResetNpcReplication();  // a new host connection has a fresh publication stream, even in the same campaign
        Logf("net: welcomed as player %u (welcome %u)", localId, welcomes);
    } else if (type == MsgPlayerActivity && packet->dataLength == sizeof(ActivityMsg)) {
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        ActivityMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (isHost) {
            const uintptr_t from = reinterpret_cast<uintptr_t>(peer->data);
            if (from < 2 || from >= MaxPlayers || peers[from] != peer) return;
            message.id = static_cast<uint8_t>(from);
        } else if (peer != server || !connected) {
            return;
        }
        if (!message.id || message.id >= MaxPlayers || message.id == localId ||
            (message.flags & 128) || ((message.flags & 1) && (message.flags & 96))) return;
        Remote& r = remotes[message.id];
        r.activity.Accept(message.sequence, enet_time_get(), message.world, message.epoch,
                          message.flags, message.downedSequence);
        r.connected = true;
        if (isHost)
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != message.id &&
                    !Send(peers[i], &message, sizeof(message), ChannelState, true))
                    Logf("net: player activity relay to player %u failed", i);
    } else if (type == MsgState) {
        if (!isHost && peer != server) return;
        if (packet->dataLength >= 1 + playerwire::FixedBytes &&
            (packet->data[2] || BoneWire16(packet->data + 1 + playerwire::FixedBytes - 2)) &&
            !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) {
            Log("net: unreliable player metadata refused");
            return;
        }
        uint8_t player = 0;
        if (!playerwire::PeekPlayerId(packet->data + 1, packet->dataLength - 1, player)) {
            Log("net: compact player state has an invalid header");
            return;
        }
        if (isHost) player = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
        if (!player || player >= MaxPlayers || player == localId) return;
        PlayerState state;
        const auto status = playerReceivers[player].Decode(packet->data + 1, packet->dataLength - 1, state, player);
        if (status == playerwire::Status::NeedKeyframe) {
            const uint32_t now = enet_time_get();
            if (!playerRequestAt[player] || now - playerRequestAt[player] >= 1000) {
                const PlayerResyncMsg request{MsgPlayerResync, player};
                if (Send(peer, &request, sizeof(request), ChannelReliable, true)) {
                    playerRequestAt[player] = now | 1u;
                    Logf("net: player metadata keyframe requested player=%u", player);
                }
            }
            return;
        }
        if (status == playerwire::Status::Stale) return;
        if (status != playerwire::Status::Ok || !Sane(state)) {
            Logf("net: invalid compact player state player=%u", player);
            return;
        }
        Store(state);
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && i != player) SendPlayerState(peers[i], state);
        }
    } else if (type == MsgPlayerResync && packet->dataLength == sizeof(PlayerResyncMsg) &&
               (packet->flags & ENET_PACKET_FLAG_RELIABLE)) {
        PlayerResyncMsg request{};
        std::memcpy(&request, packet->data, sizeof(request));
        const uint8_t to = DestinationId(peer);
        if (!to || to >= MaxPlayers || !request.player || request.player >= MaxPlayers ||
            (!isHost && (peer != server || request.player != localId))) return;
        forcePlayerFull[to][request.player] = true;
    } else if (type == MsgControlId) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return; }
        auto* data = packet->data + 1;
        const size_t size = packet->dataLength - 1;
        npcwire::Stream stream;
        if (!ControlStream(data, size, 'I', stream) || stream.generation < receivedControlGeneration) {
            ++npcRejected;
            return;
        }
        uint16_t id = 0;
        if (stream.generation > receivedControlGeneration) {
            npcwire::Context candidate;
            if (!candidate.Reset(stream.epoch, stream.generation) || !npcwire::DecodeId(candidate, data, size, id)) {
                ++npcRejected;
                return;
            }
            controlReceiver = std::move(candidate);
            receivedControlGeneration = stream.generation;
            requestedControlGeneration = 0;
        } else if (!npcwire::DecodeId(controlReceiver, data, size, id)) {
            ++npcRejected;
            return;
        }
    } else if (type == MsgControlBaseline) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return; }
        auto* data = packet->data + 1;
        const size_t size = packet->dataLength - 1;
        npcwire::Stream stream;
        if (!ControlStream(data, size, 'B', stream) || stream.generation < receivedControlGeneration ||
            size != sizeof(npcwire::WireBatchHeader) + sizeof(npcwire::WireBaselinePrefix) + sizeof(npcwire::WireFullRecord)) {
            ++npcRejected;
            return;
        }
        const uint16_t id = BoneWire16(data + sizeof(npcwire::WireBatchHeader));
        npcwire::Guid guid;
        if (!controlReceiver.Matches(stream) || !controlReceiver.FindGuid(id, guid)) {
            RequestControl(peer, stream, id, 0, 0, npcwire::ResyncReason::UnknownId);
            ++npcRejected;
            return;
        }
        npcwire::BaselineRef ref;
        if (!npcwire::DecodeBaseline(controlReceiver, data, size, ref)) { ++npcRejected; return; }
        npcwire::Signal ack;
        ack.reference = ref;
        std::vector<uint8_t> message;
        if (!npcwire::EncodeSignal(controlReceiver, ControlWire32(data + 12), ack, message) ||
            !SendBoneControl(peer, MsgControlSignal, message)) {
            if (BoneLog(controlErrors)) Log("net: could not acknowledge NPC control baseline");
        }
    } else if (type == MsgControlSignal) {
        if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return; }
        const uintptr_t peerId = reinterpret_cast<uintptr_t>(peer->data);
        if (peerId < 2 || peerId >= MaxPlayers || peers[peerId] != peer) { ++npcRejected; return; }
        npcwire::Signal signal;
        if (!npcwire::DecodeSignal(controlSender, packet->data + 1, packet->dataLength - 1, signal)) {
            ++npcRejected;
            return;
        }
        if (signal.kind == npcwire::SignalKind::Ack) {
            const auto offered = controlOffered[peerId].find(signal.reference.id);
            const auto key = controlKeys.find(signal.reference.id);
            const auto state = key == controlKeys.end() ? controlStates.end() : controlStates.find(key->second);
            if (offered == controlOffered[peerId].end() || state == controlStates.end() ||
                !SameControlRef(offered->second.ref, signal.reference) ||
                !SameControlRef(state->second.current, signal.reference) || offered->second.serial != signal.sequence) {
                ++npcRejected;
                return;
            }
            controlAcknowledged[peerId][signal.reference.id] = signal.reference;
        } else {
            const auto key = controlKeys.find(signal.reference.id);
            if (key == controlKeys.end()) { ++npcRejected; return; }
            const auto state = controlStates.find(key->second);
            if (state == controlStates.end() || !state->second.current.id) { ++npcRejected; return; }
            controlAcknowledged[peerId].erase(signal.reference.id);
            PreparedControl prepared;
            prepared.available = true;
            prepared.ref = state->second.current;
            OfferControl(static_cast<uint8_t>(peerId), prepared, true);
            SendFullNpc(peers[peerId], state->second.latest, true);
        }
    } else if (type == MsgBoneDictionary) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength < 1 + bonewire::HeaderBytes || !worldEpoch) {
            ++npcRejected;
            return;
        }
        uint8_t const* data = packet->data + 1;
        const size_t size = packet->dataLength - 1;
        const uint64_t generation = BoneWire64(data + 8);
        if (!generation || generation < receivedBoneGeneration) {
            ++npcRejected;
            if (BoneLog(boneInvalidLogs)) Log("net: rejected stale or invalid bone dictionary generation");
            return;
        }
        bonewire::Status status;
        if (generation > receivedBoneGeneration) {
            bonewire::Dictionary candidate;
            candidate.Reset(worldEpoch, generation);
            status = candidate.DecodeDictionary(data, size);
            if (status == bonewire::Status::Ok) {
                boneLayouts = std::move(candidate);
                decodedPoses.clear();
                receivedBoneGeneration = generation;
                requestedBoneEpoch = 0;
            }
        } else {
            status = boneLayouts.DecodeDictionary(data, size);
        }
        if (status != bonewire::Status::Ok) {
            ++npcRejected;
            if (BoneLog(boneInvalidLogs))
                Logf("net: bone dictionary rejected (%u)", static_cast<unsigned>(status));
            return;
        }
        const uint16_t layoutId = BoneWire16(data + 16);
        std::vector<uint8_t> ack;
        if (boneLayouts.EncodeAck(layoutId, ack) != bonewire::Status::Ok ||
            !SendBoneControl(peer, MsgBoneAck, ack)) {
            if (BoneLog(boneDefinitionFailureLogs))
                Logf("net: could not acknowledge bone layout %u", static_cast<unsigned>(layoutId));
        }
    } else if (type == MsgPoseAck) {
        if (!isHost || packet->dataLength != sizeof(PoseAckMsg)) { ++npcRejected; return; }
        const uintptr_t id = reinterpret_cast<uintptr_t>(peer->data);
        if (id < 2 || id >= MaxPlayers || peers[id] != peer) { ++npcRejected; return; }
        PoseAckMsg ack;
        std::memcpy(&ack, packet->data, sizeof(ack));
        const auto found = sentPoses[id].find(NpcKey(ack.guid));
        if (ack.epoch != worldEpoch || ack.generation != boneGeneration || !ack.incarnation ||
            found == sentPoses[id].end() || !found->second.Ack(ack.token, ack.incarnation)) {
            ++bandwidth.poseAckMisses;
            return;
        }
        ++bandwidth.poseAcks;
    } else if (type == MsgBoneAck || type == MsgBoneRequest) {
        if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return; }
        const uintptr_t id = reinterpret_cast<uintptr_t>(peer->data);
        if (id < 2 || id >= MaxPlayers || peers[id] != peer) { ++npcRejected; return; }
        if (type == MsgBoneAck) {
            uint16_t layoutId = 0;
            const auto status = boneLayouts.DecodeAck(packet->data + 1, packet->dataLength - 1, layoutId);
            if (status != bonewire::Status::Ok || !boneOffered[id].count(layoutId)) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs))
                    Logf("net: rejected bone acknowledgement (%u)", static_cast<unsigned>(status));
                return;
            }
            if (boneAcknowledged[id].insert(layoutId).second)
                if (BoneLog(boneAckLogs))
                    Logf("net: player %u acknowledged bone layout %u", static_cast<unsigned>(id), static_cast<unsigned>(layoutId));
        } else {
            std::vector<uint8_t> definition;
            const auto status = boneLayouts.RespondToRequest(packet->data + 1, packet->dataLength - 1, definition);
            if (status != bonewire::Status::Ok) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs))
                    Logf("net: rejected missing or invalid bone request (%u)", static_cast<unsigned>(status));
                return;
            }
            const uint16_t layoutId = BoneWire16(packet->data + 1 + 4);
            if (SendBoneControl(peer, MsgBoneDictionary, definition)) {
                boneOffered[id][layoutId] = enet_time_get();
            } else if (BoneLog(boneDefinitionFailureLogs)) {
                Logf("net: could not resend bone layout %u to player %u", static_cast<unsigned>(layoutId), static_cast<unsigned>(id));
            }
        }
    } else if ((type == MsgNpcs || type == MsgPosedNpc || type == MsgCompactPosedNpc ||
                type == MsgControlBatch || type == MsgControlPose) &&
               packet->dataLength >= sizeof(NpcsHeader)) {
        if (isHost) { ++npcRejected; return; }
        NpcsHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        if (!header.count || header.count > MaxNpcsPerMessage || header.epoch != worldEpoch || header.owner != 1) {
            ++npcRejected;
            return;
        }
        uint16_t boneCount = 0;
        uint8_t poseFlags = 0;
        uint8_t intervalMs = 16;
        uint16_t poseLayout = 0;
        size_t receivedPoseBytes = 0;
        bool receivedPoseKey = true;
        bonewire::PoseStats receivedPoseStats;
        const pose::Bone* bones = nullptr;
        std::vector<pose::Bone> decodedBones;
        std::vector<NpcRecord> controlRecords;
        std::vector<uint8_t> acceptedBatch;
        uint8_t const* controlData = nullptr;
        size_t controlSize = 0;
        const size_t recordsSize = sizeof(header) + header.count * sizeof(NpcRecord);
        if (type == MsgControlBatch || type == MsgControlPose) {
            size_t offset = sizeof(header);
            if (type == MsgControlPose) {
                if (header.count != 1 || packet->dataLength < offset + sizeof(uint16_t)) { ++npcRejected; return; }
                controlSize = BoneWire16(packet->data + offset);
                offset += sizeof(uint16_t);
                if (controlSize > packet->dataLength - offset) { ++npcRejected; return; }
            } else {
                if (packet->dataLength > NpcDatagramBudget) { ++npcRejected; return; }
                controlSize = packet->dataLength - offset;
            }
            controlData = packet->data + offset;
            std::vector<uint8_t> expandedControl;
            if (type == MsgControlPose) {
                if (controlSize < 4 + sizeof(npcwire::WireControl)) { ++npcRejected; return; }
                expandedControl.resize(sizeof(npcwire::WireBatchHeader));
                std::memcpy(expandedControl.data(), "NC\1\0", 4);
                std::memcpy(expandedControl.data() + 4, &header.epoch, 4);
                std::memcpy(expandedControl.data() + 8, controlData, 4);
                std::memcpy(expandedControl.data() + 12, &header.publication, 4);
                expandedControl[16] = header.count;
                expandedControl.insert(expandedControl.end(), controlData + 4, controlData + controlSize);
            }
            if (!DecodeControls(peer, header,
                                expandedControl.empty() ? controlData : expandedControl.data(),
                                expandedControl.empty() ? controlSize : expandedControl.size(),
                                controlRecords, acceptedBatch)) {
                ++npcRejected;
                if (BoneLog(controlErrors)) Log("net: compact NPC control decode failed");
                return;
            }
            offset += controlSize;
            if (type == MsgControlPose) {
                if (packet->dataLength < offset + 2 + bonewire::DeltaHeaderBytes + 1) {
                    ++npcRejected;
                    return;
                }
                poseFlags = packet->data[offset++];
                intervalMs = packet->data[offset++];
                if ((poseFlags & ~pose::Facial) || intervalMs < 16 || intervalMs > 100) {
                    ++npcRejected; return;
                }
                uint16_t layoutId = 0;
                auto const& record = controlRecords.front();
                const auto status = decodedPoses[NpcKey(record.guid)].Decode(boneLayouts, packet->data + offset,
                    packet->dataLength - offset, record.incarnation, poseFlags, decodedBones, layoutId, &receivedPoseStats);
                if (status == bonewire::Status::UnknownLayout) {
                    RequestBoneLayout(peer, layoutId);
                    ++npcRejected;
                    return;
                }
                if (status != bonewire::Status::Ok) {
                    if (status == bonewire::Status::UnknownPose) {
                        const PoseAckMsg request{MsgPoseAck, worldEpoch, receivedBoneGeneration, {}, record.incarnation, 0};
                        auto message = request;
                        std::memcpy(message.guid, record.guid, 16);
                        if (!Send(peer, &message, sizeof(message), ChannelState, false)) ++bandwidth.poseAckMisses;
                        ++bandwidth.poseReferenceMisses;
                    } else ++bandwidth.poseDecodeFailures;
                    ++npcRejected; return;
                }
                poseLayout = layoutId;
                receivedPoseBytes = packet->dataLength - sizeof(header) - 2 - controlSize;
                receivedPoseKey = bonewire::ReadPose32(packet->data + offset + 2) == 0;
                bones = decodedBones.data();
                boneCount = static_cast<uint16_t>(decodedBones.size());
            }
            std::vector<npcwire::Control> acceptedControls;
            if (!npcwire::DecodeBatch(controlReceiver, acceptedBatch.data(), acceptedBatch.size(), acceptedControls)) {
                ++npcRejected;
                return;
            }
        } else if (type == MsgPosedNpc) {
            if (header.count != 1 || packet->dataLength < recordsSize + sizeof(boneCount) + 2) { ++npcRejected; return; }
            std::memcpy(&boneCount, packet->data + recordsSize, sizeof(boneCount));
            std::memcpy(&poseFlags, packet->data + recordsSize + sizeof(boneCount), sizeof(poseFlags));
            intervalMs = packet->data[recordsSize + sizeof(boneCount) + 1];
            if (!boneCount || boneCount > pose::MaxBones ||
                (poseFlags & ~pose::Facial) ||
                packet->dataLength != recordsSize + sizeof(boneCount) + 2 + boneCount * sizeof(pose::Bone)) {
                ++npcRejected;
                return;
            }
            bones = reinterpret_cast<pose::Bone const*>(packet->data + recordsSize + sizeof(boneCount) + 2);
            receivedPoseBytes = packet->dataLength - recordsSize;
            for (int i = 0; i < boneCount; ++i) {
                if (!bones[i].name) { ++npcRejected; return; }
                for (int k = 0; k < i; ++k)
                    if (bones[k].name == bones[i].name) { ++npcRejected; return; }
            }
        } else if (type == MsgCompactPosedNpc) {
            if (header.count != 1 || packet->dataLength < recordsSize + 2 + bonewire::DeltaHeaderBytes + 1 ||
                !worldEpoch) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs)) Log("net: rejected truncated or invalid compact pose");
                return;
            }
            poseFlags = packet->data[recordsSize];
            intervalMs = packet->data[recordsSize + 1];
            if (poseFlags & ~pose::Facial) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs)) Logf("net: rejected compact pose flags=%u", static_cast<unsigned>(poseFlags));
                return;
            }
            uint8_t const* data = packet->data + recordsSize + 2;
            const size_t size = packet->dataLength - recordsSize - 2;
            uint16_t layoutId = 0;
            NpcRecord posedRecord;
            std::memcpy(&posedRecord, packet->data + sizeof(header), sizeof(posedRecord));
            if (!Sane(posedRecord)) { ++npcRejected; return; }
            const auto status = decodedPoses[NpcKey(posedRecord.guid)].Decode(boneLayouts, data, size,
                posedRecord.incarnation, poseFlags, decodedBones, layoutId, &receivedPoseStats);
            if (status == bonewire::Status::UnknownLayout) {
                RequestBoneLayout(peer, layoutId);
                ++npcRejected;
                return;
            }
            if (status != bonewire::Status::Ok) {
                if (status == bonewire::Status::UnknownPose) {
                    PoseAckMsg request{MsgPoseAck, worldEpoch, receivedBoneGeneration, {}, posedRecord.incarnation, 0};
                    std::memcpy(request.guid, posedRecord.guid, 16);
                    if (!Send(peer, &request, sizeof(request), ChannelState, false)) ++bandwidth.poseAckMisses;
                    ++bandwidth.poseReferenceMisses;
                } else ++bandwidth.poseDecodeFailures;
                ++npcRejected;
                if (BoneLog(boneInvalidLogs))
                    Logf("net: invalid compact pose layout (%u)", static_cast<unsigned>(status));
                return;
            }
            bones = decodedBones.data();
            poseLayout = layoutId;
            receivedPoseBytes = packet->dataLength - recordsSize;
            receivedPoseKey = bonewire::ReadPose32(data + 2) == 0;
            boneCount = static_cast<uint16_t>(decodedBones.size());
        } else if (packet->dataLength != recordsSize) { ++npcRejected; return; }
        if (bones && (intervalMs < 16 || intervalMs > 100)) { ++npcRejected; return; }
        if (isHost) {
            header.owner = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data));
            if (header.owner < 2 || header.owner >= MaxPlayers) return;
            header.generation = generations[header.owner];
        } else if (header.owner != 1) {
            ++npcRejected;
            return;
        }
        auto processRecords = [&](NpcRecord* records, int recordCount) {
            int count = 0;
            for (int i = 0; i < recordCount; ++i) {
                NpcRecord const& record = records[i];
                if (!Sane(record)) { ++npcRejected; continue; }
                const std::string key = NpcKey(record.guid);
                const bool newer = isHost ? npcOrigins[header.owner][key].Accept(header.sequence)
                                         : npcPublications[key].Snapshot(header.owner, header.publication);
                if (!newer) { ++npcStale; continue; }
                if (bones && poseLayout) {
                    decodedPoses[key].Store(header.publication, record.incarnation, poseLayout, poseFlags,
                                           bones, boneCount, enet_time_get(), false);
                    PoseAckMsg ack{MsgPoseAck, worldEpoch, receivedBoneGeneration, {}, record.incarnation, header.publication};
                    std::memcpy(ack.guid, record.guid, 16);
                    if (!Send(peer, &ack, sizeof(ack), ChannelState, false)) ++bandwidth.poseAckMisses;
                    else ++bandwidth.poseAcks;
                }
                if (bones) {
                    bandwidth.Pose(intervalMs, packet->dataLength, receivedPoseBytes, boneCount, receivedPoseKey);
                    if (!poseLayout) {
                        receivedPoseStats.headerBytes = 2 + boneCount * sizeof(uint32_t);
                        receivedPoseStats.rotationBits = boneCount * 144;
                        receivedPoseStats.translationBits = boneCount * 48;
                        receivedPoseStats.includedBones = boneCount;
                    }
                    bandwidth.PoseComponents(receivedPoseStats.headerBytes + 2, receivedPoseStats.maskBits,
                        receivedPoseStats.rotationBits, receivedPoseStats.translationBits, receivedPoseStats.includedBones,
                        receivedPoseKey, receivedPoseStats.predictive, packet->dataLength - receivedPoseBytes - sizeof(header),
                        sizeof(header), receivedPoseStats.rigidBones, 0, receivedPoseStats.PaddingBits());
                }
                if (bones && npcPoseHandler)
                    npcPoseHandler(record, bones, boneCount, poseFlags, (packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0, intervalMs);
                records[count++] = record;
            }
            return count;
        };
        auto finishRecords = [&](NpcRecord* records, int count) {
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
        };
        if (type == MsgControlBatch || type == MsgControlPose) {
            finishRecords(controlRecords.data(), processRecords(controlRecords.data(), static_cast<int>(controlRecords.size())));
        } else if (type == MsgPosedNpc || type == MsgCompactPosedNpc) {
            NpcRecord record;
            std::memcpy(&record, packet->data + sizeof(header), sizeof(record));
            finishRecords(&record, processRecords(&record, 1));
        } else {
            NpcRecord batchRecords[MaxNpcsPerMessage];
            std::memcpy(batchRecords, packet->data + sizeof(header), header.count * sizeof(NpcRecord));
            finishRecords(batchRecords, processRecords(batchRecords, header.count));
        }
    } else if (type == MsgNpcRemoval) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength != sizeof(NpcRemovalMsg)) {
            ++npcRejected;
            Log("net: invalid NPC removal sender or message");
            return;
        }
        NpcRemovalMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.note.epoch != worldEpoch || !message.note.incarnation) { ++npcStale; return; }
        decodedPoses.erase(NpcKey(message.note.guid));
        if (npcRemovalHandler) npcRemovalHandler(message.note);
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
            decodedPoses.erase(key);
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
        if (message.from < 2 || message.from >= MaxPlayers || message.to != 1 ||
            message.epoch != worldEpoch || !message.sequence ||
            !ValidHitContact(message.delta, message.incarnation, message.seen, message.outcome, message.reaction, message.contact) ||
            std::memcmp(message.npc, zero, 16)) { Log("net: invalid or stale NPC hit rejected"); return; }
        if (!hitClaims[message.from].Accept(message.sequence)) {
            Log("net: repeated NPC hit ignored; its original reliable result remains authoritative");
            return;
        }
        // The sender's destination is only a hint: gameplay resolves the current authority before applying or relaying.
        if (hitNpcHandler && message.from)
            hitNpcHandler(message.from, message.guid, message.delta, message.weapon, message.npc, message.outcome, message.seen,
                          message.reaction, message.sequence, message.incarnation, message.contact);
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
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || (!isHost && peer != server)) return;
        StatesHeader header;
        std::memcpy(&header, packet->data, sizeof(header));
        const uint8_t domain = header.kind & story::policy::KindMask;
        const bool snapshot = (header.kind & story::policy::Snapshot) != 0;
        if (domain < StateQuests || domain > StateWorldClock ||
            ((header.kind & story::policy::End) && (!snapshot || header.count)) ||
            ((header.kind & story::policy::Catchup) && (!snapshot || !header.count || (header.kind & story::policy::End))) ||
            (isHost && snapshot && (header.count || (header.kind & (story::policy::End | story::policy::Catchup))))) return;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if ((isHost && (from < 2 || from >= MaxPlayers || peers[from] != peer)) || (!isHost && !connected)) return;
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
        if (isHost && header.epoch) {
            auto& cache = goodsCache[header.player];
            GoodsHeader previous{};
            if (!cache.empty()) std::memcpy(&previous, cache.data(), sizeof(previous));
            if (cache.empty() || header.epoch > previous.epoch ||
                (header.epoch == previous.epoch && header.serial > previous.serial))
                cache.assign(packet->data, packet->data + packet->dataLength);
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
    } else if (type == MsgWorldState && packet->dataLength == sizeof(WorldStateMsg)) {
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || (!isHost && peer != server)) return;
        WorldStateMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        message.state.routine[sizeof(message.state.routine) - 1] = 0;
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if (isHost && (message.state.kind == 8 || message.state.kind == 9))
            message.state.npcClass = from; // a client cannot name another player (or an NPC) as its helper
        if (message.state.kind == 19) {
            if (message.state.dialog > 3) return;
            if (isHost) {
                if (from < 2 || from >= MaxPlayers || peers[from] != peer || message.state.npcClass != from) return;
            } else if (from != 1 || message.state.npcClass != 1) {
                return;
            }
        }
        if (worldStateHandler && from) worldStateHandler(from, message.state);
    } else if (type == MsgClock && packet->dataLength == sizeof(ClockMsg)) {
        ClockMsg message;
        std::memcpy(&message, packet->data, sizeof(message));
        if (clockHandler) clockHandler(message.state);
    } else if (type == MsgCharacter && packet->dataLength >= sizeof(CharacterMsg)) {
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if ((isHost && (from < 2 || from >= MaxPlayers || peers[from] != peer)) || (!isHost && (!connected || peer != server)) ||
            !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        CharacterMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        auto const& note = message.note;
        if (note.world[32] || note.save[32] || !saving::Id(note.world) || !saving::Id(note.save) ||
            !note.epoch || !note.account || note.length > saving::store::PlayerLimit ||
            packet->dataLength - sizeof(message) != note.length ||
            (isHost ? note.purpose != CharacterCapture || note.account != steam::PeerId(peer->address.port)
                    : note.purpose != CharacterRestore || note.account != steam::PlayerId())) return;
        std::vector<uint8_t> bytes(packet->data + sizeof(message), packet->data + packet->dataLength);
        if (note.crc != saving::store::Crc(bytes)) {
            Logf("net: character record CRC refused player=%u save=%s bytes=%u", from, note.save, note.length);
            return;
        }
        if (characterHandler) characterHandler(from, note, bytes);
    } else if (type == MsgSave && packet->dataLength == sizeof(SaveMsg)) {
        const uint8_t from = isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1;
        if ((isHost && (from < 2 || from >= MaxPlayers || peers[from] != peer)) || (!isHost && (!connected || peer != server))) return;
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) return;
        SaveMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        SaveNote const& note = message.note;
        if (note.world[32] || note.save[32] || note.name[48] || note.ok > 1) return;
        const bool clientOp = note.op == SaveRequest || note.op == SaveReady || note.op == SaveCancel;
        const bool hostOp = note.op == SavePrepare || note.op == SaveCancel || note.op == SaveResult;
        if ((isHost && !clientOp) || (!isHost && !hostOp)) return;
        // A client may cancel its request before Prepare supplies the shared-world ID.
        const bool pendingCancel = isHost && note.op == SaveCancel && !*note.world;
        if (!note.epoch || !saving::Id(note.save) || (!pendingCancel && !saving::Id(note.world))) return;
        if (saveHandler) saveHandler(from, note);
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
        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE) || !message.note.epoch || message.note.kind > KindSaved ||
            (isHost ? from < 2 || from >= MaxPlayers || peers[from] != peer ||
                      (message.note.op != WorldReady && message.note.op != WorldWaiting && message.note.op != WorldOther)
                    : peer != server || !connected || (message.note.op != WorldHost && message.note.op != WorldReady))) return;
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
uint32_t WorldEpoch() { return worldEpoch; }

bool SendNpcLease(uint8_t to, npclease::Message const& note)
{
    if (note.world != worldEpoch || !Connected()) return false;
    ENetPeer* peer = isHost ? (to > 1 && to < MaxPlayers ? peers[to] : nullptr) : server;
    std::vector<uint8_t> bytes;
    if (!npclease::EncodeMessage(note, bytes)) return false;
    bytes.insert(bytes.begin(), MsgNpcLease);
    return Send(peer, bytes.data(), bytes.size(), ChannelReliable, true);
}
void SetNpcLeaseHandler(NpcLeaseHandler handler) { npcLeaseHandler = handler; }
uint8_t LocalId() { return localId; }
bool IsHost() { return isHost; }
uint32_t JoinCount() { return joins; }
uint32_t WorldEntryCount() { return worldEntries; }

void RecordNpcSendDecision(NpcSendDecision decision, uint32_t count) { bandwidth.Npc(decision, count); }

void SetInWorld(uint8_t id, bool in)
{
    if (!isHost) {
        if (inWorld == in) return;
        inWorld = in;
        for (auto& receiver : playerReceivers) receiver = {};
        for (auto& senders : playerSenders) for (auto& sender : senders) sender = {};
        if (!in) {
            for (uint8_t i = 0; i < MaxPlayers; ++i) ClearPresentation(i);
            boneLayouts.Clear();
            decodedPoses.clear();
            receivedBoneGeneration = 0;
            requestedBoneEpoch = 0;
            controlReceiver = npcwire::Context{};
            receivedControlGeneration = requestedControlGeneration = 0;
        }
        if (in) ++worldEntries;
        Logf("net: this player is %s the host's world", in ? "in" : "out of");
        return;
    }
    if (id < 2 || id >= MaxPlayers || !peers[id] || clientInWorld[id] == in) return;
    if (in) {
        // A rejoin must not make an old same-layout ACK valid for a new offer.
        // The stream is shared, so rotate it for all peers and re-offer lazily.
        ResetNpcReplication();
    } else {
        ForgetBonePeer(id);
        ForgetControlPeer(id);
    }
    clientInWorld[id] = in;
    playerReceivers[id] = {};
    for (auto& sender : playerSenders[id]) sender = {};
    if (in) {
        ++joins;  // like a join for everything that sends a newcomer the whole state
        for (uint8_t from = 2; from < MaxPlayers; ++from)
            if (from != id && clientInWorld[from] && !goodsCache[from].empty())
                goodsReplay[id] |= static_cast<uint8_t>(1u << from);
    } else {
        goodsCache[id].clear();
        goodsReplay[id] = 0;
        for (auto& pending : goodsReplay) pending &= static_cast<uint8_t>(~(1u << id));
        ClearPresentation(id); // the puppet leaves; connection-level activity still describes its owner
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

bool SendWorld(WorldNote const& note, uint8_t to)
{
    if (!host) return false;
    if (isHost && note.op == WorldHost && worldEpoch != note.epoch) {
        ResetNpcReplication();
        worldEpoch = note.epoch;
    }
    const WorldMsg message{MsgWorld, note};
    if (isHost) {
        if (to > 1 && to < MaxPlayers && peers[to])
            return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    } else if (server && connected)
        return Send(server, &message, sizeof(message), ChannelReliable, true);
    return false;
}

uint32_t HashName(char const* name) { return HashWorld(name); }

uint32_t HashGuid(uint8_t const guid[16])
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; ++i) {
        hash ^= guid[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

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
#ifndef G3T_REMOTE_NPC_FOCUS_SUPPORTED
// Standalone pose branch: the player-state branch supplies these on integration.
uint32_t RemoteFocusNpc(uint8_t) { return 0; }
uint32_t RemoteTargetNpc(uint8_t) { return 0; }
uint32_t RemoteStateAgeMs(uint8_t) { return UINT32_MAX; }
#endif
void SetSendRate(int hz) { sendRate = hz < 20 ? 20 : hz > 60 ? 60 : hz; }

void SetBandwidthBudget(uint32_t bytesPerSecond)
{
    budgetRate = (std::clamp)(bytesPerSecond, 64u * 1024, 4096u * 1024);
    if (host) for (size_t i = 0; i < host->peerCount; ++i) ResetBudget(&host->peers[i]);
}

uint32_t RemoteStateAgeMs(uint8_t player)
{
    return player < MaxPlayers && remotes[player].connected && InWorld(player)
        ? enet_time_get() - remotes[player].latestTicks : UINT32_MAX;
}

uint32_t RemoteFocusNpc(uint8_t player)
{
    return RemoteStateAgeMs(player) < 1000 ? remotes[player].latest.focusNpc : 0;
}

uint32_t RemoteTargetNpc(uint8_t player)
{
    return RemoteStateAgeMs(player) < 1000 ? remotes[player].latest.targetNpc : 0;
}

BudgetDecision PreviewBandwidth(uint8_t to, BudgetClass priority, uint32_t payloadBytes)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    if (!host || !peer || peer->state != ENET_PEER_STATE_CONNECTED) return BudgetDecision::Shed;
    auto& budget = peerBudgets[PeerIndex(peer)];
    const auto decision = budget.Preview(priority,
        BudgetCharge(payloadBytes, peer->mtu, relayEnvelopeBytes, host->checksum ? 4 : 0));
    budget.RecordDecision(decision);
    return decision;
}

BudgetSnapshot PeerBandwidth(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    return host && peer ? peerBudgets[PeerIndex(peer)].Snapshot() : BudgetSnapshot{};
}

uint32_t NpcStretchPermille(uint8_t to)
{
    ENetPeer* peer = isHost ? (to < MaxPlayers ? peers[to] : nullptr) : server;
    return host && peer && peer->state == ENET_PEER_STATE_CONNECTED
        ? peerBudgets[PeerIndex(peer)].NpcStretchPermille() : PeerBudget::StretchNormal;
}

void ResetNpcReplication()
{
    for (auto& origins : npcOrigins) origins.clear();
    npcPublications.clear();
    boneLayouts.Clear();
    receivedBoneGeneration = 0;
    requestedBoneEpoch = 0;
    requestedBoneLayout = 0;
    requestedBoneAt = 0;
    controlSender = npcwire::Context{};
    controlReceiver = npcwire::Context{};
    controlStates.clear();
    controlKeys.clear();
    receivedControlGeneration = requestedControlGeneration = requestedControlAt = 0;
    requestedControlId = 0;
    for (uint8_t i = 0; i < MaxPlayers; ++i) {
        ForgetBonePeer(i);
        ForgetControlPeer(i);
    }
    if (isHost && !boneGenerationExhausted) {
        if (boneGeneration == UINT64_MAX) {
            boneGeneration = 0;
            boneGenerationExhausted = true;
        } else {
            ++boneGeneration;
        }
    }
    if (isHost && !controlGenerationExhausted) {
        if (controlGeneration == UINT32_MAX) {
            controlGeneration = 0;
            controlGenerationExhausted = true;
        } else {
            ++controlGeneration;
        }
    }
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

namespace {
bool SendUnreliableNpc(ENetPeer* peer, void const* data, size_t size)
{
    if (peer->channels[ChannelNpcs].outgoingUnreliableSequenceNumber == UINT16_MAX &&
        !Send(peer, nullptr, 0, ChannelNpcs, true)) {
        if (BoneLog(controlErrors)) Log("net: NPC sequence checkpoint could not be queued");
        return false;
    }
    return Send(peer, data, size, ChannelNpcs, false);
}

bool SendFullNpc(ENetPeer* peer, NpcRecord const& record, bool reliable)
{
    uint8_t buffer[sizeof(NpcsHeader) + sizeof(NpcRecord)];
    const NpcsHeader header{MsgNpcs, ++npcSequence, enet_time_get(), worldEpoch, 1, ++npcPublication, 1, 1};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), &record, sizeof(record));
    const bool sent = reliable ? Send(peer, buffer, sizeof(buffer), ChannelNpcs, true)
                               : SendUnreliableNpc(peer, buffer, sizeof(buffer));
    if (!sent && BoneLog(controlErrors))
        Log("net: full-record NPC fallback could not be sent");
    return sent;
}

bool SendEssentialNpc(uint8_t peerId, NpcRecord const& record, PreparedControl const& prepared)
{
    if (ControlReady(peerId, prepared)) {
        npcwire::Control exact;
        std::vector<uint8_t> batch;
        const NpcsHeader header{MsgControlBatch, ++npcSequence, enet_time_get(), worldEpoch,
                                1, ++npcPublication, 1, 1};
        if (npcwire::FromNpcRecord(record, prepared.ref.anchor, prepared.ref.baseline, true, exact) &&
            npcwire::EncodeBatch(controlSender, header.publication, {exact}, batch, true)) {
            std::vector<uint8_t> message(sizeof(header));
            std::memcpy(message.data(), &header, sizeof(header));
            message.insert(message.end(), batch.begin(), batch.end());
            if (Send(peers[peerId], message.data(), message.size(), ChannelNpcs, true)) return true;
            if (BoneLog(controlErrors)) Log("net: exact reliable NPC control send failed; using full record");
        } else if (BoneLog(controlErrors)) {
            Log("net: exact reliable NPC control encode failed; using full record");
        }
    }
    return SendFullNpc(peers[peerId], record, true);
}

struct PendingControls {
    std::vector<npcwire::Control> values;
    std::vector<NpcRecord> originals;
    std::vector<uint16_t> ids;
    size_t bytes = sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader);
};

bool FlushControls(uint8_t id, PendingControls& pending)
{
    if (pending.values.empty()) return false;
    const NpcsHeader header{MsgControlBatch, ++npcSequence, enet_time_get(), worldEpoch, 1, ++npcPublication,
                            static_cast<uint8_t>(pending.values.size()), 1};
    bool queued = false;
    std::vector<uint8_t> batch;
    if (npcwire::EncodeBatch(controlSender, header.publication, pending.values, batch) &&
        sizeof(header) + batch.size() <= NpcDatagramBudget) {
        std::vector<uint8_t> message(sizeof(header));
        std::memcpy(message.data(), &header, sizeof(header));
        message.insert(message.end(), batch.begin(), batch.end());
        if (BoneLog(controlBatchLogs))
            Logf("net: player %u compact NPC records=%u record_bytes=%u datagram_bytes=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(pending.values.size()),
                 static_cast<unsigned>(batch.size() - sizeof(npcwire::WireBatchHeader)),
                 static_cast<unsigned>(message.size()));
        queued = SendUnreliableNpc(peers[id], message.data(), message.size());
        if (!queued && BoneLog(controlErrors))
            Log("net: compact NPC batch could not be sent");
    } else {
        if (BoneLog(controlErrors)) Log("net: compact NPC batch failed; using full-record fallback");
        for (auto const& record : pending.originals) queued = SendFullNpc(peers[id], record, false) || queued;
    }
    pending = PendingControls{};
    return queued;
}
}

bool SendNpcsTo(uint8_t to, NpcRecord const* records, int count, bool reliable)
{
    if (!host || !isHost || !records || count <= 0 || count > MaxNpcsPerMessage ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;
    PendingControls pending[MaxPlayers];
    bool queued = false;
    for (int i = 0; i < count; ++i) {
        NpcRecord record = records[i];
        if (!record.sent) record.sent = enet_time_get();
        const auto prepared = PrepareControl(record);
        std::vector<uint8_t> encoded;
        const bool encodable = prepared.encodable && npcwire::EncodeRecord(controlSender, prepared.value, encoded) &&
            sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader) + encoded.size() <= NpcDatagramBudget;
        for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId) {
            if (to && peerId != to) continue;
            if (!peers[peerId] || !clientInWorld[peerId]) continue;
            OfferControl(peerId, prepared);
            auto& batch = pending[peerId];
            if (reliable || !encodable || !ControlReady(peerId, prepared)) {
                FlushControls(peerId, batch);
                queued = (reliable ? SendEssentialNpc(peerId, record, prepared)
                                   : SendFullNpc(peers[peerId], record, false)) || queued;
                continue;
            }
            if (batch.bytes + encoded.size() > NpcDatagramBudget ||
                std::find(batch.ids.begin(), batch.ids.end(), prepared.ref.id) != batch.ids.end())
                queued = FlushControls(peerId, batch) || queued;
            batch.bytes += encoded.size();
            batch.values.push_back(prepared.value);
            batch.originals.push_back(record);
            batch.ids.push_back(prepared.ref.id);
        }
    }
    for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId) {
        if (to && peerId != to) continue;
        if (peers[peerId]) queued = FlushControls(peerId, pending[peerId]) || queued;
    }
    return queued;
}

void SendNpcs(NpcRecord const* records, int count, bool reliable)
{
    SendNpcsTo(0, records, count, reliable);
}

bool SendPosedNpcTo(uint8_t to, NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags, bool reliable,
                    uint8_t intervalMs)
{
    if (!host || !isHost || !bones || count <= 0 || count > pose::MaxBones || (poseFlags & ~pose::Facial) ||
        intervalMs < 16 || intervalMs > 100 ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;
    uint32_t hashes[pose::MaxBones], sorted[pose::MaxBones];
    for (int i = 0; i < count; ++i) hashes[i] = sorted[i] = bones[i].name;
    std::sort(sorted, sorted + count);
    if (!sorted[0] || std::adjacent_find(sorted, sorted + count) != sorted + count) {
        if (BoneLog(boneInvalidLogs)) Log("net: rejected pose with zero or duplicate bone hashes");
        return false;
    }

    uint16_t layoutId = 0;
    if (worldEpoch && boneGeneration &&
        boneLayouts.Reset(worldEpoch, boneGeneration) == bonewire::Status::Ok) {
        auto status = boneLayouts.FindLayout(hashes, count, layoutId);
        if (status == bonewire::Status::UnknownLayout) {
            if (nextBoneLayout < UINT16_MAX) {
                layoutId = static_cast<uint16_t>(++nextBoneLayout);
                status = boneLayouts.Register(layoutId, hashes, count);
                if (status == bonewire::Status::Ok && BoneLog(boneLayoutLogs))
                    Logf("net: new bone layout=%u bones=%d definition_bytes=%u compact_pose_bytes=%u full_pose_bytes=%u",
                         static_cast<unsigned>(layoutId), count, static_cast<unsigned>(1 + bonewire::HeaderBytes + bonewire::CountBytes + count * 4),
                         static_cast<unsigned>(sizeof(BonePosePrefix) + count * bonewire::TransformBytes),
                         static_cast<unsigned>(3 + count * sizeof(pose::Bone)));
            } else if (!boneExhaustionLogged) {
                boneExhaustionLogged = true;
                Log("net: bone layout IDs exhausted; new layouts use full-hash pose fallback");
            }
        }
        if (status != bonewire::Status::Ok) {
            if (status != bonewire::Status::UnknownLayout && BoneLog(boneInvalidLogs))
                Logf("net: bone layout unavailable (%u); using full-hash pose", static_cast<unsigned>(status));
            layoutId = 0;
        }
    } else if (BoneLog(boneInvalidLogs)) {
        Log("net: bone dictionary has no epoch or generation; using full-hash pose");
    }
    uint8_t buffer[sizeof(NpcsHeader) + sizeof(NpcRecord) + sizeof(uint16_t) + 2 + pose::MaxBones * sizeof(pose::Bone)];
    NpcsHeader header{MsgPosedNpc, 0, enet_time_get(), worldEpoch, 1, 0, 1, 1};
    const auto preparedControl = PrepareControl(record);
    const uint16_t boneCount = static_cast<uint16_t>(count);
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), &record, sizeof(record));
    std::memcpy(buffer + sizeof(header) + sizeof(record), &boneCount, sizeof(boneCount));
    std::memcpy(buffer + sizeof(header) + sizeof(record) + sizeof(boneCount), &poseFlags, sizeof(poseFlags));
    buffer[sizeof(header) + sizeof(record) + sizeof(boneCount) + 1] = intervalMs;
    std::memcpy(buffer + sizeof(header) + sizeof(record) + sizeof(boneCount) + 2, bones, count * sizeof(*bones));
    const size_t size = sizeof(header) + sizeof(record) + sizeof(boneCount) + 2 + count * sizeof(*bones);
    std::vector<uint8_t> definition, compactMessage, controlPoseMessage;
    bool definitionAttempted = false, definitionReady = false;
    bool queued = false;
    for (uint8_t i = 2; i < MaxPlayers; ++i) {
        if (to && i != to) continue;
        if (!peers[i] || !clientInWorld[i]) continue;
        if (reliable && !SendEssentialNpc(i, record, preparedControl)) return false;
        // Essential state is an earlier publication, never a fence that makes
        // its own accompanying pose stale when reliable traffic arrives first.
        header.sequence = ++npcSequence;
        header.publication = ++npcPublication;
        if (!header.publication) header.publication = ++npcPublication;
        std::memcpy(buffer, &header, sizeof(header));
        OfferControl(i, preparedControl);
        if (layoutId && !boneAcknowledged[i].count(layoutId)) {
            const auto sent = boneOffered[i].find(layoutId);
            const uint32_t now = enet_time_get();
            if (sent == boneOffered[i].end() || static_cast<uint32_t>(now - sent->second) >= 1000) {
                if (!definitionAttempted) {
                    definitionAttempted = true;
                    definitionReady = boneLayouts.EncodeDictionary(layoutId, definition) == bonewire::Status::Ok;
                }
                if (definitionReady && SendBoneControl(peers[i], MsgBoneDictionary, definition)) {
                    boneOffered[i][layoutId] = now;
                } else if (BoneLog(boneDefinitionFailureLogs)) {
                    Logf("net: could not send bone layout %u to player %u; using full-hash pose",
                         static_cast<unsigned>(layoutId), static_cast<unsigned>(i));
                }
            }
        }
        bool compactReady = layoutId && boneAcknowledged[i].count(layoutId);
        bool poseKey = true;
        bonewire::PoseStats poseStats;
        std::vector<pose::Bone> reconstructed;
        compactMessage.clear();
        controlPoseMessage.clear();
        auto& poseCache = sentPoses[i][NpcKey(record.guid)];
        if (compactReady) {
            std::vector<uint8_t> compact;
            const auto status = poseCache.EncodeCompact(boneLayouts, layoutId, record.incarnation, poseFlags, bones, count,
                                                       record.sent, compact, poseKey, reconstructed, &poseStats);
            if (status == bonewire::Status::Ok) {
                compactMessage.resize(sizeof(header) + sizeof(record) + 2 + compact.size());
                NpcsHeader compactHeader = header;
                compactHeader.type = MsgCompactPosedNpc;
                std::memcpy(compactMessage.data(), &compactHeader, sizeof(compactHeader));
                std::memcpy(compactMessage.data() + sizeof(header), &record, sizeof(record));
                compactMessage[sizeof(header) + sizeof(record)] = poseFlags;
                compactMessage[sizeof(header) + sizeof(record) + 1] = intervalMs;
                std::memcpy(compactMessage.data() + sizeof(header) + sizeof(record) + 2, compact.data(), compact.size());
            } else if (BoneLog(boneInvalidLogs)) {
                Logf("net: could not encode bone layout %u (%u); using full-hash pose",
                     static_cast<unsigned>(layoutId), static_cast<unsigned>(status));
            }
        }
        compactReady = compactReady && !compactMessage.empty();
        if (compactReady && ControlReady(i, preparedControl)) {
            std::vector<uint8_t> batch;
            const std::vector<npcwire::Control> values{preparedControl.value};
            if (npcwire::EncodeBatch(controlSender, header.publication, values, batch) && batch.size() <= UINT16_MAX) {
                NpcsHeader controlHeader = header;
                controlHeader.type = MsgControlPose;
                controlPoseMessage.resize(sizeof(header) + 2);
                std::memcpy(controlPoseMessage.data(), &controlHeader, sizeof(controlHeader));
                const size_t controlBytes = batch.size() - sizeof(npcwire::WireBatchHeader) + 4;
                controlPoseMessage[sizeof(header)] = static_cast<uint8_t>(controlBytes);
                controlPoseMessage[sizeof(header) + 1] = static_cast<uint8_t>(controlBytes >> 8);
                controlPoseMessage.insert(controlPoseMessage.end(), batch.begin() + 8, batch.begin() + 12);
                controlPoseMessage.insert(controlPoseMessage.end(), batch.begin() + sizeof(npcwire::WireBatchHeader), batch.end());
                controlPoseMessage.insert(controlPoseMessage.end(),
                    compactMessage.begin() + sizeof(header) + sizeof(record), compactMessage.end());
            }
        }
        const bool controlReady = compactReady && ControlReady(i, preparedControl) && !controlPoseMessage.empty();
        const void* data = controlReady ? static_cast<void const*>(controlPoseMessage.data())
            : compactReady ? static_cast<void const*>(compactMessage.data()) : buffer;
        const size_t bytes = controlReady ? controlPoseMessage.size() : compactReady ? compactMessage.size() : size;
        const size_t poseBytes = compactReady ? compactMessage.size() - sizeof(header) - sizeof(record)
                                              : sizeof(boneCount) + 2 + count * sizeof(pose::Bone);
        if (BoneLog(compactReady ? boneCompactPoseLogs : boneFullPoseLogs))
            Logf("net: player %u pose=%s bones=%d pose_bytes=%u datagram_bytes=%u", static_cast<unsigned>(i),
                 compactReady ? "compact" : "full", count, static_cast<unsigned>(poseBytes),
                 static_cast<unsigned>(bytes));
        if (Send(peers[i], data, bytes, ChannelPoses, false)) {
            ++poseSent; queued = true;
            if (compactReady)
                poseCache.Store(header.publication, record.incarnation, layoutId, poseFlags, reconstructed.data(), count,
                                record.sent, poseKey);
            bandwidth.Pose(intervalMs, bytes, poseBytes, count, poseKey || !compactReady);
            if (!compactReady) {
                poseStats.headerBytes = 2 + count * sizeof(uint32_t);
                poseStats.rotationBits = count * 144;
                poseStats.translationBits = count * 48;
                poseStats.includedBones = count;
            }
            bandwidth.PoseComponents(poseStats.headerBytes + 2, poseStats.maskBits, poseStats.rotationBits,
                poseStats.translationBits, poseStats.includedBones, poseKey || !compactReady, poseStats.predictive,
                bytes - poseBytes - sizeof(header), sizeof(header), poseStats.rigidBones, poseStats.maxBasisErrorUnits,
                poseStats.PaddingBits());
        }
        else {
            ++poseSendFailed;
            if (!compactReady && BoneLog(boneFallbackFailureLogs))
                Logf("net: full-hash pose fallback failed for player %u layout=%u",
                     static_cast<unsigned>(i), static_cast<unsigned>(layoutId));
        }
    }
    return queued;
}

void SendPosedNpc(NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags)
{
    SendPosedNpcTo(0, record, bones, count, poseFlags);
}

bool SendNpcReleaseTo(uint8_t to, uint8_t const* guids, int count)
{
    if (!host || !isHost || !guids || count <= 0 || count > MaxNpcsPerMessage ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;
    uint8_t buffer[sizeof(NpcReleaseHeader) + MaxNpcsPerMessage * 16];
    const NpcReleaseHeader header{MsgNpcRelease, ++npcSequence, worldEpoch, isHost ? 1u : 0u,
                                  isHost ? ++npcPublication : 0u, static_cast<uint8_t>(count), localId};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), guids, count * 16u);
    const size_t size = sizeof(header) + count * 16u;
    bool queued = false;
    for (uint8_t id = 2; id < MaxPlayers; ++id) {
        if ((to && id != to) || !peers[id]) continue;
        if (Send(peers[id], buffer, size, ChannelNpcs, true)) {
            queued = true;
            for (int i = 0; i < count; ++i) sentPoses[id].erase(NpcKey(guids + i * 16));
        }
    }
    return queued;
}

void SendNpcRelease(uint8_t const* guids, int count)
{
    SendNpcReleaseTo(0, guids, count);
}

void SetNpcReleaseHandler(NpcReleaseHandler handler) { npcReleaseHandler = handler; }

bool SendNpcRemovalTo(uint8_t to, NpcRemovalNote const& note)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to] ||
        !PeerJoined(to) || !InWorld(to) || !note.epoch ||
        note.epoch != worldEpoch || !note.incarnation) return false;
    const NpcRemovalMsg message{MsgNpcRemoval, note};
    return Send(peers[to], &message, sizeof(message), ChannelNpcs, true);
}

void SetNpcRemovalHandler(NpcRemovalHandler handler) { npcRemovalHandler = handler; }

void SetHitHandlers(HitNpcHandler npc, HitPlayerHandler player)
{
    hitNpcHandler = npc;
    hitPlayerHandler = player;
}

uint32_t SendHitNpc(uint8_t to, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16], uint8_t outcome,
                    uint32_t seen, uint8_t reaction, uint32_t incarnation, MeleeContact const& contact)
{
    if (!host || isHost || !server || !connected || !inWorld || to != 1) {
        Log("net: NPC hit not sent: no ready authoritative host");
        return 0;
    }
    if (!ValidHitContact(delta, incarnation, seen, outcome, reaction, contact)) {
        Log("net: NPC hit not sent: invalid contact judgement");
        return 0;
    }
    if (++hitSequence == 0) ++hitSequence;
    HitNpcMsg message{MsgHitNpc, {}, delta, {}, to, localId, {}, outcome, seen, reaction, worldEpoch, hitSequence, incarnation, contact};
    std::memcpy(message.guid, guid, 16);
    std::memcpy(message.weapon, weapon, 16);
    if (npc) std::memcpy(message.npc, npc, 16);
    if (!Send(server, &message, sizeof(message), ChannelCombat, true)) {
        Log("net: NPC hit could not be queued");
        return 0;
    }
    return hitSequence;
}

void SetHitResultHandler(HitResultHandler handler) { hitResultHandler = handler; }

void SendHitResult(uint8_t to, HitResult const& result)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to]) return;
    HitResultMsg message{MsgHitResult, result};
    message.result.epoch = worldEpoch;
    message.result.processedAt = enet_time_get();
    SendToPeer(&message, sizeof(message), ChannelCombat, true, to);
}
void SendHitPlayer(uint8_t playerId, int32_t delta, uint8_t const attacker[16], uint8_t attackerPlayer, bool meansToKill)
{
    if (!host || playerId == localId || playerId >= MaxPlayers) return;
    HitPlayerMsg message{MsgHitPlayer, delta, {}, attackerPlayer, playerId, static_cast<uint8_t>(meansToKill ? 1 : 0)};
    std::memcpy(message.attacker, attacker, 16);
    SendToPeer(&message, sizeof(message), ChannelCombat, true, playerId);
}

void SetStateHandler(StateHandler handler) { stateHandler = handler; }

void SetTalkHandler(TalkHandler handler) { talkHandler = handler; }

void SetItemHandler(ItemHandler handler) { itemHandler = handler; }

void SetKilledHandler(KilledHandler handler) { killedHandler = handler; }

void SetDeathHandler(DeathHandler handler) { deathHandler = handler; }

void SetSaveHandler(SaveHandler handler) { saveHandler = handler; }
void SetCharacterHandler(CharacterHandler handler) { characterHandler = handler; }
uint64_t PeerAccount(uint8_t id)
{
    return isHost && id >= 2 && id < MaxPlayers && peers[id] ? steam::PeerId(peers[id]->address.port) : 0;
}
bool SendCharacter(uint8_t to, CharacterNote const& note, std::vector<uint8_t> const& bytes)
{
    if (!host || note.length != bytes.size() || bytes.size() > saving::store::PlayerLimit ||
        note.crc != saving::store::Crc(bytes)) return false;
    const CharacterMsg message{MsgCharacter, note};
    std::vector<uint8_t> packet(sizeof(message) + bytes.size());
    std::memcpy(packet.data(), &message, sizeof(message));
    if (!bytes.empty()) std::memcpy(packet.data() + sizeof(message), bytes.data(), bytes.size());
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to] || note.purpose != CharacterRestore) return false;
        return Send(peers[to], packet.data(), packet.size(), ChannelReliable, true);
    }
    return connected && server && to == 1 && note.purpose == CharacterCapture &&
           Send(server, packet.data(), packet.size(), ChannelReliable, true);
}

bool SendSave(uint8_t to, SaveNote const& note)
{
    if (!host) return false;
    const SaveMsg message{MsgSave, note};
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to]) return false;
        return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    }
    return connected && server && to == 1 && Send(server, &message, sizeof(message), ChannelReliable, true);
}

void SetClockHandler(ClockHandler handler) { clockHandler = handler; }

void SetWorldStateHandler(WorldStateHandler handler) { worldStateHandler = handler; }

void SendWorldState(WorldState const& state, uint8_t except)
{
    const WorldStateMsg message{MsgWorldState, state};
    if (state.kind == 19) {
        if (isHost) {
            if (state.dialog == 0) SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
            return;
        }
        if (connected && server) Send(server, &message, sizeof(message), ChannelReliable, true);
        return;
    }
    // A client may still pay to revive a partner; the host binds kind 8/9's helper to the sending peer.
    if (!host || (!isHost && state.kind != 8 && state.kind != 9 && state.kind != 10)) return;
    SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
}

bool SendWorldStateTo(uint8_t to, WorldState const& state)
{
    if (state.kind == 19) {
        if (state.dialog > 3 || state.npcClass != localId) return false;
    } else if (state.kind != 10 || (isHost ? !(state.npcClass & 0x40) : state.npcClass != 0x80)) return false;
    const WorldStateMsg message{MsgWorldState, state};
    if (isHost) {
        if (to < 2 || to >= MaxPlayers || !peers[to]) return false;
        return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
    }
    return connected && server && to == 1 && Send(server, &message, sizeof(message), ChannelReliable, true);
}

void SendClock(ClockState const& state)
{
    if (!host) return;
    const ClockMsg message{MsgClock, state};
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

void SendDeath(uint8_t const guid[16], uint8_t by, uint8_t except)
{
    if (!host || !isHost) return;
    DeathMsg message{MsgDeath, {}, by};
    std::memcpy(message.guid, guid, 16);
    SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);
}

void SendKilled(uint8_t to, uint8_t const guid[16], uint32_t incarnation)
{
    if (!host || !isHost || to <= 1 || to >= MaxPlayers || !incarnation) return;
    KilledMsg message{MsgKilled, {}, to, worldEpoch, incarnation};
    std::memcpy(message.guid, guid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

bool SendGoods(uint64_t epoch, uint32_t serial, int32_t gold, GoodsEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxGoodsEntries || gold < 0) return false;
    uint8_t buffer[sizeof(GoodsHeader) + MaxGoodsEntries * sizeof(GoodsEntry)];
    const GoodsHeader header{MsgGoods, localId, epoch, serial, gold, static_cast<uint8_t>(count)};
    std::memcpy(buffer, &header, sizeof(header));
    if (count) std::memcpy(buffer + sizeof(header), entries, count * sizeof(GoodsEntry));
    const size_t size = sizeof(header) + count * sizeof(GoodsEntry);
    if (!isHost) return server && connected && Send(server, buffer, size, ChannelReliable, true);
    bool queued = true;
    for (uint8_t i = 2; i < MaxPlayers; ++i)
        if (peers[i] && clientInWorld[i] && !Send(peers[i], buffer, size, ChannelReliable, true)) queued = false;
    return queued;
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
    SendBroadcast(buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
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
    SendToServer(buffer, sizeof(header) + count * sizeof(ContentEntry), ChannelReliable, true);
    return true;
}

void SendContainerTxResult(uint8_t to, uint8_t const guid[16], uint32_t seq, bool ok)
{
    if (!host || !isHost || to >= MaxPlayers || !peers[to]) return;
    TxResultMsg message{MsgTxResult, {}, seq, static_cast<uint8_t>(ok ? 1 : 0)};
    std::memcpy(message.guid, guid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendWatch(uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    WatchMsg message{MsgWatch, {}};
    std::memcpy(message.guid, guid, 16);
    SendToServer(&message, sizeof(message), ChannelReliable, true);
}

void SendItem(ItemNote const& note, uint8_t to)
{
    if (!host) return;
    ItemMsg message{MsgItem, note};
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
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

void SendNpcSpell(uint8_t to, char const* script, uint8_t const npcGuid[16], int32_t args)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    NpcSpellMsg message{MsgNpcSpell, localId, to, {}, {}, args};
    strncpy_s(message.script, script, _TRUNCATE);
    std::memcpy(message.target, npcGuid, 16);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SetEnclaveHandler(EnclaveHandler handler) { enclaveHandler = handler; }

void SendEnclave(EnclaveNote const& note, uint8_t to)
{
    if (!host) return;
    const EnclaveMsg message{MsgEnclave, note};
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendPlayerSpell(uint8_t to, char const* script)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    PlayerSpellMsg message{MsgPlayerSpell, localId, to, {}};
    strncpy_s(message.script, script, _TRUNCATE);
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendRansack(uint8_t to, RansackNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const RansackMsg message{MsgRansack, localId, to, note};
    SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendWitness(uint8_t to, WitnessNote const& note)
{
    if (!host || to == localId || to >= MaxPlayers) return;
    const WitnessMsg message{MsgWitness, localId, to, note};
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (to == 0) SendBroadcast(&message, sizeof(message), ChannelReliable, true);
    else if (to > 1 && to < MaxPlayers) SendToPeer(&message, sizeof(message), ChannelReliable, true, to);
}

void SendShot(ShotNote const& note)
{
    if (!host) return;
    ShotMsg message{MsgShot, localId, note};
    message.note.epoch = worldEpoch;
    if (!message.note.sent) message.note.sent = enet_time_get();
    SendBroadcast(&message, sizeof(message), ChannelCombat, true);
}

void SendRagdoll(RagdollNote const& note)
{
    if (!host || !isHost) return;
    RagdollMsg message{MsgRagdoll, localId, note};
    message.note.epoch = worldEpoch;
    SendBroadcast(&message, sizeof(message), ChannelCombat, true);
}

void SendDialogLine(DialogLineNote const& note)
{
    if (!host) return;
    const DialogLineMsg message{MsgDialogLine, localId, note};
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
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
        SendToServer(&message, size, ChannelReliable, true);
        return true;
    }
    if (!peers[note.seller] || !InWorld(note.seller)) return false;
    SendToPeer(&message, size, ChannelReliable, true, note.seller);
    return true;
}

void SendOfferResult(OfferResultNote const& note)
{
    if (!host) return;
    OfferResultMsg message{MsgOfferResult, note};
    message.note.seller = localId;
    if (!isHost) SendToServer(&message, sizeof(message), ChannelReliable, true);
    else if (note.buyer > 1 && note.buyer < MaxPlayers && peers[note.buyer])
        SendToPeer(&message, sizeof(message), ChannelReliable, true, note.buyer);
}

void SendSummon(SummonNote const& note)
{
    if (!host) return;
    SummonMsg message{MsgSummon, localId, note};
    message.note.epoch = worldEpoch;
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

void SendTalk(bool begin, uint8_t const guid[16])
{
    if (!host || isHost || !server || !connected) return;
    TalkMsg message{MsgTalk, static_cast<uint8_t>(begin ? 1 : 0), {}};
    std::memcpy(message.guid, guid, 16);
    SendToServer(&message, sizeof(message), ChannelReliable, true);
}

namespace {
bool QueueStates(uint8_t to, uint8_t kind, StateEntry const* entries, int count)
{
    if (!host || count < 0 || count > MaxStatesPerMessage || (count && !entries) ||
        (count == 0 && isHost && !(kind & story::policy::Snapshot))) return false;
    constexpr int Batch = (1168 - sizeof(StatesHeader)) / sizeof(StateEntry);
    uint8_t buffer[sizeof(StatesHeader) + Batch * sizeof(StateEntry)];
    for (int at = 0; at < count || (at == 0 && count == 0); at += Batch) {
        const int chunk = (std::min)(Batch, count - at);
        const StatesHeader header{MsgStates, kind, static_cast<uint8_t>(chunk)};
        std::memcpy(buffer, &header, sizeof(header));
        if (chunk) std::memcpy(buffer + sizeof(header), entries + at, chunk * sizeof(StateEntry));
        const size_t size = sizeof(header) + chunk * sizeof(StateEntry);
        if (!isHost) {
            if (!connected || !server || (to && to != 1) || !Send(server, buffer, size, ChannelReliable, true)) return false;
        } else if (to) {
            if (to < 2 || to >= MaxPlayers || !peers[to] || !Send(peers[to], buffer, size, ChannelReliable, true)) return false;
        } else for (uint8_t id = 2; id < MaxPlayers; ++id)
            if (peers[id] && Open(peers[id]) && !Send(peers[id], buffer, size, ChannelReliable, true)) return false;
    }
    return true;
}
}
bool SendStates(uint8_t kind, StateEntry const* entries, int count) { return QueueStates(0, kind, entries, count); }
bool SendStatesTo(uint8_t to, uint8_t kind, StateEntry const* entries, int count) { return QueueStates(to, kind, entries, count); }

void SendMotionName(uint32_t hash, char const* name)
{
    if (!host || !isHost) return;
    MotionNameMsg message{MsgMotionName, hash, {}};
    strncpy_s(message.name, name, _TRUNCATE);
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
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
    haveQueued = false;
    lastServiceAt = 0;
    haveCurrentLocal = false;
    playerCadence = {};
    connected = inWorld = false;
    activitySent = false;
    lastActivitySend = 0;
    localId = 0;
    ResetNpcReplication();
    for (uint8_t i = 0; i < MaxPlayers; ++i) Drop(i);
    if (enetInitialized) {
        enet_deinitialize();
        enetInitialized = false;
    }
    npcHandler = nullptr;
    npcPoseHandler = nullptr;
    playerStateHandler = nullptr;
    npcReleaseHandler = nullptr;
    npcRemovalHandler = nullptr;
    motionNameHandler = nullptr;
    hitNpcHandler = nullptr;
    hitResultHandler = nullptr;
    hitPlayerHandler = nullptr;
    stateHandler = nullptr;
    talkHandler = nullptr;
    itemHandler = nullptr;
    contentsHandler = nullptr;
    goodsHandler = nullptr;
    txHandler = nullptr;
    txResultHandler = nullptr;
    watchHandler = nullptr;
    killedHandler = nullptr;
    deathHandler = nullptr;
    clockHandler = nullptr;
    worldStateHandler = nullptr;
    summonHandler = nullptr;
    shotHandler = nullptr;
    ragdollHandler = nullptr;
    dialogLineHandler = nullptr;
    offerHandler = nullptr;
    offerResultHandler = nullptr;
    playerSpellHandler = nullptr;
    ransackHandler = nullptr;
    witnessHandler = nullptr;
    npcSpellHandler = nullptr;
    enclaveHandler = nullptr;
    worldHandler = nullptr;
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
    haveCurrentLocal = false;
    currentLocal = {};
    playerCadence = {};
    ResetNpcReplication();
    worldEpoch = npcPublication = npcSequence = 0;
    startupError[0] = '\0';
    if (enet_initialize() != 0) return StartFailed("ENet initialization failed");
    enetInitialized = true;
    target = ENetAddress{};
    if (enet_address_set_host_ip(&target, "127.0.0.1") != 0) return StartFailed("loopback address unavailable");
    host = enet_host_create(&target, isHost ? MaxPlayers - 1 : 1, ChannelCount, 0, 0);
    if (!host) return StartFailed("loopback ENet socket could not be created");
    bandwidth = detail::BandwidthCounters{};
    bandwidthSince = enet_time_get();
    lastSentData = host->totalSentData;
    lastSentPackets = host->totalSentPackets;
    lastReceivedData = host->totalReceivedData;
    lastReceivedPackets = host->totalReceivedPackets;
    host->mtu = Mtu;
    constexpr int PoseBurstBuffer = 4 * 1024 * 1024;
    if (enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, PoseBurstBuffer) < 0 ||
        enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, PoseBurstBuffer) < 0)
        return StartFailed("cannot configure the owned ENet socket for canonical pose bursts");
    if (!steam::Start(isHost, isHost ? host->address.port : 0)) return StartFailed(steam::Error());
    lastSteamTraffic = steam::Traffic();
    relayEnvelopeBytes = lastSteamTraffic.envelopeBytes;
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
    int events = 0;
    int result = 0;
    for (; events < 512; ++events) {
        result = enet_host_service(host, &event, 0);
        if (result <= 0) break;
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
            ResetBudget(event.peer);
            if (!isHost) for (auto& sender : playerSenders[1]) sender = {};
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
    if (events == 512) ++serviceCaps;
    if (result < 0 && ++serviceErrors == 1) Log("net: ENet service failed; subsequent failures counted in RTT diagnostics");
    FlushGoodsReplays();
    const uint32_t now = enet_time_get();
    if (!isHost && !server && !refused && now - lastConnectAttempt > 3000) {
        lastConnectAttempt = now;
        server = enet_host_connect(host, &target, ChannelCount, 0);
        Log(server ? "net: reconnecting" : "net: reconnect failed");
    }
    UpdateBudgets(now);
    if (connected) {
    if (!activitySent || local.flags != lastActivityFlags || local.downedSequence != lastActivityDowned ||
        now - lastActivitySend >= 1000) {
        const ActivityMsg activity{MsgPlayerActivity, localId, local.flags, ++sequence,
                                   local.world, worldEpoch, local.downedSequence};
        bool queued = true;
        if (isHost) {
            for (uint8_t i = 2; i < MaxPlayers; ++i)
                if (peers[i] && !Send(peers[i], &activity, sizeof(activity), ChannelState, true)) queued = false;
        } else if (server) {
            queued = Send(server, &activity, sizeof(activity), ChannelState, true);
        }
        if (!queued) Log("net: player activity heartbeat queue failed; retrying at the next heartbeat");
        activitySent = true;
        lastActivitySend = now;
        lastActivityFlags = local.flags;
        lastActivityDowned = local.downedSequence;
    }
    if (!haveLocal) return;
    const bool due = playerCadence.Due(now, static_cast<uint32_t>(sendRate));
    const bool immediate = !haveCurrentLocal || local.flags != currentLocal.flags ||
        local.downedSequence != currentLocal.downedSequence || local.world != currentLocal.world ||
        local.actionSeq != currentLocal.actionSeq;
    if (!due && !immediate) return;
    currentLocal = local;
    currentLocal.id = localId;
    currentLocal.sequence = ++sequence;
    currentLocal.sent = now;
    strncpy_s(currentLocal.name, localName, _TRUNCATE);
    haveCurrentLocal = true;
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i) if (peers[i]) SendPlayerState(peers[i], currentLocal);
    } else if (server) {
        SendPlayerState(server, currentLocal);
    }
    }
}
}

void Flush()
{
    if (!host || g3t::ai::ShuttingDown()) return;
    if (haveQueued) {
        enqueueDelays.Add(enet_time_get() - queuedAt);
        haveQueued = false;
    }
    enet_host_flush(host);
    steam::Pump();
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
                     "pose_send=%u/%u,%u/%u pose_recv=%u/%u,%u/%u waiting_bytes=%u "
                     "transport_max_rtt_ms=%d service_gap_avg_ms=%.2f service_gap_max_ms=%u "
                     "enqueue_flush_avg_ms=%.2f enqueue_flush_max_ms=%u service_caps=%u service_errors=%u "
                     "queued_commands=%u reliable_queued=%u reliable_inflight=%u reliable_bytes=%u",
           peer->roundTripTime, peer->roundTripTimeVariance,
           peer->packetLoss * 100.0 / ENET_PEER_PACKET_LOSS_SCALE, peer->packetsSent, peer->packetsLost,
           peer->packetThrottle, ENET_PEER_PACKET_THROTTLE_SCALE, who,
           poses.outgoingReliableSequenceNumber, poses.outgoingUnreliableSequenceNumber,
           alt.outgoingReliableSequenceNumber, alt.outgoingUnreliableSequenceNumber,
           poses.incomingReliableSequenceNumber, poses.incomingUnreliableSequenceNumber,
           alt.incomingReliableSequenceNumber, alt.incomingUnreliableSequenceNumber,
           static_cast<unsigned>(peer->totalWaitingData), steam::Ping(),
           serviceGaps.Average(), serviceGaps.maximum, enqueueDelays.Average(), enqueueDelays.maximum, serviceCaps, serviceErrors,
           static_cast<unsigned>(enet_list_size(&peer->outgoingCommands)),
           static_cast<unsigned>(enet_list_size(&peer->outgoingSendReliableCommands)),
           static_cast<unsigned>(enet_list_size(&peer->sentReliableCommands)), peer->reliableDataInTransit);
}
void RttLog()
{
    if (!rttLog) return;
    const enet_uint32 now = enet_time_get();
    if (now - lastRttLog < 5000) return;
    lastRttLog = now;
    char line[900];
    if (isHost) {
        for (uint8_t i = 2; i < MaxPlayers; ++i)
            if (peers[i] && peers[i]->state == ENET_PEER_STATE_CONNECTED) { RttLine(peers[i], PeerName(i), line, sizeof(line)); Log(line); }
    } else if (server && connected) {
        RttLine(server, "host", line, sizeof(line));
        Log(line);
    }
    serviceGaps = {};
    enqueueDelays = {};
}

void BandwidthLog()
{
    const uint32_t now = enet_time_get();
    const uint32_t elapsed = now - bandwidthSince;
    if (elapsed < 5000) return;
    const uint32_t sentData = host->totalSentData, sentPackets = host->totalSentPackets;
    const uint32_t receivedData = host->totalReceivedData, receivedPackets = host->totalReceivedPackets;
    const double perSecond = 1000.0 / elapsed;
    const auto traffic = steam::Traffic();
    const uint64_t steamPackets = traffic.txDatagrams - lastSteamTraffic.txDatagrams;
    const uint64_t steamBytes = traffic.txBytes - lastSteamTraffic.txBytes;
    const uint64_t framingBytes = traffic.txEnvelopeBytes - lastSteamTraffic.txEnvelopeBytes;
    const uint64_t enetHeaderBytes = traffic.txEnetProtocolBytes - lastSteamTraffic.txEnetProtocolBytes;
    const uint64_t enetAckBytes = traffic.txEnetAckBytes - lastSteamTraffic.txEnetAckBytes;
    Logf("net: bandwidth role=%s window_ms=%u enet_loopback_tx_packets_bytes=%u/%u enet_loopback_rx_packets_bytes=%u/%u "
         "npc_per_s=considered:%.1f,sent:%.1f,unchanged:%.1f,far:%.1f,irrelevant:%.1f,send_failed:%.1f,paced:%.1f",
         isHost ? "host" : "client", elapsed, sentPackets - lastSentPackets, sentData - lastSentData,
         receivedPackets - lastReceivedPackets, receivedData - lastReceivedData,
         bandwidth.considered * perSecond, bandwidth.sent * perSecond, bandwidth.skippedUnchanged * perSecond,
         bandwidth.skippedFar * perSecond, bandwidth.skippedIrrelevant * perSecond, bandwidth.sendFailed * perSecond,
         bandwidth.skippedPaced * perSecond);
    const uint64_t updates = bandwidth.poseKeys + bandwidth.poseDeltas;
    Logf("net: pose bandwidth tier_per_s=fast:%.1f,ordinary:%.1f,slow:%.1f tier_bytes_per_s=%.0f,%.0f,%.0f "
         "bone_bytes_per_update=%.1f bones_per_update=%.1f keys=%llu deltas=%llu acks=%llu ack_misses=%llu "
         "reference_misses=%llu decode_failures=%llu",
         bandwidth.tierUpdates[0] * perSecond, bandwidth.tierUpdates[1] * perSecond, bandwidth.tierUpdates[2] * perSecond,
         bandwidth.tierBytes[0] * perSecond, bandwidth.tierBytes[1] * perSecond, bandwidth.tierBytes[2] * perSecond,
         updates ? double(bandwidth.poseBytes) / updates : 0.0, updates ? double(bandwidth.poseBones) / updates : 0.0,
         static_cast<unsigned long long>(bandwidth.poseKeys), static_cast<unsigned long long>(bandwidth.poseDeltas),
         static_cast<unsigned long long>(bandwidth.poseAcks), static_cast<unsigned long long>(bandwidth.poseAckMisses),
         static_cast<unsigned long long>(bandwidth.poseReferenceMisses), static_cast<unsigned long long>(bandwidth.poseDecodeFailures));
    const double perUpdate = updates ? 1.0 / updates : 0.0;
    const double basisError = isHost ? double(bandwidth.poseMaxBasisError) : bandwidth.poseRigidBones ? 2.0 : 0.0;
    const double rotationBound = bandwidth.poseRigidBones ? bonewire::PoseRotationBoundDegrees(basisError) : 0;
    Logf("net: pose components bytes_per_update=header:%.1f,mask:%.1f,rotation:%.1f,translation:%.1f,padding:%.1f,control:%.1f,outer:%.1f "
         "included_bones_per_delta=%.1f predictive=%llu rigid_bones_per_update=%.1f "
         "host_observed_basis_error_q12=%d rotation_error_bound_deg=%.4f origin_error_cm=0 skin_200cm_error_bound_cm=%.4f",
         bandwidth.poseHeaderBytes * perUpdate, bandwidth.poseMaskBits * perUpdate / 8.0,
         bandwidth.poseRotationBits * perUpdate / 8.0, bandwidth.poseTranslationBits * perUpdate / 8.0,
         bandwidth.posePaddingBits * perUpdate / 8.0,
         bandwidth.poseControlBytes * perUpdate, bandwidth.poseOuterBytes * perUpdate,
         bandwidth.poseDeltas ? double(bandwidth.poseIncludedDeltaBones) / bandwidth.poseDeltas : 0.0,
         static_cast<unsigned long long>(bandwidth.posePredictiveUpdates), bandwidth.poseRigidBones * perUpdate,
         isHost ? static_cast<int>(bandwidth.poseMaxBasisError) : -1, rotationBound, basisError * 600.0 / 4096.0);
    // tx_total_bytes_per_s = (ENet bytes + relay-header bytes x datagrams) / window: the envelope-inclusive figure to hold
    // against 1 MB/s. steam.cpp's envelope counter already includes the payload, so the header share is the difference.
    Logf("net: bandwidth rates enet_tx_datagrams_per_s=%.1f enet_tx_bytes_per_s=%.1f "
         "steam_api_tx_datagrams_per_s=%.1f steam_api_tx_bytes_per_s=%.1f tx_total_bytes_per_s=%.1f "
         "tx_enet_relay_MB_per_s=%.3f relay_header_bytes_per_s=%.1f relay_header_pct=%.2f",
         (sentPackets - lastSentPackets) * perSecond, (sentData - lastSentData) * perSecond,
         steamPackets * perSecond, steamBytes * perSecond, framingBytes * perSecond,
         ((sentData - lastSentData) + (framingBytes - steamBytes)) * perSecond / 1000000.0,
         (framingBytes - steamBytes) * perSecond,
         steamBytes ? 100.0 * (framingBytes - steamBytes) / steamBytes : 0.0);
    Logf("net: bandwidth overhead enet_header_bytes_per_s=%.1f enet_ack_bytes_per_s=%.1f "
         "protocol_overhead_pct=%.2f enet_parse_failures=%llu enet_unparsed_datagrams=%llu",
         enetHeaderBytes * perSecond, enetAckBytes * perSecond,
         steamBytes ? 100.0 * ((framingBytes - steamBytes) + enetHeaderBytes) / steamBytes : 0.0,
         static_cast<unsigned long long>(traffic.txEnetParseFailures - lastSteamTraffic.txEnetParseFailures),
         static_cast<unsigned long long>(traffic.txEnetUnparsedDatagrams - lastSteamTraffic.txEnetUnparsedDatagrams));
    // Each line is bounded below Logf's 900-byte buffer.
    detail::ForEachKindLine(bandwidth, [](char const* line) { Logf("%s", line); });
    detail::ForEachKindRateLine(bandwidth, perSecond, [](char const* line) { Logf("%s", line); });
    for (size_t i = 0; i < host->peerCount; ++i) {
        ENetPeer const& peer = host->peers[i];
        if (peer.state != ENET_PEER_STATE_CONNECTED) continue;
        auto const snapshot = peerBudgets[i].Snapshot();
        Logf("net: valve peer=%u npc_stretch_permille=%u level=%u signals=%u queue_bytes=%u offered_bytes_per_s=%llu "
             "reliable_inflight=%u rtt_ms=%u base_rtt_ms=%u throttle=%u transitions=%u "
             "bulk_pace_bytes_per_s=%u bulk_credit_bytes=%lld bulk_deferred=%llu",
             isHost ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(peer.data)) : 1u, snapshot.stretchPermille,
             static_cast<unsigned>(snapshot.pressure), static_cast<unsigned>(snapshot.signals),
             static_cast<unsigned>(snapshot.queueBytes), static_cast<unsigned long long>(snapshot.offeredBytesPerSecond),
             peer.reliableDataInTransit, peer.roundTripTime, snapshot.baseRttMs, peer.packetThrottle,
             snapshot.pressureTransitions, budgetRate / 4, static_cast<long long>(snapshot.bulkCreditBytes),
             static_cast<unsigned long long>(snapshot.deferredCount));
    }
    bandwidth = detail::BandwidthCounters{};
    bandwidthSince = now;
    lastSentPackets = sentPackets;
    lastSentData = sentData;
    lastReceivedPackets = receivedPackets;
    lastReceivedData = receivedData;
    lastSteamTraffic = traffic;
}

void Tick(PlayerState const& local, bool haveLocal)
{
    if (g3t::ai::ShuttingDown()) return;
    if (!host) return;
    const uint32_t now = enet_time_get();
    if (lastServiceAt) serviceGaps.Add(now - lastServiceAt);
    lastServiceAt = now;
    Service(local, haveLocal);
    // FrameScope flushes ACKs and all gameplay together even on menu/rate-limited frames.
    RttLog();
    BandwidthLog();
}

// QA: NETSTATS lines, one per connection, now; rttlogoff stops the 5-s lines.
void Stats(void (*log)(char const*))
{
    char line[900];
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
