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
inline bool MirrorAnswersFromHost(bool mirrored, bool currentHostSession, bool haveRecord, uint8_t hostAttitude)
{
    return mirrored && currentHostSession && haveRecord && ValidAttitude(hostAttitude);
}

enum Mismatch : unsigned { Drift = 1, Unapplied = 2, StaleNone = 4, Unsent = 8, TruthDiffers = 16 };

struct Observation {
    bool mirrorBound = false;  // client: driven from the host's records
    bool alive = true;
    bool ledByRemote = false;
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
        if (o.hostAttitude >= 0 && o.stored != o.hostAttitude && o.stored != o.shown) bits |= Unapplied;
        return bits;
    }
    if (o.alive && o.stored == None && o.truth > None) bits |= StaleNone;
    if (o.sent >= 0 && o.stored != o.sent && o.sentAgeMs >= 1000) bits |= Unsent;
    if (!o.ledByRemote && o.alive && o.truth > None && o.stored > None && o.stored != o.truth) bits |= TruthDiffers;
    return bits;
}

}  // namespace g3t::relation
