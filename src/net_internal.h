// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <enet/enet.h>

#include "net.h"
#include "enet_policy.h"
#include "guid_key.h"
#include "latency.h"
#include "net_metrics.h"
#include "npcwire_adapter.h"
#include "npcwire_bones.h"
#include "npcwire_control.h"
#include "npcwire_control_policy.h"
#include "npcwire_pose.h"
#include "options.h"
#include "parity.h"
#include "peer_timeout.h"
#include "playerwire.h"
#include "replication.h"
#include "steam.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::net::internal {

enum : uint8_t {
    MsgHello = 1, MsgWelcome = 2, MsgState = 3, MsgLeave = 4, MsgNpcs = 5,
    MsgMotionName = 6, MsgHitNpc = 7, MsgHitPlayer = 8, MsgStates = 9, MsgTalk = 10,
    MsgItem = 11, MsgContents = 12, MsgContainerTx = 13, MsgTxResult = 14, MsgWatch = 15,
    MsgKilled = 16, MsgRefused = 17, MsgDeath = 18,
    MsgClock = 22, MsgWorldState = 23, MsgSummon = 24, MsgShot = 25,
    MsgPlayerSpell = 26, MsgNpcSpell = 27, MsgEnclave = 28, MsgWorld = 29,
    MsgDialogLine = 30, MsgGoods = 31, MsgOffer = 32, MsgOfferResult = 33,
    MsgRansack = 34, MsgWitness = 35, MsgNpcRelease = 36, MsgRagdoll = 37,
    MsgHitResult = 38, MsgPosedNpc = 39, MsgSave = 40, MsgBoneDictionary = 41,
    MsgBoneAck = 42, MsgBoneRequest = 43, MsgCompactPosedNpc = 44,
    MsgControlBatch = 45, MsgControlId = 46, MsgControlBaseline = 47,
    MsgControlSignal = 48, MsgControlPose = 49, MsgPlayerActivity = 50,
    MsgPoseAck = 51, MsgNpcRemoval = 52, MsgCharacter = 53,
    MsgPlayerResync = 60, MsgNpcLease = 61, MsgNpcEquipment = 62, MsgPlayerEquipment = 63
};

#pragma pack(push, 1)
struct RefusedMsg { uint8_t type; char reason[160]; };
struct ClockMsg { uint8_t type; ClockState state; };
struct WorldStateMsg { uint8_t type; WorldState state; };
struct Welcome { uint8_t type; uint8_t id; options::policy::Snapshot options; };
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
struct NpcEquipmentMsg { uint8_t type; NpcEquipmentNote note; };
struct MotionNameMsg { uint8_t type; uint32_t hash; char name[96]; };
struct HitNpcMsg {
    uint8_t type; uint8_t guid[16]; int32_t delta; uint8_t weapon[16];
    uint8_t to; uint8_t from; uint8_t npc[16]; uint8_t outcome; uint32_t seen;
    uint8_t reaction; uint32_t epoch, sequence, incarnation; MeleeContact contact;
};
struct HitResultMsg { uint8_t type; HitResult result; };
struct HitPlayerMsg { uint8_t type; int32_t delta; uint8_t attacker[16]; uint8_t attackerPlayer; uint8_t to; uint8_t meansToKill; };
struct StatesHeader { uint8_t type; uint8_t kind; uint8_t count; };
struct TalkMsg { uint8_t type; uint8_t begin; uint8_t guid[16]; };
struct ItemMsg { uint8_t type; ItemNote note; };
struct ContentsHeader { uint8_t type; uint8_t guid[16]; uint8_t count; };
struct GoodsHeader { uint8_t type; uint8_t player; uint64_t epoch; uint32_t serial; int32_t gold; uint8_t count; };
struct TxHeader { uint8_t type; uint8_t guid[16]; uint32_t seq; uint8_t count; };
struct TxResultMsg { uint8_t type; uint8_t guid[16]; uint32_t seq; uint8_t ok; };
struct WatchMsg { uint8_t type; uint8_t guid[16]; };
struct KilledMsg { uint8_t type; uint8_t guid[16]; uint8_t to; uint32_t epoch, incarnation; };
struct DeathMsg { uint8_t type; uint8_t guid[16]; uint8_t by; };
struct SummonMsg { uint8_t type; uint8_t from; SummonNote note; };
struct ShotMsg { uint8_t type; uint8_t from; ShotNote note; };
struct RagdollMsg { uint8_t type; uint8_t from; RagdollNote note; };
struct DialogLineMsg { uint8_t type; uint8_t from; DialogLineNote note; };
struct OfferMsg { uint8_t type; OfferNote note; };
constexpr size_t OfferHeaderSize = offsetof(OfferMsg, note) + offsetof(OfferNote, entries);
struct OfferResultMsg { uint8_t type; OfferResultNote note; };
struct PlayerSpellMsg { uint8_t type; uint8_t from; uint8_t to; char script[32]; };
struct RansackMsg { uint8_t type; uint8_t from; uint8_t to; RansackNote note; };
struct WitnessMsg { uint8_t type; uint8_t from; uint8_t to; WitnessNote note; };
struct NpcSpellMsg { uint8_t type; uint8_t from; uint8_t to; char script[32]; uint8_t target[16]; int32_t args; uint8_t targetPlayer; };
struct EnclaveMsg { uint8_t type; EnclaveNote note; };
struct WorldMsg { uint8_t type; WorldNote note; };
struct HostWorldMsg { uint8_t type; WorldNote note; options::policy::Snapshot options; };
struct SaveMsg { uint8_t type; SaveNote note; };
struct CharacterMsg { uint8_t type; CharacterNote note; };
#pragma pack(pop)

static_assert(sizeof(HitNpcMsg) == 112, "NPC hit wire layout changed");
constexpr size_t LegacyHitResultMsgSize = offsetof(HitResultMsg, result) + offsetof(HitResult, knockbackValid);
static_assert(LegacyHitResultMsgSize == 34 && sizeof(HitResultMsg) == 68, "HitResultMsg wire layout changed");
static_assert(sizeof(NpcsHeader) == 23, "NPC datagram budget must match its wire header");
static_assert(sizeof(SaveMsg) == 122, "save message wire layout changed");
static_assert(sizeof(CharacterMsg) == 88, "character envelope wire layout changed");
static_assert(sizeof(ActivityMsg) == 19, "player activity heartbeat wire layout changed");
static_assert(sizeof(BoneRequestMsg) == 7, "Bone request wire layout changed");
static_assert(sizeof(BonePosePrefix) == 4 && sizeof(PoseAckMsg) == 37, "Pose interval/ack wire layout changed");
static_assert(offsetof(BoneRequestMsg, layoutId) == 5 && offsetof(BonePosePrefix, layoutId) == 2,
              "Bone request or pose field offsets changed");
constexpr uint8_t PoseAnchorFlag = 0x80;
constexpr uint8_t PoseEssentialFlag = 0x40;
static_assert(sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader) + sizeof(npcwire::WireControl) +
              12 + npcwire::MotionSlots * 8 + sizeof(npcwire::WireQuaternion) +
              npcwire::MaxMetadataBytes + 1 + npcwire::MotionSlots * (sizeof(uint16_t) + sizeof(npcwire::WireFullMotion))
                  <= NpcDatagramBudget,
              "largest compact NPC record must fit the unfragmented budget");

struct DirectPunchSession {
    uint64_t account = 0;
    uint64_t generation = 0;
    uint8_t cookie[16] = {};
    steam::DirectEndpoint candidates[8] = {};
    unsigned candidateCount = 0;
    bool ready = false;
    bool inboundVerified = false;
    bool bidirectionalVerified = false;
    bool endpointLocked = false;
    bool connectStarted = false;
    bool directFailed = false;
    bool fallbackLogged = false;
    ENetAddress verifiedAddress{};
    uint32_t startedAt = 0;
    uint32_t lastProbeAt = 0;
};

constexpr uint32_t DirectPunchTimeoutMs = 1500;
constexpr uint32_t DirectPunchIntervalMs = 100;
constexpr size_t DirectPunchBytes = 26;

struct PoseRoute {
    bool alt = false;
    enet_uint32 checkpointed[2] = {};
};

struct ControlState {
    uint16_t id = 0, nextBaseline = 0;
    uint32_t nextAnchor = 0;
    npcwire::BaselineRef current{};
    NpcRecord latest{};
    uint8_t lifeFlags = 0;
};

struct ControlOffer {
    npcwire::BaselineRef ref{};
    npcwire::BaselineRef resyncReference{};
    uint32_t at = 0, serial = 0;
    npcwire::ResyncReason resyncReason = npcwire::ResyncReason::None;
    bool idQueued = false, baselineQueued = false, acknowledged = false, resyncHandled = false;
};

struct Handlers {
    NpcHandler npc = nullptr;
    NpcLeaseHandler npcLease = nullptr;
    NpcPoseHandler npcPose = nullptr;
    PlayerStateHandler playerState = nullptr;
    PlayerEquipmentHandler playerEquipment = nullptr;
    PeerLeftHandler peerLeft = nullptr;
    NpcReleaseHandler npcRelease = nullptr;
    NpcRemovalHandler npcRemoval = nullptr;
    NpcEquipmentHandler npcEquipment = nullptr;
    MotionNameHandler motionName = nullptr;
    HitNpcHandler hitNpc = nullptr;
    HitResultHandler hitResult = nullptr;
    HitPlayerHandler hitPlayer = nullptr;
    StateHandler state = nullptr;
    TalkHandler talk = nullptr;
    ItemHandler item = nullptr;
    ContentsHandler contents = nullptr;
    GoodsHandler goods = nullptr;
    ContainerTxHandler tx = nullptr;
    TxResultHandler txResult = nullptr;
    WatchHandler watch = nullptr;
    KilledHandler killed = nullptr;
    DeathHandler death = nullptr;
    ClockHandler clock = nullptr;
    WorldStateHandler worldState = nullptr;
    SummonHandler summon = nullptr;
    ShotHandler shot = nullptr;
    RagdollHandler ragdoll = nullptr;
    DialogLineHandler dialogLine = nullptr;
    OfferHandler offer = nullptr;
    OfferResultHandler offerResult = nullptr;
    PlayerSpellHandler playerSpell = nullptr;
    RansackHandler ransack = nullptr;
    WitnessHandler witness = nullptr;
    NpcSpellHandler npcSpell = nullptr;
    EnclaveHandler enclave = nullptr;
    WorldHandler world = nullptr;
    SaveHandler save = nullptr;
    CharacterHandler character = nullptr;
};

namespace bonewire = npcwire::bones;

inline uint16_t BoneWire16(uint8_t const* data)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

inline uint64_t BoneWire64(uint8_t const* data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
    return value;
}

inline uint32_t ControlWire32(uint8_t const* data)
{
    return static_cast<uint32_t>(BoneWire16(data)) | (static_cast<uint32_t>(BoneWire16(data + 2)) << 16);
}

using NpcKeyValue = GuidKey;
using NpcKeyHash = GuidHash;

inline NpcKeyValue NpcKey(uint8_t const* guid) noexcept
{
    return ToGuidKey(guid);
}

inline uint32_t DirectCookieTag(uint8_t const cookie[16])
{
    uint32_t tag = 0;
    std::memcpy(&tag, cookie, sizeof(tag));
    return tag ? tag : 0x67337431u;
}

extern ENetHost* host;
extern ENetPeer* server;
extern ENetAddress target;
extern uint32_t lastConnectAttempt;
extern uint32_t startedAt;
extern bool steamLinked;
extern uint64_t hostRelayGeneration;
extern bool linkEnding;
extern bool connectLogged;
extern bool everConnected;
extern bool isHost;
extern uint8_t localId;
extern bool connected;
extern bool refused;
extern char refusalReason[160];
extern uint16_t helloVersion;
extern char localName[24];
extern int sendRate;
extern uint32_t budgetRate;
extern uint32_t relayEnvelopeBytes;
extern PeerBudget peerBudgets[MaxPlayers];
extern playerwire::Encoder playerSenders[MaxPlayers][MaxPlayers];
extern playerwire::Decoder playerReceivers[MaxPlayers];
extern uint32_t playerRequestAt[MaxPlayers];
extern PlayerState currentLocal;
extern bool haveCurrentLocal;
extern playerwire::Cadence playerCadence;
extern bool forcePlayerFull[MaxPlayers][MaxPlayers];
extern PlayerEquipment pendingLocalEquip;
extern bool havePendingLocalEquip;
extern PlayerEquipment currentLocalEquip;
extern bool haveCurrentEquip;
extern uint32_t equipSequence;
extern uint32_t sentEquipSeq[MaxPlayers][MaxPlayers];
bool FlushPlayerEquipment(uint32_t now);
bool HandlePlayerEquipmentMessage(ENetPeer* peer, ENetPacket const* packet);
void ClearPeerEquipTables(uint8_t id);
void ClearAllEquipTables();
extern Remote remotes[MaxPlayers];
extern uint32_t worldEpoch;
extern detail::BandwidthCounters bandwidth;
extern Handlers handlers;
extern WorldHandler& worldHandler;
extern PlayerStateHandler& playerStateHandler;
extern WorldStateHandler& worldStateHandler;
extern uint32_t hitSequence;
extern replication::Sequence hitClaims[MaxPlayers];
extern ENetPeer* peers[MaxPlayers];
extern uint64_t peerAccounts[MaxPlayers];
extern uint64_t peerRelayGenerations[MaxPlayers];
extern bool inWorld;
extern options::policy::Snapshot hostOptions;
extern bool clientInWorld[MaxPlayers];
extern uint32_t generations[MaxPlayers];
extern std::vector<uint8_t> goodsCache[MaxPlayers];
extern uint8_t goodsReplay[MaxPlayers];
extern DirectPunchSession directSessions[MaxPlayers];
extern latency::Samples enqueueDelays;
extern latency::Samples serviceGaps;
extern unsigned serviceCaps;
extern unsigned serviceErrors;
extern bool activitySent;
extern uint32_t lastActivitySend;
extern char startupError[192];
extern bool enetInitialized;
extern uint32_t welcomes;
extern uint32_t joins;
extern uint32_t worldEntries;
extern uint32_t budgetLossAt[MaxPlayers];
extern uint32_t budgetLastLost[MaxPlayers];
extern std::unordered_map<uint64_t, uint8_t> rememberedSlots;
extern unsigned gatedIn, gatedOut;
extern bool haveQueued;
extern uint32_t queuedAt, lastServiceAt;
extern uint32_t peerConnectData[MaxPlayers];
extern PoseRoute poseRoutes[MaxPlayers];

struct PreparedControl {
    npcwire::Control value{};
    npcwire::BaselineRef ref{};
    std::vector<uint8_t> encoded{};
    bool available = false, encodable = false, exact = false;
};

// NPC replication state owned by net_npc.cpp / net_npc_pose.cpp
extern uint32_t npcSequence;
extern uint32_t npcPublication;
extern std::unordered_map<std::string, replication::Sequence> npcOrigins[MaxPlayers];
extern std::unordered_map<NpcKeyValue, replication::ReplicaOrder, NpcKeyHash> npcPublications;
extern unsigned poseSent, poseReceived, poseSendFailed;
extern unsigned npcAccepted, npcRejected, npcStale;
extern unsigned controlErrors, npcControlFallback;
extern npcwire::Context controlSender, controlReceiver;
extern uint32_t receivedControlGeneration;
extern bonewire::Dictionary boneLayouts;
extern std::unordered_map<NpcKeyValue, bonewire::PoseCache, NpcKeyHash> sentPoses[MaxPlayers], decodedPoses;
extern uint64_t boneGeneration, receivedBoneGeneration;
extern uint32_t nextBoneLayout;
extern std::unordered_map<uint16_t, uint32_t> boneOffered[MaxPlayers];
extern std::unordered_set<uint16_t> boneAcknowledged[MaxPlayers];
extern bool boneExhaustionLogged;
extern unsigned boneInvalidLogs, boneFallbackFailureLogs;
extern std::unordered_map<NpcKeyValue, ControlState, NpcKeyHash> controlStates;
extern unsigned npcControlCreated, npcControlAckAccepted;
extern uint64_t npcOrderedSetupPoses, npcOrderedSetupControls, npcAnchorsSent, npcAnchorsReceived;

int ENET_CALLBACK InterceptTransportPacket(ENetHost* currentHost, ENetEvent*);
bool PeerRelayOrDirect(ENetPeer const* peer);
bool ClaimAuthenticatedPeerIdentity(ENetPeer const* peer, uint64_t& account, char (&name)[24], uint64_t& generation);
void LockDirectSessionEndpoint(uint64_t account, uint64_t generation, bool locked);
void SyncDirectSessions(uint32_t now);
void SyncClientSteamLink(uint32_t now);
void ClearPeerPlayerTables(uint8_t id);
void ClearAllPlayerTables();
BudgetClass SendPriority(uint8_t kind, bool reliable);
void UpdateBudgets(uint32_t now);
bool StoryHandshake(void const* data, size_t size);
bool DoorHandshake(void const* data, size_t size);
bool XpHandshake(void const* data, size_t size);
bool SendPlayerState(ENetPeer* peer, PlayerState const& state);
bool Sane(PlayerState& state);
unsigned ClientCount();
void LeavePeer(ENetPeer* peer, char const* reason, bool disconnect);
DirectPunchSession* FindClientDirectSession();
bool ClientPunchWaiting(DirectPunchSession const* direct, uint32_t now);
void ConnectClientWithDirectFallback(DirectPunchSession* direct, bool& useDirect, ENetAddress& connectTo, bool logFallback);
void FinishClientConnectAttempt(DirectPunchSession* direct, uint32_t now);
void ResetClientLinkState(bool clearRefusal);
void CloseClientLink();
void HandleHello(ENetPeer* peer, bool exactSize, uint16_t version, char const* name, uint64_t const* parityItems);
void HandleSaveMessage(ENetPeer* peer, ENetPacket const* packet);
void MarkDirectConnectFailed();
bool StartFailed(char const* reason);
void DropClientRemotes();
void ProcessRelayDrops();

size_t PeerIndex(ENetPeer const* peer);
uint8_t PeerSlotOf(ENetPeer const* peer);
uint8_t AuthenticatedSender(ENetPeer const* peer);
uint8_t DestinationId(ENetPeer const* peer);
void ResetBudget(ENetPeer* peer);
bool Open(ENetPeer* peer);
bool Send(ENetPeer* peer, void const* data, size_t size, uint8_t channel, bool reliable);
void SendToServer(void const* data, size_t size, uint8_t channel, bool reliable);
void SendToPeer(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t to);
void SendBroadcast(void const* data, size_t size, uint8_t channel, bool reliable);
void SendBroadcastExcept(void const* data, size_t size, uint8_t channel, bool reliable, uint8_t except);
void Drop(uint8_t id);

bool BoneLog(unsigned& count);
bool SendBoneControl(ENetPeer* peer, uint8_t type, std::vector<uint8_t> const& payload);
bool SaneNpc(NpcRecord const& record);
PreparedControl PrepareControl(NpcRecord const& record, bool forceBaseline = false);
bool ControlReady(uint8_t peerId, PreparedControl const& prepared);
bool ControlQueued(uint8_t peerId, PreparedControl const& prepared);
bool OfferControl(uint8_t peerId, PreparedControl const& prepared, bool force = false);
bool DecodeControls(ENetPeer* peer, NpcsHeader const& header, uint8_t const* data, size_t size,
                    std::vector<NpcRecord>& records, std::vector<uint8_t>& acceptedBatch,
                    bool retainStaleAnchor = false);
bool HandleControlBaseline(ENetPeer* peer, ENetPacket const* packet);
void ResetBoneReplicationState();
void ForgetBonePeer(uint8_t id);
void ForgetControlPeer(uint8_t id);
void ClearClientControlTables();
void ClearClientNpcTables();
void ResetNpcCountersOnStart();
bool SendFullNpc(ENetPeer* peer, NpcRecord const& record, bool reliable);
void HandleControlId(ENetPeer* peer, ENetPacket const* packet);
bool HandleControlSignal(ENetPeer* peer, ENetPacket* packet, uint8_t& peerId, NpcRecord& latest);
bool HandleNpcStreamMessage(ENetPeer* peer, ENetPacket* packet, uint8_t type);

bool HandleGameplayMessage(ENetPeer* peer, ENetPacket* packet, uint8_t type);
void RttLog();
void EmitBandwidthDetails(uint32_t elapsed, uint32_t sentData, uint32_t sentPackets,
                          uint32_t receivedData, uint32_t receivedPackets,
                          uint32_t lastSentData, uint32_t lastSentPackets,
                          uint32_t lastReceivedData, uint32_t lastReceivedPackets,
                          steam::TrafficCounters const& traffic,
                          steam::TrafficCounters const& lastSteamTraffic);

} // namespace g3t::net::internal

namespace g3t::net {
using namespace internal;
}
