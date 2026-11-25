// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "body.h"
#include "config.h"
#include "guid.h"
#include "guid_key.h"
#include "net.h"
#include "npc_equipment_policy.h"
#include "npcidentity_policy.h"
#include "npc_item_quality_policy.h"
#include "npcpresence_policy.h"
#include "npcplayback.h"
#include "npcs.h"
#include "npcsend_policy.h"
#include "npcstream_diag.h"
#include "player_identity.h"
#include "pose.h"
#include "ragdoll_force.h"
#include "remote.h"
#include "replication.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::npcs::detail {

constexpr float InterestRadius = 4000.0f;
constexpr uint32_t StaleAfter = npcsend::MirrorStaleMs;
constexpr uint32_t JoinGrace = 10000;
constexpr float KeepRadius = InterestRadius + 2000.0f;
constexpr int StateHistory = 128;
constexpr uint32_t SentStatesFor = 1500;
constexpr uint32_t OfferEvery = 20000;
constexpr uint32_t RetryLookup = 1000;
constexpr uint32_t MinHold = 10000;

using NpcOpponents = std::unordered_map<uint32_t, eCEntity*>;

struct LastSent {
    net::NpcRecord record{};
    uint32_t at = 0, movingUntil = 0;
    std::vector<pose::Bone> bones;
    uint8_t poseFlags = 0;
    bool posed = false;
};

struct AwaitedHit {
    uint32_t sequence = 0, incarnation = 0, swing = 0;
};

struct QuarantinedEntity {
    npcidentity::Guid guid{};
    npcidentity::Guid rootGuid{};
    eCEntity const* root = nullptr;
    bool rendering = false, picking = false, collision = false, processingDisabled = false;
};

struct HostHitNote {
    uint8_t player = 0;
    uint32_t sequence = 0;
    uint32_t at = 0;
};

struct ForceSeqKey {
    GuidKey guid{};
    uint32_t incarnation = 0;
    bool operator==(ForceSeqKey const& other) const noexcept
    {
        return guid == other.guid && incarnation == other.incarnation;
    }
};

struct ForceSeqHash {
    size_t operator()(ForceSeqKey const& key) const noexcept
    {
        return GuidHash{}(key.guid) ^ (static_cast<size_t>(key.incarnation) * 2654435761u);
    }
};

struct SentStates {
    uint32_t at[StateHistory] = {};
    bool parrying[StateHistory] = {};
    int next = 0;
};

struct SendNote {
    char const* reason = "never";
    uint32_t at = 0;
};

struct Held {
    uint32_t since = 0, last = 0;
};

struct PlayerContact {
    uint32_t instance = 0, at = 0;
    float a[3] = {}, b[3] = {}, previousA[3] = {}, previousB[3] = {}, thickness = 0;
    int bone = -1;
    bool ready = false;
};

struct TargetingWatchState {
    eCEntity* body = nullptr;
    int weaponMode = -1, aiMode = -1;
    uint32_t combatState = 0, loggedAt = 0, killedAt = 0, serial = 0;
    bool haveKill = false;
    uint8_t victim[16] = {};
};

struct Mirror {
    Entity entity;
    bool resolved = false;
    bool missing = false;
    uint32_t resolvedAt = 0;
    uint32_t firstSeen = 0;
    bool killed = false;
    uint8_t owner = 1;
    uint32_t ownerGeneration = 0, hostSession = 0;
    bool guarded = false;
    bool defeatedBefore = false;
    bool inClientTalk = false;
    body::State body;
    npcplayback::State replay;
    bool replayReady = false;
    uint32_t shownCombatTarget = 0;
    uint32_t unresolvedCombatTarget = 0;
    uint32_t contactInstance = 0, contactAt = 0;
    bool contactConsumed = false;
    int contactBone = -1;
    float contactA[3] = {}, contactB[3] = {};
    static constexpr int MaxSwingArcSamples = 8;
    float swingArcA[MaxSwingArcSamples][3] = {}, swingArcB[MaxSwingArcSamples][3] = {};
    uint8_t swingArcCount = 0;
    uint8_t swingTests = 0;
    bool swingLogged = true;
    bool lastClockProtected = false;
    float swingMaxPhase = 0.0f;
    float swingClosestGapCm = 1e9f;
    char const* swingMissReason = "phase_below_hit_threshold";
    char swingBlocker[48] = {};
    float swingRayCm = 0.0f, swingHitCm = 0.0f;
    bool partyApplied = false, appliedWaiting = false;
    bool waitingPending = false, pendingWaitingValue = false;
    uint8_t appliedOwner = 0;
    bool relationAcknowledged = false, guideAcknowledged = false, guideDestinationApplied = false;
    net::NpcRecord previous;
    net::NpcRecord latest;
    uint32_t previousTicks = 0;
    uint32_t latestTicks = 0;
    uint32_t records = 0;
    uint32_t previousSent = 0, latestSent = 0;
    uint32_t heldRight = 0, heldLeft = 0;
    npc_equipment::BackEquipment backEquipment{};
    uint32_t holdChecked = 0;
    char skeleton[24] = {};
    bool laid = false;
    bool ragdollStarted = false;
    bool ragdollInitialized = false, corpsePlaced = false, corpseSimulating = false;
    bool ragdollPaused = false, corpseSettled = false;
    float lastGroundPos[3] = {};
    uint32_t lastGroundIncarnation = 0;
    bool haveLastGroundPos = false;
    replication::TerminalLife deathLife;
    std::vector<pose::Bone> corpsePose;
    uint8_t corpsePoseFlags = 0;
    bCMatrix corpseRoot;
    void const* corpseActor = nullptr;
    uint32_t corpsePoseAt = 0;
    unsigned ragdollStarts = 0, ragdollWakes = 0;
    uint32_t ragdollTryAt = 0;
    unsigned ragdollFails = 0;
    uint32_t lieCheckAt = 0;
    unsigned lieTries = 0;
    uint8_t lieNoted = 0;
    uint32_t killedAt = 0;
    bool lateNoted = false;
    bool released = false, staleHeld = false;
    uint32_t shownSent = 0, truthSent = 0;
    bool havePresentation = false;
    uint8_t attitudeWritten = 0xFF;
    uint8_t attitudeShown = 0xFF;
    unsigned attitudeDrifts = 0;
    eCEntity* deathReferencesCleared = nullptr;
    unsigned deathClearCount = 0, deathCombatStopCount = 0;
    bool deathLogged = false;
    char killerAttribution[32] = "host_or_npc_unknown";
    char presentationSource[16] = "gravity";
    ragdoll::DeathImpulse deathImpulse;
    uint32_t ragdollTraceAt = 0, ragdollTraceSampleAt = 0;
    unsigned ragdollTraceStage = 0;
    float ragdollTracePosition[3] = {};
    bool localHitValid = false;
    uint32_t localHitAt = 0;
    uint32_t fallbackGraceUntil = 0;
    uint32_t focusWatchUntil = 0;
    bool focusWatchLogged = false;
    eCEntity* watchedFocus = nullptr;
    unsigned focusWatchChanges = 0;
};

struct HiddenNodeState {
    npcidentity::Guid guid{};
    bool rendering = true, picking = true, collision = true;
};

struct HiddenClientState {
    bool active = false;
    HiddenNodeState root{};
    std::vector<HiddenNodeState> children;
};

inline bool fastNpcs = true;
inline std::unordered_map<GuidKey, LastSent, GuidHash> lastSent;
inline std::unordered_map<GuidKey, AwaitedHit, GuidHash> awaitedHits;
inline std::unordered_map<eCEntity const*, QuarantinedEntity> quarantinedEntities;
inline std::unordered_map<eCEntity const*, HiddenClientState> hiddenClientStates;
inline uint32_t identityCensusAt = 0, identityLogAt = 0;
inline std::unordered_map<GuidKey, npcsend::State, GuidHash> recipientNpcs[net::MaxPlayers];
inline uint32_t recipientGenerations[net::MaxPlayers] = {};
inline uint32_t syncedWorldGen[net::MaxPlayers] = {};
inline uint32_t syncedWorldId[net::MaxPlayers] = {};
inline npcsend::InitialPacer initialPacers[net::MaxPlayers];
inline npcsend::Stats sendStats;
inline uint32_t lastSend = 0;
inline int lastXp = -1;
inline uint64_t lastIdentitySignature = 0;
inline replication::Cadence60 sendCadence;
inline uint32_t captureTicks = 0, captureStartedAt = 0;
inline std::unordered_map<GuidKey, uint32_t, GuidHash> incarnations;
inline std::unordered_map<GuidKey, HostHitNote, GuidHash> hostHits;
inline std::unordered_map<GuidKey, std::vector<net::RagdollNote>, GuidHash> forceHistory, pendingForces;
inline std::unordered_map<ForceSeqKey, replication::Sequence, ForceSeqHash> forceSequences;
inline uint32_t forceSequence = 0;
inline uint32_t knownJoins = 0;
inline std::unordered_set<uint32_t> namesSent;
inline std::unordered_set<GuidKey, GuidHash> owned;
inline bool owning = true;
inline unsigned claimsRefused = 0;
inline std::unordered_set<GuidKey, GuidHash> deadOnHost;
inline bool deadStays = true;
inline bool newLife = true;
inline unsigned deadRefused = 0, deadKept = 0;
inline unsigned hostWins = 0;
inline bool hostWinsRule = true;
inline uint32_t joinedAt[net::MaxPlayers] = {};
inline unsigned joinRefused = 0;
inline bool joinGraceRule = true;
inline std::unordered_set<GuidKey, GuidHash> contested;
inline unsigned contestedRefused = 0;
inline bool contestRule = true;
inline float lastPlayers[net::MaxPlayers][3] = {};
inline int lastPlayerCount = 0;
inline std::unordered_map<GuidKey, uint8_t, GuidHash> claimedBy;
inline std::unordered_map<GuidKey, eCEntity*, GuidHash> claimActors;
inline std::unordered_map<GuidKey, uint8_t, GuidHash> directedBy;
inline std::unordered_set<GuidKey, GuidHash> myClaims;
inline std::unordered_map<GuidKey, uint32_t, GuidHash> claimSeen;
inline std::unordered_map<GuidKey, uint32_t, GuidHash> givenBack;
inline std::unordered_set<GuidKey, GuidHash> hostTaken;
inline std::unordered_set<GuidKey, GuidHash> reclaimed;
inline unsigned reclaims = 0, reclaimRefused = 0;
inline unsigned reclaimAwaitingDrive = 0, reclaimAwaitingAlignment = 0, staleMirrorReleases = 0;
inline bool reclaimRule = true;
inline std::unordered_map<GuidKey, SentStates, GuidHash> sentStates;
inline bool lagBlocks = true;
inline unsigned swingSamples = 0, swingArmed = 0;
inline unsigned fightSent = 0, fightSeenAny = 0, fightSeenUs = 0, fightSeenNear = 0;
inline unsigned claimsDenied = 0, claimsRevoked = 0;
inline identity::Companions companionIdentity;
inline std::set<std::string> identityWarnings;
inline std::unordered_map<std::string, uint32_t> offered;
inline bool offering = true;
inline bool companionNear = true;
inline bool companionWins = true;
inline unsigned companionsFar = 0, companionsOffered = 0, companionsAdopted = 0;
inline unsigned holdChanges = 0;
inline bool style = true;
inline bool retryMissing = true;
inline unsigned mirrorsFoundLater = 0, mirrorsExpired = 0, mirrorLookups = 0;
inline std::unordered_map<GuidKey, Mirror, GuidHash> mirrors;
inline std::unordered_map<GuidKey, Mirror, GuidHash> corpses;
inline std::unordered_map<GuidKey, npc_equipment::BackEquipment, GuidHash> pendingBackEquipment;
inline std::unordered_map<GuidKey, npc_equipment::BackEquipment, GuidHash> sentBackEquipment[net::MaxPlayers];
inline std::unordered_map<GuidKey, net::NpcRecord, GuidHash> absences;
inline uint32_t absenceCheckAt = 0;
inline std::unordered_map<uint32_t, std::string> motionNames;
inline std::unordered_map<GuidKey, SendNote, GuidHash> sendNotes;
inline npcstream_diag::Window streamDiag;
inline std::unordered_map<GuidKey, Held, GuidHash> sending;
inline bool hysteresis = true;
inline unsigned heldBand = 0;
inline bool receiving = true;
inline bool hostStreamSeen = false;
inline uint32_t hostStreamSession = 0, hostStreamFirstSeenAt = 0;
inline bool xpGuard = true, farXpGuard = true, deadPose = true;
inline unsigned farXpRefused = 0;
inline unsigned corpsesLaid = 0, corpseLoops = 0, slotsStopped = 0, ragdollsStarted = 0, ragdollsNotReady = 0;
inline uint32_t corpseRoiUpdateAt = 0;
inline bool nativeHits = true, localHits = true, fallbackStumbles = true;
inline unsigned localHitsNative = 0, localHitsNativeEnded = 0, localHitsShown = 0, localHitsUnknown = 0;
inline std::unordered_map<std::string, std::string> stumbleFor;
inline unsigned ragdollForcesSent = 0, ragdollForcesReceived = 0, ragdollForcesApplied = 0, ragdollForcesLate = 0;
inline unsigned ragdollGravityDeaths = 0;
inline bool ragdollHookInstalled = false;
inline unsigned localBlowsJudged = 0, localBlowsOutOfReach = 0;
inline unsigned localContactWindows = 0, localContactMissing = 0, localContactTests = 0, localContactOverlaps = 0, localContactWalls = 0;
inline unsigned frameCount = 0, slowFrames = 0, hitchFrames = 0;
inline uint32_t frameSumMs = 0, maxFrameMs = 0, lastFrameAt = 0;
inline PlayerContact playerContact;
inline unsigned playerSwingFrames = 0, playerSweepTests = 0, playerOverlaps = 0, playerObstructed = 0, playerContactHits = 0;
inline TargetingWatchState targetingWatch;

inline GuidKey Key(uint8_t const guid[16]) noexcept { return ToGuidKey(guid); }
inline std::string CompanionKey(GuidKey const& key) { return std::string(reinterpret_cast<char const*>(key.data()), 16); }
inline std::string CompanionKey(uint8_t const guid[16]) { return std::string(reinterpret_cast<char const*>(guid), 16); }
inline GuidKey FromCompanionKey(std::string const& s) noexcept
{
    return s.size() >= 16 ? ToGuidKey(reinterpret_cast<uint8_t const*>(s.data())) : GuidKey{};
}
inline void Guid(eCEntity* entity, uint8_t out[16]) { guid::CopyEntity(entity, out); }
inline Entity Resolve(uint8_t const guid[16]) { return guid::Resolve(guid); }
inline Entity Resolve(GuidKey const& key) { return guid::Resolve(key.data()); }

inline uint32_t HeldHash(Entity& npc, gESlot slot)
{
    const Entity held = npc.Inventory.GetItemFromSlot(slot);
    return held.GetInstance() ? net::HashName(held.GetName().GetText()) : 0;
}

inline GEInt StackOf(Entity& npc, uint32_t hash)
{
    if (!hash) return -1;
    eCEntity* instance = npc.GetInstance();
    auto* inv = instance ? static_cast<gCInventory_PS*>(instance->GetPropertySet(eEPropertySetType_Inventory)) : nullptr;
    const GEInt count = npc.Inventory.GetStackCount();
    for (GEInt i = 0; i < count; ++i) {
        auto const* stack = inv ? inv->GetStack(i) : nullptr;
        const bool merchandise = npc.Inventory.IsStackMerchandise(i) != GEFalse ||
            (stack && stack->GetType() == gEStackType_Merchandise);
        if (!npc_equipment::IsMirrorLootStackEligible(merchandise, npc.Inventory.GetAmount(i))) continue;
        if (Entity item = npc.Inventory.GetTemplateItem(i);
            item.GetInstance() && net::HashName(item.GetName().GetText()) == hash)
            return i;
    }
    return -1;
}

static_assert(npc_equipment::BackSlotIndices[0] == static_cast<int>(gESlot_BackLeft));
static_assert(npc_equipment::BackSlotIndices[1] == static_cast<int>(gESlot_BackRight));
static_assert(npc_equipment::BackSlotIndices[2] == static_cast<int>(gESlot_Bow));
static_assert(npc_equipment::BackSlotIndices[3] == static_cast<int>(gESlot_Crossbow));

static_assert(npc_item_quality::QualityWorn == static_cast<uint32_t>(gEItemQuality_Worn));
static_assert(npc_item_quality::CategoryWeapon == static_cast<int>(gEItemCategory_Weapon));
static_assert(npc_item_quality::UseArrow == static_cast<int>(gEUseType_Arrow));
static_assert(npc_item_quality::UseBolt == static_cast<int>(gEUseType_Bolt));
static_assert(npc_item_quality::UseFist == static_cast<int>(gEUseType_Fist));
static_assert(npc_item_quality::UseShield == static_cast<int>(gEUseType_Shield));
static_assert(npc_item_quality::UsePhysicalFist == static_cast<int>(gEUseType_PhysicalFist));

inline uint32_t MirroredItemQuality(Template const& item)
{
    eCTemplateEntityProxy proxy;
    proxy.SetEntity(static_cast<bCPropertyID const&>(item));
    eCTemplateEntity* templateEntity = proxy.GetEntity();
    if (!templateEntity) return 0;
    auto* itemPs = static_cast<gCItem_PS*>(templateEntity->GetPropertySet(eEPropertySetType_Item));
    auto* interactionPs =
        static_cast<gCInteraction_PS*>(templateEntity->GetPropertySet(eEPropertySetType_Interaction));
    const int category = itemPs ? static_cast<int>(itemPs->GetCategory()) : -1;
    const int useType = interactionPs ? static_cast<int>(interactionPs->GetUseType()) : -1;
    const uint32_t baseQuality = itemPs ? static_cast<uint32_t>(itemPs->GetQuality()) : 0u;
    return npc_item_quality::MirroredSpawnQuality(category, useType, baseQuality);
}

inline void EnsureItem(Entity& npc, uint32_t hash)
{
    if (!hash || StackOf(npc, hash) >= 0) return;
    auto name = motionNames.find(hash);
    if (name == motionNames.end()) return;
    const Template item(name->second.c_str());
    npc.Inventory.CreateItems(item, MirroredItemQuality(item), 1);
}

inline bool IsNpc(eCEntity* instance)
{
    return npcplayback::IsCharacter(instance) && instance->GetPropertySet(eEPropertySetType_ScriptRoutine) &&
           instance->GetPropertySet(eEPropertySetType_DamageReceiver) && instance != gCSession::GetInstance().GetHero() &&
           instance != remote::ControlledBody() && !instance->GetPropertySet(eEPropertySetType_PlayerMemory) &&
           !remote::IsPuppet(instance);
}

inline uint8_t PlayerOf(eCEntity* entity)
{
    if (!entity) return 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (entity == hero || entity == remote::ControlledBody()) return net::LocalId();
    if (const int owner = remote::PuppetOwner(entity)) return static_cast<uint8_t>(owner);
    return 0;
}

inline void LearnSkeleton(Mirror& m, char const* motion)
{
    if (*m.skeleton || !motion) return;
    const size_t n = std::strcspn(motion, "_");
    if (n && n < sizeof(m.skeleton) && motion[n] == '_') std::memcpy(m.skeleton, motion, n), m.skeleton[n] = '\0';
}

} // namespace g3t::npcs::detail
