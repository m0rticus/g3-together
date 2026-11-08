// SPDX-License-Identifier: GPL-3.0-or-later
#include "relation.h"

#include "ai.h"
#include "downed.h"
#include "guid.h"
#include "log.h"
#include "net.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "relation_policy.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace g3t::relation {
namespace {
constexpr float NearCm = 6000.0f;
constexpr unsigned MismatchLinesPerReport = 12;

struct Tracked {
    std::array<uint8_t, 16> guid{};
    int stored = -1;
    std::string state;
};

std::unordered_map<eCEntity*, uint32_t> healAt;
std::vector<Tracked> tracked;
unsigned healed = 0, reports = 0, mismatchLines = 0;
uint32_t nextReportAt = 0, nextWatchAt = 0;
uint32_t followUpAt[2] = {};
unsigned followUps = 0;
uint32_t previousMask = 0;
bool haveMask = false;

bool Eligible(eCEntity* entity)
{
    return entity && entity->GetPropertySet(eEPropertySetType_NPC) && entity->GetPropertySet(eEPropertySetType_ScriptRoutine);
}

int Stored(eCEntity* entity)
{
    return static_cast<int>(static_cast<gEAttitude>(Entity(entity).NPC.AttitudeToPlayer2));
}

char const* StateOf(eCEntity* entity, npcs::RelationInfo const& info)
{
    if (info.mirrored) return info.presenting ? "mirror" : info.released ? "mirror-released" : "mirror-held";
    if (npclease::LocalAuthority(entity)) return "lease";
    return net::IsHost() ? "host" : "native";
}

struct Text {
    char value[8];
    Text(int v, bool unset = false) { if (v < 0 || unset) std::strcpy(value, "-"); else std::snprintf(value, sizeof(value), "%d", v); }
    operator char const*() const { return value; }
};

std::string Kinds(unsigned bits)
{
    std::string out;
    auto add = [&](unsigned bit, char const* name) {
        if (!(bits & bit)) return;
        if (!out.empty()) out += ',';
        out += name;
    };
    add(Drift, "native-drift");
    add(Unapplied, "unapplied");
    add(StaleNone, "stale-none");
    add(Unsent, "unsent");
    add(TruthDiffers, "truth-differs");
    return out;
}

struct View {
    eCEntity* entity = nullptr;
    int stored = -1, truth = -1;
    bool alive = true;
    npcs::RelationInfo info;
    Observation observation;
};

View Inspect(eCEntity* entity, bool wantTruth)
{
    View v;
    v.entity = entity;
    Entity npc(entity);
    v.alive = !npc.IsDead();
    v.stored = Stored(entity);
    npcs::RelationOf(entity, v.info);
    if (wantTruth) {
        int truth = -1;
        if (ai::LocalAttitude(entity, truth)) v.truth = truth;
    }
    Observation& o = v.observation;
    o.mirrorBound = v.info.mirrored && v.info.bound;
    o.alive = v.alive;
    const uint8_t owner = npcs::CompanionOwner(entity);
    o.ledByRemote = owner && owner != net::LocalId();
    o.stored = v.stored;
    o.truth = v.truth;
    if (v.info.mirrored) {
        o.hostAttitude = v.info.hostAttitude == Unset ? -1 : v.info.hostAttitude;
        o.shown = v.info.shown == Unset ? -1 : v.info.shown;
        o.applied = v.info.written == Unset ? -1 : v.info.written;
    }
    if (v.info.hasSent && v.info.sentAgeMs <= 3000) {
        o.sent = v.info.sentAttitude;
        o.sentAgeMs = v.info.sentAgeMs;
    }
    return v;
}

void LogNpc(char const* tag, View const& v, float distanceCm)
{
    char identity[40];
    guid::FormatEntity(v.entity, identity, sizeof(identity));
    Entity npc(v.entity);
    const int aiMode = static_cast<int>(npc.Routine.AIMode);
    Logf("relation: %s npc=%.40s guid=%.8s d=%.1fm state=%s ai=%d stored=%d colour=%s hostile=%d truth=%s host=%s shown=%s written=%s sent=%s party=%u",
         tag, npc.GetName().GetText(), identity, distanceCm / 100.0f, StateOf(v.entity, v.info), aiMode, v.stored,
         ColourName(NameColour(v.stored, !v.alive, false)), HostileTag(v.stored, !v.alive) ? 1 : 0, static_cast<char const*>(Text(v.truth)),
         static_cast<char const*>(Text(v.info.hostAttitude, v.info.hostAttitude == Unset)),
         static_cast<char const*>(Text(v.info.shown, v.info.shown == Unset)),
         static_cast<char const*>(Text(v.info.written, v.info.written == Unset)),
         static_cast<char const*>(Text(v.info.sentAttitude, !v.info.hasSent)), static_cast<unsigned>(v.info.partyPlayer));
}

void LogMismatch(View const& v)
{
    const unsigned bits = Mismatches(v.observation);
    if (!bits) return;
    char identity[40];
    guid::FormatEntity(v.entity, identity, sizeof(identity));
    ++mismatchLines;
    Logf("relation: mismatch kinds=%s npc=%.40s guid=%.8s state=%s stored=%d truth=%s host=%s shown=%s written=%s sent=%s",
         Kinds(bits).c_str(), Entity(v.entity).GetName().GetText(), identity, StateOf(v.entity, v.info), v.stored,
         static_cast<char const*>(Text(v.truth)), static_cast<char const*>(Text(v.info.hostAttitude, v.info.hostAttitude == Unset)),
         static_cast<char const*>(Text(v.info.shown, v.info.shown == Unset)),
         static_cast<char const*>(Text(v.info.written, v.info.written == Unset)),
         static_cast<char const*>(Text(v.info.sentAttitude, !v.info.hasSent)));
}

void Dump(eCEntity* hero, char const* reason, uint32_t mask)
{
    struct Row { eCEntity* entity; float distance; };
    std::vector<Row> rows;
    unsigned counts[8] = {}, considered = 0, mirrors = 0;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount(); ++i) {
        eCEntity* entity = all.GetAt(i).GetInstance();
        if (!Eligible(entity) || entity == hero || remote::IsPuppet(entity)) continue;
        const float distance = entity->GetDistanceToEntity(*hero);
        if (distance > NearCm) continue;
        ++considered;
        npcs::RelationInfo info;
        npcs::RelationOf(entity, info);
        if (info.mirrored) ++mirrors;
        if (!Entity(entity).IsDead()) ++counts[std::min(7, std::max(0, Stored(entity)))];
        rows.push_back({entity, distance});
    }
    unsigned a, b, c, d;
    ai::RelationStats(a, b, c, d);
    ++reports;
    Logf("relation: summary reason=%s role=%s hooked=%d players_down=%#x local_down=%d npcs_within_60m=%u mirrors=%u alive_stored[none=%u friendly=%u neutral=%u angry=%u hostile=%u predator=%u panic=%u] "
         "writer_queries=%u scope_cleared=%u downed_not_refused=%u mirror_answered=%u healed=%u mismatch_lines=%u",
         reason, net::IsHost() ? "host" : "client", ai::RelationHooked() ? 1 : 0, mask, downed::CombatUnavailable(hero) ? 1 : 0,
         considered, mirrors, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6],
         a, b, c, d, healed, mismatchLines);
    const size_t nearest = std::min<size_t>(NearestCount, rows.size());
    std::partial_sort(rows.begin(), rows.begin() + nearest, rows.end(),
                      [](Row const& x, Row const& y) { return x.distance < y.distance; });
    tracked.clear();
    for (size_t i = 0; i < nearest; ++i) {
        View v = Inspect(rows[i].entity, true);
        LogNpc("npc", v, rows[i].distance);
        LogMismatch(v);
        Tracked t;
        guid::CopyEntity(rows[i].entity, t.guid.data());
        t.stored = v.stored;
        t.state = StateOf(rows[i].entity, v.info);
        tracked.push_back(std::move(t));
    }
    unsigned lines = 0, stale = 0;
    for (size_t i = nearest; i < rows.size() && lines < MismatchLinesPerReport; ++i) {
        const bool zero = Stored(rows[i].entity) == None && !Entity(rows[i].entity).IsDead();
        if (zero && stale >= 16) continue;
        stale += zero ? 1 : 0;
        View v = Inspect(rows[i].entity, zero);
        if (!Mismatches(v.observation)) continue;
        LogMismatch(v);
        ++lines;
    }
}

void Watch()
{
    for (Tracked& t : tracked) {
        Entity npc = guid::Resolve(t.guid.data());
        eCEntity* entity = npc.GetInstance();
        if (!Eligible(entity)) continue;
        const View v = Inspect(entity, false);
        const std::string state = StateOf(entity, v.info);
        if (v.stored == t.stored && state == t.state) continue;
        char identity[40];
        guid::FormatEntity(entity, identity, sizeof(identity));
        Logf("relation: change npc=%.40s guid=%.8s stored=%d->%d colour=%s hostile=%d state=%s->%s", npc.GetName().GetText(), identity,
             t.stored, v.stored, ColourName(NameColour(v.stored, !v.alive, false)), HostileTag(v.stored, !v.alive) ? 1 : 0,
             t.state.c_str(), state.c_str());
        t.stored = v.stored;
        t.state = state;
        LogMismatch(Inspect(entity, true));
    }
}
}

void Tick()
{
    if (ai::ShuttingDown() || !gCSession::GetInstance().IsGameRunning()) return;
    eCEntity* hero = remote::ControlledBody();
    if (!hero) return;
    const uint32_t now = net::Now();
    const uint32_t mask = downed::UnavailablePlayers();
    char const* reason = nullptr;
    if (!haveMask) {
        previousMask = mask;
        haveMask = true;
    } else if (mask != previousMask) {
        reason = (mask & ~previousMask) ? "knocked-down" : "revive";
        Logf("relation: transition kind=%s players_down=%#x was=%#x local_down=%d", reason, mask, previousMask,
             downed::CombatUnavailable(hero) ? 1 : 0);
        previousMask = mask;
        followUpAt[0] = now + 1000;
        followUpAt[1] = now + 4000;
        followUps = 2;
    }
    if (!reason && followUps && Due(now, followUpAt[2 - followUps])) {
        reason = "after-transition";
        --followUps;
    }
    if (!reason && DueOrUnset(now, nextReportAt)) reason = "periodic";
    if (reason) {
        nextReportAt = now + DiagnosticIntervalMs;
        nextWatchAt = now + 500;
        Dump(hero, reason, mask);
    } else if (DueOrUnset(now, nextWatchAt)) {
        nextWatchAt = now + 500;
        Watch();
    }
}

uint8_t Outgoing(eCEntity* npc, bool alive)
{
    if (!npc || !npc->GetPropertySet(eEPropertySetType_NPC)) return 0;
    const int stored = Stored(npc);
    if (stored != None || !alive) return static_cast<uint8_t>(stored);
    const uint32_t now = net::Now();
    uint32_t& next = healAt[npc];
    if (!DueOrUnset(now, next)) return 0;
    next = now + HealIntervalMs;
    if (healAt.size() > 8192) healAt.clear();
    int truth = -1;
    if (!ai::LocalAttitude(npc, truth) || !HealStored(stored, truth, alive)) return 0;
    Entity(npc).NPC.AttitudeToPlayer2 = static_cast<gEAttitude>(truth);
    ++healed;
    if (healed <= 30 || healed % 100 == 0)
        Logf("relation: heal npc=%.40s stored=0 truth=%d colour=%s (no script stored its relation yet, or a stale None); written back",
             Entity(npc).GetName().GetText(), truth, ColourName(NameColour(truth, false, false)));
    return static_cast<uint8_t>(truth);
}
}
