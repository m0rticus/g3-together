// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_qa.h"

#include "log.h"
#include "npc_corpse.h"
#include "npclease_runtime.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3t::npcs {
namespace {
using namespace detail;
} // namespace

void OwnOff(LogFn log)
{
    owning = false;
    log("OWNOFF ok=1");
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
        Format(line, "NPC guid=%s name=%s pos=%.1f,%.1f,%.1f hp=%d dead=%d mirrored=%d task=%s repacified=%u last_foreign=%s owner=%u",
               text, npc.GetName().GetText(), at[0], at[1], at[2], static_cast<int>(npc.DamageReceiver.HitPoints),
               npc.IsDead(), IsMirrored(instance), npc.Routine.GetCurrentTask().GetText(),
               mirror != mirrors.end() ? mirror->second.body.repacified : 0,
               mirror != mirrors.end() && *mirror->second.body.lastForeign ? mirror->second.body.lastForeign : "-",
               OwnerOf(instance));
        log(line);
    }
    Format(line, "NPCSYNC claims_refused=%u host_wins=%u host_wins_rule=%d join_refused=%u join_grace=%d contested_refused=%u "
                 "contested=%zu contest_rule=%d reclaims=%u reclaim_refused=%u reclaimed=%zu reclaim_rule=%d",
           claimsRefused, hostWins, hostWinsRule ? 1 : 0, joinRefused, joinGraceRule ? 1 : 0, contestedRefused,
           contested.size(), contestRule ? 1 : 0, reclaims, reclaimRefused, reclaimed.size(), reclaimRule ? 1 : 0);
    log(line);
}

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
        const GuidKey key = Key(guid);
        char text[48];
        guid::Format(guid, text, sizeof(text));
        const auto note = sendNotes.find(key);
        Format(line, "EDGENPC guid=%s name=%s nearest_m=%.1f reason=%s age_ms=%d mirror=%d",
               text, npc.GetName().GetText(), nearest / 100.0f,
               note != sendNotes.end() ? note->second.reason : "never",
               note != sendNotes.end() ? static_cast<int>(net::Now() - note->second.at) : -1,
               mirrors.count(key) ? 1 : 0);
        log(line);
    }
    log("EDGENPC end");
}

void SendInfo(char const* name, LogFn log)
{
    Entity npc;
    uint8_t bytes[16];
    if (guid::ParseExact(name, bytes)) npc = guid::Resolve(bytes);
    else npc = Entity::GetEntity(bCString(name));
    char line[300];
    if (!npc.GetInstance()) { log("REFUSED npcsend: no such NPC"); return; }
    uint8_t guid[16];
    Guid(npc.GetInstance(), guid);
    const GuidKey key = Key(guid);
    const auto note = sendNotes.find(key);
    const auto back = givenBack.find(key);
    char text[48];
    guid::Format(guid, text, sizeof(text));
    Format(line, "NPCSEND name=%s guid=%s reason=%s age_ms=%d myclaim=%d mirror=%d givenback_ms=%d", name, text,
           note != sendNotes.end() ? note->second.reason : "never",
           note != sendNotes.end() ? static_cast<int>(net::Now() - note->second.at) : -1,
           myClaims.count(key) ? 1 : 0, mirrors.count(key) ? 1 : 0,
           back != givenBack.end() ? static_cast<int>(back->second - net::Now()) : 0);
    log(line);
}

void LifeReport(char const* text, LogFn log)
{
    uint8_t guid[16];
    if (!guid::ParseExact(text, guid)) { log("REFUSED npclife: expected an NPC GUID"); return; }
    const GuidKey key = Key(guid);
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
            ready = false;
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
    const bool absent = net::IsHost() ? false : (!mirror && record != nullptr);
    Format(line, "NPCLIFE known=1 incarnation=%u shown=%u absent=%u ready=%d hp=%d hp_max=%d dead=%d captured=%u displayed=%u",
           record->incarnation, shown, absent ? 1 : 0, ready ? 1 : 0, record->hitPoints,
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

void CompanionsOff(LogFn log)
{
    offering = false;
    log("COMPANIONSOFF ok=1");
}

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
    Format(line, "MIRRORSTATS mirrors=%zu missing=%u found_later=%u expired=%u lookups=%u retry=%d",
           mirrors.size(), missing, mirrorsFoundLater, mirrorsExpired, mirrorLookups, retryMissing ? 1 : 0);
    log(line);
}

void SyncReport(LogFn log)
{
    char line[800];
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
           localContactWindows, localContactMissing, localContactTests, localContactOverlaps, localContactWalls,
           localBlowsJudged);
    log(line);
    unsigned tracked = 0, delaySum = 0, delayMax = 0;
    unsigned arrivalAgeMax = 0, transitExcessMax = 0, shownLagMax = 0, shownTracks = 0, underruns = 0;
    unsigned backlogMax = 0, oldestAgeMax = 0, catchUpTracks = 0, catchUpFrames = 0;
    size_t pendingEvents = 0;
    uint64_t backlogSum = 0;
    uint64_t shownLagSum = 0;
    double jitterSum = 0.0, jitterMax = 0.0;
    const uint32_t now = net::Now();
    for (auto const& entry : mirrors) {
        auto const& track = entry.second.body.track;
        if (!track.count) continue;
        const unsigned d = static_cast<unsigned>(body::PlaybackDelay(track));
        ++tracked, delaySum += d, delayMax = std::max(delayMax, d);
        const unsigned backlog = static_cast<unsigned>(std::max(track.backlogMs, 0));
        const unsigned oldestAge = static_cast<unsigned>(std::max(static_cast<int32_t>(
            now - static_cast<uint32_t>(track.offset) - body::OldestRetainedSent(track)), 0));
        backlogSum += backlog;
        backlogMax = std::max(backlogMax, backlog);
        oldestAgeMax = std::max(oldestAgeMax, oldestAge);
        catchUpTracks += track.catchUp ? 1u : 0u;
        catchUpFrames += track.catchUpFrames;
        pendingEvents += body::PendingEvents(track);
        Format(line, "SYNCTRACK npc_hash=%08X owner=%u samples=%d buffer_ms=%u oldest_sample_age_ms=%u "
                     "arrival_age_ms=%u transit_excess_ms=%d backlog_ms=%u catchup=%d protected=%d pending_events=%zu",
               net::HashGuid(entry.first.data()), entry.second.owner,
               track.count, d, oldestAge, now - track.lastArrived, track.transitExcessMs, backlog,
               track.catchUp ? 1 : 0, track.combatClockProtected ? 1 : 0, body::PendingEvents(track));
        log(line);
        jitterSum += track.jitterMs;
        jitterMax = std::max(jitterMax, static_cast<double>(track.jitterMs));
        arrivalAgeMax = std::max(arrivalAgeMax, now - track.lastArrived);
        const int32_t excess = static_cast<int32_t>(track.lastArrived - track.snaps[track.count - 1].sent -
                                                  static_cast<uint32_t>(track.offset));
        transitExcessMax = std::max(transitExcessMax, static_cast<unsigned>(std::max(excess, 0)));
        if (entry.second.havePresentation) {
            const int32_t lag = static_cast<int32_t>(now - static_cast<uint32_t>(track.offset) - entry.second.shownSent);
            const unsigned shownLag = static_cast<unsigned>(std::max(lag, 0));
            shownLagSum += shownLag;
            shownLagMax = std::max(shownLagMax, shownLag);
            ++shownTracks;
        }
        underruns += track.underruns;
    }
    Format(line, "SYNCFRAMES frames=%u avg_ms=%.1f over33=%u over66=%u max_ms=%u tracked=%u delay_avg=%u delay_max=%u local_blows=%u out_of_reach=%u",
           frameCount, frameCount ? static_cast<float>(frameSumMs) / frameCount : 0.0f, slowFrames, hitchFrames, maxFrameMs, tracked,
           tracked ? delaySum / tracked : 0u, delayMax, localBlowsJudged, localBlowsOutOfReach);
    log(line);
    Format(line, "SYNCLATENCY tracked=%u shown=%u buffer_avg_ms=%u buffer_max_ms=%u shown_lag_avg_ms=%u shown_lag_max_ms=%u "
                 "arrival_age_max_ms=%u transit_excess_max_ms=%u jitter_avg_ms=%.1f jitter_max_ms=%.1f underruns=%u "
                 "backlog_avg_ms=%u backlog_max_ms=%u oldest_sample_age_max_ms=%u catchup_tracks=%u catchup_frames=%u pending_events=%zu",
           tracked, shownTracks, tracked ? delaySum / tracked : 0u, delayMax,
           shownTracks ? static_cast<unsigned>(shownLagSum / shownTracks) : 0u, shownLagMax,
           arrivalAgeMax, transitExcessMax, tracked ? jitterSum / tracked : 0.0, jitterMax, underruns,
           tracked ? static_cast<unsigned>(backlogSum / tracked) : 0u, backlogMax, oldestAgeMax,
           catchUpTracks, catchUpFrames, pendingEvents);
    log(line);
    if (net::IsHost()) {
        static char const* const names[3] = {"fast", "ordinary", "slow"};
        char streams[360] = "";
        size_t used = 0;
        for (unsigned tier = 0; tier < 3; ++tier) {
            auto const& s = sendStats.tiers[tier];
            const int written = std::snprintf(streams + used, sizeof(streams) - used,
                " %s=n:%u,min:%u,avg:%.1f,max:%u,overdue:%u",
                names[tier], s.count, s.count ? s.minMs : 0u, s.AverageMs(), s.maxMs, s.overdue);
            if (written > 0 && used + static_cast<size_t>(written) < sizeof(streams)) used += static_cast<size_t>(written);
        }
        unsigned bursts = 0;
        uint32_t lastBurstMs = 0, lastBurstNpcs = 0, stretchPermille = npcsend::StretchNormal;
        bool anyPeer = false;
        net::Remote const* remotes = net::Remotes();
        for (uint8_t id = 2; id < net::MaxPlayers; ++id) {
            bursts += initialPacers[id].bursts;
            if (initialPacers[id].lastBurstMs || initialPacers[id].lastBurstNpcs) {
                lastBurstMs = initialPacers[id].lastBurstMs;
                lastBurstNpcs = initialPacers[id].lastBurstNpcs;
            }
            if (remotes[id].connected || net::InWorld(id)) {
                stretchPermille = anyPeer ? std::max(stretchPermille, net::NpcStretchPermille(id))
                                          : net::NpcStretchPermille(id);
                anyPeer = true;
            }
        }
        if (!anyPeer) stretchPermille = net::NpcStretchPermille(2);
        Format(line, "SYNCSTREAM role=host npcs=%zu%s edges=%u heartbeats=%u initials=%u paced=%u join_bursts=%u "
                     "last_join_burst_ms=%u last_join_burst_npcs=%u limits_ms=%u/%u/%u stretch_permille=%u",
               owned.size(), streams, sendStats.edges, sendStats.heartbeats, sendStats.initials, sendStats.paced, bursts,
               lastBurstMs, lastBurstNpcs, npcsend::FastMaxStaleMs, npcsend::OrdinaryMaxStaleMs,
               npcsend::SlowMaxStaleMs, stretchPermille);
        log(line);
        sendStats = npcsend::Stats{};
    } else {
        unsigned gaps = 0, gapMin = UINT32_MAX, gapMax = 0, older150 = 0, older400 = 0, older1000 = 0, live = 0;
        uint64_t gapSum = 0;
        for (auto& entry : mirrors) {
            auto& track = entry.second.body.track;
            if (!track.count) continue;
            ++live;
            const uint32_t age = now - track.lastArrived;
            if (age > 150) ++older150;
            if (age > 400) ++older400;
            if (age > 1000) ++older1000;
            gaps += track.gapCount;
            gapSum += track.gapSumMs;
            if (track.gapCount) gapMin = std::min(gapMin, track.gapMinMs);
            gapMax = std::max(gapMax, track.gapMaxMs);
            track.gapCount = 0; track.gapSumMs = 0; track.gapMinMs = UINT32_MAX; track.gapMaxMs = 0;
        }
        Format(line, "SYNCSTREAM role=client mirrors=%u updates=%u gap_min_ms=%u gap_avg_ms=%.1f gap_max_ms=%u "
                     "older_150ms=%u older_400ms=%u older_1000ms=%u",
               live, gaps, gaps ? gapMin : 0u, gaps ? static_cast<double>(gapSum) / gaps : 0.0, gapMax,
               older150, older400, older1000);
        log(line);
    }
    Format(line, "SYNCRAGDOLL hooked=%d forces_sent=%u forces_received=%u forces_applied=%u forces_late=%u ragdolls_started=%u not_ready=%u gravity_deaths=%u",
           ragdollHookInstalled ? 1 : 0, ragdollForcesSent, ragdollForcesReceived, ragdollForcesApplied,
           ragdollForcesLate, ragdollsStarted, ragdollsNotReady, ragdollGravityDeaths);
    log(line);
}

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

void DeadPoseOff(LogFn log)
{
    deadPose = false;
    log("DEADPOSEOFF ok=1");
}

void DeadPoseReport(LogFn log)
{
    char line[700];
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
               m.entity.GetName().GetText(), m.killed ? 1 : 0, body::MotionRunning(m.entity.GetInstance()) ? 1 : 0,
               body::BoundaryHeight(m.entity.GetInstance()), static_cast<int>(m.entity.Routine.AniState),
               *shows ? shows : "-", slots, kind, animation && animation->GetRagDollEnabled() ? 1 : 0,
               processed ? 1 : 0, m.entity.Routine.GetCurrentTask().GetText());
        log(line);
    };
    for (auto& entry : mirrors) {
        Mirror& m = entry.second;
        if (!m.entity.GetInstance() || (!m.killed && alive++ >= 3)) continue;
        report(m, "mirror");
    }
    for (auto& entry : corpses) {
        Mirror& c = entry.second;
        c.entity = Resolve(c.latest.guid);
        if (c.entity.GetInstance()) report(c, "journal");
    }
    Format(line, "DEADPOSE enabled=%d laid=%u loops=%u slots_stopped=%u ragdolls=%u ragdolls_not_ready=%u",
           deadPose ? 1 : 0, corpsesLaid, corpseLoops, slotsStopped, ragdollsStarted, ragdollsNotReady);
    log(line);
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
    const GuidKey key = ToGuidKey(reinterpret_cast<uint8_t const*>(&raw));
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

void ClaimRecord(char const* text, LogFn log)
{
    eCEntity* hero = gCSession::GetInstance().GetHero();
    unsigned d1, d2, d3, b[8];
    if (!hero || net::IsHost() || !text ||
        sscanf_s(text, "%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X", &d1, &d2, &d3,
                 &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) {
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

} // namespace g3t::npcs
