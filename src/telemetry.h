// SPDX-License-Identifier: GPL-3.0-or-later
// Structured, event-driven gameplay telemetry (`TELEM #<seq> [<domain>.<event>] ...`).
// Emits discrete state transitions and causal chains with zero gameplay rate limits or per-frame spam.
#pragma once

#include "log.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

class eCEntity;

namespace g3t::telemetry {

// Pure C++17 schema and transition-filter helpers (no SDK headers; directly unit-testable).
namespace schema {

struct CausalStamp {
    uint64_t seq = 0;
    uint32_t atMs = 0;
    char kind[32] = {};
    char actor[48] = {};
    char target[48] = {};
    char detail[96] = {};

    void Clear() noexcept { *this = CausalStamp{}; }

    void Record(uint64_t s, uint32_t nowMs, char const* k, char const* a, char const* t, char const* d) noexcept
    {
        seq = s;
        atMs = nowMs;
        Format(kind, "%s", k ? k : "-");
        Format(actor, "%s", a ? a : "-");
        Format(target, "%s", t ? t : "-");
        Format(detail, "%s", d ? d : "-");
    }

    uint32_t AgeMs(uint32_t nowMs) const noexcept
    {
        return seq ? static_cast<uint32_t>(nowMs - atMs) : UINT32_MAX;
    }

    bool Fresh(uint32_t nowMs, uint32_t windowMs) const noexcept
    {
        return seq != 0 && AgeMs(nowMs) <= windowMs;
    }

    void AppendTo(KvWriter& kv, uint32_t nowMs, uint32_t windowMs, char const* prefix = "cause") const noexcept
    {
        if (!Fresh(nowMs, windowMs)) return;
        char kSeq[32], kKind[32], kActor[32], kDetail[32], kAge[32];
        Format(kSeq, "%s_seq", prefix);
        Format(kKind, "%s_kind", prefix);
        Format(kActor, "%s_actor", prefix);
        Format(kDetail, "%s_detail", prefix);
        Format(kAge, "%s_age_ms", prefix);
        kv.Uint(kSeq, seq).Str(kKind, kind).Str(kActor, actor).Str(kDetail, detail).Uint(kAge, AgeMs(nowMs));
    }
};

// Suppresses redundant 0-damage "complete" probes while emitting every real HP change and every distinct block decision.
struct GroundHitFilter {
    uint32_t lastBlockEpisode = UINT32_MAX;
    char lastBlockDecision[32] = {};
    char lastBlockEntry[32] = {};

    void Reset() noexcept { *this = GroundHitFilter{}; }

    bool ShouldEmit(int beforeHp, int afterHp, char const* entry, char const* decision, uint32_t episode) noexcept
    {
        if (beforeHp != afterHp) return true;
        if (!decision || !*decision || std::strcmp(decision, "complete") == 0) return false;
        char const* safeEntry = entry ? entry : "-";
        if (episode != 0 && episode == lastBlockEpisode &&
            std::strcmp(lastBlockDecision, decision) == 0 &&
            std::strcmp(lastBlockEntry, safeEntry) == 0) {
            return false;
        }
        lastBlockEpisode = episode;
        Format(lastBlockDecision, "%s", decision);
        Format(lastBlockEntry, "%s", safeEntry);
        return true;
    }
};

constexpr int MaxTrackedSlots = 32;

struct SlotTable {
    char items[MaxTrackedSlots][64] = {};
    uint16_t qualities[MaxTrackedSlots] = {};
    bool primed = false;

    void Clear() noexcept { *this = SlotTable{}; }

    void Set(int slot, char const* name, uint16_t quality = 0) noexcept
    {
        if (slot <= 0 || slot >= MaxTrackedSlots) return;
        Format(items[slot], "%s", (name && *name) ? name : "");
        qualities[slot] = quality;
    }

    template <typename Fn>
    void DiffFrom(SlotTable const& prev, Fn&& onChange) const
    {
        if (!prev.primed) return;
        for (int s = 1; s < MaxTrackedSlots; ++s) {
            if (std::strcmp(prev.items[s], items[s]) != 0 || prev.qualities[s] != qualities[s]) {
                onChange(s, prev.items[s][0] ? prev.items[s] : "-", items[s][0] ? items[s] : "-",
                         prev.qualities[s], qualities[s]);
            }
        }
    }
};

struct Vitals {
    int hp = 0, hpMax = 0;
    int mp = 0, mpMax = 0;
    int sp = 0, spMax = 0;
    int xp = 0, level = 0;
    int lpAttribs = 0, lpPerks = 0;
    int gold = 0, chapter = 0;
    int aiMode = 0, aniState = 0;
    bool valid = false;
};

struct PeerSnap {
    bool connected = false;
    bool inWorld = false;
    uint32_t world = 0;
    uint32_t downedSeq = 0;
    uint8_t flags = 0;
    uint8_t aniState = 0;
    char name[64] = {};
    char heldRight[96] = {};
    char heldLeft[96] = {};
    char armor[96] = {};
    char helmet[96] = {};
    char shape[64] = {};
    float pos[3] = {};

    bool TeleportedFrom(PeerSnap const& prev, float thresholdCm = 1800.0f) const noexcept
    {
        if (!prev.connected || !prev.inWorld || !connected || !inWorld || prev.world != world) return false;
        const float dx = pos[0] - prev.pos[0];
        const float dy = pos[1] - prev.pos[1];
        const float dz = pos[2] - prev.pos[2];
        return (dx * dx + dy * dy + dz * dz) >= (thresholdCm * thresholdCm);
    }

    bool EquipChangedFrom(PeerSnap const& prev) const noexcept
    {
        return std::strcmp(heldRight, prev.heldRight) != 0 ||
               std::strcmp(heldLeft, prev.heldLeft) != 0 ||
               std::strcmp(armor, prev.armor) != 0 ||
               std::strcmp(helmet, prev.helmet) != 0 ||
               std::strcmp(shape, prev.shape) != 0;
    }
};

inline char const* QuestStatusLabel(int status) noexcept
{
    switch (status) {
    case 0: return "Open";
    case 1: return "Running";
    case 2: return "Success";
    case 3: return "Failed";
    case 4: return "Obsolete";
    case 5: return "Cancelled";
    case 6: return "Lost";
    case 7: return "Won";
    default: return "Unknown";
    }
}

} // namespace schema

// Lifecycle & passive per-frame transition observers.
void OnWorldActivate();
void OnWorldDeactivate();
void OnWorldChange();
void OnEntityDestroyed(eCEntity const* entity);
void Tick();

// RAII observer for personal loot, quickloot, script item moves, and merchant transactions.
class TransferScope {
public:
    TransferScope(char const* mode, eCEntity* from, eCEntity* to, int stackIndex = -1, int requestedAmount = -1);
    ~TransferScope();
    TransferScope(TransferScope const&) = delete;
    TransferScope& operator=(TransferScope const&) = delete;
    bool Finish(bool ok);

private:
    friend void OnEntityDestroyed(eCEntity const* entity);
    char const* mode_ = nullptr;
    eCEntity* from_ = nullptr;
    eCEntity* to_ = nullptr;
    uint8_t fromGuid_[16] = {};
    uint8_t toGuid_[16] = {};
    char fromName_[64] = {};
    char toName_[64] = {};
    bool haveFromGuid_ = false;
    bool haveToGuid_ = false;
    int stack_ = -1;
    int requested_ = -1;
    char stackItem_[64] = {};
    int stackAmountBefore_ = 0;
    int goldBefore_ = 0;
    bool involvesHero_ = false;
    bool nested_ = false;
    bool finished_ = false;
};

// Combat, ground immunity, downed lifecycle, ranged projectiles, and XP.
void NoteCombatJudge(char const* script, eCEntity* victim, eCEntity* attacker, eCEntity* source, int hpBefore);
void OnCombatHit(char const* path, eCEntity* attacker, eCEntity* victim, eCEntity* weapon,
                 int hpBefore, int hpAfter, int delta, uint8_t outcome, uint8_t reaction, uint32_t seq);
void OnCombatIgnored(char const* reason, eCEntity* attacker, eCEntity* victim, uint8_t attackerPlayer = 0);
void OnCombatClaim(char const* decision, uint8_t fromPlayer, eCEntity* victim, uint8_t const guid[16],
                   int32_t delta, uint8_t outcome, uint8_t reaction, uint32_t seq, int hpBefore, int hpAfter);
void OnCombatDefeat(char const* kind, eCEntity* killer, eCEntity* victim, uint8_t killerPlayer,
                    bool finishing, bool paysXp);
void OnGroundHit(eCEntity* victim, eCEntity* attacker, char const* entry, int beforeHp, int afterHp,
                 char const* source, char const* protection, char const* decision, uint32_t episode);
void OnDownedEvent(char const* event, uint8_t player, uint32_t episode, bool held, bool terminal,
                   int hp, eCEntity* attacker, uint8_t helper, char const* reason);
void OnShotFired(uint8_t shooterPlayer, eCEntity* casterNpc, char const* projectile, char const* spell,
                 uint32_t netId, uint8_t const targetGuid[16], uint8_t targetPlayer, int32_t damage, float velocity);
void OnShotSpawned(uint8_t fromPlayer, char const* projectile, char const* spell, uint32_t netId, uint8_t targetPlayer);
void OnXpEvent(char const* source, uint8_t fromPlayer, int beforeXp, int afterXp, int delta, unsigned level);

// Arena & duels.
void OnArenaBind(eCEntity* fighter, eCEntity* arenaEntity, char const* arenaName, char const* questName,
                 uint8_t challenger, bool awaitingSync);
void OnArenaEvent(char const* event, char const* arenaName, char const* questName, eCEntity* fighter,
                  uint8_t challenger, int prevStatus, int nextStatus, char const* detail = nullptr);

// Containers, pickups, player trade, pickpocket & ransack.
void OnContainerAccess(char const* action, eCEntity* actor, eCEntity* container, bool tradeMode, char const* result);
void OnWorldPickup(char const* action, uint8_t player, char const* itemName, uint8_t const guid[16],
                   uint32_t amount, eCEntity* entity);
void OnTradeOffer(char const* stage, uint8_t buyer, uint8_t seller, uint32_t serial, int32_t gold,
                  uint16_t itemCount, char const* summary, uint8_t resultCode = 0);
void OnCrimeTransfer(char const* kind, uint8_t actorPlayer, uint8_t victimPlayer, eCEntity* npc,
                     int32_t goldAmount, uint32_t seq);

// Witnesses, enclaves, and guard confrontations.
void OnWitnessEvent(char const* event, eCEntity* witnessNpc, eCEntity* subject, uint8_t player,
                    int reason, int crimeBefore, int crimeAfter, char const* detail = nullptr);
void OnEnclaveEvent(char const* event, char const* enclaveName, uint8_t fromPlayer, char const* modeName,
                    int32_t const crimeBefore[5], int32_t const crimeAfter[5],
                    int32_t knownBefore, int32_t knownAfter, int32_t fameBefore, int32_t fameAfter);
void OnGuardEvent(char const* event, eCEntity* guardNpc, eCEntity* targetBody, int attackReason,
                  int lastCrime, bool lethal);

// Dialogs, NPC progression, guides, companions & world interactables.
void OnTalkEvent(char const* event, uint8_t player, eCEntity* npc, uint8_t const guid[16], char const* detail = nullptr);
void OnNpcProgression(char const* source, uint8_t fromPlayer, eCEntity* npc, uint8_t dialogBits, uint8_t npcBits,
                      uint8_t guardStatus, uint8_t npcClass, uint8_t alignment, uint8_t immortal,
                      uint8_t enclaveStatus, bool moved, char const* routine);
void OnSectorChange(char const* source, uint8_t fromPlayer, char const* sectorName, bool enabled);
void OnGuideEvent(char const* event, uint8_t player, eCEntity* npc, uint8_t const guid[16],
                  bool follower, char const* questOrRoutine);
void OnOccupancyEvent(char const* event, uint8_t player, eCEntity* object, uint8_t const guid[16],
                      eCEntity* user, char const* reason);

// NPC leases, client visibility & authored cluster removal.
void OnLeaseEvent(char const* event, uint8_t ownerBefore, uint8_t ownerAfter, uint8_t const guid[16],
                  eCEntity* npc, uint32_t epoch, uint32_t incarnation, char const* reason);
void OnVisibilityEvent(char const* event, eCEntity* npc, uint8_t const guid[16], bool visible, char const* reason);
void OnNpcRemoval(char const* event, char const* name, uint8_t const guid[16], eCEntity* npc, char const* reason);

// Story / quests & save / reload / client admission.
void OnStoryDelta(char const* source, uint8_t fromPlayer, char const* domain, char const* entryName,
                  uint32_t beforeVal, uint32_t afterVal);
void OnPersistEvent(char const* event, char const* slotOrSave, char const* worldId, uint32_t epoch,
                    bool ok, char const* detail = nullptr);
void OnAdmissionStage(char const* status, char const* cause, uint32_t epoch, uint8_t kind, char const* campaign,
                      bool ownHeroReady, bool storyReady, bool doorsReady, bool hostAck, bool inWorld);

} // namespace g3t::telemetry
