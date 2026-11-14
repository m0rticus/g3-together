// SPDX-License-Identifier: GPL-3.0-or-later
// Share future earned XP through admission-bound cumulative counters; never lift individually saved totals.
#pragma once

#include <cstdint>

namespace g3t::net { struct WorldState; }

namespace g3t::xpshare {
namespace counter {
constexpr uint8_t Live = 0;
constexpr uint8_t Baseline = 1;
constexpr uint8_t Ack = 2;
constexpr uint8_t Request = 3;
constexpr uint32_t BaselineEarned = 0;

inline uint32_t Gain(uint32_t applied, uint32_t earned)
{
    return earned > applied ? earned - applied : 0;
}

// World_MCP is exclusively gCQuest_PS::SetStatus, which story.cpp replays natively on every peer.
inline bool SharesNativeSource(bool isWorldEntity)
{
    return !isWorldEntity;
}

struct Sender {
    uint32_t nonce = 0, origin = 0, relayed = 0;
    void Begin(uint32_t id, uint32_t earned) { nonce = id; origin = earned; relayed = 0; }
    void Relay(uint32_t gain) { relayed += gain; }
    uint32_t Outbound(uint32_t earned) const { return earned + relayed; }
    uint32_t Total(uint32_t earned) const { return (earned - origin) + relayed; }
};

struct Receiver {
    bool known = false;
    uint32_t nonce = 0, applied = 0;
    bool Snapshot(uint32_t id, uint32_t baseline)
    {
        if (!id || baseline != BaselineEarned) return false;
        if (!known || nonce != id) { known = true; nonce = id; applied = baseline; }
        return true;
    }
    uint32_t Take(uint32_t id, uint32_t earned, uint32_t maximum)
    {
        if (!known || nonce != id) return 0;
        const uint32_t gain = Gain(applied, earned);
        if (gain > maximum) return 0;
        if (earned > applied) applied = earned;
        return gain;
    }
};

constexpr uint32_t BaselineRetryMs = 200;
constexpr uint32_t HeartbeatMs = 2000;
constexpr uint32_t UnsentEarned = 0xffffffffu;

struct OutboundPlan {
    bool sendBaseline = false;
    bool sendLive = false;
};

struct PeerLink : Receiver {
    uint32_t total = 0;
    uint32_t generation = 0;
    bool wasInWorld = false;
    Sender outgoing;
    uint32_t ackedNonce = 0;
    uint32_t lastBaselineAt = 0;
    uint32_t attemptedEarned = UnsentEarned;
    uint32_t ackedSentEarned = UnsentEarned;

    bool SyncGeneration(bool joined, uint32_t currentGeneration)
    {
        if (!joined) {
            *this = PeerLink{};
            return false;
        }
        if (generation != currentGeneration) {
            *this = PeerLink{};
            generation = currentGeneration;
        }
        return true;
    }

    void LeaveWorld()
    {
        wasInWorld = false;
        outgoing = Sender{};
        ResetAdmissionHandshake();
    }

    void ResetAdmissionHandshake()
    {
        ackedNonce = 0;
        lastBaselineAt = 0;
        attemptedEarned = UnsentEarned;
        ackedSentEarned = UnsentEarned;
    }

    bool ShouldSendBaseline(uint32_t now) const
    {
        return wasInWorld && outgoing.nonce != 0 && ackedNonce != outgoing.nonce &&
               (!lastBaselineAt || now - lastBaselineAt >= BaselineRetryMs);
    }

    void MarkBaselineSent(uint32_t now)
    {
        lastBaselineAt = now ? now : 1u;
    }

    void NoteAck(uint32_t theirNonce)
    {
        if (theirNonce && theirNonce == outgoing.nonce) ackedNonce = theirNonce;
    }

    OutboundPlan PlanLive(uint32_t earned, uint32_t now, bool heartbeatDue) const
    {
        if (!wasInWorld || !outgoing.nonce) return {};
        const uint32_t streamEarned = outgoing.Outbound(earned);
        if (ackedNonce == outgoing.nonce) {
            if (ackedSentEarned != streamEarned || heartbeatDue) return {false, true};
            return {};
        }
        if (outgoing.Total(earned) == 0) return {};
        if (attemptedEarned != streamEarned || !lastBaselineAt || now - lastBaselineAt >= BaselineRetryMs) {
            return {true, true};
        }
        return {};
    }

    void MarkLiveSent(uint32_t earned, uint32_t now, bool sentBaseline)
    {
        const uint32_t streamEarned = outgoing.Outbound(earned);
        if (sentBaseline) MarkBaselineSent(now);
        attemptedEarned = streamEarned;
        if (ackedNonce == outgoing.nonce) ackedSentEarned = streamEarned;
    }
};
}

void Tick();                                  // sends this machine's total, applies what is waiting
void NoteGiveXp(int gained);                  // spells.cpp: the game's GiveXP raised the local hero's experience by this much
void OnState(net::WorldState const& s);       // kind 19: another player's total
void OnWorldChange();                         // reset sharing counters, retaining the loaded hero's own XP/level
using LogFn = void (*)(char const*);
void Report(LogFn log);                       // QA: XPSHARE line
void Off(LogFn log);                          // QA negative control (dialogxpoff): experience stays with whoever earned it
}
