// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot.h"
#include "npcsend_policy.h"
#include "combat_timing.h"
#include "ragdoll_force.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3t::body {
bool offsetWindow = true;
bool arrivalSpeed = false;
unsigned sampledBetween = 0, sampledAhead = 0, sampledHeld = 0, sampledOldest = 0;
namespace {
constexpr uint32_t OffsetWindow = 2000;
constexpr uint32_t RestartAfter = 1500;
constexpr std::size_t MaxPendingEvents = 512u;

void PushPendingEvent(Track& track, Track::Snap const& snap)
{
    track.pendingEvents.push_back(snap);
    while (track.pendingEvents.size() > MaxPendingEvents)
        track.pendingEvents.pop_front();
}

bool EventChanged(Track const& track, MotionState const& a, MotionState const& b)
{
    if (a.flags != b.flags || a.motion != b.motion) return true;
    if (track.predictive) return a.actionSeq != b.actionSeq;
    if (a.incarnation != b.incarnation || a.hitSequence != b.hitSequence || a.hitPlayer != b.hitPlayer ||
        a.contact != b.contact || a.action != b.action || a.combatTarget != b.combatTarget) return true;
    for (int i = 0; i < net::NpcMotionSlots; ++i) {
        auto const& x = a.motions[i];
        auto const& y = b.motions[i];
        if (x.active != y.active || ((x.active || y.active) && (x.name != y.name || x.instance != y.instance)))
            return true;
    }
    return false;
}

int32_t RequiredDelay(Track const& track)
{
    const uint32_t measured = std::min(track.cadenceMin, track.previousCadenceMin);
    const uint32_t cadence = !track.predictive && track.count < 2
        ? (track.poseIntervalMs ? track.poseIntervalMs : 17u)
        : !track.predictive && track.poseIntervalMs
        ? std::max(track.poseIntervalMs, measured) : measured;
    return std::clamp(static_cast<int32_t>(2 * cadence + std::ceil(track.jitterMs * 2.0f + track.recentTransitMs)),
                      track.predictive ? PlayerBufferFloor : NpcBufferFloor, MaximumBufferDelay);
}

void AdvanceAdaptive(Track& track, uint32_t now)
{
    if (!track.haveAdaptiveTime) {
        track.haveAdaptiveTime = true;
        track.adaptiveAt = now;
        return;
    }
    const int32_t elapsed = static_cast<int32_t>(now - track.adaptiveAt);
    if (elapsed <= 0) return;
    track.adaptiveAt = now;
    track.recentTransitMs = std::max(0.0f, track.recentTransitMs - static_cast<float>(elapsed) * 0.1f);
    const int32_t required = RequiredDelay(track);
    if (required >= track.delayMs) {
        track.delayMs = required;
        track.delayDecayCredit = 0.0;
    } else if (!track.recoveryUntil || static_cast<int32_t>(now - track.recoveryUntil) >= 0) {
        track.delayDecayCredit += elapsed * 0.1;
        const int32_t decay = std::min(track.delayMs - required, static_cast<int32_t>(track.delayDecayCredit));
        track.delayMs -= decay;
        track.delayDecayCredit -= decay;
        if (track.delayMs == required) track.delayDecayCredit = 0.0;
    }
}

bool IncomingCombatAt(Track const& track, uint32_t sent)
{
    if (track.predictive || !track.combatPlayer || !track.count) return false;
    auto incoming = [&](Track::Snap const& snap) {
        return !snap.motion.nonAttackCombat &&
               combat_timing::IncomingCombat(snap.motion.flags, snap.motion.combatTarget, track.combatPlayer);
    };
    Track::Snap const* atSent = track.haveRetained && static_cast<int32_t>(sent - track.snaps[0].sent) < 0
        ? &track.retained : &track.snaps[0];
    for (int i = track.count - 1; i >= 0; --i) {
        if (static_cast<int32_t>(sent - track.snaps[i].sent) >= 0) {
            atSent = &track.snaps[i];
            break;
        }
    }
    const bool passiveAtSent = track.havePresentedMotion
        ? track.presentedMotion.nonAttackCombat
        : atSent->motion.nonAttackCombat;
    auto withinPreAttackHorizon = [&](uint32_t futureSent) {
        return !passiveAtSent || static_cast<int32_t>(sent - futureSent) >= 0;
    };
    // Protect before the start is displayed, even if its pose was evicted; when currently showing
    // non-attack locomotion/recovery/stumble after a large stall, catch up until approaching the swing.
    for (auto const& event : track.pendingEvents)
        if (incoming(event) && withinPreAttackHorizon(event.sent)) return true;
    if (track.havePresentedMotion && !track.presentedMotion.nonAttackCombat &&
        combat_timing::IncomingCombat(track.presentedMotion.flags, track.presentedMotion.combatTarget, track.combatPlayer))
        return true;
    if (incoming(track.snaps[track.count - 1]) && withinPreAttackHorizon(track.snaps[track.count - 1].sent)) return true;
    return incoming(*atSent);
}

uint32_t RenderTime(Track& track, uint32_t now, int32_t shift, bool counted)
{
    const uint32_t desired = now - static_cast<uint32_t>(track.offset) + static_cast<uint32_t>(shift);
    const int32_t rawDelta = static_cast<int32_t>(now - track.renderNow);
    const uint32_t elapsed = rawDelta > 0 ? static_cast<uint32_t>(rawDelta) : 0u;
    if (!counted) {
        uint32_t result = !track.haveRenderTime ? desired
            : track.renderSent + elapsed + static_cast<uint32_t>(shift - track.renderShift);
        if (!track.pendingEvents.empty() && static_cast<int32_t>(result - track.pendingEvents.front().sent) >= 0)
            result = track.pendingEvents.front().sent;
        return result;
    }
    const bool hadRenderTime = track.haveRenderTime;
    const uint32_t previousRender = track.renderSent;
    track.combatClockProtected = IncomingCombatAt(track, track.haveRenderTime ? track.renderSent : desired);
    track.catchUp = false;
    if (!track.haveRenderTime) {
        track.renderSent = desired;
        if (!track.pendingEvents.empty() &&
            static_cast<int32_t>(desired - track.pendingEvents.front().sent) >= 0)
            track.renderSent = track.haveRetained ? track.retained.sent : track.snaps[0].sent;
        track.fractionalAdvance = 0.0;
        track.haveRenderTime = true;
    } else {
        // Slew clock-offset changes instead of rewinding or jumping the presentation timeline.
        const int32_t error = static_cast<int32_t>(desired - (track.renderSent + elapsed));
        const double catchUpRate = error > 100 ? 1.0 : (track.combatPlayer && error > 35 ? 0.5 : 0.1);
        const double maximumAdjustment = elapsed * (!track.combatClockProtected ? catchUpRate : 0.1);
        const double advance = elapsed + std::clamp(static_cast<double>(error), -maximumAdjustment,
                                                    track.combatClockProtected ? 0.0 : maximumAdjustment)
                               + track.fractionalAdvance;
        const uint32_t whole = static_cast<uint32_t>(advance);
        track.renderSent += whole;
        track.fractionalAdvance = advance - whole;
        if (!track.combatClockProtected && error > 5000 && track.count &&
            static_cast<int32_t>(track.snaps[track.count - 1].sent - track.renderSent) > 4000) {
            track.renderSent = track.snaps[track.count - 1].sent -
                static_cast<uint32_t>(std::clamp(track.delayMs, 0, 250));
            track.fractionalAdvance = 0.0;
            while (!track.pendingEvents.empty() &&
                   static_cast<int32_t>(track.renderSent - track.pendingEvents.front().sent) > 0)
                track.pendingEvents.pop_front();
        }
    }
    if (!track.pendingEvents.empty() &&
        static_cast<int32_t>(track.renderSent - track.pendingEvents.front().sent) >= 0) {
        const uint32_t boundary = track.pendingEvents.front().sent;
        track.renderSent = hadRenderTime && static_cast<int32_t>(previousRender - boundary) > 0
            ? previousRender : boundary;
        track.fractionalAdvance = 0.0;
    }
    // Normal display clocks and incoming-combat windup holds both stop at the newest complete snapshot.
    if ((track.combatClockProtected || shift <= 0) &&
        static_cast<int32_t>(track.renderSent - track.snaps[track.count - 1].sent) > 0) {
        const uint32_t newest = track.snaps[track.count - 1].sent;
        track.renderSent = hadRenderTime && static_cast<int32_t>(previousRender - newest) > 0 ? previousRender : newest;
        track.fractionalAdvance = 0.0;
    }
    const uint32_t newest = track.snaps[track.count - 1].sent;
    const uint32_t availableTarget = static_cast<int32_t>(desired - newest) > 0 ? newest : desired;
    const int32_t remaining = static_cast<int32_t>(availableTarget - track.renderSent);
    track.backlogMs = std::max(0, remaining);
    if (hadRenderTime && elapsed && !track.combatClockProtected &&
        static_cast<int32_t>(track.renderSent - previousRender) > static_cast<int32_t>(elapsed * 1.1)) {
        track.catchUp = true;
        ++track.catchUpFrames;
    }
    track.renderNow = now;
    track.renderShift = shift;
    return track.renderSent;
}

void Describe(Presentation* out, Track::Snap const& state, float speed, uint32_t sent)
{
    if (!out) return;
    out->movementMode = state.movementMode;
    out->speed = speed;
    out->sent = sent;
    out->stateSent = state.sent;
    out->motion = state.motion;
}

void Interpolate(Track const& track, Track::Snap const& a, Track::Snap const& b, uint32_t render,
                 float position[3], float rotation[4], Presentation* out)
{
    const int32_t span = static_cast<int32_t>(b.sent - a.sent);
    const float fraction = std::clamp(static_cast<float>(static_cast<int32_t>(render - a.sent)) / span, 0.0f, 1.0f);
    const bool holdPreDeath = ragdoll::ShouldHoldPreDeathTransform(
        a.motion.flags, static_cast<uint8_t>(a.movementMode), b.motion.flags, static_cast<uint8_t>(b.movementMode), fraction);
    const float poseFraction = holdPreDeath ? 0.0f : fraction;
    for (int k = 0; k < 3; ++k) position[k] = a.position[k] + (b.position[k] - a.position[k]) * poseFraction;
    float dot = 0.0f, length = 0.0f;
    for (int k = 0; k < 4; ++k) dot += a.rotation[k] * b.rotation[k];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) {
        rotation[k] = a.rotation[k] * (1.0f - poseFraction) + sign * b.rotation[k] * poseFraction;
        length += rotation[k] * rotation[k];
    }
    length = std::sqrt(length);
    if (length > 0.0f) for (int k = 0; k < 4; ++k) rotation[k] /= length;
    const float travelSpeed = (!holdPreDeath && span > 0)
        ? std::min(MaximumTravelSpeed, Distance(a.position, b.position) * 1000.0f / span)
        : 0.0f;
    Describe(out, fraction < 1.0f ? a : b, travelSpeed, render);
    if (out && !track.predictive) {
        for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
            auto const& first = a.motion.motions[slot];
            auto const& second = b.motion.motions[slot];
            if (!first.active) continue;
            auto& shown = out->motion.motions[slot];
            if (!second.active || first.name != second.name || first.instance != second.instance) {
                if (slot == 0 && fraction < 1.0f && a.motion.contact && !b.motion.contact && first.speed >= 0.0f)
                    shown.phase = std::clamp(first.phase + (1.0f - first.phase) * fraction, 0.0f, 1.0f);
                continue;
            }
            float phase = second.phase - first.phase;
            if (first.speed >= 0.0f && phase < 0.0f) phase += 1.0f;
            if (first.speed < 0.0f && phase > 0.0f) phase -= 1.0f;
            shown.phase = first.phase + phase * fraction;
            if (shown.phase > 1.0f) shown.phase -= 1.0f;
            if (shown.phase < 0.0f) shown.phase += 1.0f;
            shown.weight = first.weight + (second.weight - first.weight) * fraction;
        }
    }
}
}

float Distance(float const a[3], float const b[3])
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

void SetPoseInterval(Track& track, uint32_t interval)
{
    if (track.predictive || interval < 16) return;
    if (interval == npcsend::QuietCadenceMs) {
        // Stationary keepalive keyframes report QuietCadenceMs (250 ms) even while an NPC
        // remains in its 16/33 ms combat tier; preserve the active tier instead of inflating
        // playback delay to 200 ms.
        track.quietHold = true;
        if (!track.poseIntervalMs) track.poseIntervalMs = 16;
        return;
    }
    track.quietHold = false;
    const uint32_t clamped = std::min<uint32_t>(interval, 100u);
    if (clamped == track.poseIntervalMs) return;
    track.poseIntervalMs = clamped;
    // Escalate the buffer immediately on a slower tier. Faster tiers still
    // decay gradually, without rewinding the presentation clock.
    track.delayMs = std::max(track.delayMs, static_cast<int32_t>(clamped * 2));
}

void SoftResetTrack(Track& track, bool preserveOffset)
{
    const uint32_t serial = track.serial + 1;
    const bool predictive = track.predictive;
    const uint8_t combatPlayer = track.combatPlayer;
    const uint32_t interval = track.poseIntervalMs;
    const unsigned underruns = track.underruns;
    const unsigned catchUpFrames = track.catchUpFrames;
    const uint32_t gapCount = track.gapCount, gapMinMs = track.gapMinMs, gapMaxMs = track.gapMaxMs;
    const uint64_t gapSumMs = track.gapSumMs;
    const bool haveOffset = preserveOffset && track.haveOffset;
    const int32_t offset = track.offset;
    const uint32_t lastArrived = preserveOffset ? track.lastArrived : 0;
    track = Track{};
    track.serial = serial;
    track.predictive = predictive;
    track.combatPlayer = combatPlayer;
    track.underruns = underruns;
    track.catchUpFrames = catchUpFrames;
    track.gapCount = gapCount;
    track.gapMinMs = gapMinMs;
    track.gapMaxMs = gapMaxMs;
    track.gapSumMs = gapSumMs;
    track.haveOffset = haveOffset;
    track.offset = offset;
    track.lastArrived = lastArrived;
    SetPoseInterval(track, interval);
}

void Feed(Track& track, float const position[3], float const rotation[4], uint32_t sent, uint32_t arrived, int movementMode,
          MotionState const& motion)
{
    if (track.count) {
        Track::Snap const& last = track.snaps[track.count - 1];
        const int32_t span = static_cast<int32_t>(sent - last.sent);
        if (span < 0) return;
        if (!span) {
            if (Distance(last.position, position) <= 300.0f) {
                Track::Snap& state = track.snaps[track.count - 1];
                const bool edge = EventChanged(track, state.motion, motion);
                if (edge && !track.haveRenderTime && track.count == 1 && track.pendingEvents.empty())
                    PushPendingEvent(track, state);
                std::memcpy(state.position, position, sizeof(state.position));
                std::memcpy(state.rotation, rotation, sizeof(state.rotation));
                state.movementMode = movementMode;
                state.motion = motion;
                if (edge) PushPendingEvent(track, state);
                else if (!track.pendingEvents.empty() && track.pendingEvents.back().sent == sent)
                    track.pendingEvents.back() = state;
                return;
            }
            SoftResetTrack(track, true);
        } else if (static_cast<uint32_t>(span) > RestartAfter ||
                   Distance(last.position, position) > 300.0f + MaximumTravelSpeed * span / 1000.0f) {
            const bool clockStall = static_cast<uint32_t>(span) > RestartAfter;
            SoftResetTrack(track, !clockStall);
        } else if (track.predictive ||
                   (!track.quietHold && !track.lastQuietHold &&
                    static_cast<uint32_t>(span) <= npcsend::QuietCadenceMs)) {
            // Sparse idle NPC keyframes are intentional, not evidence of network jitter.
            const uint32_t interval = std::clamp(static_cast<uint32_t>(span), 8u, 100u);
            if (!track.haveCadenceWindow) {
                track.haveCadenceWindow = true;
                track.cadenceWindowAt = arrived;
                track.cadenceMin = interval;
                track.previousCadenceMin = interval;
            } else if (static_cast<int32_t>(arrived - track.cadenceWindowAt) >= static_cast<int32_t>(OffsetWindow)) {
                track.previousCadenceMin = track.cadenceMin;
                track.cadenceMin = interval;
                track.cadenceWindowAt = arrived;
            } else {
                track.cadenceMin = std::min(track.cadenceMin, interval);
            }
            const int32_t arrivalSpan = static_cast<int32_t>(arrived - track.lastArrived);
            if (arrivalSpan >= 0) {
                const float variation = static_cast<float>(std::min(std::abs(static_cast<double>(arrivalSpan) - span), 250.0));
                track.jitterMs += (variation - track.jitterMs) / 16.0f;
            }
        }
    }
    if (!track.count && !track.predictive)
        track.delayMs = std::max(NpcBufferFloor, static_cast<int32_t>(track.poseIntervalMs * 2));
    if (track.count) {
        const int32_t gap = static_cast<int32_t>(arrived - track.lastArrived);
        if (gap >= 0) {
            ++track.gapCount;
            track.gapSumMs += static_cast<uint32_t>(gap);
            track.gapMinMs = std::min(track.gapMinMs, static_cast<uint32_t>(gap));
            track.gapMaxMs = std::max(track.gapMaxMs, static_cast<uint32_t>(gap));
        }
    }
    track.lastArrived = arrived;
    const int32_t offset = static_cast<int32_t>(arrived - sent);
    if (!track.haveOffset) {
        track.offset = offset;
        track.haveOffset = true;
    } else if (static_cast<int32_t>(static_cast<uint32_t>(offset) - static_cast<uint32_t>(track.offset)) < 0) {
        // The lifetime minimum is a clock reference, not a moving network queue.
        track.offset = offset;
    } else if (!offsetWindow) {
        ++track.offset; // QA negative control: the legacy per-snapshot clock drift.
    }
    track.transitExcessMs = std::max(0, static_cast<int32_t>(static_cast<uint32_t>(offset) -
                                                              static_cast<uint32_t>(track.offset)));
    AdvanceAdaptive(track, arrived);
    track.recentTransitMs = std::max(track.recentTransitMs,
                                    static_cast<float>(std::min(track.transitExcessMs, MaximumBufferDelay)));
    const int32_t required = RequiredDelay(track);
    if (required > track.delayMs) {
        track.delayMs = required;
        track.delayDecayCredit = 0.0;
    }
    if (track.count == SnapshotCount) {
        Track::Snap const& oldest = track.snaps[0];
        if (!track.haveRetained ||
            (track.haveRenderTime && static_cast<int32_t>(track.renderSent - oldest.sent) >= 0 &&
             (track.pendingEvents.empty() || static_cast<int32_t>(oldest.sent - track.pendingEvents.front().sent) < 0))) {
            track.retained = oldest;
            track.haveRetained = true;
        }
        for (int i = 1; i < SnapshotCount; ++i) track.snaps[i - 1] = track.snaps[i];
        --track.count;
    }
    const bool edge = track.count && EventChanged(track, track.snaps[track.count - 1].motion, motion);
    Track::Snap& state = track.snaps[track.count++];
    state.sent = sent;
    state.movementMode = movementMode;
    state.motion = motion;
    std::memcpy(state.position, position, sizeof(state.position));
    std::memcpy(state.rotation, rotation, sizeof(state.rotation));
    track.lastQuietHold = track.quietHold;
    if (edge) PushPendingEvent(track, state);
}

namespace {
void SampleNpc(Track& track, Track::Snap const& newest, uint32_t now, int32_t shift, bool counted,
               float position[3], float rotation[4], Presentation* out)
{
    std::memcpy(position, newest.position, sizeof(newest.position));
    std::memcpy(rotation, newest.rotation, sizeof(newest.rotation));
    Describe(out, newest, 0.0f, newest.sent);
    if (counted) {
        ++sampledHeld, track.lastCase = 3;
        const uint32_t desired = now - static_cast<uint32_t>(track.offset) + static_cast<uint32_t>(shift);
        const int32_t late = std::max(0, static_cast<int32_t>(desired - newest.sent));
        const uint32_t measuredCadence = std::min(track.cadenceMin, track.previousCadenceMin);
        const uint32_t interval = track.poseIntervalMs ? std::max(track.poseIntervalMs, measuredCadence) : measuredCadence;
        const bool stationaryHold = track.count >= 2 &&
            (track.quietHold ||
             (Distance(track.snaps[track.count - 2].position, newest.position) < npcsend::RootChangeCm &&
              !newest.motion.contact));
        const uint32_t expectedGap = stationaryHold ? std::max<uint32_t>(interval, npcsend::QuietCadenceMs) : interval;
        // A whole missing interval, not a rounding-boundary hold or intentional stationary keepalive pause, warrants more buffering.
        if (track.count >= 2 && late > static_cast<int32_t>(expectedGap) && !(newest.motion.flags & 1) &&
            (!track.underrunAt || now - track.underrunAt >= 100)) {
            track.underrunAt = now;
            track.recoveryUntil = now + OffsetWindow;
            track.delayMs = std::min(track.delayMs + static_cast<int32_t>(interval), MaximumBufferDelay);
            ++track.underruns;
        }
    }
}

bool SamplePlayer(Track& track, Track::Snap const& newest, uint32_t now, uint32_t render, int32_t shift, bool counted,
                  float position[3], float rotation[4], Presentation* out)
{
    const uint32_t desired = now - static_cast<uint32_t>(track.offset) + static_cast<uint32_t>(shift);
    const int32_t wanted = shift <= 0
        ? std::max(0, static_cast<int32_t>(desired - newest.sent))
        : static_cast<int32_t>(render - newest.sent);
    const int32_t ahead = std::min(wanted, MaxAhead);
    if (counted) {
        if (wanted > MaxAhead) {
            ++sampledHeld, track.lastCase = 3;
            if (!track.underrunAt || now - track.underrunAt >= 100) {
                track.underrunAt = now;
                track.recoveryUntil = now + OffsetWindow;
                track.delayMs = std::min(track.delayMs + 25, MaximumBufferDelay);
                ++track.underruns;
            }
        }
        else ++sampledAhead, track.lastCase = 2;
    }
    std::memcpy(position, newest.position, sizeof(newest.position));
    std::memcpy(rotation, newest.rotation, sizeof(newest.rotation));
    float speed = 0.0f;
    bool canPredict = false;
    if (track.count >= 2) {
        Track::Snap const& before = track.snaps[track.count - 2];
        const int32_t span = static_cast<int32_t>(newest.sent - before.sent);
        if (span > 0 && span <= static_cast<int32_t>(RestartAfter) && newest.movementMode != 1 &&
            newest.motion.locomotion && !(newest.motion.flags & 0x1eu)) {
            canPredict = true;
            speed = Distance(before.position, newest.position) * 1000.0f / span;
            const float scale = speed > MaximumTravelSpeed ? MaximumTravelSpeed / speed : 1.0f;
            for (int k = 0; k < 3; ++k)
                position[k] += (newest.position[k] - before.position[k]) * ahead / span * scale;
            speed = std::min(speed, MaximumTravelSpeed);
        }
    }
    if (wanted > MaxAhead) speed = 0.0f;
    const bool predicted = canPredict && ahead > 0;
    Describe(out, newest, speed, newest.sent + static_cast<uint32_t>(canPredict ? ahead : 0));
    return predicted;
}

bool FinalizeSample(Track& track, uint32_t now, uint32_t render, bool counted, bool predicted,
                    bool eventPresentation, float position[3], Presentation const& out)
{
    if (!counted) return true;
    if (track.predictive) {
        const bool moving = out.movementMode != 1 && out.motion.locomotion && !(out.motion.flags & 0x1eu);
        if (!moving || eventPresentation) track.playerReconciling = false;
        else if (track.havePlayerPosition && track.playerWasPredicted && !predicted)
            track.playerReconciling = true;
        if (track.playerReconciling) {
            // Ease back from the bounded prediction when delayed samples resume.
            const int32_t elapsed = static_cast<int32_t>(now - track.lastPlayerNow);
            const float distance = Distance(track.lastPlayerPosition, position);
            const float step = MaximumTravelSpeed * static_cast<float>(std::clamp(elapsed, 0, 1000)) / 1000.0f;
            if (distance > step && distance > 0.0f) {
                for (int k = 0; k < 3; ++k)
                    position[k] = track.lastPlayerPosition[k] +
                                  (position[k] - track.lastPlayerPosition[k]) * step / distance;
            } else track.playerReconciling = false;
        }
        std::memcpy(track.lastPlayerPosition, position, sizeof(track.lastPlayerPosition));
        track.lastPlayerNow = now;
        track.havePlayerPosition = true;
        track.playerWasPredicted = predicted;
    }
    track.presentedMotion = out.motion;
    track.havePresentedMotion = true;
    if (track.haveRetained && static_cast<int32_t>(render - track.snaps[0].sent) >= 0 &&
        (track.pendingEvents.empty() || static_cast<int32_t>(track.pendingEvents.front().sent - track.snaps[0].sent) >= 0))
        track.haveRetained = false;
    return true;
}
}

bool Sample(Track& track, uint32_t now, float position[3], float rotation[4], int32_t shift, bool counted, Presentation* presentation)
{
    if (counted) track.lastCase = 0;
    if (!track.count) return false;
    if (counted) AdvanceAdaptive(track, now);
    const uint32_t render = RenderTime(track, now, shift, counted);
    Presentation local;
    Presentation* out = presentation ? presentation : &local;
    if (!track.pendingEvents.empty() && static_cast<int32_t>(render - track.pendingEvents.front().sent) >= 0) {
        Track::Snap const event = track.pendingEvents.front();
        std::memcpy(position, event.position, sizeof(event.position));
        std::memcpy(rotation, event.rotation, sizeof(event.rotation));
        Describe(out, event, 0.0f, event.sent);
        if (counted) {
            track.retained = event;
            track.haveRetained = true;
            track.pendingEvents.pop_front();
            ++sampledBetween;
            track.lastCase = 1;
        }
        return FinalizeSample(track, now, render, counted, false, true, position, *out);
    }
    Track::Snap const& newest = track.snaps[track.count - 1];
    if (static_cast<int32_t>(render - newest.sent) >= 0) {
        bool predicted = false;
        if (!track.predictive) SampleNpc(track, newest, now, shift, counted, position, rotation, out);
        else predicted = SamplePlayer(track, newest, now, render, shift, counted, position, rotation, out);
        return FinalizeSample(track, now, render, counted, predicted, false, position, *out);
    }
    for (int i = track.count - 1; i > 0; --i) {
        Track::Snap const& a = track.snaps[i - 1];
        Track::Snap const& b = track.snaps[i];
        if (static_cast<int32_t>(render - a.sent) < 0) continue;
        Interpolate(track, a, b, render, position, rotation, out);
        if (counted) ++sampledBetween, track.lastCase = 1;
        return FinalizeSample(track, now, render, counted, false, false, position, *out);
    }
    if (track.haveRetained && static_cast<int32_t>(render - track.retained.sent) >= 0) {
        Track::Snap const* next = &track.snaps[0];
        if (!track.pendingEvents.empty() &&
            static_cast<int32_t>(track.pendingEvents.front().sent - next->sent) < 0)
            next = &track.pendingEvents.front();
        if (static_cast<int32_t>(next->sent - track.retained.sent) > 0) {
            Interpolate(track, track.retained, *next, render, position, rotation, out);
            if (counted) ++sampledBetween, track.lastCase = 1;
            return FinalizeSample(track, now, render, counted, false, false, position, *out);
        }
    }
    if (counted) ++sampledOldest, track.lastCase = 4;
    Track::Snap const& oldest = track.haveRetained ? track.retained : track.snaps[0];
    std::memcpy(position, oldest.position, sizeof(oldest.position));
    std::memcpy(rotation, oldest.rotation, sizeof(oldest.rotation));
    Describe(out, oldest, 0.0f, oldest.sent);
    return FinalizeSample(track, now, render, counted, false, false, position, *out);
}

int32_t PlaybackDelay(Track const& track) { return track.delayMs; }

std::size_t PendingEvents(Track const& track) { return track.pendingEvents.size(); }

uint32_t OldestRetainedSent(Track const& track)
{
    if (!track.count) return 0;
    uint32_t oldest = track.snaps[0].sent;
    if (track.haveRetained && static_cast<int32_t>(track.retained.sent - oldest) < 0) oldest = track.retained.sent;
    if (!track.pendingEvents.empty() && static_cast<int32_t>(track.pendingEvents.front().sent - oldest) < 0)
        oldest = track.pendingEvents.front().sent;
    return oldest;
}

bool IsLocomotionMotion(char const* motion)
{
    return motion && (std::strstr(motion, "_Move_Walk_") || std::strstr(motion, "_Move_Run_") ||
                      std::strstr(motion, "_Move_Sprint_") || std::strstr(motion, "_Move_Sneak_"));
}

float Speed(float const from[3], float const to[3], uint32_t sentFrom, uint32_t sentTo, uint32_t arrivedFrom, uint32_t arrivedTo)
{
    const uint32_t interval = arrivalSpeed ? arrivedTo - arrivedFrom : sentTo - sentFrom;
    return interval ? Distance(from, to) * 1000.0f / interval : 0.0f;
}
}
