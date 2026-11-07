// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace g3t::downed {
inline bool HeldPuppet(uint8_t flags, uint32_t sequence)
{
    return (flags & 8) && !(flags & 4) && sequence != 0;
}

inline bool UnpaidEpisode(uint32_t sequence, uint32_t paidSequence)
{
    return sequence != 0 && sequence != paidSequence;
}

inline bool CombatUnavailable(bool player, bool held, bool terminal, bool nativeDown, bool nativeDead, uint8_t flags)
{
    return player && (held || terminal || nativeDown || nativeDead || (flags & 14) != 0);
}

// DoLogicalDamage +0xB51A8/+0xB51B7, CP 1.75: not a blanket stumble or fall timer.
constexpr bool VanillaProne(int animation)
{
    return animation == 15 || animation == 16;
}

constexpr uint32_t ReviveGraceMs = 1500;
constexpr uint32_t InteractionGraceMs = 1500;

constexpr uint32_t HeldStallMs = 2500;
constexpr uint32_t HeldEntryMaxMs = 5000;

struct HeldConvergence
{
    bool observing = false, settled = false, requested = false, awaitingRepair = false;
    uint32_t enteredAt = 0, progressAt = 0, repairedAt = 0;
    uint16_t furthest = 0;

    void Clear() { *this = HeldConvergence{}; }
    void Request() { requested = true; }
    bool Observe(bool stable, uint16_t fraction, uint32_t now)
    {
        if (stable) {
            settled = true;
            observing = requested = awaitingRepair = false;
            return false;
        }
        if (awaitingRepair && now - repairedAt < HeldStallMs) return false;
        if (!observing) {
            observing = true;
            enteredAt = progressAt = now;
            furthest = fraction == UINT16_MAX ? 0 : fraction;
        } else if (fraction != UINT16_MAX && fraction > furthest) {
            furthest = fraction;
            progressAt = now;
        }
        // Repeated restarts cannot extend the deadline by replaying the same fraction.
        if (!requested && !settled && now - progressAt < HeldStallMs && now - enteredAt < HeldEntryMaxMs)
            return false;
        requested = false;
        observing = false;
        settled = true;
        awaitingRepair = true;
        repairedAt = now;
        return true;
    }
};

// High bits are interaction reasons only while the existing playing bit is set.
// Without playing, 32/64 retain their loading/menu heartbeat meanings.
enum InteractionFlag : uint8_t { InDialogue = 32, InTrade = 64, InteractionGrace = 128 };
constexpr uint8_t InteractionMask = InDialogue | InTrade | InteractionGrace;

constexpr int InteractionPage(int mainPage, int beforeTutorial)
{
    return mainPage == 15 ? beforeTutorial : mainPage;
}

inline bool DialogueObserved(int page, bool running, bool ending, bool heroPartner, bool livePartner)
{
    return page == 6 || (running && !ending && heroPartner && livePartner);
}

inline bool ValidActivityFlags(uint8_t flags)
{
    if (flags & 1) return !(flags & 14) || !(flags & InteractionMask);
    return !(flags & InteractionGrace);
}

struct InteractionProtection
{
    uint8_t windows = 0;
    bool grace = false;
    uint32_t closedAt = 0;

    void Clear() { windows = 0; grace = false; closedAt = 0; }
    uint8_t Flags(uint32_t now) const
    {
        return windows ? windows : (grace && now - closedAt < InteractionGraceMs ? InteractionGrace : 0);
    }
    void Observe(bool eligible, bool dialogue, bool trade, uint32_t now)
    {
        if (!eligible) { Clear(); return; }
        const uint8_t next = (dialogue ? InDialogue : 0) | (trade ? InTrade : 0);
        if (next) grace = false;
        else if (windows) { grace = true; closedAt = now; }
        windows = next;
        if (grace && now - closedAt >= InteractionGraceMs) grace = false;
    }
};

inline uint8_t PeerInteraction(bool connected, bool known, uint32_t arrived, uint32_t peerWorld,
                               uint32_t currentWorld, uint8_t flags, uint32_t now)
{
    return connected && known && now - arrived < 5000 && peerWorld == currentWorld &&
           (flags & 1) && !(flags & 14) ? flags & InteractionMask : 0;
}

inline char const* InteractionReason(uint8_t flags)
{
    if (flags & InTrade) return "in-trade";
    if (flags & InDialogue) return "in-dialogue";
    return flags & InteractionGrace ? "interaction-close-grace" : "none";
}

struct Recovery
{
    bool rising = false, grace = false;
    uint32_t stoodAt = 0;

    void Begin() { rising = true; grace = false; }
    void Clear() { rising = grace = false; stoodAt = 0; }
    bool Protected(uint32_t now) const { return rising || (grace && now - stoodAt < ReviveGraceMs); }

    void Observe(bool standing, uint32_t now)
    {
        if (rising && standing) {
            rising = false;
            grace = true;
            stoodAt = now;
        }
        if (grace && now - stoodAt >= ReviveGraceMs) grace = false;
    }
};

inline bool RecoveryStanding(int animation, bool nativeDown, char const* motion)
{
    return (animation == 2 || animation == 3 || animation == 4) && !nativeDown && motion && *motion &&
           !std::strstr(motion, "GetUp") && !std::strstr(motion, "StandUp");
}

struct PeerRecovery
{
    bool held = false;
    Recovery recovery;

    void Clear() { held = false; recovery.Clear(); }
    void Observe(bool present, uint8_t flags, bool standing, uint32_t now)
    {
        if (!present || !(flags & 1) || (flags & 4)) { Clear(); return; }
        if (flags & 8) { held = true; recovery.Clear(); return; }
        if (held) { held = false; recovery.Begin(); }
        recovery.Observe(standing && !(flags & 2), now);
    }
};

inline bool HarmImmune(bool player, bool held, bool terminal, uint8_t flags, bool recovering, bool interacting = false)
{
    return player && !terminal && !(flags & 4) && (held || (flags & 8) || recovering || interacting);
}

inline bool HarmfulReaction(char const* state)
{
    for (char const* prefix : {"ZS_QuickStumble", "ZS_Stumble", "ZS_PierceStumble", "ZS_ParadeStumble",
                              "ZS_QuickParadeStumble", "ZS_HeavyParadeStumble", "ZS_Freeze"})
        if (!std::strncmp(state, prefix, std::strlen(prefix))) return true;
    for (char const* entry : {"ZS_SitKnockDown", "ZS_LieKnockDown", "ZS_Unconscious", "ZS_PiercedKO",
                             "ZS_Dead", "ZS_RagDollDead", "ZS_PiercedDead"})
        if (!std::strcmp(state, entry)) return true;
    return false;
}

enum TargetBinding : uint32_t {
    Current = 1, Alternative = 2, Attacker = 4, LastAttacker = 8, Spell = 16, Alignment = 32, Inflictor = 64
};

inline bool StopTargetInstruction(uint32_t bindings)
{
    return (bindings & (Current | Spell)) != 0;
}

constexpr uint32_t ActivityStaleMs = 5000;
constexpr uint32_t AbsenceGraceMs = 5000;

enum class Availability { Able, Down, Disconnected, Unknown, Stale, NotPlaying };

inline Availability HelperAvailability(bool connected, bool known, uint32_t arrived, uint8_t flags, uint32_t now)
{
    if (!connected) return Availability::Disconnected;
    if (!known) return Availability::Unknown;
    if (now - arrived >= ActivityStaleMs) return Availability::Stale;
    if (!(flags & 1)) return Availability::NotPlaying;
    return flags & 14 ? Availability::Down : Availability::Able;
}

inline bool Playing(Availability availability)
{
    return availability == Availability::Able || availability == Availability::Down;
}

enum class EndDecision { Wait, AllDown, AloneGrace };

inline EndDecision DecideEnd(bool helperAble, bool allOthersDown, uint32_t now, uint32_t& aloneSince)
{
    if (helperAble) {
        aloneSince = 0;
        return EndDecision::Wait;
    }
    if (allOthersDown) return EndDecision::AllDown;
    if (!aloneSince) aloneSince = now ? now : 1;
    return static_cast<int32_t>(now - aloneSince) >= static_cast<int32_t>(AbsenceGraceMs)
        ? EndDecision::AloneGrace : EndDecision::Wait;
}

// Native animation, HP and loaded alive snapshots are not authority to release a downed life.
struct Life
{
    bool held = false;
    bool terminal = false;
    bool deathStarted = false;
    bool deathFinished = false;
    uint32_t sequence = 0;

    void Rebind()
    {
        if (++sequence == 0) ++sequence;
    }

    void Hold()
    {
        if (terminal) return;
        if (!held) Rebind();
        held = true;
    }

    void Restore(bool waiting)
    {
        if (waiting) Hold();
    }

    bool Revive(uint32_t request)
    {
        if (terminal || !held || !request || request != sequence) return false;
        held = false;
        return true;
    }

    bool End()
    {
        if (terminal) return false;
        terminal = true;
        held = false;
        return true;
    }

    bool BeginDeath()
    {
        if (!terminal || deathStarted) return false;
        deathStarted = true;
        return true;
    }

    void FinishDeath(bool nativeDead)
    {
        if (terminal && deathStarted && nativeDead) deathFinished = true;
    }

    bool TerminalStateAllowed(char const* state) const
    {
        if (!terminal) return true;
        return !std::strcmp(state, deathStarted ? "ZS_RagDollDead_Loop" : "ZS_RagDollDead");
    }

    bool TerminalTaskAllowed(char const* task) const
    {
        return !terminal || (!deathStarted && !std::strcmp(task, "ZS_RagDollDead"));
    }

    bool TerminalModeAllowed(int mode) const
    {
        return !terminal || mode == 9 || (!deathStarted && mode == 1);
    }

    bool TerminalHpAllowed(int hp) const
    {
        return !terminal || hp <= 0;
    }

    void OnWorldChange()
    {
        if (held) Rebind();
        terminal = false;
        deathStarted = false;
        deathFinished = false;
    }
};
}
