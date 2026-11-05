// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot.h"

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
constexpr int32_t MinDelay = 50;  // ms: three 60 Hz intervals; a lost packet at 60 Hz leaves a 33 ms hole
constexpr float MaximumTravelSpeed = 3000.0f;  // cm/s; larger jumps are discontinuities, not interpolation paths

uint32_t RenderTime(Track& track, uint32_t now, int32_t shift, bool counted)
{
    const uint32_t desired = now - static_cast<uint32_t>(track.offset) + static_cast<uint32_t>(shift);
    if (!counted) {
        if (!track.haveRenderTime) return desired;
        return track.renderSent + (now - track.renderNow) + static_cast<uint32_t>(shift - track.renderShift);
    }
    const uint32_t elapsed = now - track.renderNow;
    if (!track.haveRenderTime || elapsed > RestartAfter) {
        track.renderSent = desired;
        track.fractionalAdvance = 0.0;
        track.haveRenderTime = true;
    } else {
        // Slew clock-offset changes instead of rewinding or jumping the presentation timeline.
        const int32_t error = static_cast<int32_t>(desired - (track.renderSent + elapsed));
        const double maximumAdjustment = elapsed * 0.1;
        const double advance = elapsed + std::clamp(static_cast<double>(error), -maximumAdjustment, maximumAdjustment)
                               + track.fractionalAdvance;
        const uint32_t whole = static_cast<uint32_t>(advance);
        track.renderSent += whole;
        track.fractionalAdvance = advance - whole;
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
}

float Distance(float const a[3], float const b[3])
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

void Feed(Track& track, float const position[3], float const rotation[4], uint32_t sent, uint32_t arrived, int movementMode,
          MotionState const& motion)
{
    if (track.count) {
        Track::Snap const& last = track.snaps[track.count - 1];
        const int32_t span = static_cast<int32_t>(sent - last.sent);
        if (span < 0) return;
        if (!span) {
            Track::Snap& state = track.snaps[track.count - 1];
            std::memcpy(state.position, position, sizeof(state.position));
            std::memcpy(state.rotation, rotation, sizeof(state.rotation));
            state.movementMode = movementMode;
            state.motion = motion;
            return;
        }
        if (static_cast<uint32_t>(span) > RestartAfter ||
            Distance(last.position, position) > 300.0f + MaximumTravelSpeed * span / 1000.0f) {
            const uint32_t serial = track.serial + 1;
            const bool predictive = track.predictive;
            track = Track{};
            track.serial = serial;
            track.predictive = predictive;
        } else {
            const uint32_t interval = std::clamp(static_cast<uint32_t>(span), 8u, 100u);
            if (arrived - track.cadenceWindowAt >= OffsetWindow) {
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
            const int32_t required = std::clamp(static_cast<int32_t>(2 * std::min(track.cadenceMin, track.previousCadenceMin)
                                                                   + std::ceil(track.jitterMs * 2.0f)), MinDelay, 250);
            if (required > track.delayMs) track.delayMs = required;
            else if ((!track.recoveryUntil || static_cast<int32_t>(arrived - track.recoveryUntil) >= 0) && track.delayMs > required)
                --track.delayMs;
        }
    }
    track.lastArrived = arrived;
    const int32_t offset = static_cast<int32_t>(arrived - sent);
    if (!track.haveOffset) {
        track.offset = track.windowMin = track.lastWindowMin = offset;
        track.windowAt = arrived;
        track.haveOffset = true;
    } else if (offsetWindow) {
        if (arrived - track.windowAt >= OffsetWindow) {
            track.lastWindowMin = track.windowMin;
            track.windowMin = offset;
            track.windowAt = arrived;
        } else if (offset < track.windowMin) {
            track.windowMin = offset;
        }
        track.offset = std::min(track.windowMin, track.lastWindowMin);
    } else if (offset < track.offset) {
        track.offset = offset;
    } else {
        ++track.offset;
    }
    if (track.count == SnapshotCount) {
        for (int i = 1; i < SnapshotCount; ++i) track.snaps[i - 1] = track.snaps[i];
        --track.count;
    }
    Track::Snap& state = track.snaps[track.count++];
    state.sent = sent;
    state.movementMode = movementMode;
    state.motion = motion;
    std::memcpy(state.position, position, sizeof(state.position));
    std::memcpy(state.rotation, rotation, sizeof(state.rotation));
}

bool Sample(Track& track, uint32_t now, float position[3], float rotation[4], int32_t shift, bool counted, Presentation* presentation)
{
    if (counted) track.lastCase = 0;
    if (!track.count) return false;
    const uint32_t render = RenderTime(track, now, shift, counted);
    Track::Snap const& newest = track.snaps[track.count - 1];
    if (static_cast<int32_t>(render - newest.sent) >= 0) {
        if (!track.predictive) {
            std::memcpy(position, newest.position, sizeof(newest.position));
            std::memcpy(rotation, newest.rotation, sizeof(newest.rotation));
            Describe(presentation, newest, 0.0f, newest.sent);
            if (counted) ++sampledHeld, track.lastCase = 3;
            return true;
        }
        const int32_t wanted = static_cast<int32_t>(render - newest.sent);
        const int32_t ahead = std::min(wanted, MaxAhead);
        if (counted) {
            if (wanted > MaxAhead) {
                ++sampledHeld, track.lastCase = 3;
                if (!track.underrunAt || now - track.underrunAt >= 100) {
                    track.underrunAt = now;
                    track.recoveryUntil = now + OffsetWindow;
                    track.delayMs = std::min(track.delayMs + 25, 250);
                }
            }
            else ++sampledAhead, track.lastCase = 2;
        }
        std::memcpy(position, newest.position, sizeof(newest.position));
        std::memcpy(rotation, newest.rotation, sizeof(newest.rotation));
        float speed = 0.0f;
        if (track.count >= 2) {
            Track::Snap const& before = track.snaps[track.count - 2];
            const int32_t span = static_cast<int32_t>(newest.sent - before.sent);
            if (span > 0 && span <= static_cast<int32_t>(RestartAfter)) {
                speed = Distance(before.position, newest.position) * 1000.0f / span;
                if (newest.movementMode != 1)  // standing must not continue its previous movement after a stop
                    for (int k = 0; k < 3; ++k) position[k] += (newest.position[k] - before.position[k]) * ahead / span;
                else speed = 0.0f;
            }
        }
        if (wanted > MaxAhead) speed = 0.0f;
        Describe(presentation, newest, speed, newest.sent + static_cast<uint32_t>(ahead));
        return true;
    }
    for (int i = track.count - 1; i > 0; --i) {
        Track::Snap const& a = track.snaps[i - 1];
        Track::Snap const& b = track.snaps[i];
        if (static_cast<int32_t>(render - a.sent) < 0) continue;
        const int32_t span = static_cast<int32_t>(b.sent - a.sent);
        const float fraction = static_cast<float>(static_cast<int32_t>(render - a.sent)) / span;
        for (int k = 0; k < 3; ++k) position[k] = a.position[k] + (b.position[k] - a.position[k]) * fraction;
        float dot = 0.0f, length = 0.0f;
        for (int k = 0; k < 4; ++k) dot += a.rotation[k] * b.rotation[k];
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        for (int k = 0; k < 4; ++k) {
            rotation[k] = a.rotation[k] * (1.0f - fraction) + sign * b.rotation[k] * fraction;
            length += rotation[k] * rotation[k];
        }
        length = std::sqrt(length);
        if (length > 0.0f) for (int k = 0; k < 4; ++k) rotation[k] /= length;
        Describe(presentation, fraction < 1.0f ? a : b, Distance(a.position, b.position) * 1000.0f / span, render);
        if (presentation && !track.predictive) {
            for (int slot = 0; slot < net::NpcMotionSlots; ++slot) {
                auto const& first = a.motion.motions[slot];
                auto const& second = b.motion.motions[slot];
                if (!first.active || !second.active || first.name != second.name || first.instance != second.instance) continue;
                auto& shown = presentation->motion.motions[slot];
                float phase = second.phase - first.phase;
                if (first.speed >= 0.0f && phase < 0.0f) phase += 1.0f;
                if (first.speed < 0.0f && phase > 0.0f) phase -= 1.0f;
                shown.phase = first.phase + phase * fraction;
                if (shown.phase > 1.0f) shown.phase -= 1.0f;
                if (shown.phase < 0.0f) shown.phase += 1.0f;
                shown.weight = first.weight + (second.weight - first.weight) * fraction;
            }
        }
        if (counted) ++sampledBetween, track.lastCase = 1;
        return true;
    }
    if (counted) ++sampledOldest, track.lastCase = 4;
    std::memcpy(position, track.snaps[0].position, sizeof(track.snaps[0].position));
    std::memcpy(rotation, track.snaps[0].rotation, sizeof(track.snaps[0].rotation));
    Describe(presentation, track.snaps[0], 0.0f, track.snaps[0].sent);
    return true;
}

int32_t PlaybackDelay(Track const& track) { return track.delayMs; }

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
