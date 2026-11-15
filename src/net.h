// SPDX-License-Identifier: GPL-3.0-or-later
// Transport: ENet through private loopback Steam bridges. One player hosts (id 1), others connect. Kept free of SDK headers so that
// winsock2 and the game's headers never meet in one translation unit.
#pragma once

#include <cstdint>
#include <vector>
#include "netbudget.h"
#include "player_state.h"
#include "pose.h"
#include "npclease.h"

namespace g3t::net {
constexpr int NpcMotionSlots = 8;
#pragma pack(push, 1)
struct NpcMotion {
    uint32_t name = 0, instance = 0;
    float phase = 0.0f, speed = 1.0f, weight = 0.0f;  // weight is the current rendered blend, not its target
    float intro = 0.0f, extro = 0.0f;
    int32_t blend = 0;
    uint32_t options[7] = {};
    uint8_t active = 0;  // bit 0: active; bit 1: the host passed a null native descriptor
};
#pragma pack(pop)

// One NPC as the host sees it. Identity is the entity's GUID, which is the same in every copy of the world.
#pragma pack(push, 1)
struct NpcRecord
{
    uint8_t guid[16] = {};
    float position[3] = {};
    float rotation[4] = {};
    uint8_t movementMode = 0;
    // bit 0: dead, bit 1: down (knocked out), bit 2: fighting (AI mode combat, Stufe 4.2), bits 3-5: the player it fights (its
    // hero, puppet or party member; 0 none; also sent outside combat), bit 6: the owner's actual IsInParadeMode result, bit 7: it died by ragdoll
    // (the game's ZS_RagDollDead; the mirrors then fall by physics too).
    uint8_t flags = 0;
    int32_t hitPoints = 0;
    int32_t hitPointsMax = 1;
    uint32_t motion = 0;     // hash of the visible motion's name; names travel once via SendMotionName
    uint8_t aniState = 0;    // gEAniState (2 standing, 4 fighting stance ...); 0 = unknown
    uint32_t heldRight = 0;  // HashName of the item in the right hand (a drawn weapon), 0 = empty
    uint32_t heldLeft = 0;   // the same for the left hand (shield, bow)
    uint32_t sent = 0;  // protocol 61: capture time in the owner's clock, shared by pose, motion and parry history
    uint8_t partyPlayer = 0;  // protocol 66: co-op party leader, independent of which machine simulates this NPC
    uint8_t partyType = 0, waiting = 0, guidePlayer = 0;
    uint8_t aiMode = 0;
    uint8_t action = 0, attackReason = 0, damageType = 0;
    int32_t damageAmount = 0;
    float damageMultiplier = 0.0f;
    uint32_t incarnation = 1;
    uint8_t hitPlayer = 0;
    uint16_t hitSequence = 0;
    uint8_t contact = 0;
    uint8_t attitude = 0;    // protocol 71: gCNPC_PS.AttitudeToPlayer2; the game colours an NPC's name tag from it
    uint32_t combatTarget = 0; // stable FNV-1a32 GUID reference to the actual NPC opponent
    NpcMotion motions[NpcMotionSlots];
};
#pragma pack(pop)
static_assert(sizeof(NpcMotion) == 61, "NPC motion wire layout changed");
static_assert(sizeof(NpcRecord) == 588, "NPC full-record fallback wire layout changed");

struct Remote
{
    PlayerState previous;
    PlayerState latest;
    uint32_t previousTicks = 0;  // net::Now() when previous/latest arrived
    uint32_t latestTicks = 0;
    bool connected = false;
    PlayerActivity activity; // global helper evidence, independent of loaded/presented position
    uint32_t acceptedStates = 0, metadataWaits = 0, staleStates = 0, preEntryStates = 0;
};

bool Start(bool host);
char const* StartupError();  // a startup failure; no direct-IP fallback is opened
void Stop();  // at shutdown: disconnect now so the others notice at once
void Stats(void (*log)(char const*));      // QA: NETSTATS (rtt and packet loss per connection)
void RttLogOff(void (*log)(char const*));  // QA negative control: no "net: rtt=" lines
// ENet's round trip to that player in ms (host: its connection; client: the one to the host, which relays the others), 0 unknown.
uint32_t RoundTrip(uint8_t player);
void Reconnect(void (*log)(char const*));  // QA, client (stage W1): leave the host and connect again at once
// Service ENet/gameplay on the engine thread and send local state at the configured rate.
void Tick(PlayerState const& local, bool haveLocal);
// Flush all gameplay queued after Tick in this frame and wake the independent Steam relay.
void Flush();
void SetSendRate(int hz);  // 20..60 Hz for player states, default 30; NPC captures are independent
void SetBandwidthBudget(uint32_t bytesPerSecond);
#define G3T_REMOTE_NPC_FOCUS_SUPPORTED 1
uint32_t RemoteFocusNpc(uint8_t player);
uint32_t RemoteTargetNpc(uint8_t player);
uint32_t RemoteStateAgeMs(uint8_t player); // UINT32_MAX when no current in-world state is known
// Successful network sends charge once in Send. Do not CommitQueued again from callers.
BudgetSnapshot PeerBandwidth(uint8_t to);
// Emergency congestion factor for the NPC scheduler: 1000 normally, up to 4000 only on real congestion signals
// (ENet throttle, a standing outgoing queue, RTT growth with queued data, fresh loss). Applied to every NPC alike.
uint32_t NpcStretchPermille(uint8_t to);
uint8_t LocalId();
// Host: always. Client: welcomed by the host and in the host's world (SetInWorld); gameplay pauses otherwise.
bool Connected();
Remote const* Remotes();  // indexed by player id, size MaxPlayers; activity survives closed presentation/world gates
constexpr int MaxPlayers = 8;
uint32_t HashWorld(char const* name);
uint32_t HashName(char const* name);
uint32_t HashGuid(uint8_t const guid[16]);
bool IsHost();
bool SendNpcLease(uint8_t to, npclease::Message const& note);
using NpcLeaseHandler = void (*)(uint8_t from, npclease::Message const& note);
void SetNpcLeaseHandler(NpcLeaseHandler handler);
// Increments whenever a client enters the host's world (after a join, or after loading the host's save again); the host
// resends its motion-name table and container contents then.
uint32_t JoinCount();
uint32_t WorldEntryCount();  // client: local world-gate openings, including re-entry on the same connection

// The world gate (persist.cpp, PLAN-2026-10-22 0.1/0.2): gameplay messages flow only between the host and clients that
// are in the host's world. Joining, the save transfer and MsgWorld itself always pass.
void SetInWorld(uint8_t id, bool in);  // host: that client entered (left) the host's world; client: id = LocalId(), this machine
bool InWorld(uint8_t id);
bool Joined();                   // client: welcomed by the host, in its world or not
uint32_t ConnectingFor();        // client: ms since it began to connect, while it never reached the host; else 0
char const* RefusalReason();     // client: why the host refused this machine (empty if it did not), as the host wrote it
void ClaimVersionForTest(uint16_t version);  // QA stage W7 only: the next hello claims this protocol version (0: ours)
void PrimePoseWrapForTest();  // QA stage W7 only: the next hello claims this protocol version (0: ours)
uint32_t WelcomeSerial();        // client: counts the welcomes this machine got, local only (persist binds an autoload to one)
void AdvanceWelcomeSerialForTest();  // QA stage W6 only: simulate a new local welcome without a wire/server change
bool PeerJoined(uint8_t id);     // host: a client with this id is connected, in the host's world or not
// Host: which connection holds slot id, counted up at every hello it accepts there. A player who leaves and joins again
// within one frame keeps its slot, so PeerJoined never turns false in between (Codex review 22.10. R7).
uint32_t PeerGeneration(uint8_t id);
char const* PeerName(uint8_t id);
void GateCounts(unsigned& in, unsigned& out);  // gameplay messages dropped at the gate so far
// World identity, reliable. Host -> one client: WorldHost, the world the host plays (epoch changes with every load, new
// game and return to the menu; campaign: the save a player loads to be in it). Client -> host: WorldReady (this player is
// ready for that world); host -> client WorldReady acknowledges after reliable initial catch-up. WorldOther means
// another save, a new game of its own, or no game. WorldWaiting means it is in
// the host's save, but its own hero record was refused; it waits until the record loads, owner 23.10.; hosts before it
// ignore the op).
enum : uint8_t { WorldHost = 1, WorldReady = 2, WorldOther = 3, WorldWaiting = 4 };
enum : uint8_t { KindMenu = 0, KindNew = 1, KindSaved = 2 };
enum : uint8_t { WorldSyncOff = 1 };  // flags: QA negative control, the host passes no world change on (the old behaviour)
enum : uint8_t { WorldEntryConfirmed = 2 };  // client confirms applied cutover after the host's WorldReady ACK
#pragma pack(push, 1)
struct WorldNote {
    uint8_t op = 0;
    uint8_t kind = 0;      // host's world: KindMenu, KindNew (not saved yet), KindSaved; client: its own
    uint8_t flags = 0;
    uint32_t epoch = 0;    // the host's world; a client answers with the epoch it means
    char campaign[64] = {};
};
#pragma pack(pop)
bool SendWorld(WorldNote const& note, uint8_t to);  // reliable queue result; host: client `to`; client: host
using WorldHandler = void (*)(uint8_t from, WorldNote const& note);
void SetWorldHandler(WorldHandler handler);

// NPC snapshots (normally unreliable; life transitions reliable) and motion names (reliable).
// Only the host publishes NPC state. Motion slots and pose share one capture time.
constexpr int MaxNpcsPerMessage = 48;  // receiving limit
constexpr int NpcsPerSend = MaxNpcsPerMessage;
constexpr int NpcDatagramBudget = 1172; // ENet payload before fragmentation.
static_assert(NpcsPerSend <= MaxNpcsPerMessage, "NPC batch exceeds receive limit");
void SendNpcs(NpcRecord const* records, int count, bool reliable = false);
void SendPosedNpc(NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags = 0);
// Per-recipient variants return false if the message could not be queued.
bool SendNpcsTo(uint8_t to, NpcRecord const* records, int count, bool reliable = false);
bool SendPosedNpcTo(uint8_t to, NpcRecord const& record, pose::Bone const* bones, int count,
                    uint8_t poseFlags = 0, bool reliable = false, uint8_t intervalMs = 16);
// Essential describes the source update, not setup/anchor transport reliability.
using NpcPoseHandler = void (*)(NpcRecord const&, pose::Bone const*, int, uint8_t poseFlags, bool essential, uint8_t intervalMs);
void SetNpcPoseHandler(NpcPoseHandler handler);
void SendNpcRelease(uint8_t const* guids, int count);  // host retires interest; this never restores client NPC AI
bool SendNpcReleaseTo(uint8_t to, uint8_t const* guids, int count);
using NpcReleaseHandler = int (*)(uint8_t owner, uint32_t generation, uint8_t* guids, int count);
void SetNpcReleaseHandler(NpcReleaseHandler handler);
#pragma pack(push, 1)
struct NpcRemovalNote {
    uint8_t guid[16] = {};
    uint32_t epoch = 0, world = 0, incarnation = 0;
};
#pragma pack(pop)
static_assert(sizeof(NpcRemovalNote) == 28, "NPC removal wire layout");
bool SendNpcRemovalTo(uint8_t to, NpcRemovalNote const& note);
using NpcRemovalHandler = void (*)(NpcRemovalNote const&);
void SetNpcRemovalHandler(NpcRemovalHandler handler);
void ResetNpcReplication();  // forget ordering/history fences on a world change, not the host publication clock
void NpcReplicationCounts(unsigned& accepted, unsigned& rejected, unsigned& stale);
void NpcPoseCounts(unsigned& sent, unsigned& received, unsigned& failed);
void SendMotionName(uint32_t hash, char const* name);
// Client side: received NPC snapshots and motion names are handed to these callbacks.
// owner: always 1; client NPC publications are refused.
// sent: the sender's net::Now() when the snapshot went out.
// Compact accepted records in place and return their count.
using NpcHandler = int (*)(NpcRecord* records, int count, uint32_t now, uint32_t sent, uint8_t owner, uint32_t generation);
using MotionNameHandler = void (*)(uint32_t hash, char const* name);
void SetNpcHandlers(NpcHandler npcs, MotionNameHandler names);
using PlayerStateHandler = void (*)(PlayerState const& state, uint32_t arrived, bool reset);
void SetPlayerStateHandler(PlayerStateHandler handler);  // each accepted network state, before the next render frame
using PeerLeftHandler = void (*)(uint8_t id, char const* name);
void SetPeerLeftHandler(PeerLeftHandler handler);

// Damage, reliable. Client -> host: this client hit an NPC (by GUID) for delta (negative) hit points.
// Host -> one client: an NPC hit that player's puppet for delta.
// Any machine -> the NPC's owner `to` (through the host): this player hit it. weapon: template ID, zeros = none.
// npc: the GUID of one of the sender's NPCs that hit it (zeros or null: the sender's hero). outcome: what the game decided
// for this hit of the sender's hero on the sender's machine (its hit judgement runs before AddHitPoints), see HitOutcome.
// seen: the owner's send time of the NPC's state the sender showed when its hero struck (Stufe 3.3), 0 unknown.
enum HitOutcome : uint8_t { HitUnjudged = 0, HitKnockOut = 1, HitDeath = 2 };
enum HitReaction : uint8_t { HitNoReaction = 0, HitQuickStumble = 1, HitStumble = 2, HitPierceStumble = 3, HitSitKnockDown = 4 };
enum MeleeMode : uint8_t { NotMelee = 0, PredictedMelee = 1, HostJudgedMelee = 2 };
#pragma pack(push, 1)
struct MeleeContact {
    // HostJudgedMelee submits contact intent without a local damage judgement or reaction.
    uint8_t melee = 0, targetOnly = 0, item = 0;
    uint8_t focus[16] = {}, target[16] = {};
    uint32_t motion = 0;
};
#pragma pack(pop)
static_assert(sizeof(MeleeContact) == 39, "melee contact wire layout changed");
inline bool ValidHitContact(int32_t delta, uint32_t incarnation, uint32_t seen,
                            uint8_t outcome, uint8_t reaction, MeleeContact const& contact)
{
    if (contact.melee > HostJudgedMelee || contact.targetOnly > 1 || contact.item > 1 ||
        outcome > HitDeath || reaction > HitSitKnockDown) return false;
    if (contact.melee == HostJudgedMelee)
        return delta == 0 && outcome == HitUnjudged && reaction == HitNoReaction && contact.motion &&
               (incarnation || !seen);
    return incarnation && delta < 0 && delta >= -1000000;
}
uint32_t SendHitNpc(uint8_t to, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16] = nullptr,
                   uint8_t outcome = HitUnjudged, uint32_t seen = 0, uint8_t reaction = HitNoReaction, uint32_t incarnation = 1,
                   MeleeContact const& contact = {});
// attacker: the hitting NPC (GUID), or zeros with attackerPlayer = the player whose hero hit. meansToKill: its owner says the NPC
// means to kill (the game's HasAttackReasonToKill; the victim's machine has only a mirror of it, which has no target or reason).
void SendHitPlayer(uint8_t playerId, int32_t delta, uint8_t const attacker[16], uint8_t attackerPlayer, bool meansToKill = false);
using HitNpcHandler = void (*)(uint8_t from, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16],
                               uint8_t outcome, uint32_t seen, uint8_t reaction, uint32_t sequence, uint32_t incarnation,
                               MeleeContact const& contact);
using HitPlayerHandler = void (*)(int32_t delta, uint8_t const attacker[16], uint8_t attackerPlayer, bool meansToKill);
void SetHitHandlers(HitNpcHandler npc, HitPlayerHandler player);
#pragma pack(push, 1)
struct HitResult {
    uint8_t guid[16] = {};
    uint32_t epoch = 0, sequence = 0, incarnation = 0, processedAt = 0;
    uint8_t accepted = 0;
    uint8_t knockbackValid = 0;
    uint8_t hostReaction = 0;
    uint16_t hitDurationMs = 0;
    uint16_t moveDurationMs = 0;
    float originPos[3] = {};
    float goalPos[3] = {};
    float stepCm = 0.0f;
};
#pragma pack(pop)
void SendHitResult(uint8_t to, HitResult const& result);
using HitResultHandler = void (*)(HitResult const&);
void SetHitResultHandler(HitResultHandler handler);
// Host -> one client: your hit killed the NPC with this GUID (the killer earns the experience).
void SendKilled(uint8_t to, uint8_t const guid[16], uint32_t incarnation);
using KilledHandler = void (*)(uint8_t const guid[16], uint32_t incarnation);
void SetKilledHandler(KilledHandler handler);
// An NPC died where nobody mirrored it (journal.cpp). Client: to the host. Host: to every client except
// `except`, with `by` = the player in whose region it died.
void SendDeath(uint8_t const guid[16], uint8_t by, uint8_t except);
using DeathHandler = void (*)(uint8_t by, uint8_t const guid[16]);
void SetDeathHandler(DeathHandler handler);
// Reliable host-save controls. Values 4/5/8 from paired checkpoints are retired.
enum SaveOp : uint8_t { SaveRequest = 1, SavePrepare = 2, SaveReady = 3, SaveCancel = 6, SaveResult = 7 };
#pragma pack(push, 1)
struct SaveNote {
    uint8_t op = 0, ok = 0;
    uint32_t epoch = 0;
    char world[33] = {}, save[33] = {}, name[49] = {};
};
#pragma pack(pop)
static_assert(sizeof(SaveNote) == 121, "save note wire layout changed");
bool SendSave(uint8_t to, SaveNote const& note); // client: to 1; host: to one welcomed client
using SaveHandler = void (*)(uint8_t from, SaveNote const& note);
void SetSaveHandler(SaveHandler handler);
enum CharacterPurpose : uint8_t { CharacterCapture = 1, CharacterRestore = 2 };
#pragma pack(push, 1)
struct CharacterNote {
    uint8_t purpose = 0;
    uint32_t epoch = 0;
    uint64_t account = 0;
    uint32_t length = 0, crc = 0;
    char world[33] = {}, save[33] = {};
};
#pragma pack(pop)
static_assert(sizeof(CharacterNote) == 87, "character wire header changed");
bool SendCharacter(uint8_t to, CharacterNote const& note, std::vector<uint8_t> const& bytes);
using CharacterHandler = void (*)(uint8_t from, CharacterNote const&, std::vector<uint8_t> const&);
void SetCharacterHandler(CharacterHandler handler);
uint64_t PeerAccount(uint8_t id);
// Host -> every client: the game clock (clock.cpp).
#pragma pack(push, 1)
struct ClockState { uint32_t timestamp = 0; uint16_t year = 0; uint16_t day = 0; uint8_t hour = 0; uint8_t minute = 0; };
#pragma pack(pop)
void SendClock(ClockState const& state);  // host: to every client; client: a request to the host (a sleep)
using ClockHandler = void (*)(ClockState const& state);
void SetClockHandler(ClockHandler handler);
// A world state after a dialog command (infocmd.cpp). kind 1: an entity's state by GUID; kind 2: a sector
// (name in routine, status in enclave); kind 3: a dialog started a fight (guid = attacker, guard = the player
// attacked, routine = the attack reason's name); kind 4/5: a client took over / gave back a companion (guid); kind 6: host to player `guard`: that claim is refused
// or ended; kind 7: host to player `guard`: this NPC from the host's save is your companion;
// kind 8/9: downed.cpp (guard=target, npcClass=helper, first four guid bytes=target's downedSequence);
// kind 10: a door or lock (doors.cpp: guard = open, alignment = locked, npcClass bits 1 door, 2 lock); kind 16: client to host,
// player `guard` asks for NPC guid that has an important info for its hero (address.cpp); kind 18: for player `guard`, NPC
// guid now remembers its fight with that player as `npc` (LastFightAgainstPlayer, fightmem.cpp).
// Client: to the host; host: to every client except `except`.
#pragma pack(push, 1)
struct WorldState {
    uint8_t kind = 0;
    uint8_t guid[16] = {};
    uint8_t dialog = 0;     // bits: teach, trade, party, mob, slavery; 0x80: has a dialog
    uint8_t npc = 0;        // 1: has the NPC fields below
    uint8_t guard = 0, npcClass = 0, alignment = 0, immortal = 0;
    uint8_t enclave = 0;    // bits: has an enclave, raid, revolution
    uint8_t enclaveStatus = 0;
    uint8_t moved = 0;      // 1: a MoveTo command moved it; only then is the position taken
    char routine[48] = {};
    float position[3] = {};
    float rotation[4] = {};
};
#pragma pack(pop)
static_assert(sizeof(WorldState) == 102, "world state wire layout changed");
void SendWorldState(WorldState const& state, uint8_t except);
// Targeted XP baseline/ack and shared-door snapshot request/reply, including pre-admission restoration.
bool SendWorldStateTo(uint8_t to, WorldState const& state);
using WorldStateHandler = void (*)(uint8_t from, WorldState const& state);
void SetWorldStateHandler(WorldStateHandler handler);

// Named story state (story.cpp), reliable. Host -> all clients: the host's values. Client -> host: values this
// player changed. Kind flag 0x80 brackets a pre-admission snapshot; empty 0x80 requests/begins it,
// empty 0xc0 ends it; 0xa0 carries reward-free cutover changes before entry confirmation.
// Quest/info names use stable IDs; runtime uses named u32 values; domain 5 carries the bootstrap calendar;
// domain 6 carries host-authoritative camp (m_arrPoliticalFame) and enclave (PlayerFame) reputations.
enum : uint8_t { StateQuests = 1, StateInfos = 2, StateEvents = 3, StateQuestRuntime = 4, StateWorldClock = 5, StateReputation = 6 };
struct StateEntry {
    uint8_t value;
    char name[63];
};
constexpr int MaxStatesPerMessage = 64;
bool SendStates(uint8_t kind, StateEntry const* entries, int count);
bool SendStatesTo(uint8_t to, uint8_t kind, StateEntry const* entries, int count);
using StateHandler = void (*)(uint8_t from, uint8_t kind, StateEntry const* entries, int count);  // from: client id, 1 = host
void SetStateHandler(StateHandler handler);

// Dialogs, reliable, client -> host: this client's hero began (or ended) a dialog with the NPC with this GUID.
void SendTalk(bool begin, uint8_t const guid[16]);
using TalkHandler = void (*)(uint8_t from, bool begin, uint8_t const guid[16]);
void SetTalkHandler(TalkHandler handler);

// World items, reliable. Client -> host: ItemTook (this player's hero took that world item). Host -> clients:
// ItemGone (remove it from the world). Host -> one client: ItemDenied (it was gone already; give it back).
enum : uint8_t { ItemTook = 1, ItemGone = 2, ItemDenied = 3 };
struct ItemNote {
    uint8_t op;
    uint8_t guid[16];
    uint32_t amount;
    char name[48];  // item template
};
void SendItem(ItemNote const& note, uint8_t to);  // client: to the host (to ignored); host: to that player, 0 = all
using ItemHandler = void (*)(uint8_t from, ItemNote const& note);
void SetItemHandler(ItemHandler handler);

// Containers (containers.cpp), reliable. Host -> all: a container's contents. Client -> host: a transaction
// (container-side deltas). Host -> one client: its result. Client -> host: watch this trader for a while.
struct ContentEntry {
    uint8_t id[16];  // item template ID (names are not unique: "Appow" and "Arrow" merged when recreated by name)
    char name[28];   // for logs
    int32_t amount;
};
// The count goes as one byte. 48 was too few for a big trader: the contents were silently not sent (Codex review R4,
// 22.10.). One message, up to 250 * 48 bytes; ENet splits a reliable packet that size itself and it arrives whole.
constexpr int MaxContentEntries = 250;
bool SendContents(uint8_t const guid[16], ContentEntry const* entries, int count);  // false: not sent
bool SendContainerTx(uint8_t const guid[16], uint32_t seq, ContentEntry const* entries, int count);  // false: not sent
void SendContainerTxResult(uint8_t to, uint8_t const guid[16], uint32_t seq, bool ok);
void SendWatch(uint8_t const guid[16]);
// Player trade (trade.cpp): what this player can trade, sent whenever it changes; the host passes it on. On receipt
// player is the sender (set by the host). Items of one template with different qualities (sharp, blessed, forged ...) are
// entries of their own; the size stays that of a ContentEntry.
struct GoodsEntry {
    uint8_t id[16];    // item template ID
    uint16_t quality;  // gEItemQuality flags (below 1024, as a hero record's)
    char name[26];     // for logs
    int32_t amount;
};
constexpr int MaxGoodsEntries = MaxContentEntries;
bool SendGoods(uint64_t epoch, uint32_t serial, int32_t gold, GoodsEntry const* entries, int count);
using GoodsHandler = void (*)(uint8_t player, uint64_t epoch, uint32_t serial, int32_t gold, GoodsEntry const* entries, int count);
void SetGoodsHandler(GoodsHandler goods);
using ContentsHandler = void (*)(uint8_t const guid[16], ContentEntry const* entries, int count);
using ContainerTxHandler = void (*)(uint8_t from, uint8_t const guid[16], uint32_t seq, ContentEntry const* entries, int count);
using TxResultHandler = void (*)(uint8_t const guid[16], uint32_t seq, bool ok);
using WatchHandler = void (*)(uint8_t from, uint8_t const guid[16]);
void SetContainerHandlers(ContentsHandler contents, ContainerTxHandler tx, TxResultHandler result, WatchHandler watch);
// Summoned things (summon.cpp, P1), reliable. A player's own summon got a net ID (caster id in the top byte); the
// others replay it on that player's puppet. op 1: spawned; op 2: gone. Client -> host; host -> every other client
// (relayed, `from` = the caster).
enum : uint8_t { SummonSpawned = 1, SummonGone = 2, SummonCreature = 3, SummonCreatureRequest = 4 };
#pragma pack(push, 1)
struct SummonNote {
    uint8_t op = 0;
    uint32_t netId = 0;
    uint8_t target[16] = {};  // GUID of the spell's target (zeros: none)
    float position[3] = {};
    float rotation[4] = {};
    char spellItem[40] = {};  // the item cast from (It_Spell_Meteor, a scroll ...)
    char spawn[40] = {};      // the summoned entity's template (Smn_Meteor ...)
    uint8_t guid[16] = {};    // SummonCreature: the creature's GUID, which its copy takes over (SetID)
    uint8_t ownerPlayer = 0;
    int32_t args = 0;
    uint32_t epoch = 0;
};
#pragma pack(pop)
void SendSummon(SummonNote const& note);
using SummonHandler = void (*)(uint8_t from, SummonNote const& note);
void SetSummonHandler(SummonHandler handler);
// Who did a deed and with what (A0/B0, agreed by agents A and B on 20.10.): the context a message about an arrow, a
// spell, a summon or a controlled creature carries, so that witnesses, crimes and reputation (enclaves.cpp) can blame
// the right player. Not on the wire yet: whoever sends it first adds it to that message and raises the protocol.
#pragma pack(push, 1)
struct DeedContext {
    char player[24] = {};   // the responsible player's name (PlayerState::name, config `name`): the same in every session
    uint8_t playerId = 0;   // that player's net ID: this session only
    uint32_t deedId = 0;    // NextDeedId() on the player's machine: a (player, deedId) counts once, repeats are dropped
    uint8_t body[16] = {};  // GUID of the body that acted: zeros for the player's own hero, else a creature it controls
    uint8_t source[16] = {};  // GUID of what did it (an arrow, a summon ...); zeros: the body itself
    uint8_t target[16] = {};  // GUID of what it was done to; zeros: nothing or an area
};
#pragma pack(pop)
uint32_t NextDeedId();  // counts up from 1 per session on this machine
// Shots: player and locally simulated NPC projectiles. Reliable on the combat channel, independent of bulk state.
// `from` identifies the publishing machine; caster identifies an NPC when it is not that machine's player.
#pragma pack(push, 1)
struct ShotNote {
    uint32_t netId = 0;
    uint8_t target[16] = {};  // GUID of the shooter's target (zeros: none)
    float position[3] = {};
    float rotation[4] = {};
    float direction[3] = {};  // PSProjectile.TargetDirection
    float velocity = 0.0f;    // PSProjectile.ShootVelocity
    uint8_t path = 0;         // gEProjectilePath
    uint8_t touch = 0;        // gEProjectileTouchBehavior
    int32_t damage = 0;       // PSDamage.DamageAmount (arrow plus bow)
    float multiplier = 0.0f;  // PSDamage.DamageHitMultiplier
    char arrow[40] = {};      // the arrow's template (or a spell's projectile: Mis_Fireball ...)
    char spellItem[40] = {};  // a spell's projectile: the item cast from (empty for arrows)
    uint8_t caster[16] = {};  // zero: player `from`; otherwise the NPC that actually cast/fired
    uint8_t targetPlayer = 0;
    char spell[64] = {};      // NPCs can cast directly without a spell item in their hand
    uint32_t epoch = 0;
    uint32_t sent = 0;
};
#pragma pack(pop)
void SendShot(ShotNote const& note);
using ShotHandler = void (*)(uint8_t from, ShotNote const& note);
void SetShotHandler(ShotHandler handler);
// Host ragdoll impulses. Clients wait for the matching incarnation, presentation time and active physics.
// Reliable on the combat channel; clients cannot publish NPC forces.
#pragma pack(push, 1)
struct RagdollNote {
    uint8_t guid[16] = {};
    float at[3] = {};     // where the force acts, relative to the NPC's own position
    float force[3] = {};
    uint32_t epoch = 0;
    uint32_t sequence = 0, incarnation = 1, sent = 0;
};
#pragma pack(pop)
void SendRagdoll(RagdollNote const& note);
using RagdollHandler = void (*)(RagdollNote const& note);
void SetRagdollHandler(RagdollHandler handler);
// A spell a player cast on another player's puppet (spells.cpp): the target player replays the script on its own hero.
// Any machine -> player `to` (through the host). script: the script's name (only a fixed list is replayed).
void SendPlayerSpell(uint8_t to, char const* script);
using PlayerSpellHandler = void (*)(uint8_t from, char const* script);
void SetPlayerSpellHandler(PlayerSpellHandler handler);
// A spell a player cast on NPCs another machine simulates (spells.cpp): it runs there. Any machine -> player `to`
// (through the host). target: the NPC's GUID (zeros: the NPCs round the caster); args: the script's int argument.
void SendNpcSpell(uint8_t to, char const* script, uint8_t const target[16], int32_t args);
using NpcSpellHandler = void (*)(uint8_t from, char const* script, uint8_t const target[16], int32_t args);
void SetNpcSpellHandler(NpcSpellHandler handler);
// Robbery (ransack.cpp; owner 24.10.: a guard robs a knocked-out partner as in single-player), reliable, through the host.
// The machine whose NPC robs a player's puppet -> that player: RansackHold (the robber is at work: stay down, as the game
// resets a robbed body's knock-out timer), RansackTake (it took `amount` gold from the puppet: pay it). The robbed player
// -> that machine: RansackPaid (`amount` paid for `seq`; less if the hero no longer had that much).
enum : uint8_t { RansackHold = 1, RansackTake = 2, RansackPaid = 3 };
#pragma pack(push, 1)
struct RansackNote {
    uint8_t op = 0;
    uint32_t seq = 0;         // the robbing machine's number for one robbery (take and paid)
    int32_t amount = 0;       // gold taken (take) or paid (paid)
    uint8_t robber[16] = {};  // GUID of the robbing NPC
};
#pragma pack(pop)
void SendRansack(uint8_t to, RansackNote const& note);
using RansackHandler = void (*)(uint8_t from, RansackNote const& note);
void SetRansackHandler(RansackHandler handler);
// Witnesses (witness.cpp, owner 24.10.), reliable, through the host. WitnessSaw -> the machine that runs the NPC (`to`):
// it saw the sender's player commit a crime (value: the attack reason its game chose); it reacts there, to the sender's
// puppet. WitnessHolds -> everyone (`to` 0; reputation = together): the NPC now holds crime `value` (gEPlayerCrime)
// against the sender's player, and so against everybody's. WitnessJudge -> the machine of the player it judged (P7): an
// NPC judged that player's puppet a criminal here (value: the attack reason, Murder for the witness of a murder this
// machine applied for the other player); there it judges that player's hero, so its game remembers the crime (and an NPC
// run there reacts).
enum : uint8_t { WitnessSaw = 1, WitnessHolds = 2, WitnessJudge = 3 };
#pragma pack(push, 1)
struct WitnessNote {
    uint8_t op = 0;
    uint8_t npc[16] = {};  // GUID of the witness
    int32_t value = 0;
};
#pragma pack(pop)
void SendWitness(uint8_t to, WitnessNote const& note);  // to 0: everybody else
using WitnessHandler = void (*)(uint8_t from, WitnessNote const& note);
void SetWitnessHandler(WitnessHandler handler);
// Town reputation (enclaves.cpp), reliable. Host -> clients (`to`, 0 = all): EnclaveState, the host's counters of one
// enclave, and the host's reputation mode. Client -> host: EnclaveDelta (what this player's own deeds added to the
// counters; suspect/known: the new values) or EnclaveRequest (send me every enclave). Personal mode, the machine that
// simulates a victim -> the killer's machine (client -> host, host -> that client): EnclaveCredit, a murder by that player
// for its own counters (crime: added). EnclavePolitical (protocol 56): a political crime of the sender's player (a town it
// freed; known = gEPoliticalAlignment, fame = how many), for the receiving hero's own memory (enclaves.cpp).
// EnclaveRepDelta: client -> host non-quest dialog reputation delta (ModReputPol: empty name + alignment in known;
// ModReputEnc: enclave name + known = -1; signed delta in fame).
enum : uint8_t {
    EnclaveState = 1,
    EnclaveDelta = 2,
    EnclaveRequest = 3,
    EnclaveCredit = 4,
    EnclavePolitical = 5,
    EnclaveRepDelta = 6,
};
#pragma pack(push, 1)
struct EnclaveNote {
    uint8_t op = 0;
    uint8_t mode = 0;          // host's reputation mode (config.h Reputation)
    char name[32] = {};        // the enclave entity's name (Ardea, Silden ...)
    int32_t crime[5] = {};     // CrimeCount per gEPlayerCrime (state: values; delta: added)
    int32_t suspect[5] = {};   // SuspectComment per crime (values)
    int32_t known = 0;         // KnownPlayerCrime
    int32_t fame = 0;          // PlayerFame (state: value; delta: added)
};
#pragma pack(pop)
void SendEnclave(EnclaveNote const& note, uint8_t to);
using EnclaveHandler = void (*)(uint8_t from, EnclaveNote const& note);
void SetEnclaveHandler(EnclaveHandler handler);
// A line of a player's dialog (dialogaudio.cpp): the other players play the same voice file at the speaker. Any machine
// -> everyone else (through the host), reliable and in order, so a line's end never arrives before its start.
// DialogOptions (phase 5, owner 25.10.: the one who talks chooses, the other sees the choice): one note per option of the
// sender's dialog menu, reason = its number from 1, lineId = how many, text = the option's text id; DialogOptionsGone: the
// menu is gone (an option was chosen, or the dialog ended).
enum : uint8_t { DialogLineStart = 1, DialogLineEnd = 2, DialogOptions = 3, DialogOptionsGone = 4 };
enum : uint8_t { DialogSpeakerNpc = 1, DialogSpeakerPlayer = 2 };
#pragma pack(push, 1)
struct DialogLineNote {
    uint8_t kind = 0;           // DialogLineStart / DialogLineEnd
    uint8_t reason = 0;         // end: 1 finished or skipped, 2 replaced by the next line, 3 dialog over
    uint8_t speakerKind = 0;    // DialogSpeakerNpc: speaker is the NPC's GUID; DialogSpeakerPlayer: the sender's hero
    uint32_t lineId = 0;        // per sender, counts up
    uint8_t speaker[16] = {};
    char sample[128] = {};      // the voice file the game resolved (INFO_..._GER.wav), start only
    char text[64] = {};         // start: the line's text id; its text is INFO_<text> in the receiver's language (empty: none)
};
#pragma pack(pop)
void SendDialogLine(DialogLineNote const& note);
using DialogLineHandler = void (*)(uint8_t from, DialogLineNote const& note);
void SetDialogLineHandler(DialogLineHandler handler);
// Automatic exchanges: owner-side validation, cached outcomes and acknowledgment before saving. Reliable packets may
// be fragmented by ENet; a native basket has the same capacity as the advertised goods.
constexpr int MaxOfferEntries = MaxGoodsEntries * 2;
enum : uint8_t { OfferAccepted = 1, OfferUnavailable = 4, OfferFinalized = 6 };
#pragma pack(push, 1)
struct OfferEntry {
    uint8_t item[16] = {};      // template ID
    int32_t amount = 0;         // + goes to the buyer, - from the buyer
    uint16_t quality = 0;       // gEItemQuality flags of what goes
    char name[26] = {};         // the buyer's name of it (its focus name, UTF-8), for the seller's screen
};
struct OfferNote {
    uint8_t buyer = 0, seller = 0;
    uint8_t acknowledge = 0;
    uint16_t count = 0;
    uint64_t buyerEpoch = 0, sellerEpoch = 0;
    uint32_t serial = 0;        // the buyer's
    int32_t gold = 0;           // + goes to the buyer, - from the buyer
    OfferEntry entries[MaxOfferEntries];
};
struct OfferResultNote {
    uint8_t buyer = 0, seller = 0;
    uint8_t result = 0;
    uint64_t buyerEpoch = 0, sellerEpoch = 0;
    uint32_t serial = 0;
    uint32_t goodsSerial = 0;
};
#pragma pack(pop)
bool SendOffer(OfferNote const& note);              // false: no route to note.seller
void SendOfferResult(OfferResultNote const& note);  // to note.buyer (through the host)
using OfferHandler = void (*)(OfferNote const& note);
using OfferResultHandler = void (*)(OfferResultNote const& note);
void SetOfferHandlers(OfferHandler offer, OfferResultHandler result);
// The clock that stamps received states (ENet's). Compare stamps only against this clock.
uint32_t Now();
uint32_t WorldEpoch();
}
