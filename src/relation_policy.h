// SPDX-License-Identifier: GPL-3.0-or-later
// NPC relation (name colour) rules. gCNPC_PS.AttitudeToPlayer2 is the only input of the name tag's colour
// (Game.dll gCGUIEntityRenderer::GetAttitude 0x7A0C0, read every frame) and only scripts write it, as
// GetAttitude(self, GetPlayer()): the NPC's relation to the local player. Whether a player is knocked down is no input.
#pragma once

#include <cstdint>

namespace g3t::relation {

enum Attitude : int { None = 0, Friendly = 1, Neutral = 2, Angry = 3, Hostile = 4, Predator = 5, Panic = 6 };
constexpr uint8_t Unset = 0xFF;
constexpr uint32_t HealIntervalMs = 1500;
constexpr uint32_t DiagnosticIntervalMs = 5000;
constexpr unsigned NearestCount = 6;

enum class Colour : uint8_t { Cream, Green, Blue, Tan, Orange, Red, OrangeRed };

// The renderer's mapping: a dead NPC is tan whatever is stored; 0 is the pale cream "white" of an unevaluated NPC.
inline Colour NameColour(int attitude, bool dead, bool inHeroParty)
{
    if (dead) return Colour::Tan;
    switch (attitude) {
    case None: return Colour::Cream;
    case Friendly: return inHeroParty ? Colour::Blue : Colour::Green;
    case Neutral: return Colour::Tan;
    case Angry: return Colour::Orange;
    case Hostile:
    case Predator: return Colour::Red;
    case Panic: return Colour::OrangeRed;
    default: return Colour::Tan;
    }
}

inline char const* ColourName(Colour colour)
{
    switch (colour) {
    case Colour::Cream: return "cream";
    case Colour::Green: return "green";
    case Colour::Blue: return "blue";
    case Colour::Tan: return "tan";
    case Colour::Orange: return "orange";
    case Colour::Red: return "red";
    case Colour::OrangeRed: return "orange-red";
    }
    return "?";
}

inline bool HostileTag(int attitude, bool dead) { return NameColour(attitude, dead, false) == Colour::Red; }
inline bool ValidAttitude(int attitude) { return attitude >= None && attitude <= Panic; }

// The targeting guards answer None for a downed player so NPCs stop choosing it. The name-colour query is the NPC's relation,
// not a targeting decision: refusing it stored 0 (white) into every NPC a script touched while a player was down, and
// nothing restores the value after the revive.
inline bool RefuseDownedAttitude(bool focusNameQuery, bool playerUnavailable) { return playerUnavailable && !focusNameQuery; }

enum class Context : uint8_t { AsAsked, LocalPlayer };

// Scripts that run under another player's puppet (reaction states, callbacks) must not store that puppet's relation: the
// shared property is read against this machine's hero. An NPC led by a remote player keeps the scope it runs in.
inline Context FocusNameContext(bool scoped, bool ledByRemotePlayer)
{
    return scoped && !ledByRemotePlayer ? Context::LocalPlayer : Context::AsAsked;
}

// The value a name-colour writer stores for an NPC whose true relation is `truth`.
inline int StoredByWriter(int truth, bool playerUnavailable, bool focusNameQuery)
{
    return RefuseDownedAttitude(focusNameQuery, playerUnavailable) ? None : truth;
}

// Host or native owner: only a live NPC no script evaluated yet (0) is repaired; a stored relation is the writers' to change.
inline bool HealStored(int stored, int truth, bool alive)
{
    return alive && stored == None && truth > None && ValidAttitude(truth);
}

inline bool Due(uint32_t now, uint32_t nextAt) { return static_cast<int32_t>(now - nextAt) >= 0; }
// 0 means "never scheduled": due at once, whatever the clock's value (a clock past 24.8 days reads negative as int32).
inline bool DueOrUnset(uint32_t now, uint32_t nextAt) { return nextAt == 0 || Due(now, nextAt); }

// Client mirror: the host's attitude is the final state. `lastWritten` is what this machine wrote last (Unset: nothing yet).
enum class Apply : uint8_t { Keep, Write, Repair, Ignore };
inline Apply ApplyHost(int stored, uint8_t wanted, uint8_t lastWritten)
{
    if (!ValidAttitude(wanted)) return Apply::Ignore;
    if (stored == wanted) return Apply::Keep;
    return lastWritten != Unset && stored != lastWritten ? Apply::Repair : Apply::Write;
}

// A script of this machine asking a passive mirror for its relation gets the host's answer.
inline bool MirrorAnswersFromHost(bool mirrored, bool currentHostSession, bool haveRecord, uint8_t hostAttitude,
                                  bool sharedAttitudeValid = true)
{
    return mirrored && currentHostSession && haveRecord && ValidAttitude(hostAttitude) && sharedAttitudeValid;
}

constexpr bool MirrorUsesHostAttitude(bool enclavesTogether, uint8_t companionOwner,
                                      bool hasLocalOverride, bool sharedCombatHostile) noexcept
{
    return ((enclavesTogether || companionOwner != 0) && !hasLocalOverride) || sharedCombatHostile;
}

constexpr bool HostRecordIsCalm(int aiMode, uint8_t flags, uint8_t attitude) noexcept
{
    return aiMode == 0 && (flags & 4u) == 0 &&
           attitude != Angry && attitude != Hostile && attitude != Predator;
}

inline bool MirrorSharesCombatHostility(bool mirrored, bool currentHostSession, bool haveRecord,
                                        uint8_t hostAttitude, bool inCombat, bool remoteSharedHostile,
                                        bool targetsCoopSide = false)
{
    if (!mirrored || !currentHostSession) return false;
    if (haveRecord) {
        if (inCombat && (targetsCoopSide || hostAttitude == Hostile || hostAttitude == Predator))
            return true;
        if (!inCombat && hostAttitude != Hostile && hostAttitude != Predator && hostAttitude != Angry)
            return false;
    }
    return remoteSharedHostile;
}

// Non-combat AI modes (Flee=4, Arena=5, Talk=6, Sleep=7, Down=8, Dead=9, Abandoned=10) are never active shared
// combat, even if stale LastFightAgainstPlayer==Running or CombatState lingers while the NPC walks to an arena,
// talks, sleeps, or flees.
inline bool NonCombatAiMode(int aiMode)
{
    return aiMode >= 4 && aiMode <= 10;
}

// Humanoid combat species in Gothic 3 CP 1.75 (gESpecies_Human == 0, gESpecies_Orc == 5) use non-lethal knockouts
// (Angry / orange) unless a lethal attack reason, lethal security zone, or hostile faction applies. Non-humanoid
// creatures and monsters always fight to the death (Hostile / red).
inline bool HumanoidCombatSpecies(int species)
{
    return species == 0 || species == 5;
}

// gEAttackReason values that always mean lethal intent (Enemy=12, Murder=13, Revolution=19, Kill=20).
inline bool LethalCombatAttackReason(int attackReason)
{
    return attackReason == 12 || attackReason == 13 || attackReason == 19 || attackReason == 20;
}

// Non-lethal gEAttackReason values (brawls, warnings, property crimes, and arena/duel bouts).
inline bool NonLethalCombatAttackReason(int attackReason)
{
    return (attackReason >= 1 && attackReason <= 9) || attackReason == 15 || attackReason == 16 ||
           attackReason == 17 || attackReason == 18;
}

inline bool IsLethalSharedCombat(bool humanoid, bool hasAttackReasonToKill, int attackReason,
                                 int unpromotedAttitude, bool remoteLethal = false)
{
    if (!humanoid || remoteLethal || hasAttackReasonToKill || LethalCombatAttackReason(attackReason))
        return true;
    return unpromotedAttitude == Hostile || unpromotedAttitude == Predator;
}

enum class CombatRole : uint8_t { OrdinaryNpc, PlayerOrPuppet, CoopCompanion };

struct SharedCombatInput {
    bool alive = true;
    bool down = false;
    CombatRole role = CombatRole::OrdinaryNpc;
    int storedAttitude = None;
    int aiMode = 0;            // gEAIMode_Combat == 3
    uint32_t combatState = 0;  // gCNPC_PS.CombatState != 0
    int lastFight = 0;         // gELastFightAgainstPlayer_Running == 4
    int attackReason = 0;      // Duel == 17, Arena == 18
    bool targetsCoopSide = false;
    bool clientLeasedEngagement = false;
    bool syncedSharedHostile = false;
    bool mirrored = false;
    bool lethal = true;        // callers set from IsLethalSharedCombat; non-lethal combat promotes to Angry (orange)
};

inline bool BoutAttackReason(int attackReason)
{
    return attackReason == 17 || attackReason == 18;
}

inline bool ActiveSharedCombatHostility(SharedCombatInput const& in)
{
    if (!in.alive || in.down || in.role != CombatRole::OrdinaryNpc) return false;
    if (BoutAttackReason(in.attackReason)) return false;
    const bool inCombat = !NonCombatAiMode(in.aiMode) && (in.aiMode == 3 || in.combatState != 0);
    if (in.mirrored && in.syncedSharedHostile) return true;
    if (in.clientLeasedEngagement && (inCombat || in.syncedSharedHostile)) return true;
    return inCombat && (in.targetsCoopSide || in.syncedSharedHostile);
}

inline int EffectiveAttitude(int baseAttitude, SharedCombatInput const& in)
{
    if (!ActiveSharedCombatHostility(in)) return baseAttitude;
    if (baseAttitude == Hostile || baseAttitude == Predator || baseAttitude == Panic) return baseAttitude;
    return in.lethal ? Hostile : Angry;
}

struct FocusPriorityInput {
    bool alive = true;
    bool down = false;
    bool combatTask = false;
    bool mirrored = false;
    bool sharedHostile = false;
    bool targetsCoopSide = false;
    bool inCombat = false;
    bool inBout = false;
    CombatRole role = CombatRole::OrdinaryNpc;
    int attitude = None;
};

inline int CombatFocusPriorityOverride(FocusPriorityInput const& in)
{
    if (!in.alive || in.down || !in.combatTask || in.role != CombatRole::OrdinaryNpc || in.inBout)
        return -1;
    if (in.sharedHostile) return 3;
    if (in.mirrored && in.inCombat &&
        (in.attitude == Hostile || in.attitude == Predator || in.targetsCoopSide))
        return 3;
    return -1;
}

enum Mismatch : unsigned { Drift = 1, Unapplied = 2, StaleNone = 4, Unsent = 8, TruthDiffers = 16 };

struct Observation {
    bool mirrorBound = false;  // client: driven from the host's records
    bool alive = true;
    bool ledByRemote = false;
    bool sharedHostile = false;
    int stored = -1;           // gCNPC_PS.AttitudeToPlayer2 now
    int truth = -1;            // unfiltered GetAttitude(npc, local player); -1 unknown
    int hostAttitude = -1;     // client: latest attitude the host sent
    int shown = -1;            // client: attitude of the presentation sample in use
    int applied = -1;          // client: the last value written
    int sent = -1;             // host: attitude of the last record queued
    uint32_t sentAgeMs = 0;
};

inline unsigned Mismatches(Observation const& o)
{
    unsigned bits = 0;
    if (o.mirrorBound) {
        if (o.applied >= 0 && o.stored != o.applied) bits |= Drift;
        if (o.hostAttitude >= 0 && o.stored != o.hostAttitude && o.stored != o.shown && !o.sharedHostile) bits |= Unapplied;
        return bits;
    }
    if (o.alive && o.stored == None && o.truth > None) bits |= StaleNone;
    if (o.sent >= 0 && o.stored != o.sent && o.sentAgeMs >= 1000) bits |= Unsent;
    if (!o.ledByRemote && !o.sharedHostile && o.alive && o.truth > None && o.stored > None && o.stored != o.truth)
        bits |= TruthDiffers;
    return bits;
}

}  // namespace g3t::relation
