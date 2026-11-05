// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcs.h"

#include "body.h"
#include "combat.h"
#include "config.h"
#include "guide.h"
#include "guid.h"
#include "arena.h"
#include "ai.h"
#include "log.h"
#include "net.h"
#include "remote.h"
#include "journal.h"
#include "talk.h"
#include "world.h"
#include "npcplayback.h"
#include "replication.h"
#include "contact.h"
#include "guards.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::npcs {
namespace {
constexpr float InterestRadius = 4000.0f;  // cm around any player
// At 20 Hz only what changed since its last snapshot goes out every time; an unchanged NPC still every 100 ms, so the
// bandwidth stays near the old one while moving NPCs show twice as often.
constexpr uint32_t FullEvery = 100;        // ms
bool fastNpcs = true;  // QA negative control: fastnpcsoff (every NPC every 100 ms, as before)
// send_rate (60 Hz by default): the tick of the NPC scan. Every NPC that changed goes out each tick; unchanged ones every FullEvery.
uint32_t SendTick() { return fastNpcs ? std::max<uint32_t>(8, 1000u / static_cast<uint32_t>(GetConfig().sendRate) - 1) : FullEvery; }
struct LastSent { net::NpcRecord record; uint32_t at = 0; };
std::unordered_map<std::string, LastSent> lastSent;
unsigned npcsUnchangedSkipped = 0;
bool Changed(net::NpcRecord const& a, net::NpcRecord const& b)
{
    float d = 0.0f;
    for (int k = 0; k < 3; ++k) d += (a.position[k] - b.position[k]) * (a.position[k] - b.position[k]);
    float q = 0.0f;
    for (int k = 0; k < 4; ++k) q += std::fabs(a.rotation[k] - b.rotation[k]);
    return d > 4.0f || q > 0.002f || a.motion != b.motion || a.flags != b.flags || a.hitPoints != b.hitPoints ||
           a.aniState != b.aniState || a.movementMode != b.movementMode || a.heldRight != b.heldRight || a.heldLeft != b.heldLeft ||
           a.motions[0].instance != b.motions[0].instance || a.hitPointsMax != b.hitPointsMax || a.partyPlayer != b.partyPlayer;
}
constexpr uint32_t StaleAfter = 3000;      // ms without a snapshot: hold presentation and reject contacts

// Owned NPCs (the host's processing area; a client's own region): sent to the others
uint32_t lastSend = 0;
replication::Cadence60 sendCadence;
uint32_t captureTicks = 0, captureStartedAt = 0;
void ClaimTick(uint32_t now);
std::unordered_map<std::string, uint32_t> incarnations;
struct HostHitNote { uint8_t player = 0; uint32_t sequence = 0; };
std::unordered_map<std::string, HostHitNote> hostHits;
std::unordered_map<std::string, std::vector<net::RagdollNote>> forceHistory, pendingForces;
std::unordered_map<std::string, replication::Sequence> forceSequences;
uint32_t forceSequence = 0;
uint32_t knownJoins = 0;
bool wasConnected = false;
std::unordered_set<uint32_t> namesSent;
std::unordered_set<std::string> owned;  // GUIDs this machine sent in its last update
bool owning = true;                     // QA negative control (client): send nothing of its own
unsigned claimsRefused = 0;             // host: a client's snapshots of NPCs the host simulates itself
// Dead stays dead (playtest 27.10.: wolves the host killed lived on at the client, which then simulated them and they
// attacked the host, unseen there). Client: what the host reported dead is held dead here and never sent as its own.
// Host: a client's snapshots of an NPC dead here are refused (combat.cpp drops its hits).
std::unordered_set<std::string> deadOnHost;
bool deadStays = true;  // false: as before 27.10.
bool newLife = true;     // QA negative control deadnewlifeoff: a respawned NPC stays held dead (code review P2, as before)
unsigned deadRefused = 0, deadKept = 0;
unsigned hostWins = 0;                  // client: NPCs it simulated itself until the host's snapshots came (now mirrors)
bool hostWinsRule = true;               // QA negative control: HostWinsOff
constexpr uint32_t JoinGrace = 10000;   // ms after a client joined in which the host keeps the NPCs near its hero
uint32_t joinedAt[net::MaxPlayers] = {};  // host: when each client was first seen connected (net::Now), 0 = not now
unsigned joinRefused = 0;               // host: a joining client's snapshots of NPCs near the host, refused
bool joinGraceRule = true;              // QA negative control: JoinGraceOff
// The edge of the host's range (stageM2 24.10.): an NPC the host's game still processes just outside the host's 40 m, which
// the client's own copy puts just inside, went to the client (Orc_GameStartRaider_Scout_07: 47 m here, 37.7 m there), so two
// machines simulated it, and from the client it fought the host and killed Slave_03 next to it. Whoever simulates an NPC
// first keeps it: such a contested NPC stays the host's while it is within KeepRadius of a player here; the host refuses the
// client's snapshots of it and sends its own, and the client's copy becomes a mirror again. NPCs the host does not process
// (a client's region far from the host, stage E) are not touched.
constexpr float KeepRadius = InterestRadius + 2000.0f;  // cm
std::unordered_set<std::string> contested;  // host: NPCs a client sent that this machine processes in the band
unsigned contestedRefused = 0;
bool contestRule = true;                    // QA negative control: ContestOff
float lastPlayers[net::MaxPlayers][3] = {};  // host: the players of the last SendOwned
int lastPlayerCount = 0;
// Companions: a client that recruits one of the host's NPCs takes it over for good (it follows that player, and
// the game's follow state follows the local hero, GetPlayer()); the host then only mirrors it.
std::unordered_map<std::string, uint8_t> claimedBy;  // host: NPC -> the client that took it over
std::unordered_map<std::string, uint8_t> directedBy;
std::unordered_set<std::string> myClaims;            // client: NPCs this player took over
std::unordered_map<std::string, uint32_t> claimSeen; // host: last sign of life of a claim (claim or snapshot)
std::unordered_map<std::string, uint32_t> givenBack; // client: NPCs handed back, not sent until the host's arrive
std::unordered_set<std::string> hostTaken;           // host: clients' NPCs that guide or follow the host's hero (P9)
// host_sim = elastic (Stufe 2): a client that jumped (partner key) simulates the NPCs around it before the host's sphere
// follows, and whoever simulates an NPC first keeps it, so they stayed the client's while it stood still (stageRoiHold 28.10.:
// 67 % the host's after a jump, 100 % after walking). Inside the host's sphere for ReclaimAfter, near a player, not fighting
// (no attack, parade, stumble or knock-down in its last ReclaimCalm, not lying) and nobody's companion, the host takes it back: own AI here, and the client's copy
// becomes a mirror when the host's snapshots arrive (hostWinsRule). While it stays inside, the client's snapshots of it are
// refused; after it left the sphere (or the sphere is off) it is not taken back for ReclaimPause, so nothing goes to and fro.
constexpr uint32_t ReclaimAfter = 2000;   // ms inside the sphere
constexpr uint32_t ReclaimPause = 15000;  // ms after it left the sphere
constexpr float ReclaimInside = 1000.0f;  // cm inside the sphere's edge
constexpr uint32_t ReclaimCalm = 5000;    // ms without a combat motion
constexpr float ReclaimAlignment = 100.0f;  // cm: local copy must already match its owner's snapshot
constexpr float MaxDisplacement = 500.0f;   // cm: the most a local blow's shove can keep a mirror from its owner's pose
constexpr uint32_t DisplaceHold = 300;      // ms the shove stands after the local stagger ended (the owner's own stagger arrives)
constexpr float DisplaceFade = 700.0f;      // ms: time constant of its fade
constexpr float CorpseAlignment = 300.0f;   // cm: a corpse further from the owner's is moved to it, nearer ones lie where they fell
std::unordered_map<std::string, uint32_t> inSphereSince;  // host: clients' NPCs inside the sphere, since
std::unordered_set<std::string> reclaimed;                // host: taken back; the client's snapshots refused
std::unordered_map<std::string, uint32_t> reclaimPause;   // host: left the sphere; not taken back before then
unsigned reclaims = 0, reclaimRefused = 0;
unsigned reclaimAwaitingDrive = 0, reclaimAwaitingAlignment = 0, staleMirrorReleases = 0;
bool reclaimRule = true;  // QA negative control: reclaimoff
// Stufe 3.3: whether each NPC this machine simulates parried in the states it sent, by send time, so that a player's blow is
// judged against the parade the player saw on its mirror (half a round trip and a snapshot old), not against the parade now.
// The game's IsInParadeMode, not AniState alone: an NPC keeps AniState parade for a while after it stopped parrying (Hamlar,
// stageLagBlock 28.10.: AniState 4 with parade_mode=0), and every blow counted as one on a parrying NPC.
constexpr int StateHistory = 128;  // 60 Hz sends: 2 s of states, more than SentStatesFor needs
struct SentStates
{
    uint32_t at[StateHistory] = {};
    bool parrying[StateHistory] = {};
    int next = 0;
};
std::unordered_map<std::string, SentStates> sentStates;
constexpr uint32_t SentStatesFor = 1500;  // ms
// On by default since 29.10. (stageLagBlock under G3_NETSIM 800: without it every blow on an NPC parrying when the claim came,
// though the client saw him not parrying, was blocked; with it they land as seen, 4 of 4 runs). QA lagblockon / lagblockoff.
bool lagBlocks = true;
unsigned swingSamples = 0, swingArmed = 0;  // Stufe 4.1 (SwingSamples)
unsigned fightSent = 0, fightSeenAny = 0, fightSeenUs = 0, fightSeenNear = 0;  // Stufe 4.2 (FightStats)
constexpr uint32_t RevokeAfter = 15000;   // ms: a claim whose owner sends nothing goes back to the host
constexpr uint32_t GiveBackFor = 5000;    // ms: a handed-back NPC is not sent while the host takes it over
unsigned claimsDenied = 0, claimsRevoked = 0;
// Host: whose companion each NPC is, by player name. Unlike claimedBy it outlives a disconnect and goes into the
// save's sidecar: the recruiting (JoinPlayerSlavery: leader, type Slave, not waiting) ran only on that player's
// machine, so in the host's save the NPC is nobody's companion.
std::unordered_map<std::string, std::string> companions;
std::unordered_map<std::string, uint32_t> offered;  // host: when a saved companion was last handed to its player
constexpr uint32_t OfferEvery = 20000;  // ms
bool offering = true;  // QA negative control: CompanionsOff
bool companionNear = true;  // false: a saved companion is handed over wherever it is (as before 27.10.)
unsigned companionsFar = 0;
unsigned companionsOffered = 0, companionsAdopted = 0;

// Client side
struct Mirror
{
    Entity entity;
    bool resolved = false;
    bool missing = false;  // GUID not found in this copy
    uint32_t resolvedAt = 0;  // net::Now() of the last lookup, 0: never looked up
    uint32_t firstSeen = 0;   // net::Now() of its first snapshot
    bool killed = false;
    uint8_t owner = 1;             // player whose machine simulates it
    uint32_t ownerGeneration = 0, hostSession = 0;
    bool guarded = false;          // DefeatedByPlayer forced on while mirrored
    bool defeatedBefore = false;   // its own value, restored on release
    body::State body;
    npcplayback::State replay;
    bool replayReady = false;
    uint32_t contactInstance = 0, contactAt = 0;
    bool contactConsumed = false;
    int contactBone = -1;
    float contactA[3] = {}, contactB[3] = {};
    bool partyApplied = false, appliedWaiting = false;
    uint8_t appliedOwner = 0;
    bool relationAcknowledged = false, guideAcknowledged = false;
    net::NpcRecord previous;
    net::NpcRecord latest;
    uint32_t previousTicks = 0;
    uint32_t latestTicks = 0;
    uint32_t previousSent = 0, latestSent = 0;  // the sender's clock (body::Speed)
    uint32_t heldRight = 0, heldLeft = 0;  // what this mirror holds now (hashes)
    uint32_t holdChecked = 0;
    char skeleton[24] = {};        // prefix of its motion names (Orc, Hero, ...), for the lying-dead motions
    bool laid = false;             // lying-dead start motion played
    bool ragdollStarted = false;
    bool ragdollInitialized = false, corpsePlaced = false, corpseSimulating = false;
    replication::TerminalLife deathLife;
    std::vector<pose::Bone> corpsePose;
    uint8_t corpsePoseFlags = 0;
    bCMatrix corpseRoot;
    void const* corpseActor = nullptr;
    uint32_t corpsePoseAt = 0;
    unsigned ragdollStarts = 0, ragdollWakes = 0;
    uint32_t ragdollTryAt = 0;     // next attempt to enable the physics (the body must be processed for it)
    unsigned ragdollFails = 0;     // attempts that left the ragdoll off: the corpse lies by animation meanwhile
    uint32_t lieCheckAt = 0;
    unsigned lieTries = 0;
    uint8_t lieNoted = 0;          // log lines written about lying down (PLAN-2026-10-22 1.5): 1 no skeleton, 2 lies, 4 gave up
    uint32_t killedAt = 0;         // net::Now() when it died here (mirrored death or journal), for the late check
    bool lateNoted = false;        // "corpse X after 45 s: lies / STANDS" written
    bool wasMirror = false;        // a dead mirror whose owner stopped sending it, laid down here further
    bool released = false, staleHeld = false;
    uint32_t shownSent = 0, truthSent = 0;
    bool havePresentation = false;
    // Our blows shove a mirror here before its owner's snapshots show the same: the displacement (where it stood minus where the
    // owner's buffered pose had it) is kept for a moment once the local stagger ends and then fades, so it blends back to the
    // owner's pose instead of jumping there.
    bool displacePending = false;
    float displace[3] = {};
    uint32_t displaceAt = 0;
    uint32_t staggerSuppressUntil = 0;  // until then the owner's replay of the stagger our own blow already showed is skipped
    // The owner's ragdoll force for this body (net::RagdollNote): applied once the physics run here.
    bool forcePending = false;
    float forceAt[3] = {}, force[3] = {};
    uint32_t forceWhen = 0;
};
unsigned holdChanges = 0;
bool style = true;  // QA negative control: StyleOff (no weapons in hand, no stance on mirrors)
constexpr uint32_t RetryLookup = 1000;  // ms between lookups of a mirror whose NPC is not here (yet)
bool retryMissing = true;  // QA negative control: mirrorretryoff (a missing mirror stays missing and stays, as before 23.10.)
unsigned mirrorsFoundLater = 0, mirrorsExpired = 0, mirrorLookups = 0;
std::unordered_map<std::string, Mirror> mirrors;
// Deaths applied here as a state, outside mirroring (journal.cpp: an NPC that died in another player's region, applied
// once this machine processes it). They got AIMode 9 and ZS_Dead, which lays nothing down: the body froze in its last
// pose (deaddiag 19.10., the same as dead mirrors before LayDown; Brenton stood on the host on 21.10.). Laid down like
// dead mirrors. Looked up by GUID every time, never kept as a raw pointer (save-forget-npclist-c.md).
std::unordered_map<std::string, Mirror> corpses;
std::unordered_map<std::string, net::NpcRecord> absences;
uint32_t absenceCheckAt = 0;
std::unordered_map<uint32_t, std::string> motionNames;

std::string Key(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }

void Guid(eCEntity* entity, uint8_t out[16]) { guid::CopyEntity(entity, out); }

Entity Resolve(uint8_t const guid[16]) { return guid::Resolve(guid); }

// Which item an NPC holds in a hand, as the hash of the item's name (the same template name on every machine).
uint32_t HeldHash(Entity& npc, gESlot slot)
{
    const Entity held = npc.Inventory.GetItemFromSlot(slot);
    return held.GetInstance() ? net::HashName(held.GetName().GetText()) : 0;
}

// The stack of the mirror's own inventory with that item, or -1.
GEInt StackOf(Entity& npc, uint32_t hash)
{
    if (!hash) return -1;
    const GEInt count = npc.Inventory.GetStackCount();
    for (GEInt i = 0; i < count; ++i)
        if (Entity item = npc.Inventory.GetTemplateItem(i); item.GetInstance() && net::HashName(item.GetName().GetText()) == hash)
            return i;
    return -1;
}

void EnsureItem(Entity& npc, uint32_t hash)
{
    if (!hash || StackOf(npc, hash) >= 0) return;
    auto name = motionNames.find(hash);  // the name table carries item names too
    if (name == motionNames.end()) return;
    npc.Inventory.CreateItems(Template(name->second.c_str()), 0, 1);
}

bool IsNpc(eCEntity* instance)
{
    return instance && instance->GetPropertySet(eEPropertySetType_NPC) && instance->GetPropertySet(eEPropertySetType_ScriptRoutine) &&
           instance->GetPropertySet(eEPropertySetType_DamageReceiver) && instance != gCSession::GetInstance().GetHero() &&
           instance != remote::ControlledBody() && !instance->GetPropertySet(eEPropertySetType_PlayerMemory) &&
           !remote::IsPuppet(instance);
}

// QA (NPCSEND): why SendOwned last left an NPC out, or "sent", and when (net::Now()).
struct SendNote { char const* reason = "never"; uint32_t at = 0; };
std::unordered_map<std::string, SendNote> sendNotes;
// Fewer changes of hands around the edge (owner 27.10.: NPCs changed hands three times as often when the players stood
// some 40 m apart, every change can jerk). What this machine sends it keeps up to KeepRadius from the nearest player as
// long as it processes it, not just to InterestRadius; for MinHold after it began, and while it fights, up to HoldRadius.
// Taking one over still needs InterestRadius. With host_roi the host processes far enough for this (world.cpp).
struct Held { uint32_t since = 0, last = 0; };
std::unordered_map<std::string, Held> sending;  // since when and until when this machine sent each NPC
bool hysteresis = true;  // false: sent only within InterestRadius (as before 27.10.)
constexpr uint32_t MinHold = 10000;  // ms
unsigned heldBand = 0;

// Host: a client's NPC becomes a mirror here (stageM2 24.10.: Orc_GameStartRaider_Scout_07 ran on the client 12 s after a
// join, 1 run in 3): where it is here and there, and why this machine last left it out of its own snapshots.
void NoteTakeover(std::string const& key, net::NpcRecord const& record, uint8_t owner)
{
    static unsigned noted = 0;
    if (noted >= 60) return;
    ++noted;
    Entity npc = Resolve(reinterpret_cast<uint8_t const*>(key.data()));
    eCEntity* body = remote::ControlledBody();
    if (!npc.GetInstance() || !body) return;
    const bCVector& h = body->GetWorldPosition();
    const bCVector& p = npc.GetInstance()->GetWorldPosition();
    const float hero[3] = {h.GetX(), h.GetY(), h.GetZ()}, here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    const auto sent = sendNotes.find(key);
    Logf("npcs: host mirrors %s of player %u: here %.1f m from the host's hero, there %.1f m; last left out here: %s, %d ms ago",
         npc.GetName().GetText(), owner, body::Distance(here, hero) / 100.0f, body::Distance(record.position, hero) / 100.0f,
         sent == sendNotes.end() ? "never seen" : sent->second.reason, sent == sendNotes.end() ? -1 : static_cast<int>(net::Now() - sent->second.at));
}

// Host: is this client's NPC one this machine processes in the band (left out of its last snapshots for range only, within
// KeepRadius of a player)? Then it stays here (see KeepRadius).
bool Contested(std::string const& key, uint8_t owner)
{
    const auto sent = sendNotes.find(key);
    if (sent == sendNotes.end() || std::strcmp(sent->second.reason, "range") || static_cast<int32_t>(net::Now() - sent->second.at) > 1000) return false;
    Entity npc = Resolve(reinterpret_cast<uint8_t const*>(key.data()));
    if (!npc.GetInstance() || npc.IsDead() || !lastPlayerCount) return false;
    const bCVector& p = npc.GetInstance()->GetWorldPosition();
    const float here[3] = {p.GetX(), p.GetY(), p.GetZ()};
    float nearest = 1e30f;
    for (int k = 0; k < lastPlayerCount; ++k) nearest = std::min(nearest, body::Distance(here, lastPlayers[k]));
    if (nearest > KeepRadius) return false;
    if (contested.insert(key).second && contested.size() < 200)
        Logf("npcs: %s of player %u refused: here %.1f m from the nearest player, in this machine's band; sent from here now",
             npc.GetName().GetText(), owner, nearest / 100.0f);
    return true;
}

bool receiving = true;  // QA (invariant self-test): RecvOff drops all NPC snapshots, so both machines simulate

int OnNpcs(net::NpcRecord* records, int count, uint32_t now, uint32_t sent, uint8_t owner, uint32_t generation)
{
    if (net::IsHost() || owner != 1) return 0;
    if (!receiving) return 0;
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        const std::string key = Key(records[i].guid);
        const uint32_t captured = records[i].sent ? records[i].sent : sent;
        const auto existing = mirrors.find(key);
        if (existing != mirrors.end() &&
            static_cast<int32_t>(records[i].incarnation - existing->second.latest.incarnation) < 0) continue;
        const auto absent = absences.find(key);
        if (absent != absences.end() &&
            static_cast<int32_t>(records[i].incarnation - absent->second.incarnation) <= 0) continue;
        if (records[i].absent) {
            absences[key] = records[i];
            mirrors.erase(key);
            corpses.erase(key);
            pendingForces.erase(key);
            absenceCheckAt = 0;
            records[accepted++] = records[i];
            continue;
        }
        if (absent != absences.end()) {
            absences.erase(absent);
            world::RequestProcessingUpdate();
        }
        if (!(records[i].flags & 1)) journal::NewLife(records[i].guid);
        if (net::IsHost() && existing != mirrors.end()) {
            Mirror const& current = existing->second;
            const bool ownerConnected = current.owner > 1 && current.owner < net::MaxPlayers &&
                                        net::PeerJoined(current.owner) && net::InWorld(current.owner) &&
                                        current.ownerGeneration == net::PeerGeneration(current.owner);
            if (current.owner != owner && !current.released && ownerConnected) {
                ++claimsRefused;
                continue;  // packet arrival order must not transfer a connected owner's NPC to another client
            }
        }
        // Whoever simulated an NPC first keeps it. Both starting at once: the host keeps it, and the client
        // turns its copy into a mirror as soon as the host's snapshots arrive.
        if (net::IsHost() && hostTaken.count(key)) { ++claimsRefused; continue; }  // it guides or follows the host's hero (P9)
        if (net::IsHost() && reclaimed.count(key)) { ++reclaimRefused; continue; }  // taken back into the elastic sphere
        if (deadStays && net::IsHost() && owner > 1 && !(records[i].flags & 1)) {
            Entity here = Resolve(records[i].guid);
            if (here.GetInstance() && here.IsDead()) { ++deadRefused; continue; }
        }
        if (owned.count(key) && !claimedBy.count(key)) {
            // Host: it keeps what it simulates. Client: host-approved snapshots win over its own simulation (a mirror gone
            // stale for 3 s gets its own AI back and is then sent from here; refusing the host's snapshots left both
            // simulating the NPC for good: Slave_04 following the host in stageControl, Lester in stageP3a, 20.10.).
            if (net::IsHost() || !hostWinsRule) { ++claimsRefused; continue; }
            owned.erase(key);
            ++hostWins;
        }
        const auto claim = claimedBy.find(key);
        if (claim != claimedBy.end() && claim->second != owner) { ++claimsRefused; continue; }  // someone else's companion
        if (net::IsHost() && owner > 1 && owner < net::MaxPlayers && claim == claimedBy.end() && joinGraceRule) {
            // A client that just joined sends what it processes before the host's first snapshots reach it; the host then
            // took over as mirrors the NPCs around its own hero (37 of 40 runs, 20.10.: raiders fought by the host ran on
            // the client). Near the host's hero, the host keeps them for a while after a join; the client turns its copies
            // into mirrors when the host's snapshots arrive.
            if (!joinedAt[owner]) joinedAt[owner] = now ? now : 1;
            if (static_cast<int32_t>(now - joinedAt[owner]) < static_cast<int32_t>(JoinGrace) && !mirrors.count(key)) {
                eCEntity* body = remote::ControlledBody();
                const bCVector& h = body ? body->GetWorldPosition() : bCVector();
                const float heroAt[3] = {h.GetX(), h.GetY(), h.GetZ()};
                if (body && body::Distance(records[i].position, heroAt) <= InterestRadius) { ++joinRefused; continue; }
            }
        }
        if (claim != claimedBy.end()) claimSeen[key] = now;
        givenBack.erase(key);  // the host sends it again: mirror it
        if (net::IsHost() && owner > 1 && claim == claimedBy.end() && !mirrors.count(key)) {
            if (contestRule && Contested(key, owner)) { ++contestedRefused; continue; }
            NoteTakeover(key, records[i], owner);
        }
        Mirror& m = mirrors[key];
        const bool sameOwner = m.owner == owner && m.ownerGeneration == generation &&
                               m.hostSession == net::WelcomeSerial() && m.latest.incarnation == records[i].incarnation;
        if (m.latestTicks && sameOwner && static_cast<int32_t>(captured - m.latestSent) < 0) continue;
        if (m.latestTicks && !sameOwner) {
            if (m.latest.incarnation != records[i].incarnation) {
                m.deathLife = replication::TerminalLife{};
                m.ragdollStarted = m.ragdollInitialized = m.corpsePlaced = m.corpseSimulating = false;
                m.corpsePose.clear();
                m.corpseActor = nullptr;
                m.corpsePoseAt = m.ragdollTryAt = m.lieCheckAt = 0;
                m.ragdollStarts = m.ragdollWakes = 0;
                m.ragdollFails = m.lieTries = m.lieNoted = 0;
                m.laid = false;
                if (auto forces = pendingForces.find(key); forces != pendingForces.end()) {
                    auto& queue = forces->second;
                    queue.erase(std::remove_if(queue.begin(), queue.end(), [&](net::RagdollNote const& force) {
                        return static_cast<int32_t>(force.incarnation - records[i].incarnation) < 0;
                    }), queue.end());
                    if (queue.empty()) pendingForces.erase(forces);
                }
                std::string previousLife = key;
                previousLife.append(reinterpret_cast<char const*>(&m.latest.incarnation), sizeof(m.latest.incarnation));
                forceSequences.erase(previousLife);
            }
            m.body.track = body::Track{};
            m.body.presentationSerial = 0;
            m.body.actionSeq = -1;
            m.latestTicks = 0;  // timestamps and velocity from different owners must never be mixed
            m.havePresentation = false;
        }
        if (!m.firstSeen) m.firstSeen = now ? now : 1;
        m.owner = owner;
        m.ownerGeneration = generation;
        m.hostSession = net::WelcomeSerial();
        m.previous = m.latestTicks ? m.latest : records[i];
        m.previousTicks = m.latestTicks ? m.latestTicks : now;
        m.previousSent = m.latestTicks ? m.latestSent : captured;
        m.latest = records[i];
        m.latestTicks = now;
        m.latestSent = captured;
        m.released = false;
        m.staleHeld = false;
        body::MotionState motion;
        motion.motion = records[i].motion;
        motion.heldRight = records[i].heldRight;
        motion.heldLeft = records[i].heldLeft;
        motion.aniState = records[i].aniState ? records[i].aniState : -1;
        motion.actionSeq = static_cast<int>(records[i].motions[0].instance);
        motion.motionFrac = records[i].motions[0].active ? static_cast<int>(records[i].motions[0].phase * 65535.0f) : 65535;
        motion.flags = records[i].flags;
        motion.hitPoints = records[i].hitPoints;
        motion.hitPointsMax = records[i].hitPointsMax;
        motion.incarnation = records[i].incarnation;
        motion.hitPlayer = records[i].hitPlayer;
        motion.hitSequence = records[i].hitSequence;
        motion.partyPlayer = records[i].partyPlayer;
        motion.partyType = records[i].partyType;
        motion.waiting = records[i].waiting;
        motion.guidePlayer = records[i].guidePlayer;
        motion.aiMode = records[i].aiMode;
        motion.action = records[i].action;
        motion.attackReason = records[i].attackReason;
        motion.damageType = records[i].damageType;
        motion.damageAmount = records[i].damageAmount;
        motion.damageMultiplier = records[i].damageMultiplier;
        motion.presentationReady = records[i].presentationReady;
        motion.contact = records[i].contact;
        motion.attitude = records[i].attitude;
        std::memcpy(motion.motions, records[i].motions, sizeof(motion.motions));
        m.body.track.predictive = false;
        // Feed every packet, including several packets serviced in one engine frame.
        body::Feed(m.body.track, records[i].position, records[i].rotation, captured, now, records[i].movementMode, motion);
        records[accepted++] = records[i];
    }
    return accepted;
}

int OnRelease(uint8_t owner, uint32_t generation, uint8_t* guids, int count)
{
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        const auto it = mirrors.find(Key(guids + i * 16));
        if (it != mirrors.end() && it->second.owner == owner && it->second.ownerGeneration == generation) {
            it->second.released = true;
            std::memmove(guids + accepted++ * 16, guids + i * 16, 16);
        }
    }
    return accepted;
}

// Dead as a state, the way dead mirrors die here (MirrorTick): no death script, so no experience and no crime here.
void KeepDead(Entity& npc)
{
    if (npc.IsDead()) return;
    npc.NPC.DefeatedByPlayer = GETrue;  // the game's scripts reward nothing for it
    npc.Routine.FullStop();
    npc.DamageReceiver.HitPoints = 0;
    npc.Routine.AIMode = gEAIMode_Dead;
    npc.Routine.SetTask(bCString("ZS_Dead"));
    if (++deadKept < 20) Logf("npcs: %s is dead on the host; dead here again", npc.GetName().GetText());
}

void OnMotionName(uint32_t hash, char const* name)
{
    if (motionNames.size() < 50000 || motionNames.count(hash)) motionNames[hash] = name;  // the game has some 20000 motions
}

// Stufe 4.2: NpcRecord.flags bits 2-5 for an NPC in combat mode: whom it fights, as a player id (our hero, a puppet, or a party
// member of either: the game's fight music counts the player's party).
uint8_t PlayerOf(eCEntity* entity)
{
    if (!entity) return 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (entity == hero || entity == remote::ControlledBody()) return net::LocalId();
    if (const int owner = remote::PuppetOwner(entity)) return static_cast<uint8_t>(owner);
    return 0;
}

uint8_t FightFlags(Entity& npc)
{
    const bool fighting = npc.Routine.AIMode == gEAIMode_Combat;
    if (fighting) ++fightSent;
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    uint8_t player = PlayerOf(target);
    if (!player && target && target->GetPropertySet(eEPropertySetType_Party))
        player = PlayerOf(Entity(target).Party.GetPartyLeader().GetInstance());
    return static_cast<uint8_t>((fighting ? 4 : 0) | ((player & 7) << 3));
}

// Every NPC this machine processes and nobody else simulates, near a player: one snapshot each.
void SendOwned(eCEntity* hero, float const players[][3], int playerCount)
{
    if (!net::IsHost()) return;
    const auto previousOwned = owned;
    owned.clear();
    lastPlayerCount = playerCount;
    for (int k = 0; k < playerCount; ++k) std::memcpy(lastPlayers[k], players[k], sizeof(float) * 3);
    eCEntity* controlled = remote::ControlledBody();
    const bTObjArray<Entity> all = Entity::GetNPCs();
    net::NpcRecord batch[net::MaxNpcsPerMessage];
    int n = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity const& npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == hero || remote::IsPuppet(instance)) continue;
        if (instance == controlled) continue;  // a transformed player's creature travels as its player state
        uint8_t guid[16];
        guid::CopyEntity(instance, guid);
        SendNote& note = sendNotes[Key(guid)];
        note.at = net::Now();
        if (journal::Pending(instance)) { note.reason = "pending"; continue; }  // a recorded death not applied yet: never show it alive
        if (mirrors.count(Key(guid))) { note.reason = "mirror"; continue; }      // simulated elsewhere: we show theirs
        if (deadStays && !net::IsHost() && deadOnHost.count(Key(guid))) {
            Entity corpse(instance);
            KeepDead(corpse);
            note.reason = "dead on host";
            continue;
        }
        const auto back = givenBack.find(Key(guid));
        if (back != givenBack.end() && static_cast<int32_t>(net::Now() - back->second) < 0) { note.reason = "givenback"; continue; }  // handed back
        const bCVector& p = instance->GetWorldPosition();
        const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
        float nearest = 1e30f;
        for (int k = 0; k < playerCount; ++k) nearest = std::min(nearest, body::Distance(at, players[k]));
        bool inRange = nearest <= GetConfig().hostRoi;
        // This player's companion, as long as this machine processes it: dropped at the radius, the host ended the claim
        // for want of snapshots and handed it over again (Gorn, playtest 27.10.).
        if (!inRange && companionNear && !net::IsHost() && myClaims.count(Key(guid)) && nearest <= KeepRadius) inRange = true;
        if (!inRange) {
            const auto held = contested.find(Key(guid));
            if (held != contested.end() && nearest <= KeepRadius) inRange = true;  // contested: kept here in the band
            else if (held != contested.end()) contested.erase(held);
        }
        if (!inRange && hysteresis) {
            const auto held = sending.find(Key(guid));
            const uint32_t t = net::Now();
            if (held != sending.end() && static_cast<int32_t>(t - held->second.last) <= 1500) {  // ours until now
                const bool young = static_cast<int32_t>(t - held->second.since) < static_cast<int32_t>(MinHold);
                const bool fighting = Entity(instance).Routine.AIMode == gEAIMode_Combat;
                if (nearest <= KeepRadius || ((young || fighting) && nearest <= KeepRadius + 2000.0f)) { inRange = true; ++heldBand; }
            }
        }
        if (!inRange) { note.reason = "range"; continue; }
        note.reason = "sent";
        {
            Held& held = sending[Key(guid)];
            const uint32_t t = net::Now();
            if (!held.last || static_cast<int32_t>(t - held.last) > 1500) held.since = t;
            held.last = t;
        }
        net::NpcRecord& r = batch[n];
        r = net::NpcRecord{};
        std::memcpy(r.guid, guid, 16);
        owned.insert(Key(guid));
        std::memcpy(r.position, at, sizeof(at));
        const bCQuaternion q = instance->GetWorldRotation();
        r.rotation[0] = q.GetX(); r.rotation[1] = q.GetY(); r.rotation[2] = q.GetZ(); r.rotation[3] = q.GetW();
        Entity self(instance);
        r.movementMode = static_cast<uint8_t>(self.CharacterMovement.GetMovementMode());
        r.flags = static_cast<uint8_t>((self.IsDead() ? 1 : 0) | (self.IsDown() ? 2 : 0) | FightFlags(self));
        if (instance->GetPropertySet(eEPropertySetType_Party)) {
            r.partyPlayer = PlayerOf(self.Party.GetPartyLeader().GetInstance());
            r.partyType = static_cast<uint8_t>(static_cast<gEPartyMemberType>(self.Party.PartyMemberType));
            r.waiting = self.Party.Waiting ? 1 : 0;
        }
        for (uint8_t player = 1; player < net::MaxPlayers; ++player) {
            eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
            if (actor && actor->GetPropertySet(eEPropertySetType_Party) && Entity(actor).Party.GetPartyLeader().GetInstance() == instance)
                r.guidePlayer = player;
        }
        if (self.IsDead() && (r.movementMode == gECharMovementMode_RagDollDead ||
                              !std::strncmp(self.Routine.GetCurrentTask().GetText(), "ZS_RagDollDead", 14)))
            r.flags |= 128;  // died by ragdoll: the mirrors fall by physics too
        r.hitPoints = self.DamageReceiver.HitPoints;
        r.hitPointsMax = self.DamageReceiver.HitPointsMax;
        r.aniState = static_cast<uint8_t>(static_cast<int>(self.Routine.AniState));
        r.attitude = static_cast<uint8_t>(static_cast<int>(static_cast<gEAttitude>(self.NPC.AttitudeToPlayer2)));
        r.aiMode = static_cast<uint8_t>(body::AIModeOf(instance));
        r.action = static_cast<uint8_t>(static_cast<gEAction>(self.Routine.Action));
        r.attackReason = static_cast<uint8_t>(static_cast<gEAttackReason>(self.NPC.AttackReason));
        Entity damageSource = self.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (!damageSource.GetInstance() || !damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) damageSource = self;
        if (damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) {
            r.damageType = static_cast<uint8_t>(static_cast<gEDamageType>(damageSource.Damage.DamageType));
            r.damageAmount = damageSource.Damage.DamageAmount;
            r.damageMultiplier = damageSource.Damage.DamageHitMultiplier;
        }
        r.sent = net::Now();
        uint32_t& incarnation = incarnations[Key(guid)];
        if (!incarnation) incarnation = 1;
        r.incarnation = incarnation;
        if (auto hit = hostHits.find(Key(guid)); hit != hostHits.end()) {
            r.hitPlayer = hit->second.player;
            r.hitSequence = hit->second.sequence;
        }
        const bool parrying = r.aniState == gEAniState_Parade && combat::InParadeMode(instance);
        if (parrying) r.flags |= 64;
        r.heldRight = HeldHash(self, gESlot_RightHand);
        r.heldLeft = HeldHash(self, gESlot_LeftHand);
        for (gESlot slot : {gESlot_RightHand, gESlot_LeftHand}) {
            const Entity held = self.Inventory.GetItemFromSlot(slot);
            if (!held.GetInstance()) continue;
            const uint32_t hash = net::HashName(held.GetName().GetText());
            if (namesSent.insert(hash).second) net::SendMotionName(hash, held.GetName().GetText());
        }
        char motion[96];
        if (body::CurrentMotion(instance, motion, sizeof(motion)) && *motion) {
            r.motion = net::HashName(motion);
            if (namesSent.insert(r.motion).second) net::SendMotionName(r.motion, motion);
        } else {
            *motion = '\0';
        }
        const bool motionReady = npcplayback::Capture(instance, r, [](uint32_t hash, char const* name) {
            if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
        });
        std::vector<pose::Bone> bones;
        uint8_t poseFlags = 0;
        const bool posed = !(r.flags & 128) && npcplayback::CapturePose(instance, bones, poseFlags,
            [](uint32_t hash, char const* name) {
                if (namesSent.insert(hash).second) net::SendMotionName(hash, name);
            });
        r.presentationReady = (r.flags & 128) ? static_cast<uint8_t>(motionReady) : static_cast<uint8_t>(posed);
        r.contact = motionReady && r.presentationReady && r.motions[0].weight > 0.0001f && body::LocallyAttacking(instance) ? 1 : 0;
        bool essential = false;
        {
            LastSent& last = lastSent[Key(guid)];
            const uint32_t t = net::Now();
            if (!r.presentationReady && !(r.flags & 1) && (!last.at || last.record.presentationReady))
                Logf("npcs: %s is waiting for its native skeletal pose; contacts remain disabled", self.GetName().GetText());
            const bool terminalChanged = (r.flags & 3) &&
                                         (!last.at || (r.flags & 3) != (last.record.flags & 3));
            const bool revived = last.at && (last.record.flags & 3) && !(r.flags & 3);
            if (revived && (last.record.flags & 1) && r.incarnation == last.record.incarnation) {
                uint32_t& incarnation = incarnations[Key(guid)];
                if (++incarnation == 0) ++incarnation;
                r.incarnation = incarnation;
                hostHits.erase(Key(guid));
                forceHistory.erase(Key(guid));
                r.hitPlayer = 0;
                r.hitSequence = 0;
            }
            const bool ragdollNow = (r.flags & 128) && !(last.record.flags & 128);
            const bool partyChanged = last.at && (r.partyPlayer != last.record.partyPlayer || r.waiting != last.record.waiting ||
                                                  r.partyType != last.record.partyType || r.guidePlayer != last.record.guidePlayer);
            const bool hitChanged = last.at && (last.record.hitSequence != r.hitSequence || last.record.hitPlayer != r.hitPlayer);
            const bool contactChanged = last.at && last.record.contact != r.contact;
            const bool attitudeChanged = last.at && last.record.attitude != r.attitude;
            last.record = r;
            last.at = t;
            SentStates& states = sentStates[Key(guid)];
            states.at[states.next] = r.sent;
            states.parrying[states.next] = parrying;
            states.next = (states.next + 1) % StateHistory;
            if (terminalChanged || revived || ragdollNow || partyChanged || hitChanged || contactChanged || attitudeChanged) {
                essential = true;
            }
        }
        if (posed) {
            if (essential) net::SendNpcs(&r, 1, true);
            net::SendPosedNpc(r, bones.data(), static_cast<int>(bones.size()), poseFlags);
            continue;
        }
        if (essential) { net::SendNpcs(&r, 1, true); continue; }
        if (++n == net::NpcsPerSend) { net::SendNpcs(batch, n); n = 0; }
    }
    if (n) net::SendNpcs(batch, n);
    uint8_t released[net::MaxNpcsPerMessage * 16];
    int count = 0;
    for (auto const& key : previousOwned) {
        if (owned.count(key)) continue;
        const auto note = sendNotes.find(key);
        const auto final = lastSent.find(key);
        if (note != sendNotes.end() && !std::strcmp(note->second.reason, "range") && final != lastSent.end())
            net::SendNpcs(&final->second.record, 1, true);  // final known pose precedes its ordered release
        std::memcpy(released + count++ * 16, key.data(), 16);
        if (count == net::MaxNpcsPerMessage) {
            net::SendNpcRelease(released, count);
            count = 0;
        }
    }
    if (count) net::SendNpcRelease(released, count);
}

// Host, host_sim = elastic: takes back the clients' NPCs inside its sphere (see ReclaimAfter).
void Reclaim(float const players[][3], int playerCount)
{
    const bool on = reclaimRule && world::Extended();
    const uint32_t now = net::Now();
    const auto inside = [&](float const at[3]) { return on && world::HostProcesses(at, ReclaimInside); };
    for (auto it = reclaimed.begin(); it != reclaimed.end();) {
        Entity npc = Resolve(reinterpret_cast<uint8_t const*>(it->data()));
        bool keep = false;
        if (npc.GetInstance() && !npc.IsDead()) {
            const bCVector& p = npc.GetInstance()->GetWorldPosition();
            const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
            keep = inside(at);
        }
        if (keep) { ++it; continue; }
        reclaimPause[*it] = now + ReclaimPause;
        it = reclaimed.erase(it);
    }
    if (!on) { inSphereSince.clear(); return; }
    std::unordered_map<std::string, uint32_t> stillInside;
    std::vector<std::string> take;
    for (auto& entry : mirrors) {
        Mirror const& m = entry.second;
        if (m.owner <= 1 || m.killed || !m.entity.GetInstance() || (m.latest.flags & 3)) continue;
        // A snapshot can be inside the new sphere while this unprocessed copy is still at its old location.
        // Taking authority before MirrorTick drove it there resumes stale local AI and immediately changes hands again.
        if (!m.body.pacified || !m.body.lastAt || now - m.body.lastAt > FullEvery * 2) {
            ++reclaimAwaitingDrive;
            continue;
        }
        const bCVector& current = m.entity.GetInstance()->GetWorldPosition();
        const float here[3] = {current.GetX(), current.GetY(), current.GetZ()};
        if (body::Distance(here, m.latest.position) > ReclaimAlignment) {
            ++reclaimAwaitingAlignment;
            continue;
        }
        // Not in a fight: a slave holds a tool in its hand all day (stageRoiHold 28.10.: Slave_06 stayed the client's), so the
        // motions tell, not the hands.
        if (m.latest.aniState == gEAniState_Parade || m.latest.aniState >= gEAniState_SitKnockDown) continue;
        if (m.body.combatMoveAt && now - m.body.combatMoveAt < ReclaimCalm) continue;
        if (const auto name = motionNames.find(m.latest.motion); name != motionNames.end()) {
            char const* motion = name->second.c_str();
            if (std::strstr(motion, "Attack") || std::strstr(motion, "Parade") || std::strstr(motion, "Stumble") || std::strstr(motion, "KnockDown") ||
                std::strstr(motion, "GetUp"))
                continue;
        }
        if (claimedBy.count(entry.first) || hostTaken.count(entry.first)) continue;
        if (const auto pause = reclaimPause.find(entry.first); pause != reclaimPause.end()) {
            if (static_cast<int32_t>(now - pause->second) < 0) continue;
            reclaimPause.erase(pause);
        }
        float nearest = 1e30f;
        for (int k = 0; k < playerCount; ++k) nearest = std::min(nearest, body::Distance(m.latest.position, players[k]));
        if (nearest > InterestRadius || !inside(m.latest.position)) continue;
        const auto since = inSphereSince.find(entry.first);
        const uint32_t from = since != inSphereSince.end() ? since->second : now;
        stillInside[entry.first] = from;
        if (now - from >= ReclaimAfter) take.push_back(entry.first);
    }
    inSphereSince.swap(stillInside);
    for (auto const& key : take) {
        auto it = mirrors.find(key);
        Mirror& m = it->second;
        const unsigned owner = m.owner;
        body::Release(m.entity, m.body);  // its own AI again, here
        if (m.guarded) m.entity.NPC.DefeatedByPlayer = m.defeatedBefore;
        if (++reclaims <= 50) Logf("npcs: took back %s from player %u (inside the host's range)", m.entity.GetName().GetText(), owner);
        mirrors.erase(it);
        reclaimed.insert(key);
        inSphereSince.erase(key);
    }
}

void HostTick()
{
    const uint32_t now = net::Now();
    if (!sendCadence.Due(now)) return;
    lastSend = now;
    ClaimTick(now);
    if (net::JoinCount() != knownJoins) {  // a new client needs every motion name again
        knownJoins = net::JoinCount();
        namesSent.clear();
        for (auto const& entry : forceHistory) for (auto const& force : entry.second) net::SendRagdoll(force);
        for (auto const& entry : lastSent)
            if (entry.second.record.absent) net::SendNpcs(&entry.second.record, 1, true);
    }
    // Players in this world: the host's hero and every remote player here.
    float players[net::MaxPlayers][3];
    int playerCount = 0;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (hero) {
        const bCVector& p = remote::ControlledBody()->GetWorldPosition();  // a transformed host: where its creature is
        players[playerCount][0] = p.GetX(); players[playerCount][1] = p.GetY(); players[playerCount][2] = p.GetZ();
        ++playerCount;
    }
    net::Remote const* remotes = net::Remotes();
    bool anyClient = false;
    for (int id = 2; id < net::MaxPlayers; ++id) {
        if (!remotes[id].connected) { joinedAt[id] = 0; continue; }
        anyClient = true;
        if (!(remotes[id].latest.flags & 1) || remotes[id].latest.world != remote::CurrentWorld()) continue;
        std::memcpy(players[playerCount++], remotes[id].latest.position, sizeof(float) * 3);
    }
    if (!anyClient || !playerCount) return;
    if (!captureStartedAt) captureStartedAt = now ? now : 1;
    ++captureTicks;
    SendOwned(hero, players, playerCount);
}

// A client simulates what it processes and the host does not send (its own surroundings far from the host);
// it sends those NPCs to the host, which shows them and passes them on.
void OwnTick()
{
    const bool connected = net::Connected();
    if (connected && !wasConnected) namesSent.clear();  // a (new) host needs every motion name
    if (!connected && wasConnected) {
        // The host ends our claims when we drop; holding on to them would ignore its snapshots after a rejoin.
        myClaims.clear();
        givenBack.clear();
    }
    wasConnected = connected;
    if (!connected || !owning) { owned.clear(); return; }
    const uint32_t now = net::Now();
    if (now - lastSend < SendTick()) return;
    lastSend = now;
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    const bCVector& p = remote::ControlledBody()->GetWorldPosition();  // a transformed player: where its creature is
    const float players[1][3] = {{p.GetX(), p.GetY(), p.GetZ()}};
    SendOwned(hero, players, 1);
}

// Host: a mirror of a companion goes back to the host's own simulation.
void DropMirror(std::string const& key)
{
    auto it = mirrors.find(key);
    if (it == mirrors.end()) return;
    if (net::IsHost() && !it->second.killed && it->second.entity.GetInstance()) {
        body::Release(it->second.entity, it->second.body);
        if (it->second.guarded) it->second.entity.NPC.DefeatedByPlayer = it->second.defeatedBefore;
    }
    mirrors.erase(it);
}

void SendRevoke(std::string const& key, uint8_t player)
{
    net::WorldState s{};
    s.kind = 6;  // to `player` (guard): this NPC is not yours (any more)
    std::memcpy(s.guid, key.data(), 16);
    s.guard = player;
    net::SendWorldState(s, 0);
}

// Host: claims whose player left, or that stay silent (the player no longer processes the NPC, or left before its
// first snapshot), go back to the host; a silent but connected player is told so.
void OfferCompanions(uint32_t now)
{
    if (!offering) return;
    for (auto const& [key, name] : companions) {
        if (claimedBy.count(key)) continue;
        auto last = offered.find(key);
        if (last != offered.end() && static_cast<int32_t>(now - last->second) < static_cast<int32_t>(OfferEvery)) continue;
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            net::Remote const& r = net::Remotes()[id];
            if (!r.connected || !(r.latest.flags & 1) || r.latest.world != remote::CurrentWorld() || name != r.latest.name) continue;
            Entity npc = Resolve(reinterpret_cast<uint8_t const*>(key.data()));
            if (!npc.GetInstance()) break;
            // Only near its player: one far away is not processed there and never sent, the claim ran out for want of
            // snapshots and it was handed over again every 20 s (Gorn some 320 times, playtest 27.10.). The host keeps it
            // until its player comes close.
            if (companionNear) {
                const bCVector& p = npc.GetInstance()->GetWorldPosition();
                const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
                if (body::Distance(at, r.latest.position) > InterestRadius) { ++companionsFar; break; }
            }
            net::WorldState s{};
            s.kind = 7;  // to player `guard`: this NPC is your companion (from the save)
            std::memcpy(s.guid, key.data(), 16);
            s.guard = id;
            net::SendWorldState(s, 0);
            claimedBy[key] = id;
            claimSeen[key] = now;
            if (guide::IsQuestGuide(npc.GetInstance()))
                Guide(reinterpret_cast<uint8_t const*>(key.data()), id);
            else {
                const bool party = npc.GetInstance()->GetPropertySet(eEPropertySetType_Party) != nullptr;
                const auto type = party ? static_cast<gEPartyMemberType>(npc.Party.PartyMemberType) : gEPartyMemberType_Slave;
                Follow(reinterpret_cast<uint8_t const*>(key.data()), id, static_cast<uint8_t>(type), party && npc.Party.Waiting);
            }
            offered[key] = now;
            ++companionsOffered;
            Logf("npcs: %s is %s's companion (saved); handed over", Resolve(reinterpret_cast<uint8_t const*>(key.data())).GetName().GetText(), name.c_str());
            break;
        }
    }
}

void ClaimTick(uint32_t now)
{
    OfferCompanions(now);
    for (auto it = claimedBy.begin(); it != claimedBy.end();) {
        const uint8_t player = it->second;
        const bool gone = !net::Remotes()[player].connected || !net::InWorld(player);
        Entity npc = Resolve(reinterpret_cast<uint8_t const*>(it->first.data()));
        if (!gone) {
            eCEntity* actor = remote::PuppetOf(player);
            const auto joined = claimSeen.find(it->first);
            const bool pending = joined != claimSeen.end() && now - joined->second < 2000;
            if (!npc.GetInstance() || !actor || pending) { ++it; continue; }
            const bool follower = npc.GetInstance()->GetPropertySet(eEPropertySetType_Party) &&
                                  npc.Party.GetPartyLeader().GetInstance() == actor;
            const bool guide = actor->GetPropertySet(eEPropertySetType_Party) &&
                               Entity(actor).Party.GetPartyLeader().GetInstance() == npc.GetInstance();
            if (follower || guide) { ++it; continue; }
            Logf("npcs: %s's canonical relationship with player %u ended", npc.GetName().GetText(), player);
            SendRevoke(it->first, player);
            companions.erase(it->first);
            claimSeen.erase(it->first);
            directedBy.erase(it->first);
            it = claimedBy.erase(it);
            continue;
        }
        ++claimsRevoked;
        Logf("npcs: claim of player %u on %s ended (%s)", it->second,
             Resolve(reinterpret_cast<uint8_t const*>(it->first.data())).GetInstance() ? Resolve(reinterpret_cast<uint8_t const*>(it->first.data())).GetName().GetText() : "?",
             "player left this world");
        if (npc.GetInstance() && npc.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
            npc.Party.SetPartyLeader(Entity());
            npc.NPC.SetCurrentTarget(Entity());
            npc.Routine.FullStop();
            npc.Routine.SetTask(bCString("ZS_HangAround_DoNothing"));
        }
        claimSeen.erase(it->first);
        directedBy.erase(it->first);
        it = claimedBy.erase(it);
    }
}

bool xpGuard = true;  // QA negative control: XpGuardOff
bool farXpGuard = true;  // QA negative control: FarXpOff
unsigned farXpRefused = 0;
bool deadPose = true;  // QA negative control: DeadPoseOff
unsigned corpsesLaid = 0, corpseLoops = 0, slotsStopped = 0, ragdollsStarted = 0, ragdollsNotReady = 0;
uint32_t corpseRoiUpdateAt = 0;

// Our own hero's blow on a mirror (LocalHit below): the stagger it causes runs here as the game's own state.
bool nativeHits = true;  // QA negative control (nativehitsoff): our blow's stagger is the guessed short motion again, as before 3 Nov.
constexpr uint32_t NativeHitMax = 2500;  // ms: the longest a stagger state of ours holds the mirror (it ends with the state)
constexpr uint32_t NativeHitTail = 450;  // ms after it: the owner's replay of the same stagger arrives about now and is not shown twice
unsigned localHitsNative = 0, localHitsNativeEnded = 0;

bool StaggerState(char const* name)
{
    static char const* const names[] = {"ZS_Stumble", "ZS_QuickStumble", "ZS_PierceStumble", "ZS_ParadeStumble", "ZS_HeavyParadeStumble"};
    for (char const* known : names)
        if (!std::strcmp(name, known)) return true;
    return false;
}

// The motion-name prefix of a body's skeleton ("Orc" of "Orc_Stand_None_2H_..."), from what it shows or last showed
// on the owner's machine.
void LearnSkeleton(Mirror& m, char const* motion)
{
    if (*m.skeleton || !motion) return;
    const size_t n = std::strcspn(motion, "_");
    if (n && n < sizeof(m.skeleton) && motion[n] == '_') std::memcpy(m.skeleton, motion, n), m.skeleton[n] = '\0';
}

// A dead mirror lies down the way the game lays its dead down (deaddiag 19.10., agent B's finding "dead orcs stand on
// the client"): on the client ZS_Dead, with AIMode 9 set first so that it skips the death script, played no motion at
// all and the body froze in its last pose, mostly standing; the owner's ZS_Dead plays <skeleton>_..._LieDead_Begin and
// then LieDead_Ambient_Loop every ~400 ms, and a ragdoll death falls by physics there. Here: the start motion once,
// then the loop whenever slot 0 shows something else.
// Lying as the QA's deadpose measures it: a lying motion runs on slot 0, nothing on the locomotion slots 1-3.
bool Lies(eCEntity* entity, char* shows, size_t size, char* slots, size_t slotsSize)
{
    body::CurrentMotion(entity, shows, size);
    bool still = true;
    *slots = '\0';
    if (auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)))
        for (int slot = 1; slot <= 3; ++slot) {
            char name[128] = {};
            body::MotionName(animation, slot, name, sizeof(name));
            const bool runs = animation->GetActor() && animation->GetActor()->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot));
            still = still && !runs;
            Format(slots + strlen(slots), slotsSize - strlen(slots), "%s%d:%s:%s", slot > 1 ? "|" : "", slot, runs ? "run" : "off", *name ? name : "-");
        }
    return std::strstr(shows, "_Lie") && body::MotionRunning(entity) && still;  // LieDead, or a knock-out's LieKnockDown
}

// Once per corpse, 45 s after its death here: does it lie? A play evening's log counts standing corpses from these lines
// (owner, 22.10.: far more stood than the two examples).
void LateCheck(Mirror& m, uint32_t now, char const* kind)
{
    if (m.lateNoted || !m.killedAt || static_cast<int32_t>(now - m.killedAt) < 45000) return;
    m.lateNoted = true;
    char shows[128], slots[400];
    const bool lies = Lies(m.entity.GetInstance(), shows, sizeof(shows), slots, sizeof(slots));
    auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    const bool localPhysics = animation && animation->GetRagDollEnabled();
    Logf("npcs: corpse %s (%s) after 45 s: %s (shows %s, slots %s)", m.entity.GetName().GetText(), kind,
         localPhysics ? "local ragdoll active" : lies ? "lies" : "STANDS", shows, slots);
}

// Ragdoll deaths. The game's death script for them (ZS_RagDollDead in Script_Game.dll) pushes the body with
// PSAnimation::AddForceAtPos: from where its last inflictor stood, harder for a power attack. A mirror never ran that script (it must
// not: it runs the death's bookkeeping), so it fell where it stood. The owner now tells the others what it applied (the script DLL's
// import of that call is hooked) and each mirror applies the same to its own ragdoll.
struct Vec3 { float x, y, z; };
using AddForceFn = void(__fastcall*)(void* self, void* edx, Vec3 at, Vec3 force);
AddForceFn originalAddForce = nullptr;
unsigned ragdollForcesSent = 0, ragdollForcesReceived = 0, ragdollForcesApplied = 0, ragdollForcesLate = 0;

void __fastcall HookAddForce(void* self, void* edx, Vec3 at, Vec3 force)
{
    auto* set = self ? *static_cast<eCEntityPropertySet**>(self) : nullptr;  // PSAnimation starts with its engine property set
    eCEntity* entity = set ? set->GetEntity() : nullptr;
    if (npcplayback::IsPassive(entity)) return;
    originalAddForce(self, edx, at, force);
    if (!net::IsHost()) return;
    if (!entity || !net::Connected() || !entity->GetPropertySet(eEPropertySetType_NPC)) return;
    if (entity == gCSession::GetInstance().GetHero() || remote::IsPuppet(entity)) return;
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    if (!animation || !animation->GetRagDollEnabled()) return;
    uint8_t guid[16];
    Guid(entity, guid);
    if (mirrors.count(Key(guid))) return;  // mirrored here: its owner's script decides
    net::RagdollNote note;
    std::memcpy(note.guid, guid, 16);
    if (++forceSequence == 0) ++forceSequence;
    note.sequence = forceSequence;
    note.incarnation = Incarnation(guid);
    note.sent = net::Now();
    const bCVector& position = entity->GetWorldPosition();
    note.at[0] = at.x - position.GetX(), note.at[1] = at.y - position.GetY(), note.at[2] = at.z - position.GetZ();
    note.force[0] = force.x, note.force[1] = force.y, note.force[2] = force.z;
    auto& history = forceHistory[Key(guid)];
    if (history.size() == 64) {
        history.erase(history.begin());
        Log("npcs: pruned the oldest historical ragdoll impulse; the live impulse is still published");
    }
    history.push_back(note);
    net::SendRagdoll(note);
    if (++ragdollForcesSent <= 20)
        Logf("npcs: ragdoll force of %s sent: at %.0f/%.0f/%.0f (relative), force %.0f/%.0f/%.0f", Entity(entity).GetName().GetText(), note.at[0],
             note.at[1], note.at[2], force.x, force.y, force.z);
}

void InstallRagdollHook(uint32_t now)
{
    static bool done = false;
    static uint32_t lastTry = 0;
    if (done || now - lastTry < 1000) return;
    lastTry = now;
    HMODULE game = GetModuleHandleA("Script_Game.dll");
    if (!game) return;
    done = true;
    originalAddForce = reinterpret_cast<AddForceFn>(
        ai::PatchImport(game, "Script.dll", "?AddForceAtPos@PSAnimation@@QAEXVbCVector@@0@Z", reinterpret_cast<void*>(&HookAddForce)));
    Log(originalAddForce ? "npcs: ragdoll forces hooked" : "npcs: ragdoll forces NOT hooked (the script DLL does not import AddForceAtPos)");
}

bool TryApplyForce(Mirror& m, uint32_t now)
{
    const auto pending = pendingForces.find(Key(m.latest.guid));
    if (pending == pendingForces.end()) return false;
    auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    if (!animation || !animation->GetRagDollEnabled()) return false;
    const bCVector& position = m.entity.GetInstance()->GetWorldPosition();
    bool applied = false;
    for (auto it = pending->second.begin(); it != pending->second.end();) {
        if (it->incarnation < m.latest.incarnation) { it = pending->second.erase(it); ++ragdollForcesLate; continue; }
        if (it->incarnation != m.latest.incarnation) { ++it; continue; }
        if (static_cast<int32_t>(m.shownSent - it->sent) < 0) { ++it; continue; }
        animation->AddForceAtPos(bCVector(position.GetX() + it->at[0], position.GetY() + it->at[1], position.GetZ() + it->at[2]),
                                 bCVector(it->force[0], it->force[1], it->force[2]));
        it = pending->second.erase(it);
        ++ragdollForcesApplied;
        applied = true;
    }
    if (pending->second.empty()) pendingForces.erase(pending);
    return applied;
}

bool HasPendingCorpseForce(Mirror const& m)
{
    const auto pending = pendingForces.find(Key(m.latest.guid));
    if (pending == pendingForces.end()) return false;
    for (auto const& force : pending->second)
        if (force.incarnation == m.latest.incarnation && static_cast<int32_t>(m.shownSent - force.sent) >= 0) return true;
    return false;
}

void CaptureCorpsePose(Mirror& m, uint32_t now)
{
    if (!m.corpsePose.empty() && now - m.corpsePoseAt < 100) return;
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance()) return;
    std::vector<pose::Bone> bones;
    uint8_t flags = 0;
    if (!npcplayback::CapturePose(entity, bones, flags, nullptr)) return;
    m.corpsePose = std::move(bones);
    m.corpsePoseFlags = flags;
    m.corpseRoot = entity->GetWorldMatrix();
    m.corpseActor = actor->GetInstance();
    m.corpsePoseAt = now;
}

void RestoreCorpseActor(Mirror& m)
{
    if (!m.ragdollInitialized || m.corpsePose.empty()) return;
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance() || actor->GetInstance() == m.corpseActor) return;
    animation->SetRagDollEnabled(GEFalse, 0.0f, GETrue);
    entity->SetToWorldMatrix(m.corpseRoot);
    if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
        rigid->SetPose(m.corpseRoot);
    m.corpseActor = actor->GetInstance();
    m.corpseSimulating = false;
}

void UpdateCorpsePresentation(Mirror& m, uint32_t now)
{
    auto* entity = m.entity.GetInstance();
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!animation || !actor) return;
    if (animation->GetRagDollEnabled()) {
        npcplayback::UpdatePresentation(entity);
        const bool active = animation->GetRagDollEnabled() != GEFalse;
        if (active) CaptureCorpsePose(m, now);
        m.corpseSimulating = active;
    } else if (m.ragdollInitialized) {
        m.corpseSimulating = false;
        if (!m.corpsePose.empty() &&
            npcplayback::ReplayFrozenPose(entity, m.replay, m.corpsePose, m.corpsePoseFlags)) return;
        if (!(m.lieNoted & 16)) {
            m.lieNoted |= 16;
            Logf("npcs: settled corpse %s awaits its cached render pose; death is not restarted", entity->GetName().GetText());
        }
    } else {
        npcplayback::UpdatePresentation(entity);
    }
}

void OnRagdollNote(net::RagdollNote const& note)
{
    const auto absent = absences.find(Key(note.guid));
    if (absent != absences.end() && static_cast<int32_t>(note.incarnation - absent->second.incarnation) <= 0) {
        ++ragdollForcesLate;
        return;
    }
    const auto current = mirrors.find(Key(note.guid));
    if (current != mirrors.end() && static_cast<int32_t>(note.incarnation - current->second.latest.incarnation) < 0) {
        ++ragdollForcesLate;
        return;
    }
    std::string identity = Key(note.guid);
    identity.append(reinterpret_cast<char const*>(&note.incarnation), sizeof(note.incarnation));
    auto& queue = pendingForces[Key(note.guid)];
    if (queue.size() >= 64) { Log("npcs: ragdoll force queue full; publication refused"); return; }
    if (!forceSequences[identity].Accept(note.sequence)) return;
    queue.push_back(note);
    ++ragdollForcesReceived;
    const auto it = mirrors.find(Key(note.guid));
    if (it == mirrors.end()) return;
    Mirror& m = it->second;
    m.lieCheckAt = 0;
    if (!m.entity.GetInstance()) return;
    TryApplyForce(m, net::Now());
}

void OnHitResult(net::HitResult const& result)
{
    const auto found = mirrors.find(Key(result.guid));
    if (found == mirrors.end()) return;
    Mirror& m = found->second;
    if (result.incarnation != m.latest.incarnation || result.sequence != m.replay.pendingHit) return;
    if (!result.accepted) {
        m.replay.pendingHit = 0;
        m.replay.localMotion[0] = '\0';
        m.replay.initialized = false;
        Logf("npcs: host refused hit %u on %s; authoritative state retained", result.sequence, m.entity.GetName().GetText());
    } else {
        m.replay.acknowledgedHit = result.sequence;
        m.replay.acknowledgedAt = result.processedAt;
    }
}

void LayDown(Mirror& m, uint32_t now)
{
    RestoreCorpseActor(m);
    if (!deadPose || static_cast<int32_t>(now - m.lieCheckAt) < 0) return;
    // A ragdoll death on its owner (flag 128, or its movement mode): not an animation here. The mode alone came too late for most
    // orcs (their death state was not set yet in the death snapshot), and they went the animation way and stood.
    if (m.ragdollStarted || (m.latest.flags & 128) || m.latest.movementMode == gECharMovementMode_RagDollDead) {
        auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
        if (m.ragdollInitialized) {
            if (animation && animation->GetRagDollEnabled()) TryApplyForce(m, now);
            else if (animation && animation->GetActor() && HasPendingCorpseForce(m) &&
                     static_cast<int32_t>(now - m.ragdollTryAt) >= 0 && !m.corpsePose.empty() &&
                     npcplayback::ReplayFrozenPose(m.entity.GetInstance(), m.replay, m.corpsePose, m.corpsePoseFlags)) {
                animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);
                m.ragdollTryAt = now + 1000;
                if (animation->GetRagDollEnabled()) {
                    ++m.ragdollWakes;
                    TryApplyForce(m, now);
                }
            }
            m.lieCheckAt = now + (HasPendingCorpseForce(m) ? 100 : 5000);
            return;
        }
        if (!m.ragdollStarted) {
            if (!npcplayback::IsPassive(m.entity.GetInstance())) body::Pacify(m.entity, m.body);
            auto* routine = static_cast<gCScriptRoutine_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine));
            routine->AccessAIMode().AccessValue() = gEAIMode_Dead;
            m.entity.CharacterMovement.StopMovement();
            m.entity.CharacterMovement.SetMovementMode(gECharMovementMode_RagDollDead);
            m.ragdollStarted = true;
            // SetRagDollEnabled silently returns while Engine's scene node is inactive. Rebuild the processing range,
            // then retry on a later frame instead of treating an out-of-range body as a permanent ragdoll failure.
            if (!corpseRoiUpdateAt || now - corpseRoiUpdateAt >= 1000) {
                world::RequestProcessingUpdate();
                corpseRoiUpdateAt = now ? now : 1;
            }
            m.lieCheckAt = now + 250;
            return;
        }
        if (animation && animation->GetRagDollEnabled()) {
            m.ragdollInitialized = true;
            TryApplyForce(m, now);
            m.lieCheckAt = now + (HasPendingCorpseForce(m) ? 100 : 5000);
            return;
        }
        if (animation && animation->GetActor() && static_cast<int32_t>(now - m.ragdollTryAt) >= 0) {
            using Owner = eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner;
            Owner owners[net::NpcMotionSlots];
            for (int slot = 0; slot < net::NpcMotionSlots; ++slot)
                owners[slot] = animation->GetMotionOwner(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot));
            // CP 1.75's ragdoll setup (Game+0xdc810): stop animation blending, then enable immediately.
            // Setting the movement mode alone can be skipped for an already-dead/unprocessed mirror.
            for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
                const auto type = static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot);
                animation->GetActor()->StopMotion(type, 0.0f);
                animation->GetActor()->SetTargetWeight(type, 0.0f);
                animation->SetMotionOwner(type, static_cast<Owner>(2));
            }
            animation->SetRagDollEnabled(GETrue, 0.0f, GETrue);
            const bool on = animation->GetRagDollEnabled() != GEFalse;
            if (on) {
                m.ragdollInitialized = true;
                ++m.ragdollStarts;
                ++ragdollsStarted;
                Logf("npcs: corpse %s ragdoll physics enabled", m.entity.GetName().GetText());
                TryApplyForce(m, now);
                m.lieCheckAt = now + (HasPendingCorpseForce(m) ? 100 : 5000);
                return;
            }
            // Not ready (the body is not processed here yet): the motions are stopped, and a body without one stands in its bind
            // pose (2 Nov. play session: two corpses stood after 45 s). Give the slots back and lie by animation below until the
            // physics can be enabled.
            for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
                const auto type = static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot);
                animation->SetMotionOwner(type, owners[slot]);
                animation->GetActor()->SetTargetWeight(type, 1.0f);
            }
            ++m.ragdollFails;
            ++ragdollsNotReady;
            m.ragdollTryAt = now + (m.ragdollFails < 4 ? 1500 : 5000);
            if ((m.ragdollFails <= 3 || m.ragdollFails % 10 == 0) &&
                (!corpseRoiUpdateAt || now - corpseRoiUpdateAt >= 1000)) {
                world::RequestProcessingUpdate();
                corpseRoiUpdateAt = now ? now : 1;
            }
            if (m.ragdollFails <= 2 || m.ragdollFails == 10)
                Logf("npcs: corpse %s ragdoll physics not ready (try %u); lying by animation until it is", m.entity.GetName().GetText(), m.ragdollFails);
        }
    }
    if (m.lieTries >= 40) {
        if (!(m.lieNoted & 4)) {
            m.lieNoted |= 4;
            char shows[128];
            body::CurrentMotion(m.entity.GetInstance(), shows, sizeof(shows));
            Logf("npcs: corpse %s: lying motion not running after 40 tries; retrying slowly (shows %s)", m.entity.GetName().GetText(), shows);
        }
    }
    m.lieCheckAt = now + (m.lieTries >= 40 ? 5000 : 400);
    char shows[128];
    body::CurrentMotion(m.entity.GetInstance(), shows, sizeof(shows));
    LearnSkeleton(m, shows);
    // Creature deaths can use Fwd variants, not the humanoid Back names below. Prefer an exact observed resource name.
    char const* knownLoop = std::strstr(shows, "_LieDead_") && std::strstr(shows, "_Ambient_Loop_") ? shows : nullptr;
    auto ownerMotion = motionNames.find(m.latest.motion);
    if (ownerMotion != motionNames.end() && std::strstr(ownerMotion->second.c_str(), "_LieDead_") &&
        std::strstr(ownerMotion->second.c_str(), "_Ambient_Loop_"))
        knownLoop = ownerMotion->second.c_str();
    if (!*m.skeleton) {
        auto name = motionNames.find(m.latest.motion);
        if (name != motionNames.end()) LearnSkeleton(m, name->second.c_str());
    }
    if (!*m.skeleton) {
        if (!(m.lieNoted & 1)) {
            m.lieNoted |= 1;
            Logf("npcs: corpse %s: no skeleton known from its motions (shows %s); it cannot be laid down yet", m.entity.GetName().GetText(), shows);
        }
        return;
    }
    // Nothing of its life may go on underneath, at every check: the movement controller (a mirror walking when it died)
    // and the locomotion slots 1-3 kept playing standing and walking motions, and 3 of 12 corpses stood with the lying
    // loop running on slot 0 (deaddiag 19.10.); stopped once, 1 of 12 still stood (it had been in its fighting stance).
    m.entity.CharacterMovement.StopMovement();
    if (auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation)))
        for (int slot = 1; slot <= 3; ++slot)
            if (animation->GetActor() && animation->GetActor()->IsMotionRunning(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot)))
                animation->StopMotion(static_cast<eCWrapper_emfx2Actor::eEMotionType>(slot), 0.2f), ++slotsStopped;
    // Lying already: the start motion or the loop still plays (once a motion ends the body falls back to its bind pose).
    if (std::strstr(shows, "LieDead") && body::MotionRunning(m.entity.GetInstance())) {
        m.lieTries = 0;  // the cap counts only starts in a row that did not run
        if (!(m.lieNoted & 2)) {
            m.lieNoted |= 2;
            Logf("npcs: corpse %s lies (%s)", m.entity.GetName().GetText(), shows);
        }
        return;
    }
    if (knownLoop) {
        if (!m.laid) {
            m.laid = true;
            ++corpsesLaid;
        }
        m.entity.Routine.AniState = gEAniState_LieDead;
        ++corpseLoops;
        ++m.lieTries;
        if (!(m.lieNoted & 8)) {
            m.lieNoted |= 8;
            Logf("npcs: corpse %s uses observed death loop %s", m.entity.GetName().GetText(), knownLoop);
        }
        if (!body::PlayAniInstruction(m.entity.GetInstance(), m.body, knownLoop))
            m.entity.StartPlayAni(bCString(knownLoop), 0, GEFalse, 1.0f, GEFalse);
        return;
    }
    char motion[128];
    if (!m.laid) {
        Format(motion, "%s_Stand_None_None_P0_LieDead_Begin_N_Back_00_%%_00_P0_200", m.skeleton);
        m.laid = true;
        m.lieCheckAt = now + 2000;  // the start motion takes ~2 s (owner's trace)
        ++corpsesLaid;
    } else {
        // The loop belongs to the lying-dead stance: it plays only once the body has that stance (body.cpp: motions of
        // another stance do not play), and like the game's own ZS_Dead_Loop as a play-ani instruction (deaddiag: started
        // with StartPlayAni it showed on slot 0 but did not run, and 2 of 4 corpses stood).
        Format(motion, "%s_LieDead_None_None_P0_Ambient_Loop_N_Back_00_%%_00_P0_0", m.skeleton);
        m.entity.Routine.AniState = gEAniState_LieDead;
        ++corpseLoops;
        ++m.lieTries;
        if (!body::PlayAniInstruction(m.entity.GetInstance(), m.body, motion)) m.entity.StartPlayAni(bCString(motion), 0, GEFalse, 1.0f, GEFalse);
        return;
    }
    ++m.lieTries;
    m.entity.StartPlayAni(bCString(motion), 0, GEFalse, 1.0f, GEFalse);
}

unsigned displacements = 0;

float DisplaceWeight(Mirror& m, uint32_t now)
{
    if (!m.displaceAt) return 0.0f;
    const int32_t age = static_cast<int32_t>(now - m.displaceAt);
    if (age <= static_cast<int32_t>(DisplaceHold)) return 1.0f;
    const float weight = std::exp(-static_cast<float>(age - static_cast<int32_t>(DisplaceHold)) / DisplaceFade);
    if (weight < 0.03f) { m.displaceAt = 0; return 0.0f; }
    return weight;
}

void AddDisplacement(Mirror& m, float at[3], uint32_t now)
{
    const float weight = DisplaceWeight(m, now);
    if (weight > 0.0f)
        for (int k = 0; k < 3; ++k) at[k] += m.displace[k] * weight;
}

// `at` is the owner's buffered pose. When our blow's stagger has ended, the body stands where our blow left it: keep that as an
// offset from the owner's pose (not a jump back to it), and let it fade.
void ApplyDisplacement(Mirror& m, float at[3], uint32_t now)
{
    if (m.displacePending && (!m.body.localHitUntil || static_cast<int32_t>(m.body.localHitUntil - now) <= 0)) {
        m.displacePending = false;
        const bCVector& here = m.entity.GetInstance()->GetWorldPosition();
        const float dx = here.GetX() - at[0], dz = here.GetZ() - at[2];
        const float length = std::sqrt(dx * dx + dz * dz);
        if (length > 30.0f) {
            const float scale = std::min(1.0f, MaxDisplacement / length);
            m.displace[0] = dx * scale, m.displace[1] = 0.0f, m.displace[2] = dz * scale;
            m.displaceAt = now ? now : 1;
            if (++displacements <= 20)
                Logf("npcs: %s shoved %.0f cm from its owner's pose by our blows; blended back", m.entity.GetName().GetText(), length * scale);
        }
    }
    AddDisplacement(m, at, now);
}

// A mirrored NPC's blow on our own hero, judged here: when its visible swing is at the part where the weapon lands, the hero is in
// reach and in front of it. The native contact of a mirror's weapon never reaches the hero (foreign_judged stayed 0 in 92 swings),
// and the host does not decide damage to this machine's hero (defender-local), so the swing we see is the one that counts.
unsigned localBlowsJudged = 0, localBlowsOutOfReach = 0;
unsigned localContactWindows = 0, localContactMissing = 0, localContactTests = 0, localContactOverlaps = 0, localContactWalls = 0;
unsigned frameCount = 0, slowFrames = 0, hitchFrames = 0;
uint32_t frameSumMs = 0, maxFrameMs = 0, lastFrameAt = 0;
bool AttachedTo(eCEntity* entity, eCEntity* owner)
{
    if (!entity || !owner) return false;
    eCEntity* right = nullptr;
    eCEntity* left = nullptr;
    if (owner->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity actor(owner);
        right = actor.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance();
        left = actor.Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance();
    }
    eCEntity* pending[16] = {entity};
    unsigned count = 1;
    for (unsigned i = 0; i < count; ++i) {
        eCEntity* current = pending[i];
        if (current == owner || current == right || current == left) return true;
        auto add = [&](eCEntity* next) {
            if (next && count < 16 && std::find(pending, pending + count, next) == pending + count) pending[count++] = next;
        };
        add(current->GetParent());
        if (current->GetPropertySet(eEPropertySetType_Interaction)) add(Entity(current).Interaction.GetOwner().GetInstance());
    }
    return false;
}

bool ClearContact(eCEntity* attacker, eCEntity* hero)
{
    bCVector start = attacker->GetWorldPosition(), end = hero->GetWorldPosition();
    auto* a = static_cast<gCCharacterMovement_PS*>(attacker->GetPropertySet(eEPropertySetType_CharacterMovement));
    auto* h = static_cast<gCCharacterMovement_PS*>(hero->GetPropertySet(eEPropertySetType_CharacterMovement));
    if (!a || !h) return false;
    start.AccessY() += a->GetCharacterHeight() * 0.5f;
    end.AccessY() += h->GetCharacterHeight() * 0.5f;
    auto& physics = eCPhysicsScene::GetInstance();
    const GEU32 hints = eETraceRayHints_TestStaticShapes | eETraceRayHints_TestDynamicShapes | eETraceRayHints_Distance |
                        eETraceRayHints_IncludeCollisionGroup17;
    if (!physics.TraceRayAllHits(start, end - start, attacker, hints)) return true;
    auto const& report = physics.GetQueryReport();
    const float length = (end - start).GetMagnitude();
    for (int i = 0; i < report.m_arrHitEntity.GetCount(); ++i) {
        eCEntity* hit = report.m_arrHitEntity[i];
        if (i < report.m_arrDistance.GetCount() && report.m_arrDistance[i] > length + 1.0f) continue;
        if (!hit || (!AttachedTo(hit, attacker) && !AttachedTo(hit, hero))) {
            static unsigned noted = 0;
            if (noted++ < 12)
                Logf("npcs: displayed contact %s -> %s obstructed by %s (ray %.0f cm)",
                     attacker->GetName().GetText(), hero->GetName().GetText(), hit ? hit->GetName().GetText() : "(static geometry)", length);
            return false;
        }
    }
    return true;
}

void Point(bCVector const& vector, float out[3])
{
    out[0] = vector.GetX(); out[1] = vector.GetY(); out[2] = vector.GetZ();
}

bool PresentedContactPoints(Entity& entity, uint32_t heldRight, float a[3], float b[3], float& thickness, int& bone)
{
    eCEntity* weapon = nullptr;
    if (heldRight) {
        Entity held = entity.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (!held.GetInstance() || HeldHash(entity, gESlot_RightHand) != heldRight) return false;
        // Fist is an invisible damage proxy, not the animal's mouth or a rendered blade.
        if (held.GetInstance()->GetPropertySet(eEPropertySetType_Interaction) && held.Interaction.GetUseType() != gEUseType_Fist)
            weapon = held.GetInstance();
    }
    if (weapon) {
        bCBox const& box = weapon->GetLocalNodeBoundary();
        float low[3], high[3];
        Point(box.GetMin(), low); Point(box.GetMax(), high);
        int axis = 0;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(low[i]) || !std::isfinite(high[i]) || high[i] < low[i]) return false;
            if (high[i] - low[i] > high[axis] - low[axis]) axis = i;
        }
        if (high[axis] - low[axis] < 1.0f) return false;
        float first[3], last[3];
        thickness = 0.0f;
        for (int i = 0; i < 3; ++i) {
            first[i] = last[i] = (low[i] + high[i]) * 0.5f;
            if (i != axis) thickness = std::max(thickness, (high[i] - low[i]) * 0.5f);
        }
        first[axis] = low[axis]; last[axis] = high[axis];
        Point(bCVector(first[0], first[1], first[2]).GetTransformed(weapon->GetWorldMatrix()), a);
        Point(bCVector(last[0], last[1], last[2]).GetTransformed(weapon->GetWorldMatrix()), b);
        bone = -2;
        return true;
    }
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor || !actor->GetInstance()) return false;
    bCMatrix right, left;
    if (actor->GetNodeByName(bCString("Bip01 R Hand")) && actor->GetNodeByName(bCString("Bip01 L Hand")) &&
        animation->GetBoneMatrix(bCString("Bip01 R Hand"), right) &&
        animation->GetBoneMatrix(bCString("Bip01 L Hand"), left)) {
        Point(right.GetTranslation(), a);
        Point(left.GetTranslation(), b);
        thickness = 8.0f;
        bone = -3;
        return true;
    }
    const bCMatrix& world = entity.GetInstance()->GetWorldMatrix();
    const bCVector forward = bCVector(0, 0, 1).GetTransformed(world) - world.GetTranslation();
    const bCVector& origin = entity.GetInstance()->GetWorldPosition();
    float farthest = -1e20f;
    bone = -1;
    const int bones = actor->GetInstance()->mNodes.mLength;
    if (bones <= 0 || bones > 4096) return false;
    for (int i = 0; i < std::min(bones, 128); ++i) {
        bCMatrix at;
        if (!animation->GetBoneMatrix(i, at)) break;
        const bCVector delta = at.GetTranslation() - origin;
        const float projection = delta.GetX() * forward.GetX() + delta.GetY() * forward.GetY() + delta.GetZ() * forward.GetZ();
        if (projection > farthest) { farthest = projection; bone = i; Point(at.GetTranslation(), a); }
    }
    if (bone < 0) return false;
    std::memcpy(b, a, sizeof(float) * 3);
    thickness = 10.0f;
    return true;
}

bool SweepsBody(eCEntity* target, float const oldA[3], float const oldB[3], float const a[3], float const b[3],
                float thickness, bool weapon)
{
    bCBox const& box = target->GetWorldNodeBoundary();
    const bCVector& low = box.GetMin();
    const bCVector& high = box.GetMax();
    const float radius = std::min(high.GetX() - low.GetX(), high.GetZ() - low.GetZ()) * 0.5f;
    if (!(radius > 0.0f) || !std::isfinite(radius)) return false;
    float first[3] = {(low.GetX() + high.GetX()) * 0.5f, low.GetY() + radius, (low.GetZ() + high.GetZ()) * 0.5f};
    float last[3] = {first[0], std::max(first[1], high.GetY() - radius), first[2]};
    const float limit = (radius + thickness) * (radius + thickness);
    return contact::SegmentDistanceSquared(oldA, a, first, last) <= limit ||
           contact::SegmentDistanceSquared(oldB, b, first, last) <= limit ||
           (weapon && contact::SegmentDistanceSquared(a, b, first, last) <= limit);
}

struct PlayerContact {
    uint32_t instance = 0, at = 0;
    float a[3] = {}, b[3] = {}, previousA[3] = {}, previousB[3] = {}, thickness = 0;
    int bone = -1;
    bool ready = false;
};
PlayerContact playerContact;
unsigned playerSwingFrames = 0, playerSweepTests = 0, playerOverlaps = 0, playerObstructed = 0, playerContactHits = 0;

void SamplePlayerContact(uint32_t now)
{
    eCEntity* hero = remote::ControlledBody();
    if (!hero || !net::Connected() || !body::LocallyAttacking(hero)) { playerContact = PlayerContact{}; return; }
    ++playerSwingFrames;
    const uint32_t instance = npcplayback::MotionInstance(hero);
    Entity player(hero);
    float a[3], b[3], thickness = 0;
    int bone = -1;
    if (!instance || !PresentedContactPoints(player, HeldHash(player, gESlot_RightHand), a, b, thickness, bone)) {
        playerContact = PlayerContact{};
        return;
    }
    if (instance != playerContact.instance) {
        const auto& origin = hero->GetWorldPosition();
        Logf("npcs: player sweep %u root %.0f/%.0f/%.0f blade %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f thickness %.1f",
             instance, origin.GetX(), origin.GetY(), origin.GetZ(), a[0], a[1], a[2], b[0], b[1], b[2], thickness);
        auto* animation = static_cast<eCVisualAnimation_PS*>(hero->GetPropertySet(eEPropertySetType_Animation));
        if (animation && animation->GetActor() && animation->GetActor()->GetNodeByName(bCString("Bip01 R Hand"))) {
            bCMatrix hand;
            if (animation->GetBoneMatrix(bCString("Bip01 R Hand"), hand)) {
                const auto& at = hand.GetTranslation();
                Logf("npcs: player sweep %u rendered hand %.0f/%.0f/%.0f", instance, at.GetX(), at.GetY(), at.GetZ());
            }
        }
        if (auto* target = player.NPC.GetCurrentTarget().GetInstance()) {
            const auto& box = target->GetWorldNodeBoundary();
            Logf("npcs: player sweep %u target %s bounds %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f", instance,
                 target->GetName().GetText(), box.GetMin().GetX(), box.GetMin().GetY(), box.GetMin().GetZ(),
                 box.GetMax().GetX(), box.GetMax().GetY(), box.GetMax().GetZ());
        }
    }
    const bool previous = playerContact.at && now - playerContact.at <= 100 &&
                          playerContact.instance == instance && playerContact.bone == bone;
    std::memcpy(playerContact.previousA, playerContact.a, sizeof(a));
    std::memcpy(playerContact.previousB, playerContact.b, sizeof(b));
    std::memcpy(playerContact.a, a, sizeof(a));
    std::memcpy(playerContact.b, b, sizeof(b));
    playerContact.instance = instance;
    playerContact.at = now;
    playerContact.thickness = thickness;
    playerContact.bone = bone;
    playerContact.ready = previous;
}

void TryPlayerBlow(Mirror& m)
{
    eCEntity* hero = remote::ControlledBody();
    if (!playerContact.ready || !hero || !m.replayReady || m.killed) return;
    ++playerSweepTests;
    if (!SweepsBody(m.entity.GetInstance(), playerContact.previousA, playerContact.previousB,
                    playerContact.a, playerContact.b, playerContact.thickness, playerContact.bone == -2)) return;
    ++playerOverlaps;
    if (!ClearContact(hero, m.entity.GetInstance())) { ++playerObstructed; return; }
    if (combat::PlayerBlow(m.entity.GetInstance())) ++playerContactHits;
}

void TryLocalBlow(Mirror& m, net::NpcRecord const& shown, uint32_t now)
{
    eCEntity* hero = m.body.contactTarget;
    if (!hero || !m.replayReady || !shown.contact || (shown.flags & 3) || *m.replay.localMotion ||
        now - m.latestTicks > 250 || !shown.motions[0].active) { m.contactAt = 0; return; }
    if (combat::IgnoreFriendlyHit(hero, m.entity.GetInstance())) return;
    ++localContactWindows;
    float a[3], b[3], thickness = 0;
    int bone = -1;
    if (!PresentedContactPoints(m.entity, shown.heldRight, a, b, thickness, bone)) {
        ++localContactMissing;
        m.contactAt = 0;
        return;
    }
    const uint32_t instance = shown.motions[0].instance;
    if (m.contactInstance != instance) {
        m.contactInstance = instance; m.contactConsumed = false; m.contactAt = 0;
        static unsigned contactsLogged = 0;
        if (contactsLogged++ < 16) {
            const auto& at = hero->GetWorldPosition();
            Logf("npcs: visible contact %s instance=%u bone=%d at %.0f/%.0f/%.0f -> %.0f/%.0f/%.0f hero %.0f/%.0f/%.0f",
                 m.entity.GetName().GetText(), instance, bone, a[0], a[1], a[2], b[0], b[1], b[2],
                 at.GetX(), at.GetY(), at.GetZ());
        }
    }
    const bool previous = m.contactAt && now - m.contactAt <= 100 && m.contactBone == bone;
    float oldA[3], oldB[3];
    std::memcpy(oldA, m.contactA, sizeof(oldA)); std::memcpy(oldB, m.contactB, sizeof(oldB));
    std::memcpy(m.contactA, a, sizeof(a)); std::memcpy(m.contactB, b, sizeof(b));
    m.contactAt = now; m.contactBone = bone;
    if (!previous || m.contactConsumed) return;
    ++localContactTests;
    const bool touches = SweepsBody(hero, oldA, oldB, a, b, thickness, bone == -2);
    if (!touches) { ++localBlowsOutOfReach; return; }
    ++localContactOverlaps;
    if (!ClearContact(m.entity.GetInstance(), hero)) { ++localContactWalls; ++localBlowsOutOfReach; return; }
    m.contactConsumed = true;
    ++localBlowsJudged;
    combat::LocalBlow(m.entity.GetInstance());
}

void MirrorTick()
{
    static char const* const DeathTask = "ZS_Dead";
    const uint32_t now = net::Now();
    if (lastFrameAt) {
        const uint32_t gap = now - lastFrameAt;
        ++frameCount, frameSumMs += gap, slowFrames += gap > 33, hitchFrames += gap > 66;
        maxFrameMs = std::max(maxFrameMs, gap);
    }
    lastFrameAt = now;
    // The host also mirrors NPCs a client simulates; it drives only those it processes itself (the others are
    // frozen here anyway, and get their state when the host arrives).
    if (net::IsHost()) ClaimTick(now);
    std::unordered_set<eCEntity*> processed;
    if (net::IsHost()) {
        const bTObjArray<Entity> all = Entity::GetNPCs();
        for (int i = 0; i < all.GetCount(); ++i) processed.insert(all.GetAt(i).GetInstance());
    }
    for (auto it = mirrors.begin(); it != mirrors.end();) {
        Mirror& m = it->second;
        if (!m.resolved) {
            const bool again = m.resolvedAt != 0;
            m.entity = Resolve(m.latest.guid);
            m.resolved = true;
            m.resolvedAt = now ? now : 1;
            m.missing = !IsNpc(m.entity.GetInstance());  // the host's hero, a chest or a puppet is no mirror (Fable 1.6)
            if (again && !m.missing) {
                ++mirrorsFoundLater;
                Logf("npcs: mirror %s found here, %u ms after its first snapshot", m.entity.GetName().GetText(), now - m.firstSeen);
            }
            // The game's death and knock-out scripts reward the player (GiveXP) for any NPC not yet marked as
            // defeated by the player, and then mark it. For mirrors that rewarded this client for every NPC the
            // host or anyone else defeated (1250 XP in stage 3c without a single own hit; 500 when marked only
            // once the host reported it down or dead, since hit points arrive first). The host decides who
            // earned it (net::SendKilled), so a mirror is marked for as long as it is mirrored.
            if (!m.missing && xpGuard) {
                m.defeatedBefore = m.entity.NPC.DefeatedByPlayer;
                m.entity.NPC.DefeatedByPlayer = GETrue;
                m.guarded = true;
            }
        }
        // Not here (yet), or gone: looked up again every second while its owner sends it, and dropped once stale (Codex
        // review 22.10.: an NPC missing at its first lookup stayed missing, and its entry stayed for good; only summons were
        // bound again, through Rebind).
        if (m.missing || !m.entity.GetInstance()) {
            if (retryMissing && static_cast<int32_t>(now - m.latestTicks) > static_cast<int32_t>(StaleAfter)) {
                ++mirrorsExpired;
                it = mirrors.erase(it);
                continue;
            }
            if (retryMissing && static_cast<int32_t>(now - m.resolvedAt) >= static_cast<int32_t>(RetryLookup)) {
                m.resolved = false;
                ++mirrorLookups;
            }
            ++it;
            continue;
        }
        if (net::IsHost() && !processed.count(m.entity.GetInstance()) &&
            static_cast<int32_t>(now - m.latestTicks) <= static_cast<int32_t>(StaleAfter)) {
            if (!m.killed && world::HostProcesses(m.latest.position, 0.0f)) {
                const bCVector& position = m.entity.GetInstance()->GetWorldPosition();
                const float here[3] = {position.GetX(), position.GetY(), position.GetZ()};
                if (body::Distance(here, m.latest.position) > ReclaimAlignment) {
                    // Catch up before first death; an initialized corpse keeps its independent local ragdoll.
                    m.entity.GetInstance()->SetToWorldMatrix(body::Pose(*m.entity.GetInstance(), m.latest.position, m.latest.rotation));
                    world::RequestProcessingUpdate();
                }
            }
            ++it;
            continue;
        }
        const auto claim = claimedBy.find(it->first);
        if (claim != claimedBy.end() && !net::Remotes()[claim->second].connected) claimedBy.erase(claim);  // its player left
        else if (claim != claimedBy.end()) { if (static_cast<int32_t>(now - m.latestTicks) > static_cast<int32_t>(StaleAfter)) { ++it; continue; } }
        const bool stale = static_cast<int32_t>(now - m.latestTicks) > static_cast<int32_t>(StaleAfter);
        const bool validOwner = m.owner > 0 && m.owner < net::MaxPlayers;
        const bool ownerAvailable = validOwner && (net::IsHost() ? net::PeerJoined(m.owner) && net::InWorld(m.owner)
                                                && m.ownerGeneration == net::PeerGeneration(m.owner)
                                                : net::Connected() && (m.owner == 1 || net::Remotes()[m.owner].connected));
        if (stale && !m.released && ownerAvailable && !m.killed && !(m.latest.flags & 1)) {
            if (!m.staleHeld) {
                m.staleHeld = true;
                m.entity.CharacterMovement.StopMovement();
                m.body.walking = m.body.steered = false;
                Logf("npcs: mirror %s held while owner %u is connected but snapshots are stale", m.entity.GetName().GetText(), m.owner);
            }
            ++it;
            continue;
        }
        if ((m.released && !(m.latest.flags & 1)) || stale) {
            if (m.entity.IsDead()) m.killed = true;
            if (!m.killed) {
                ++staleMirrorReleases;
                m.entity.GetInstance()->SetToWorldMatrix(body::Pose(*m.entity.GetInstance(), m.latest.position, m.latest.rotation));
                m.entity.DamageReceiver.HitPoints = m.latest.hitPoints;
                body::Release(m.entity, m.body);  // out of the host's interest area: own AI again
                if (m.guarded) m.entity.NPC.DefeatedByPlayer = m.defeatedBefore;
            } else if (!m.lateNoted) {
                // Dead, and its owner stopped sending it before it lay (or before its late check): lying down goes on here,
                // looked up by GUID (B's stageK1 run 20261022-153310-p2: Slave_03 stood in a get-up motion, and no line at all
                // said it lay or gave up).
                char shows[128], slots[400];
                const bool lies = Lies(m.entity.GetInstance(), shows, sizeof(shows), slots, sizeof(slots));
                Logf("npcs: corpse %s: its owner stopped sending it (%s); laid down here further", m.entity.GetName().GetText(),
                     lies ? "lies" : "not lying yet");
                Mirror& c = corpses[it->first];
                c = m;
                c.wasMirror = true;
            }
            it = mirrors.erase(it);
            continue;
        }
        if (m.latest.flags & 1) {  // dead on the host
            // Die like the game's scripts let NPCs die. Entity::Kill removed the body from the NPC list (and
            // from view), so a client never showed a corpse.
            const bool first = !m.killed;
            if (!net::IsHost()) deadOnHost.insert(it->first);
            m.killed = true;  // before SetTask: the AI filter lets the death state run only for killed mirrors
            if (first) {
                m.killedAt = now ? now : 1;
                // The corpse lies where this body fell (local ragdoll); a steered body's movement controller is reset first so
                // it does not keep walking into the ragdoll. Only a body far from where the host's fell is moved there.
                m.entity.CharacterMovement.StopMovement();
                m.body.walking = m.body.steered = false;
                const bCVector& fell = m.entity.GetInstance()->GetWorldPosition();
                const float here[3] = {fell.GetX(), fell.GetY(), fell.GetZ()};
                if (body::Distance(here, m.latest.position) > CorpseAlignment)
                    m.entity.MoveTo(body::Pose(*m.entity.GetInstance(), m.latest.position, m.latest.rotation));
            }
            if (first)
                Logf("npcs: mirror %s dead on the host; here dead=%d defeated_by_player=%d task=%s", m.entity.GetName().GetText(),
                     m.entity.IsDead() ? 1 : 0, m.entity.NPC.DefeatedByPlayer ? 1 : 0, m.entity.Routine.GetCurrentTask().GetText());
            if (first && !m.entity.IsDead() && !xpGuard) {
                // QA negative control: the old way, the death task with the game's death script Kill here too.
                m.entity.Routine.FullStop();
                m.entity.Routine.SetTask(bCString("ZS_RagDollDead"));
            } else if (first && !m.entity.IsDead()) {
                // Dead as a state, not as an event: the host ran the death script Kill (quest OnNPCKilled,
                // experience, plunder) and shares its results. With AIMode 9 set first, ZS_Dead only lays the
                // body down and skips Kill (static analysis); ZS_RagDollDead ran Kill here as well, and the
                // client's quest manager completed the raid quest on its own (stage 3c regression).
                m.entity.Routine.FullStop();
                m.entity.DamageReceiver.HitPoints = 0;
                m.entity.Routine.AIMode = gEAIMode_Dead;
                m.entity.Routine.SetTask(bCString(DeathTask));
            }
            if (xpGuard) LayDown(m, now);
            LateCheck(m, now, "mirror");
            ++it;
            continue;
        }
        // Dead here (a death from the journal, applied while the host had not processed it yet) but alive in
        // the host's snapshot: wait for the host's own state instead of driving a corpse.
        if (m.entity.IsDead()) { ++it; continue; }
        if (static_cast<int32_t>(m.entity.DamageReceiver.HitPoints) != m.latest.hitPoints)
            m.entity.DamageReceiver.HitPoints = m.latest.hitPoints;
        if (m.latest.flags & 2) { m.body.localHitUntil = 0; m.body.localNative = false; }
        if (m.body.localNative) {
            // Ours ends with the game's own stagger state; the owner's replay of that blow arrives about then, so the mirror
            // stays out of the replay for a moment longer (once, not twice).
            Entity staggering = m.entity;
            if (!StaggerState(staggering.Routine.GetCurrentTask().GetText())) {
                m.body.localNative = false;
                // Not held any longer (that stood the mirror frozen for 450 ms): it follows its owner again at once, only the
                // owner's replay of this same stagger is not shown a second time.
                m.body.localHitUntil = 0;
                m.body.actionUntil = 0;
                m.staggerSuppressUntil = now + NativeHitTail;
                ++localHitsNativeEnded;
            }
        }
        if (talk::TalkingWithHero(m.entity.GetInstance())) { ++it; continue; }  // stands with our hero; the host holds it
        float shownAt[3], shownTurn[4];
        const bool timeline = GetConfig().movementSmoothing && body::interpolate;
        body::Presentation presentation;
        const int32_t delay = body::PlaybackDelay(m.body.track);
        const bool buffered = timeline && body::Sample(m.body.track, now, shownAt, shownTurn, -delay, true, &presentation);
        if (buffered) ApplyDisplacement(m, shownAt, now);
        m.havePresentation = buffered;
        m.shownSent = buffered ? presentation.sent : m.latestSent;
        net::NpcRecord shown = m.latest;
        if (buffered) {
            shown.motion = presentation.motion.motion;
            shown.heldRight = presentation.motion.heldRight;
            shown.heldLeft = presentation.motion.heldLeft;
            shown.aniState = presentation.motion.aniState < 0 ? 0 : static_cast<uint8_t>(presentation.motion.aniState);
            std::memcpy(shown.motions, presentation.motion.motions, sizeof(shown.motions));
            shown.flags = presentation.motion.flags;
            if (presentation.movementMode >= 0) shown.movementMode = static_cast<uint8_t>(presentation.movementMode);
        }
        // Weapons in hand and the fighting stance: without them a mirror walks unarmed and its attack motions
        // never show (first human test: the client saw no fight at all; 0 of 170 attack motions matched).
        // What the mirror really holds, not what it was last given: stopping its AI (Pacify: FullStop) puts weapons
        // away again. Checked at most every 250 ms.
        if (style && static_cast<int32_t>(now - m.holdChecked) >= 250 &&
            (shown.heldRight != HeldHash(m.entity, gESlot_RightHand) || shown.heldLeft != HeldHash(m.entity, gESlot_LeftHand))) {
            m.holdChecked = now;
            // A mirror never ran its own AI, which fills an NPC's inventory with its equipment (its inventory was empty;
            // AssureAll froze the client): create the held item itself, by the name the owner sent once.
            EnsureItem(m.entity, shown.heldRight);
            EnsureItem(m.entity, shown.heldLeft);
            const GEInt right = StackOf(m.entity, shown.heldRight), left = StackOf(m.entity, shown.heldLeft);
            const GEBool held = m.entity.Inventory.HoldStacks(left, right);
            ++holdChanges;
            if (holdChanges < 20)
                Logf("npcs: %s holds stacks %d/%d (wanted %08x/%08x) ok=%d now=%s", m.entity.GetName().GetText(), left, right, shown.heldLeft,
                     shown.heldRight, held ? 1 : 0, m.entity.Inventory.GetItemFromSlot(gESlot_RightHand).GetInstance() ? "right" : "-");
            m.heldRight = shown.heldRight;
            m.heldLeft = shown.heldLeft;
        }
        const float speed = body::Speed(m.previous.position, m.latest.position, m.previousSent, m.latestSent, m.previousTicks, m.latestTicks);
        auto name = motionNames.find(shown.motion);
        if (name != motionNames.end()) LearnSkeleton(m, name->second.c_str());
        // QA (Stufe 3.2): how far the mirror stood from where its owner had the NPC (half the round trip before it arrived)
        if (m.latestSent != m.truthSent && speed > 100.0f)
            body::NoteTruth(m.body, m.latest.position, m.latestTicks - net::RoundTrip(m.owner) / 2);
        m.truthSent = m.latestSent;
        const float steady = body::Steadiness(m.body.track, m.body, now);  // also the glide causes (overshoot after a stop or turn)
        float ahead[3];
        body::Target target{buffered ? shownAt : m.latest.position, buffered ? shownTurn : m.latest.rotation, m.latest.movementMode,
                                  name != motionNames.end() ? name->second.c_str() : nullptr, speed,
                                  style && shown.aniState ? static_cast<int>(shown.aniState) : -1,
                                  body::SteerGoal(m.body.track, now, ahead, buffered ? m.latest.position : nullptr,
                                                  body::mirrorLead && steady >= 1.0f)};
        target.actionSeq = static_cast<int>(shown.motions[0].instance);
        target.motionFrac = shown.motions[0].active ? static_cast<int>(shown.motions[0].phase * 65535.0f) : 65535;
        if (static_cast<int32_t>(m.staggerSuppressUntil - now) > 0 && target.motion && std::strstr(target.motion, "Stumble")) {
            target.motion = nullptr;
            target.motionFrac = -1;
        }
        target.motionAgeMs = buffered ? presentation.sent - presentation.stateSent
                                     : now - m.latestTicks + net::RoundTrip(m.owner) / 2;
        float smoothGoal[3], goalTurn[4];
        if (buffered) {
            if (presentation.movementMode >= 0 && presentation.movementMode <= 255)
                target.movementMode = static_cast<uint8_t>(presentation.movementMode);
            target.speed = presentation.speed;
            target.goal = body::Sample(m.body.track, now, smoothGoal, goalTurn, body::GoalAhead - delay, false)
                          ? smoothGoal : shownAt;
            if (target.goal == smoothGoal) AddDisplacement(m, smoothGoal, now);
            target.smooth = true;
            target.timeline = true;
            target.presentationSerial = m.body.track.serial;
        }
        m.body.combatMoves = true;  // attacks replay as the game's combat moves (body.cpp)
        m.body.contactTarget = (shown.flags & 4) && ((shown.flags >> 3) & 7) == net::LocalId()
                                   ? remote::ControlledBody() : nullptr;
        if (m.body.contactTarget)
            m.entity.NPC.SetCurrentTarget(Entity(m.body.contactTarget));
        body::Drive(m.entity, m.body, target, now);
        TryLocalBlow(m, shown, now);
        if (m.body.combatMoveAt && now - m.body.combatMoveAt < 1000) {
            ++swingSamples;
            Entity weapon = m.entity.Inventory.GetItemFromSlot(gESlot_RightHand);
            if (weapon.GetInstance() && weapon.GetInstance()->GetPropertySet(eEPropertySetType_Damage) &&
                static_cast<gEDamageType>(weapon.Damage.DamageType) != gEDamageType_None)
                ++swingArmed;
        }
        ++it;
    }
}
}

void PassiveMirrorTick()
{
    if (net::IsHost()) return;
    const uint32_t now = net::Now();
    if (!absenceCheckAt || now - absenceCheckAt >= 250) {
        absenceCheckAt = now ? now : 1;
        for (auto const& entry : absences) {
            auto const& record = entry.second;
            Entity live = Resolve(record.guid);
            eCEntity* entity = live.GetInstance();
            if (!IsNpc(entity)) continue;
            const bCMatrix pose = body::Pose(*entity, record.position, record.rotation);
            entity->SetToWorldMatrix(pose);
            if (auto* rigid = static_cast<eCRigidBody_PS*>(entity->GetPropertySet(eEPropertySetType_RigidBody)))
                rigid->SetPose(pose);
        }
    }
    SamplePlayerContact(now);
    if (lastFrameAt) {
        const uint32_t gap = now - lastFrameAt;
        ++frameCount;
        frameSumMs += gap;
        slowFrames += gap > 33;
        hitchFrames += gap > 66;
        maxFrameMs = std::max(maxFrameMs, gap);
    }
    lastFrameAt = now;
    for (auto it = mirrors.begin(); it != mirrors.end();) {
        Mirror& m = it->second;
        if (m.released) { it = mirrors.erase(it); continue; }
        Entity live = Resolve(m.latest.guid);
        if (live.GetInstance() != m.entity.GetInstance() || (m.resolved && !IsNpc(live.GetInstance()))) {
            if (m.resolved) Logf("npcs: mirror %s unloaded or changed property sets; waiting for its live instance",
                                live.GetInstance() ? live.GetName().GetText() : "(unloaded)");
            m.entity = live;
            m.resolved = false;
            m.replayReady = false;
            m.resolvedAt = 0;
            m.replay = npcplayback::State{};
            m.ragdollStarted = false;
            m.corpsePlaced = false;
            m.corpseSimulating = false;
            m.guarded = false;
        }
        if (!m.resolved || !m.entity.GetInstance()) {
            if (m.resolvedAt && now - m.resolvedAt < RetryLookup) { ++it; continue; }
            m.entity = Resolve(m.latest.guid);
            m.resolvedAt = now ? now : 1;
            m.resolved = IsNpc(m.entity.GetInstance());
            m.missing = !m.resolved;
            if (!m.resolved) { ++it; continue; }
            npcplayback::Silence(m.entity.GetInstance());
            m.body.pacified = true;
            m.defeatedBefore = m.entity.NPC.DefeatedByPlayer;
            m.entity.NPC.DefeatedByPlayer = GETrue;
            m.guarded = true;
        }
        m.replayReady = false;
        if (!net::Connected() || now - m.latestTicks > StaleAfter) { ++it; continue; }
        body::Presentation presentation;
        net::NpcRecord shown = m.latest;
        m.body.track.predictive = false;
        if (!body::Sample(m.body.track, now, shown.position, shown.rotation, -body::PlaybackDelay(m.body.track), true, &presentation)) {
            ++it;
            continue;
        }
        shown.flags = presentation.motion.flags;
        shown.hitPoints = presentation.motion.hitPoints;
        shown.hitPointsMax = presentation.motion.hitPointsMax;
        if (presentation.movementMode >= 0) shown.movementMode = static_cast<uint8_t>(presentation.movementMode);
        shown.aniState = presentation.motion.aniState < 0 ? 0 : static_cast<uint8_t>(presentation.motion.aniState);
        shown.heldRight = presentation.motion.heldRight;
        shown.heldLeft = presentation.motion.heldLeft;
        shown.partyPlayer = presentation.motion.partyPlayer;
        shown.partyType = presentation.motion.partyType;
        shown.waiting = presentation.motion.waiting;
        shown.guidePlayer = presentation.motion.guidePlayer;
        shown.aiMode = presentation.motion.aiMode;
        shown.action = presentation.motion.action;
        shown.attackReason = presentation.motion.attackReason;
        shown.damageType = presentation.motion.damageType;
        shown.damageAmount = presentation.motion.damageAmount;
        shown.damageMultiplier = presentation.motion.damageMultiplier;
        shown.incarnation = presentation.motion.incarnation;
        shown.hitPlayer = presentation.motion.hitPlayer;
        shown.hitSequence = presentation.motion.hitSequence;
        shown.presentationReady = presentation.motion.presentationReady;
        shown.contact = presentation.motion.contact;
        shown.attitude = presentation.motion.attitude;
        std::memcpy(shown.motions, presentation.motion.motions, sizeof(shown.motions));
        if (!m.deathLife.Observe(shown.incarnation, (shown.flags & 1) != 0)) { ++it; continue; }
        m.shownSent = presentation.sent;
        shown.sent = presentation.sent;
        m.havePresentation = true;
        if (!(shown.flags & 3)) {
            auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
            if (animation && animation->GetRagDollEnabled()) animation->StopRagDoll();
        }
        if (m.killed && !(shown.flags & 1)) {
            m.ragdollStarted = m.laid = false;
            deadOnHost.erase(it->first);
        }
        m.killed = (shown.flags & 1) != 0;
        auto* routine = static_cast<gCScriptRoutine_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_ScriptRoutine));
        if (!routine) {
            Logf("npcs: %s lost its script property during presentation; rebinding next frame", m.entity.GetName().GetText());
            m.resolved = false;
            m.resolvedAt = 0;
            ++it;
            continue;
        }
        // The script property setter also executes AI-mode transitions. Mirrors only copy the property value.
        routine->AccessAIMode().AccessValue() = m.killed ? gEAIMode_Dead : static_cast<gEAIMode>(shown.aiMode);
        routine->AccessAction().AccessValue() = static_cast<gEAction>(shown.action);
        auto* npcProperties = static_cast<gCNPC_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_NPC));
        npcProperties->AccessAttackReason().AccessValue() = static_cast<gEAttackReason>(shown.attackReason);
        // The game colours the name tag (green friendly, white neutral, orange angry, red hostile) from this property.
        npcProperties->AccessAttitudeToPlayer2().AccessValue() = static_cast<gEAttitude>(shown.attitude);
        const uint8_t target = (shown.flags >> 3) & 7;
        npcProperties->AccessCurrentTargetEntity().SetEntity(target
            ? (target == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(target)) : nullptr);
        auto* receiver = static_cast<gCDamageReceiver_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_DamageReceiver));
        receiver->AccessHitPointsMax() = shown.hitPointsMax;
        receiver->AccessHitPoints() = shown.hitPoints;
        if (m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
            const bool waiting = m.entity.Party.Waiting != GEFalse;
            const uint8_t actualOwner = PlayerOf(m.entity.Party.GetPartyLeader().GetInstance());
            if (m.partyApplied && m.appliedOwner != net::LocalId() && actualOwner == net::LocalId() && !myClaims.count(it->first)) {
                Claim(m.entity.GetInstance());
                net::WorldState request{};
                request.kind = 4;
                request.npcClass = 1;
                std::memcpy(request.guid, shown.guid, 16);
                net::SendWorldState(request, 0);
            } else if (m.partyApplied && m.appliedOwner == net::LocalId() && actualOwner != net::LocalId() &&
                       myClaims.count(it->first) && m.relationAcknowledged) {
                Unclaim(m.entity.GetInstance());
                net::WorldState request{};
                request.kind = 5;
                std::memcpy(request.guid, shown.guid, 16);
                net::SendWorldState(request, 0);
            }
            if (shown.partyPlayer == net::LocalId()) m.relationAcknowledged = true;
            if (m.partyApplied && m.appliedOwner == net::LocalId() && waiting != m.appliedWaiting) {
                net::WorldState request{};
                request.kind = 22;
                std::memcpy(request.guid, shown.guid, 16);
                request.immortal = waiting ? 1 : 0;
                net::SendWorldState(request, 0);
            }
            eCEntity* leader = shown.partyPlayer == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(shown.partyPlayer);
            const bool pending = myClaims.count(it->first) && !m.relationAcknowledged && actualOwner == net::LocalId();
            if (!pending) {
                if (m.entity.Party.GetPartyLeader().GetInstance() != leader) m.entity.Party.SetPartyLeader(Entity(leader));
                m.entity.Party.PartyMemberType = static_cast<gEPartyMemberType>(shown.partyType);
                m.entity.Party.Waiting = shown.waiting ? GETrue : GEFalse;
            }
            m.partyApplied = true;
            m.appliedOwner = shown.partyPlayer;
            m.appliedWaiting = shown.waiting != 0;
        }
        eCEntity* hero = remote::ControlledBody();
        if (hero && hero->GetPropertySet(eEPropertySetType_Party)) {
            Entity player(hero);
            if (shown.guidePlayer == net::LocalId()) {
                m.guideAcknowledged = true;
                player.Party.SetPartyLeader(m.entity);
            } else if (m.guideAcknowledged && player.Party.GetPartyLeader().GetInstance() == m.entity.GetInstance())
                player.Party.SetPartyLeader(Entity());
        }
        routine->AccessAniState().AccessValue() = static_cast<gEAniState>(shown.aniState);
        if (m.killed) deadOnHost.insert(it->first);
        if (shown.flags & 128) {
            if (!m.corpsePlaced) {
                const bCMatrix pose = m.ragdollInitialized && !m.corpsePose.empty()
                    ? m.corpseRoot : body::Pose(*m.entity.GetInstance(), shown.position, shown.rotation);
                m.entity.GetInstance()->SetToWorldMatrix(pose);
                if (auto* rigid = static_cast<eCRigidBody_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_RigidBody)))
                    rigid->SetPose(pose);
                m.corpsePlaced = true;
            }
            LayDown(m, now);
            TryApplyForce(m, now);
            UpdateCorpsePresentation(m, now);
            ++it;
            continue;
        }
        if (!shown.presentationReady) { ++it; continue; }
        if (shown.heldRight != HeldHash(m.entity, gESlot_RightHand) || shown.heldLeft != HeldHash(m.entity, gESlot_LeftHand)) {
            EnsureItem(m.entity, shown.heldRight);
            EnsureItem(m.entity, shown.heldLeft);
            m.entity.Inventory.HoldStacks(StackOf(m.entity, shown.heldLeft), StackOf(m.entity, shown.heldRight));
            ++holdChanges;
        }
        if (auto name = motionNames.find(shown.motions[0].name); name != motionNames.end()) LearnSkeleton(m, name->second.c_str());
        Entity damageSource = m.entity.Inventory.GetItemFromSlot(gESlot_RightHand);
        if (!damageSource.GetInstance() || !damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage)) damageSource = m.entity;
        if (auto* damage = static_cast<gCDamage_PS*>(damageSource.GetInstance()->GetPropertySet(eEPropertySetType_Damage))) {
            damage->AccessDamageType().AccessValue() = static_cast<gEDamageType>(shown.damageType);
            damage->AccessDamageAmount() = shown.damageAmount;
            damage->AccessDamageHitMultiplier() = shown.damageMultiplier;
        }
        m.replayReady = npcplayback::Replay(m.entity.GetInstance(), m.replay, shown, &MotionNameOf, now);
        if (m.replayReady) {
            const uint32_t dt = m.body.lastAt ? now - m.body.lastAt : 0;
            const float traveled = body::Distance(m.body.lastPos, shown.position);
            char const* expected = MotionNameOf(shown.motions[0].name);
            if (body::IsLocomotionMotion(expected) && dt && dt < 250 && traveled < 300.0f) {
                m.body.walkExpectedMs += dt;
                if (traveled * 1000.0f / static_cast<float>(dt) > 10.0f) {
                    m.body.walkTravelMs += dt;
                    char visible[128] = {};
                    body::CurrentMotion(m.entity.GetInstance(), visible, sizeof(visible));
                    if (!body::IsLocomotionMotion(visible) || !body::MotionRunning(m.entity.GetInstance()))
                        m.body.walkSlideMs += dt;
                } else m.body.walkStillMs += dt;
            }
            std::memcpy(m.body.lastPos, shown.position, sizeof(m.body.lastPos));
            m.body.lastAt = now;
            m.body.contactTarget = (shown.flags & 4) && ((shown.flags >> 3) & 7) == net::LocalId()
                                      ? remote::ControlledBody() : nullptr;
            TryPlayerBlow(m);
            TryLocalBlow(m, shown, now);
        }
        ++it;
    }
}

bool MirrorFightsHero()
{
    eCEntity* body = remote::ControlledBody();
    if (!body || !net::LocalId()) return false;
    const bCVector& p = body->GetWorldPosition();
    const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
    bool any = false, us = false;
    for (auto const& entry : mirrors) {
        Mirror const& m = entry.second;
        if (m.killed || !(m.latest.flags & 4) || (m.latest.flags & 1)) continue;
        any = true;
        if (((m.latest.flags >> 3) & 7) != net::LocalId()) continue;
        us = true;
        if (body::Distance(m.latest.position, at) <= 3000.0f) {  // HandleMusicSituation's 30 m (SG 0xb4070)
            ++fightSeenAny, ++fightSeenUs, ++fightSeenNear;
            return true;
        }
    }
    fightSeenAny += any;
    fightSeenUs += us;
    return false;
}

void FightStats(unsigned& sent, unsigned& seenAny, unsigned& seenUs, unsigned& seenNear)
{
    sent = fightSent, seenAny = fightSeenAny, seenUs = fightSeenUs, seenNear = fightSeenNear;
}

void SwingSamples(unsigned& armed, unsigned& samples)
{
    armed = swingArmed;
    samples = swingSamples;
}

uint32_t SeenAt(eCEntity const* entity)
{
    if (!entity) return 0;
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return entry.second.havePresentation ? entry.second.shownSent
                   : GetConfig().movementSmoothing && body::interpolate ? 0 : entry.second.latestSent;
    return 0;
}

bool ParryingAt(eCEntity const* entity, uint32_t at, bool& parrying)
{
    if (!lagBlocks || !entity || !at || static_cast<int32_t>(net::Now() - at) < 0 ||
        net::Now() - at > SentStatesFor) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    const auto it = sentStates.find(Key(guid));
    if (it == sentStates.end()) return false;
    // Never use a future block state: pose and parry share the exact capture timestamp on the wire.
    int best = -1;
    for (int i = 0; i < StateHistory; ++i) {
        const uint32_t t = it->second.at[i];
        if (!t || static_cast<int32_t>(t - at) > 0 || static_cast<int32_t>(at - t) > static_cast<int32_t>(SentStatesFor)) continue;
        if (best < 0 || static_cast<int32_t>(t - it->second.at[best]) > 0) best = i;
    }
    if (best < 0) return false;
    parrying = it->second.parrying[best];
    return true;
}

void Start()
{
    net::SetNpcHandlers(&OnNpcs, &OnMotionName);
    net::SetNpcReleaseHandler(&OnRelease);
    net::SetRagdollHandler(&OnRagdollNote);
    net::SetHitResultHandler(&OnHitResult);
}

bool InAction(eCEntity const* entity)
{
    if (!entity) return false;
    const uint32_t now = net::Now();
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return static_cast<int32_t>(entry.second.body.actionUntil - now) > 0;
    return false;
}

void WalkingReport(char const* guid, LogFn log)
{
    unsigned d1 = 0, d2 = 0, d3 = 0, bytes[8] = {};
    if (!guid || std::sscanf(guid, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3,
                             &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5], &bytes[6], &bytes[7]) != 11) {
        log("WALKING refused: bad GUID");
        return;
    }
    _GUID raw{};
    raw.Data1 = d1; raw.Data2 = static_cast<unsigned short>(d2); raw.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) raw.Data4[i] = static_cast<unsigned char>(bytes[i]);
    const std::string key(reinterpret_cast<char const*>(&raw), 16);
    const auto found = mirrors.find(key);
    if (found == mirrors.end()) { log("WALKING refused: not a mirrored NPC"); return; }
    Mirror const& m = found->second;
    body::State const& state = m.body;
    char shown[128] = {};
    body::CurrentMotion(m.entity.GetInstance(), shown, sizeof(shown));
    const uint32_t now = net::Now();
    char line[480];
    Format(line, "WALKING expected_ms=%u travel_ms=%u slide_ms=%u still_ms=%u flags=%u fights_player=%u contact_target=%d "
                 "combat_move_age=%d action_left=%d combat_moves=%d motion=%.90s",
           state.walkExpectedMs, state.walkTravelMs, state.walkSlideMs, state.walkStillMs,
           static_cast<unsigned>(m.latest.flags), static_cast<unsigned>((m.latest.flags >> 3) & 7), state.contactTarget ? 1 : 0,
           state.combatMoveAt ? static_cast<int>(now - state.combatMoveAt) : -1, static_cast<int>(state.actionUntil - now),
           state.combatMoves ? 1 : 0, shown);
    log(line);
}

bool DiedOnHost(eCEntity const* entity)
{
    if (!entity) return false;
    for (auto const& entry : mirrors)
        if (entry.second.killed && entry.second.entity.GetInstance() == entity) return true;
    return false;
}

// QA, client (stage X1): one NPC record with this machine's hero GUID (the same entity as the host's hero in a shared
// world) and a NaN position, sent as if it were an NPC of this client's region.
void BadRecord(LogFn log)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || net::IsHost()) { log("REFUSED badrecord: client with a hero only"); return; }
    net::NpcRecord record{};
    Guid(hero, record.guid);
    record.position[0] = std::nanf("");
    record.position[1] = record.position[2] = 0.0f;
    record.rotation[3] = 1.0f;
    record.hitPoints = 1;
    net::SendNpcs(&record, 1);
    log("BADRECORD sent=1");
}

// QA (client, stage EdgeClaim): one snapshot of the host NPC with this GUID as if this machine simulated it, where this
// player's hero stands: the state of stageM2, where the client's own copy of a raid scout was near the players while the
// host's was just outside its range.
void ClaimRecord(char const* text, LogFn log)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    unsigned d1, d2, d3, b[8];
    if (!hero || net::IsHost() || !text ||
        sscanf_s(text, "%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) {
        log("REFUSED claimrecord: client with a hero, <GUID>");
        return;
    }
    _GUID raw;
    raw.Data1 = d1;
    raw.Data2 = static_cast<unsigned short>(d2);
    raw.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) raw.Data4[i] = static_cast<unsigned char>(b[i]);
    net::NpcRecord record{};
    std::memcpy(record.guid, &raw, 16);
    const bCVector& p = hero->GetWorldPosition();
    record.position[0] = p.GetX() + 200.0f;
    record.position[1] = p.GetY();
    record.position[2] = p.GetZ();
    record.rotation[3] = 1.0f;
    Entity copy = Resolve(record.guid);
    record.hitPoints = copy.GetInstance() ? copy.DamageReceiver.HitPoints : 100;
    net::SendNpcs(&record, 1);
    char line[120];
    Format(line, "CLAIMRECORD sent=1 known_here=%d", copy.GetInstance() ? 1 : 0);
    log(line);
}

char const* MotionNameOf(uint32_t hash)
{
    auto name = motionNames.find(hash);
    return name == motionNames.end() ? nullptr : name->second.c_str();
}

eCEntity* NpcByGuid(uint8_t const guid[16])
{
    eCEntity* instance = Resolve(guid).GetInstance();
    return IsNpc(instance) ? instance : nullptr;
}

// Our hero's blow on a mirror shows at once (Stufe 1, owner 27.10.): its owner judges it a network round later, and until
// then the NPC stood unmoved. The mirror plays a short stumble here, only the look: no hit points, no AI. Its motion is
// one the owner already sent for this skeleton (a stumble's hit motion, of the same weapon stance if one is known), so no
// name is guessed; with none known yet nothing plays. The owner's own snapshot takes over after LocalHitFor: a parade or
// a miss there shows as such then.
bool localHits = true;  // QA negative control: localhitsoff
bool fallbackStumbles = true;  // a first blow on a skeleton the owner sent no stumble of yet
constexpr uint32_t LocalHitFor = 350;  // ms
unsigned localHitsShown = 0, localHitsUnknown = 0;
std::unordered_map<std::string, std::string> stumbleFor;  // "<skeleton>_<stance>" -> a stumble motion the owner sent

std::string StumbleMotion(char const* skeleton, char const* stance)
{
    const std::string key = std::string(skeleton) + "_" + stance;
    if (auto it = stumbleFor.find(key); it != stumbleFor.end()) return it->second;
    std::string any, same;
    const std::string prefix = std::string(skeleton) + "_Stand_";
    for (auto const& [hash, name] : motionNames) {
        if (name.compare(0, prefix.size(), prefix) || name.find("Stumble") == std::string::npos || name.find("_Hit_") == std::string::npos) continue;
        if (name.find("Parade") != std::string::npos) continue;
        if (any.empty()) any = name;
        if (*stance && name.find(std::string("_") + stance + "_") != std::string::npos) { same = name; break; }
    }
    std::string chosen = same.empty() ? any : same;
    // None sent yet (a first blow): the stumbles seen in the play evening's logs, 27.10. (Hero without a weapon
    // StumbleR ..._P0_90, with one QuickStumble ..._P0_90; a waran StumbleR ..._P0_200, used for every other skeleton).
    if (chosen.empty() && fallbackStumbles) {
        if (!std::strcmp(skeleton, "Hero") && (!*stance || !std::strcmp(stance, "None")))
            chosen = "Hero_Stand_None_None_P0_StumbleR_Hit_N_Back_00_%_00_P0_90";
        else if (!std::strcmp(skeleton, "Hero"))
            chosen = std::string("Hero_Stand_None_") + stance + "_P0_QuickStumble_Hit_N_Fwd_00_%_00_P0_90";
        else
            chosen = std::string(skeleton) + "_Stand_None_Fist_P0_StumbleR_Hit_N_Back_00_%_00_P0_200";
        return chosen;  // not kept: a name the owner sends later replaces it
    }
    if (const size_t dot = chosen.find(".xmot"); dot != std::string::npos) chosen.resize(dot);
    if (!chosen.empty()) stumbleFor[key] = chosen;
    return chosen;
}

void LocalHit(eCEntity* entity)
{
    LocalHit(entity, 0);
}

void LocalHit(eCEntity* entity, uint32_t sequence)
{
    if (!entity || !sequence || !Ready(entity)) return;
    for (auto& entry : mirrors) {
        Mirror& m = entry.second;
        if (m.entity.GetInstance() != entity || m.killed) continue;
        char shown[128] = {}, stance[16] = {};
        body::CurrentMotion(entity, shown, sizeof(shown));
        LearnSkeleton(m, shown);
        int field = 0;
        for (char const* c = shown; *c && field < 4; ++c) {
            if (*c == '_') { ++field; continue; }
            if (field == 3) {
                const size_t n = std::strlen(stance);
                if (n + 1 < sizeof(stance)) stance[n] = *c, stance[n + 1] = '\0';
            }
        }
        std::string motion = StumbleMotion(m.skeleton, stance);
        if (!motion.empty() && motion.find(".xmot") == std::string::npos) motion += ".xmot";
        if (motion.empty() || !npcplayback::LocalHit(entity, m.replay, motion.c_str(), m.latest.motions[0], sequence, net::Now())) {
            Logf("npcs: immediate hit presentation could not start for %s", m.entity.GetName().GetText());
            return;
        }
        ++localHitsShown;
        return;
    }
}

void HostHit(eCEntity* entity, uint8_t player, uint32_t sequence)
{
    if (!net::IsHost() || !entity || !entity->GetPropertySet(eEPropertySetType_NPC) || remote::IsPuppet(entity)) return;
    uint8_t guid[16];
    Guid(entity, guid);
    hostHits[Key(guid)] = {player, sequence};
}

void Despawn(eCEntity* entity)
{
    if (!net::IsHost() || !IsNpc(entity)) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const std::string key = Key(guid);
    LastSent& last = lastSent[key];
    auto& record = last.record;
    std::memcpy(record.guid, guid, sizeof(record.guid));
    record.incarnation = Incarnation(guid);
    record.sent = net::Now();
    record.absent = 1;
    record.flags |= 1;
    record.hitPoints = 0;
    record.presentationReady = record.contact = 0;
    const auto& at = entity->GetWorldPosition();
    const auto turn = entity->GetWorldRotation();
    record.position[0] = at.GetX(); record.position[1] = at.GetY(); record.position[2] = at.GetZ();
    record.rotation[0] = turn.GetX(); record.rotation[1] = turn.GetY();
    record.rotation[2] = turn.GetZ(); record.rotation[3] = turn.GetW();
    last.at = record.sent;
    forceHistory.erase(key);
    net::SendNpcs(&record, 1, true);
}

uint32_t Incarnation(uint8_t const guid[16])
{
    const std::string key = Key(guid);
    if (net::IsHost()) {
        const auto found = incarnations.find(key);
        return found == incarnations.end() || !found->second ? 1 : found->second;
    }
    const auto found = mirrors.find(key);
    if (found != mirrors.end()) return found->second.latest.incarnation;
    const auto absent = absences.find(key);
    return absent == absences.end() ? 0 : absent->second.incarnation;
}

bool LocalReaction(eCEntity const* entity, char const* state)
{
    if (npcplayback::IsPassive(entity)) return false;
    if (!entity || !nativeHits) return false;
    const uint32_t now = net::Now();
    for (auto const& entry : mirrors) {
        Mirror const& m = entry.second;
        if (m.entity.GetInstance() != entity) continue;
        if (!m.body.localNative || static_cast<int32_t>(m.body.localHitUntil - now) <= 0) return false;
        // The stagger state itself, and the helpers it calls (AI functions are no ZS_ states) while that state is the task; never
        // another state, which would let a driven body fight.
        if (StaggerState(state)) return true;
        if (!std::strncmp(state, "ZS_", 3)) return false;
        Entity task = m.entity;
        return StaggerState(task.Routine.GetCurrentTask().GetText());
    }
    return false;
}

bool IsMirrored(eCEntity const* entity)
{
    if (npcplayback::IsPassive(entity)) return true;
    if (!entity) return false;
    for (auto const& entry : mirrors)
        if (entry.second.body.pacified && entry.second.entity.GetInstance() == entity) return true;
    return false;
}

bool Ready(eCEntity const* entity)
{
    if (!IsMirrored(entity)) return true;
    for (auto const& entry : mirrors)
        if (entry.second.entity.GetInstance() == entity)
            return net::Connected() && net::Now() - entry.second.latestTicks <= StaleAfter && entry.second.replayReady;
    return false;
}

bool PresentationPending(eCEntity const* entity)
{
    if (!entity || !IsMirrored(entity)) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    const auto key = Key(guid);
    if (absences.count(key)) return false;
    const auto found = mirrors.find(key);
    if (found == mirrors.end()) return !Entity(const_cast<eCEntity*>(entity)).IsDead();
    const auto& mirror = found->second;
    if (mirror.killed || (mirror.latest.flags & 1)) return false;
    return net::Now() - mirror.latestTicks > StaleAfter || !mirror.replayReady;
}

void OnWorldDeactivate()
{
    playerContact = PlayerContact{};
    net::ResetNpcReplication();
    mirrors.clear();
    corpses.clear();
    absences.clear();
    absenceCheckAt = 0;
    namesSent.clear();
    // A loaded save or another world starts without companions taken over (nothing of them is saved yet).
    claimedBy.clear();
    directedBy.clear();
    claimSeen.clear();
    myClaims.clear();
    givenBack.clear();
    contested.clear();
    hostTaken.clear();
    reclaimed.clear();
    reclaimPause.clear();
    sentStates.clear();
    inSphereSince.clear();
    deadOnHost.clear();
    lastSent.clear();
    incarnations.clear();
    hostHits.clear();
    forceHistory.clear();
    pendingForces.clear();
    forceSequences.clear();
    sendCadence = replication::Cadence60{};
    captureTicks = captureStartedAt = 0;
    owned.clear();
    sending.clear();
}

void CorpseTick(uint32_t now)
{
    for (auto it = corpses.begin(); it != corpses.end();) {
        Mirror& c = it->second;
        c.entity = Resolve(c.latest.guid);
        if (!c.entity.GetInstance()) { ++it; continue; }  // not in this part of the world now; again when it is
        if (deadStays && !net::IsHost() && deadOnHost.count(it->first) && !mirrors.count(it->first)) KeepDead(c.entity);
        if (!c.entity.IsDead() || mirrors.count(it->first)) { it = corpses.erase(it); continue; }  // alive again, or mirrored now
        LayDown(c, now);
        if (npcplayback::IsPassive(c.entity.GetInstance())) UpdateCorpsePresentation(c, now);
        LateCheck(c, now, c.wasMirror ? "mirror" : "journal");
        ++it;
    }
}

void Tick()
{
    gCSession& session = gCSession::GetInstance();
    if (!session.IsGameRunning() || !session.GetHero()) return;
    // Diagnostic: every change of the hero's experience, to find who rewards whom (stage 3c).
    static int lastXp = -1;
    Entity hero(session.GetHero());
    const int xp = static_cast<int>(hero.PlayerMemory.XP);
    if (lastXp >= 0 && xp != lastXp) Logf("xp: %+d total=%d", xp - lastXp, xp);
    lastXp = xp;
    if (net::IsHost()) HostTick();
    InstallRagdollHook(net::Now());
    PassiveMirrorTick();
    CorpseTick(net::Now());
}

void LayDownCorpse(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const std::string key = Key(guid);
    if (corpses.count(key)) return;
    Mirror& c = corpses[key];
    std::memcpy(c.latest.guid, guid, 16);
    c.killed = true;
    c.killedAt = net::Now() ? net::Now() : 1;
}

void Claim(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    const std::string key = Key(guid);
    myClaims.insert(key);
    if (auto mirror = mirrors.find(key); mirror != mirrors.end())
        mirror->second.relationAcknowledged = mirror->second.guideAcknowledged = false;
    // Ours again right after the host took it back (kind 7 follows the revoke within milliseconds): the hand-back pause
    // from Unclaim would keep it from being sent, and the host ends the claim again for want of snapshots (run 223228).
    givenBack.erase(key);
    Logf("npcs: requested player relation for %s; NPC remains host simulated", Entity(entity).GetName().GetText());
}

void Unclaim(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    myClaims.erase(Key(guid));
}

void Foreign(uint8_t const guid[16])
{
    if (net::IsHost()) return;
    Rebind(guid);
}

void Rebind(uint8_t const guid[16])
{
    const auto it = mirrors.find(Key(guid));
    if (it == mirrors.end()) return;
    it->second.resolved = false;  // the next tick resolves it again
    it->second.missing = false;
}

void Revoked(uint8_t const guid[16])
{
    Entity e = Resolve(guid);
    Logf("npcs: the host took back %s", e.GetInstance() ? e.GetName().GetText() : "?");
    if (e.GetInstance()) {
        Unclaim(e.GetInstance());
        eCEntity* hero = remote::ControlledBody();
        if (hero && hero->GetPropertySet(eEPropertySetType_Party)) {
            Entity player(hero);
            if (player.Party.GetPartyLeader().GetInstance() == e.GetInstance()) player.Party.SetPartyLeader(Entity());
            if (e.GetInstance()->GetPropertySet(eEPropertySetType_Party) && e.Party.GetPartyLeader().GetInstance() == hero)
                e.Party.SetPartyLeader(Entity());
        }
    }
    else myClaims.erase(Key(guid));
}

bool OnClaim(uint8_t const guid[16], uint8_t player, bool take)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers || !net::PeerJoined(player)) return false;
    const std::string key = Key(guid);
    const auto current = claimedBy.find(key);
    if (take && current != claimedBy.end() && current->second != player && net::Remotes()[current->second].connected) {
        // Two players recruited it at once: the first keeps it, the second mirrors it again.
        ++claimsDenied;
        SendRevoke(key, player);
        Logf("npcs: player %u cannot take over %s (player %u has it)", player,
             Resolve(guid).GetInstance() ? Resolve(guid).GetName().GetText() : "?", current->second);
        return false;
    }
    if (!take && (current == claimedBy.end() || current->second != player)) return false;  // not its companion
    if (take) {
        // No placeholder mirror: one without a snapshot drove the NPC to the origin (0,0,0), out of processing.
        // The player's first snapshot creates the mirror; until then the host just stops sending it.
        claimedBy[key] = player;
        claimSeen[key] = net::Now();
        companions[key] = net::Remotes()[player].latest.name;
        DropMirror(key);  // an older mirror from another owner
    } else {
        claimedBy.erase(key);
        companions.erase(key);
        claimSeen.erase(key);
        DropMirror(key);  // the host simulates and sends it again; the player stops sending it
    }
    Logf("npcs: player %u %s %s", player, take ? "took over" : "gave back", Resolve(guid).GetInstance() ? Resolve(guid).GetName().GetText() : "?");
    return true;
}

void PlaceGivenBack(uint8_t const[16], float const[3], float const[4])
{
}

void StoryMoved(uint8_t const guid[16], float const position[3], float const rotation[4])
{
    // OnEnclaveSectorRefresh on the host moved this NPC far (infocmd.cpp, kind 1 with moved 2). A mirror here was put back
    // to the host's last snapshot by its body driver (stageSectorRefresh 26.10.: Diego stayed at Ardea on the client, 1751 m
    // from the host's in Braga). The mirror ends; the copy goes where the host's stands, until the host sends it again.
    DropMirror(Key(guid));
    Entity e = Resolve(guid);
    eCEntity* instance = e.GetInstance();
    if (!IsNpc(instance) || e.IsDead()) return;
    e.MoveTo(body::Pose(*instance, position, rotation));
    Logf("npcs: the story moved %s on the host; moved here too", e.GetName().GetText());
}

bool companionWins = true;  // false: the host takes a client's companion for a guide too (as before 27.10.)

bool HostTake(eCEntity* entity)
{
    if (!entity || !net::IsHost()) return false;
    uint8_t guid[16];
    Guid(entity, guid);
    const std::string key = Key(guid);
    // A client's companion stays with its player: taken back here for a guide, its player's snapshots were refused, the claim
    // ran out and the host handed it over again every 15 s (Gorn guiding the host while the client's companion, playtest 27.10.).
    if (companionWins)
        if (const auto c = claimedBy.find(key); c != claimedBy.end() && net::Remotes()[c->second].connected) return false;
    auto it = mirrors.find(key);
    if (it == mirrors.end()) return false;
    const unsigned owner = it->second.owner;
    hostTaken.insert(key);
    if (!it->second.killed) body::Release(it->second.entity, it->second.body);  // its own AI again, here
    if (it->second.guarded) it->second.entity.NPC.DefeatedByPlayer = it->second.defeatedBefore;
    mirrors.erase(it);
    Logf("npcs: took %s from player %u (it guides or follows the host)", Entity(entity).GetName().GetText(), owner);
    return true;
}

void HostRelease(eCEntity* entity)
{
    if (!entity) return;
    uint8_t guid[16];
    Guid(entity, guid);
    hostTaken.erase(Key(guid));
}

bool HostTaken(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return hostTaken.count(Key(guid)) != 0;
}

void Mirrored(std::vector<eCEntity*>& out)
{
    for (auto& [key, m] : mirrors)
        if (m.resolved && !m.missing && !m.killed && m.entity.GetInstance()) out.push_back(m.entity.GetInstance());
}

bool DeadStays() { return deadStays; }

uint8_t CompanionOwner(eCEntity* entity)
{
    if (!entity) return 0;
    if (!net::IsHost()) {
        uint8_t guid[16]; Guid(entity, guid);
        if (auto mirror = mirrors.find(Key(guid)); mirror != mirrors.end()) return mirror->second.latest.partyPlayer;
    }
    if (entity->GetPropertySet(eEPropertySetType_Party)) {
        const uint8_t player = PlayerOf(Entity(entity).Party.GetPartyLeader().GetInstance());
        if (player) return player;
    }
    uint8_t guid[16];
    Guid(entity, guid);
    const std::string key = Key(guid);
    if (const auto it = claimedBy.find(key); it != claimedBy.end()) return it->second;
    if (const auto it = mirrors.find(key); it != mirrors.end()) return it->second.latest.partyPlayer;
    return 0;
}

void SetContext(eCEntity* entity, uint8_t player)
{
    if (!net::IsHost() || !entity || player >= net::MaxPlayers) return;
    uint8_t guid[16];
    Guid(entity, guid);
    if (player) directedBy[Key(guid)] = player;
    else directedBy.erase(Key(guid));
}

uint8_t ContextPlayer(eCEntity* entity)
{
    if (!net::IsHost() || !entity || entity == gCSession::GetInstance().GetHero() ||
        entity == remote::ControlledBody() || remote::IsPuppet(entity) || !entity->GetPropertySet(eEPropertySetType_NPC)) return 0;
    uint8_t guid[16];
    Guid(entity, guid);
    if (auto found = directedBy.find(Key(guid)); found != directedBy.end()) return found->second;
    if (const uint8_t owner = CompanionOwner(entity)) return owner;
    Entity npc(entity);
    eCEntity* target = npc.NPC.GetCurrentTarget().GetInstance();
    if (const uint8_t player = PlayerOf(target)) return player;
    if (target) if (const uint8_t owner = CompanionOwner(target)) return owner;
    eCEntity* hero = remote::ControlledBody();
    float best = 1e20f;
    if (hero) {
        const bCVector delta = entity->GetWorldPosition() - hero->GetWorldPosition();
        best = std::sqrt(delta.GetX() * delta.GetX() + delta.GetY() * delta.GetY() + delta.GetZ() * delta.GetZ());
    }
    uint8_t player = 1;
    for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
        eCEntity* other = remote::PuppetOf(id);
        if (!other || !net::Remotes()[id].connected || !net::InWorld(id)) continue;
        const bCVector delta = other->GetWorldPosition() - entity->GetWorldPosition();
        const float distance = std::sqrt(delta.GetX() * delta.GetX() + delta.GetY() * delta.GetY() + delta.GetZ() * delta.GetZ());
        if (distance < best) { best = distance; player = id; }
    }
    return player;
}

bool Guide(uint8_t const guid[16], uint8_t player)
{
    if (!net::IsHost() || !player || player >= net::MaxPlayers) return false;
    Entity npc(NpcByGuid(guid));
    eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
    if (!npc.GetInstance() || !actor) { Log("npcs: guide request refused: missing NPC or player"); return false; }
    SetContext(npc.GetInstance(), player);
    guards::Acting context(player == net::LocalId() ? nullptr : actor);
    GetScriptAdminExt().CallScript(bCString("StartGuidePlayer"), npc.GetInstance(), actor, 0);
    npc.NPC.SetCurrentTarget(Entity(actor));
    npc.Routine.FullStop();
    npc.Routine.ContinueRoutine();
    return true;
}

bool Follow(uint8_t const guid[16], uint8_t player, uint8_t memberType, bool waiting)
{
    Entity npc(NpcByGuid(guid));
    eCEntity* actor = player == net::LocalId() ? remote::ControlledBody() : remote::PuppetOf(player);
    if (!net::IsHost() || !npc.GetInstance() || !actor || !npc.GetInstance()->GetPropertySet(eEPropertySetType_Party)) {
        Logf("npcs: follower relation for player %u could not be applied", player);
        return false;
    }
    guards::Acting context(actor);
    npc.Party.SetPartyLeader(Entity(actor));
    npc.Party.PartyMemberType = static_cast<gEPartyMemberType>(memberType);
    npc.Party.Waiting = waiting ? GETrue : GEFalse;
    npc.NPC.SetCurrentTarget(Entity(actor));
    npc.Routine.FullStop();
    if (waiting) npc.Routine.ContinueRoutine();
    else npc.Routine.SetTask(bCString("ZS_FollowPlayer"));
    return true;
}

void NewLife(uint8_t const guid[16])
{
    if (net::IsHost()) {
        uint32_t& incarnation = incarnations[Key(guid)];
        if (!incarnation) incarnation = 1;
        if (++incarnation == 0) ++incarnation;
        hostHits.erase(Key(guid));
        forceHistory.erase(Key(guid));
    }
    if (newLife && deadOnHost.erase(Key(guid))) Logf("npcs: %s no longer held dead here (it stood up again)", Resolve(guid).GetName().GetText());
}

bool Claimed(eCEntity const* entity)
{
    if (!entity) return false;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return myClaims.count(Key(guid)) != 0;
}

void OwnOff(LogFn log)
{
    owning = false;
    log("OWNOFF ok=1");
}

uint8_t ClaimedBy(eCEntity const* entity)
{
    if (!entity || !net::IsHost()) return 0;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    const auto it = claimedBy.find(Key(guid));
    return it == claimedBy.end() ? 0 : it->second;
}

uint8_t RemoteOwner(uint8_t const guid[16])
{
    if (net::IsHost()) return 0;
    return npcplayback::IsPassive(NpcByGuid(guid)) ? uint8_t(1) : uint8_t(0);
}

uint8_t OwnerOf(eCEntity const* entity)
{
    if (net::IsHost()) return 0;
    if (npcplayback::IsPassive(entity)) return 1;
    if (!entity) return 0;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(entity), guid);
    return RemoteOwner(guid);
}

bool PaysDeathWithoutKiller(eCEntity const* victim)
{
    if (npcplayback::IsPassive(victim)) return false;
    if (net::IsHost() || !farXpGuard || !victim || IsMirrored(victim) || Claimed(victim)) return true;
    uint8_t guid[16];
    Guid(const_cast<eCEntity*>(victim), guid);
    if (owned.count(Key(guid))) return true;
    ++farXpRefused;
    return false;
}

void FarXpOff(LogFn log)
{
    farXpGuard = false;
    log("FARXPOFF ok=1");
}

void XpGuardOff(LogFn log)
{
    xpGuard = false;
    log("XPGUARDOFF ok=1");
}

void Report(LogFn log)
{
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) return;
    const bCVector& h = hero->GetWorldPosition();
    const float heroAt[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const bTObjArray<Entity> all = Entity::GetNPCs();
    char line[400];
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == hero || remote::IsPuppet(instance)) continue;
        const bCVector& p = instance->GetWorldPosition();
        const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
        if (body::Distance(at, heroAt) > InterestRadius) continue;
        uint8_t guid[16];
        Guid(instance, guid);
        char text[48];
        guid::Format(guid, text, sizeof(text));
        auto mirror = mirrors.find(Key(guid));
        Format(line, "NPC guid=%s name=%s pos=%.1f,%.1f,%.1f hp=%d dead=%d mirrored=%d task=%s repacified=%u last_foreign=%s owner=%u", text,
                  npc.GetName().GetText(), at[0], at[1], at[2], static_cast<int>(npc.DamageReceiver.HitPoints), npc.IsDead(),
                  IsMirrored(instance), npc.Routine.GetCurrentTask().GetText(),
                  mirror != mirrors.end() ? mirror->second.body.repacified : 0,
                  mirror != mirrors.end() && *mirror->second.body.lastForeign ? mirror->second.body.lastForeign : "-",
                  OwnerOf(instance));
        log(line);
    }
    Format(line, "NPCSYNC claims_refused=%u host_wins=%u host_wins_rule=%d join_refused=%u join_grace=%d contested_refused=%u contested=%zu contest_rule=%d reclaims=%u reclaim_refused=%u reclaimed=%zu reclaim_rule=%d",
              claimsRefused, hostWins, hostWinsRule ? 1 : 0, joinRefused, joinGraceRule ? 1 : 0, contestedRefused, contested.size(), contestRule ? 1 : 0,
              reclaims, reclaimRefused, reclaimed.size(), reclaimRule ? 1 : 0);
    log(line);
}

// QA (host, stage EdgeClaim): the NPCs this machine processes in its band, between InterestRadius and KeepRadius of the
// nearest player, with why its last snapshots left them out.
void EdgeReport(LogFn log)
{
    const bTObjArray<Entity> all = Entity::GetNPCs();
    char line[300];
    for (int i = 0; i < all.GetCount(); ++i) {
        Entity npc = all.GetAt(i);
        eCEntity* instance = npc.GetInstance();
        if (!instance || instance == gCSession::GetInstance().GetHero() || remote::IsPuppet(instance) || npc.IsDead()) continue;
        const bCVector& p = instance->GetWorldPosition();
        const float at[3] = {p.GetX(), p.GetY(), p.GetZ()};
        float nearest = 1e30f;
        for (int k = 0; k < lastPlayerCount; ++k) nearest = std::min(nearest, body::Distance(at, lastPlayers[k]));
        if (nearest <= InterestRadius || nearest > KeepRadius) continue;
        uint8_t guid[16];
        Guid(instance, guid);
        char text[48];
        guid::Format(guid, text, sizeof(text));
        const auto note = sendNotes.find(Key(guid));
        Format(line, "EDGENPC guid=%s name=%s nearest_m=%.1f reason=%s age_ms=%d mirror=%d", text, npc.GetName().GetText(), nearest / 100.0f,
                  note != sendNotes.end() ? note->second.reason : "never", note != sendNotes.end() ? static_cast<int>(net::Now() - note->second.at) : -1,
                  mirrors.count(Key(guid)) ? 1 : 0);
        log(line);
    }
    log("EDGENPC end");
}

void SendInfo(char const* name, LogFn log)
{
    // A GUID too (stage EdgeClaim: scavengers share one name).
    Entity npc;
    uint8_t bytes[16];
    if (guid::ParseExact(name, bytes)) npc = guid::Resolve(bytes);
    else npc = Entity::GetEntity(bCString(name));
    char line[300];
    if (!npc.GetInstance()) { log("REFUSED npcsend: no such NPC"); return; }
    uint8_t guid[16];
    Guid(npc.GetInstance(), guid);
    const std::string key = Key(guid);
    const auto note = sendNotes.find(key);
    const auto back = givenBack.find(key);
    char text[48];
    guid::Format(guid, text, sizeof(text));
    Format(line, "NPCSEND name=%s guid=%s reason=%s age_ms=%d myclaim=%d mirror=%d givenback_ms=%d", name, text,
              note != sendNotes.end() ? note->second.reason : "never",
              note != sendNotes.end() ? static_cast<int>(net::Now() - note->second.at) : -1, myClaims.count(key) ? 1 : 0,
              mirrors.count(key) ? 1 : 0, back != givenBack.end() ? static_cast<int>(back->second - net::Now()) : 0);
    log(line);
}

void LifeReport(char const* text, LogFn log)
{
    uint8_t guid[16];
    if (!guid::ParseExact(text, guid)) { log("REFUSED npclife: expected an NPC GUID"); return; }
    const std::string key = Key(guid);
    net::NpcRecord const* record = nullptr;
    Mirror const* mirror = nullptr;
    uint32_t shown = 0, displayed = 0;
    bool ready = false;
    if (net::IsHost()) {
        const auto found = lastSent.find(key);
        if (found != lastSent.end()) {
            record = &found->second.record;
            shown = record->incarnation;
            displayed = record->sent;
            ready = record->presentationReady != 0;
        }
    } else {
        const auto found = mirrors.find(key);
        if (found != mirrors.end()) {
            mirror = &found->second;
            record = &found->second.latest;
            shown = found->second.replay.incarnation;
            displayed = found->second.shownSent;
            ready = found->second.replayReady;
        } else if (const auto absent = absences.find(key); absent != absences.end()) {
            record = &absent->second;
            shown = record->incarnation;
        }
    }
    if (!record) { log("NPCLIFE known=0"); return; }
    char line[384], visible[96] = "-";
    if (eCEntity* entity = NpcByGuid(guid)) body::CurrentMotion(entity, visible, sizeof(visible));
    Format(line, "NPCLIFE known=1 incarnation=%u shown=%u absent=%u ready=%d hp=%d hp_max=%d dead=%d captured=%u displayed=%u",
           record->incarnation, shown, record->absent, ready ? 1 : 0, record->hitPoints,
           record->hitPointsMax, (record->flags & 1) ? 1 : 0, record->sent, displayed);
    log(line);
    if (mirror) {
        auto* entity = mirror->entity.GetInstance();
        auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
        float error = -1;
        if (entity && animation && !animation->GetRagDollEnabled() && !mirror->corpsePose.empty()) {
            std::vector<pose::Bone> current;
            uint8_t flags = 0;
            if (npcplayback::CapturePose(entity, current, flags, nullptr)) {
                error = 0;
                for (auto const& expected : mirror->corpsePose) {
                    const auto found = std::find_if(current.begin(), current.end(),
                        [&](pose::Bone const& bone) { return bone.name == expected.name; });
                    if (found == current.end()) { error = -1; break; }
                    const auto a = pose::Decode(expected), b = pose::Decode(*found);
                    float squared = 0;
                    for (int i = 9; i < 12; ++i) squared += (a.values[i] - b.values[i]) * (a.values[i] - b.values[i]);
                    error = std::max(error, std::sqrt(squared));
                }
            }
        }
        Format(line, "NPCCORPSE initialized=%d starts=%u wakes=%u simulating=%d bones=%u incarnation=%u pose_error_cm=%.3f",
               mirror->ragdollInitialized ? 1 : 0, mirror->ragdollStarts, mirror->ragdollWakes,
               animation && animation->GetRagDollEnabled() ? 1 : 0, static_cast<unsigned>(mirror->corpsePose.size()),
               mirror->deathLife.incarnation, error);
        log(line);
    }
    Format(line, "NPCMOTION active=%u phase=%.4f locomotion=%d motion=%s", record->motions[0].active,
           record->motions[0].phase, body::IsLocomotionMotion(visible) ? 1 : 0, visible);
    log(line);
}

void CorpseSleep(char const* text, LogFn log)
{
    uint8_t guid[16];
    if (net::IsHost() || !guid::ParseExact(text, guid)) { log("REFUSED corpsesleep: expected a client corpse GUID"); return; }
    const auto found = mirrors.find(Key(guid));
    if (found == mirrors.end() || !found->second.ragdollInitialized || found->second.corpsePose.empty()) {
        log("REFUSED corpsesleep: corpse has not initialized and cached its pose");
        return;
    }
    auto* entity = found->second.entity.GetInstance();
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    if (!animation) { log("REFUSED corpsesleep: animation is unavailable"); return; }
    animation->SetRagDollEnabled(GEFalse, 0.0f, GETrue);
    log("CORPSESLEEP stopped=1");
}

void JoinGraceOff(LogFn log)
{
    joinGraceRule = false;
    log("JOINGRACEOFF ok=1");
}

void ContestOff(LogFn log)
{
    contestRule = false;
    log("CONTESTOFF ok=1");
}

void HostWinsOff(LogFn log)
{
    hostWinsRule = false;
    log("HOSTWINSOFF ok=1");
}
}

namespace g3t::npcs {
// Client: the host hands back a companion from its save. Recruit it again here exactly as the dialog command
// JoinPlayer true does (Script_Game.dll 0xB7F10: stop, SlaveryEnabled, the script JoinPlayerSlavery, which sets leader =
// the local hero, not waiting, type Slave; then ZS_FollowPlayer). The game's own script rather than the fields by hand:
// whether the leader's member list needs more than SetPartyLeader is unknown (static analysis, re/findings/party-members.md).
void Adopt(uint8_t const guid[16])
{
    Entity e = Resolve(guid);
    if (!IsNpc(e.GetInstance())) return;
    Claim(e.GetInstance());
    if (guide::Readopt(e.GetInstance())) { ++companionsAdopted; return; }  // a guide of a running follow quest (P9)
    if (arena::Readopt(e.GetInstance())) return;  // an arena opponent taken over for a fight: the host's again
    e.Routine.FullStop();
    e.Dialog.SlaveryEnabled = GETrue;
    GetScriptAdminExt().CallScript(bCString("JoinPlayerSlavery"), e.GetInstance(), nullptr, 0);
    e.Routine.SetTask(bCString("ZS_FollowPlayer"));
    ++companionsAdopted;
    Logf("npcs: %s is our companion again (from the host's save)", e.GetName().GetText());
}

unsigned WriteCompanions(FILE* file)
{
    unsigned n = 0;
    for (auto const& [key, name] : companions) {
        fprintf(file, "companion ");
        for (unsigned char c : key) fprintf(file, "%02x", c);
        fprintf(file, " %s\n", name.c_str());
        ++n;
    }
    return n;
}

unsigned ReadCompanions(FILE* file)
{
    companions.clear();
    offered.clear();
    char line[256];
    unsigned n = 0;
    while (fgets(line, sizeof(line), file)) {
        // The player's name is the rest of the line (names may contain spaces).
        char hex[33] = {};
        int consumed = 0;
        if (sscanf_s(line, "companion %32s %n", hex, 33u, &consumed) != 1 || !consumed) continue;
        std::string name = line + consumed;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name.empty()) continue;
        std::string key(16, '\0');
        for (int i = 0; i < 16; ++i) {
            unsigned v = 0;
            sscanf_s(hex + 2 * i, "%2x", &v);
            key[i] = static_cast<char>(v);
        }
        companions[key] = name;
        ++n;
    }
    return n;
}

void ClearCompanions()
{
    companions.clear();
    offered.clear();
}
}

namespace g3t::npcs {
void CompanionsOff(LogFn log)
{
    offering = false;
    log("COMPANIONSOFF ok=1");
}
}

namespace g3t::npcs {
void StyleOff(LogFn log)
{
    style = false;
    log("STYLEOFF ok=1");
}

void MirrorRetryOff(LogFn log)
{
    retryMissing = false;
    log("MIRRORRETRYOFF ok=1 (a mirror missing at its first lookup stays missing and stays, as before 23.10.)");
}

void MirrorStats(LogFn log)
{
    unsigned missing = 0;
    for (auto const& entry : mirrors)
        if (entry.second.resolved && (entry.second.missing || !entry.second.entity.GetInstance())) ++missing;
    char line[200];
    Format(line, "MIRRORSTATS mirrors=%zu missing=%u found_later=%u expired=%u lookups=%u retry=%d", mirrors.size(), missing,
              mirrorsFoundLater, mirrorsExpired, mirrorLookups, retryMissing ? 1 : 0);
    log(line);
}

void SyncReport(LogFn log)
{
    char line[400];
    unsigned accepted, rejected, stale;
    net::NpcReplicationCounts(accepted, rejected, stale);
    Format(line, "SYNCNPC role=%s simulated=%zu mirrors=%zu takebacks=%u awaiting_drive=%u awaiting_alignment=%u "
                 "stale_releases=%u ownership_refused=%u host_wins=%u accepted=%u rejected=%u out_of_order=%u "
                 "capture_ticks=%u capture_missed=%u capture_ms=%u",
           net::IsHost() ? "host" : "client", owned.size(), mirrors.size(), reclaims, reclaimAwaitingDrive,
           reclaimAwaitingAlignment, staleMirrorReleases, claimsRefused, hostWins, accepted, rejected, stale,
           captureTicks, sendCadence.missed, captureStartedAt ? net::Now() - captureStartedAt : 0);
    log(line);
    unsigned posesSent, posesReceived, posesFailed;
    net::NpcPoseCounts(posesSent, posesReceived, posesFailed);
    Format(line, "NPCPOSE sent=%u received=%u failed=%u", posesSent, posesReceived, posesFailed);
    log(line);
    Format(line, "SYNCCONTACT swing_frames=%u sweep_tests=%u overlaps=%u obstructed=%u hits=%u incoming_windows=%u "
                 "incoming_missing=%u incoming_tests=%u incoming_overlaps=%u incoming_walls=%u incoming_hits=%u",
           playerSwingFrames, playerSweepTests, playerOverlaps, playerObstructed, playerContactHits,
           localContactWindows, localContactMissing, localContactTests, localContactOverlaps, localContactWalls, localBlowsJudged);
    log(line);
    unsigned tracked = 0, delaySum = 0, delayMax = 0;
    for (auto const& entry : mirrors) {
        if (!entry.second.body.track.count) continue;
        const unsigned d = static_cast<unsigned>(body::PlaybackDelay(entry.second.body.track));
        ++tracked, delaySum += d, delayMax = std::max(delayMax, d);
    }
    Format(line, "SYNCFRAMES frames=%u avg_ms=%.1f over33=%u over66=%u max_ms=%u tracked=%u delay_avg=%u delay_max=%u local_blows=%u out_of_reach=%u",
           frameCount, frameCount ? static_cast<float>(frameSumMs) / frameCount : 0.0f, slowFrames, hitchFrames, maxFrameMs, tracked,
           tracked ? delaySum / tracked : 0u, delayMax, localBlowsJudged, localBlowsOutOfReach);
    log(line);
    Format(line, "SYNCRAGDOLL hooked=%d forces_sent=%u forces_received=%u forces_applied=%u forces_late=%u ragdolls_started=%u not_ready=%u",
           originalAddForce ? 1 : 0, ragdollForcesSent, ragdollForcesReceived, ragdollForcesApplied, ragdollForcesLate, ragdollsStarted, ragdollsNotReady);
    log(line);
}
}

namespace g3t::npcs {
unsigned HoldChanges() { return holdChanges; }
void RecvOn(LogFn log)
{
    receiving = true;
    log("NPCRECVON ok=1");
}

void RecvOff(LogFn log)
{
    receiving = false;
    log("NPCRECVOFF ok=1");
}
}

namespace g3t::npcs {
void DeadPoseOff(LogFn log)
{
    deadPose = false;
    log("DEADPOSEOFF ok=1");
}

void DeadPoseReport(LogFn log)
{
    char line[700];
    // Per mirror: the motion on slot 0, whether it runs, and the height of the animated body (lying: low).
    unsigned alive = 0;
    auto report = [&](Mirror& m, char const* kind) {
        char shows[128], slots[400];
        Lies(m.entity.GetInstance(), shows, sizeof(shows), slots, sizeof(slots));
        auto* animation = static_cast<eCVisualAnimation_PS*>(m.entity.GetInstance()->GetPropertySet(eEPropertySetType_Animation));
        bool processed = false;
        if (auto* entities = world::ProcessedEntities())
            for (GEU16 i = 0; i < entities->GetCount(); ++i)
                if (entities->GetAt(i) == m.entity.GetInstance()) { processed = true; break; }
        Format(line, "DEADPOSE mirror=%s killed=%d running=%d height=%.0f ani=%d motion=%s slots=%s kind=%s ragdoll=%d processed=%d task=%s",
                  m.entity.GetName().GetText(),
                  m.killed ? 1 : 0, body::MotionRunning(m.entity.GetInstance()) ? 1 : 0, body::BoundaryHeight(m.entity.GetInstance()),
                  static_cast<int>(m.entity.Routine.AniState), *shows ? shows : "-", slots, kind,
                  animation && animation->GetRagDollEnabled() ? 1 : 0, processed ? 1 : 0, m.entity.Routine.GetCurrentTask().GetText());
        log(line);
    };
    for (auto& entry : mirrors) {
        Mirror& m = entry.second;
        if (!m.entity.GetInstance() || (!m.killed && alive++ >= 3)) continue;
        report(m, "mirror");
    }
    for (auto& entry : corpses) {  // the "mirror=" key stays: the suites parse it
        Mirror& c = entry.second;
        c.entity = Resolve(c.latest.guid);
        if (c.entity.GetInstance()) report(c, "journal");
    }
    Format(line, "DEADPOSE enabled=%d laid=%u loops=%u slots_stopped=%u ragdolls=%u ragdolls_not_ready=%u", deadPose ? 1 : 0, corpsesLaid, corpseLoops, slotsStopped,
              ragdollsStarted, ragdollsNotReady);
    log(line);
}
// QA negative controls of the play-evening fixes (27.10.): each switches its fix off (on: clientfameon).
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "localhitsoff")) { localHits = false; log("SWITCH localhitsoff ok=1"); return true; }
    if (!std::strcmp(action, "nativehitsoff")) { nativeHits = false; log("SWITCH nativehitsoff ok=1"); return true; }
    if (!std::strcmp(action, "fastnpcsoff")) { fastNpcs = false; log("SWITCH fastnpcsoff ok=1"); return true; }
    if (!std::strcmp(action, "deadstaysoff")) { deadStays = false; log("SWITCH deadstaysoff ok=1"); return true; }
    if (!std::strcmp(action, "deadnewlifeoff")) { newLife = false; log("SWITCH deadnewlifeoff ok=1"); return true; }
    if (!std::strcmp(action, "companionnearoff")) { companionNear = false; log("SWITCH companionnearoff ok=1"); return true; }
    if (!std::strcmp(action, "hysteresisoff")) { hysteresis = false; log("SWITCH hysteresisoff ok=1"); return true; }
    if (!std::strcmp(action, "companionwinsoff")) { companionWins = false; log("SWITCH companionwinsoff ok=1"); return true; }
    if (!std::strcmp(action, "reclaimoff")) { reclaimRule = false; log("SWITCH reclaimoff ok=1"); return true; }
    if (!std::strcmp(action, "lagblockoff")) { lagBlocks = false; log("SWITCH lagblockoff ok=1"); return true; }
    if (!std::strcmp(action, "lagblockon")) { lagBlocks = true; log("SWITCH lagblockon ok=1"); return true; }
    return false;
}
}
