// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "telemetry.h"

#include "net.h"
#include "trace.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdint>

namespace g3t::telemetry::detail {

struct CombatContext {
    uint32_t atMs = 0;
    eCEntityProxy victim;
    eCEntityProxy attacker;
    eCEntityProxy source;
    char script[32] = {};
    char attackerName[64] = {};
    char sourceName[64] = {};
};

struct Event {
    char buf[4608];
    KvWriter kv{buf};

    Event() noexcept
    {
        kv.Str("role", net::IsHost() ? "host" : "client")
          .Uint("local", net::LocalId())
          .Hex("epoch", net::WorldEpoch());
    }

    uint64_t Send(char const* domainEvent) const
    {
        const uint64_t seq = NextTelemetrySeq();
        LogTelemetryLine(seq, domainEvent, kv.c_str());
        if (trace::Active()) trace::Marker(seq, domainEvent, kv.c_str());
        return seq;
    }
};

extern CombatContext recentJudge;
extern schema::CausalStamp lastCombatCause;
extern schema::CausalStamp lastCrimeCause;
extern schema::CausalStamp lastLootCause;
extern schema::CausalStamp lastQuestCause;
extern schema::GroundHitFilter groundFilter;

inline uint32_t NowMs() noexcept { return static_cast<uint32_t>(GetTickCount()); }

inline char const* PeerLabel(uint8_t player) noexcept
{
    return player == net::LocalId() ? "local_hero" : net::PeerName(player);
}

char const* SafeEntityName(eCEntity const* entity) noexcept;
void AppendEntity(KvWriter& kv, char const* prefix, eCEntity* entity,
                  uint8_t const fallbackGuid[16] = nullptr, char const* fallbackName = nullptr);
float DistanceMeters(eCEntity const* a, eCEntity const* b) noexcept;
int CountHeroGold(Entity& hero) noexcept;
char const* CrimeLabel(int crime) noexcept;
char const* ReasonLabel(int reason) noexcept;
char const* OutcomeLabel(uint8_t outcome) noexcept;
char const* ReactionLabel(uint8_t reaction) noexcept;

} // namespace g3t::telemetry::detail
