// SPDX-License-Identifier: GPL-3.0-or-later
#include "body.h"
#include "body_internal.h"

#include "config.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace g3t::body {
Glide glide[2];
unsigned stanceStops = 0;
unsigned stopOwners[8] = {};
std::map<std::string, unsigned> glideMotions[2];
std::map<std::string, unsigned> stillMotions[2];
bool settleStance = true;   // QA negative control: glideold
bool stopMotion = true;     // QA comparison: glidestopoff (only StopPlayAni on set-off)
unsigned seeks[2] = {};
unsigned paceSteps[2] = {};
std::vector<float> motionLags[2];  // ms the receiver's combat motion was behind where the sender's was, per Drive (+ behind)
bool lagMsUnit = false;            // the last measured combat motion length looked like milliseconds (see MotionUnitMs)
unsigned combatTargets[2] = {}, followSkips[2][6] = {}, otherNamesLogged[2] = {};

void GlideReport(void (*log)(char const*))
{
    char line[320];
    for (int kind = 0; kind < 2; ++kind) {
        Glide const& g = glide[kind];
        Format(line, "GLIDE kind=%s moving_ms=%u glide_ms=%u steer_ms=%u place_ms=%u snaps=%u stance_stops=%u owners=%u/%u/%u/%u/%u/%u/%u",
                  kind ? "puppet" : "mirror", g.movingMs, g.glideMs, g.steerMs, g.placeMs, g.snaps, stanceStops, stopOwners[0], stopOwners[1],
                  stopOwners[2], stopOwners[3], stopOwners[4], stopOwners[5], stopOwners[6]);
        log(line);
        Format(line, "SMOOTH kind=%s frames=%u jumps30=%u max_step_cm=%.0f still=%u lag_cm=%.0f interp=%d still_by=%u/%u/%u/%u "
                     "still_case=%u/%u/%u/%u/%u offset_window=%d goal_ahead=%d glide_by=%u/%u/%u lead_factor=%.2f lead_old=%d",
                  kind ? "puppet" : "mirror", g.frames, g.jumps, g.maxStep, g.stillFrames, g.frames ? g.lagSum / g.frames : 0.0f,
                  interpolate ? 1 : 0, g.stillBy[0], g.stillBy[1], g.stillBy[2], g.stillBy[3], g.stillCase[0], g.stillCase[1],
                  g.stillCase[2], g.stillCase[3], g.stillCase[4], offsetWindow ? 1 : 0, goalAhead ? 1 : 0, g.glidePushMs,
                  g.glideOvershootMs, g.glideOtherMs, g.leadFactorFrames ? g.leadFactorSum / g.leadFactorFrames : 0.0f, leadOld ? 1 : 0);
        log(line);
        std::vector<float> errors = g.displayErrors;
        std::sort(errors.begin(), errors.end());
        Format(line, "SNAPS kind=%s hard=%u action=%u same_name=%u replayed=%u action_seq=%d", kind ? "puppet" : "mirror", g.hardSnaps,
                  g.actionSnaps, sameNameStarts, sameNameReplayed, actionSeq ? 1 : 0);
        log(line);
        Format(line, "LEAD kind=%s samples=%zu err_median_cm=%.0f err_p95_cm=%.0f sampled=%u/%u/%u/%u", kind ? "puppet" : "mirror",
                  errors.size(), errors.empty() ? 0.0f : errors[errors.size() / 2],
                  errors.empty() ? 0.0f : errors[std::min(errors.size() - 1, errors.size() * 95 / 100)], sampledBetween, sampledAhead,
                  sampledHeld, sampledOldest);
        log(line);
        // The five motions shown longest while gliding (a name without _Move_ is not always a still body: starts, stops).
        std::vector<std::pair<unsigned, std::string>> top;
        for (auto const& [name, ms] : glideMotions[kind]) top.emplace_back(ms, name);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 5; ++i) {
            Format(line, "GLIDEMOTION kind=%s ms=%u motion=%.120s", kind ? "puppet" : "mirror", top[i].first, top[i].second.c_str());
            log(line);
        }
        std::vector<std::pair<unsigned, std::string>> still;
        for (auto const& [name, frames] : stillMotions[kind]) still.emplace_back(frames, name);
        std::sort(still.rbegin(), still.rend());
        for (size_t i = 0; i < still.size() && i < 5; ++i) {
            Format(line, "STILLMOTION kind=%s frames=%u motion=%.120s", kind ? "puppet" : "mirror", still[i].first, still[i].second.c_str());
            log(line);
        }
    }
}

void SyncReport(void (*log)(char const*))
{
    char line[280];
    for (int kind = 0; kind < 2; ++kind) {
        Glide const& g = glide[kind];
        Format(line, "SYNCBODY kind=%s interpolation=%d moving_ms=%u glide_ms=%u frames=%u still=%u jumps30=%u "
                     "hard_corrections=%u action_corrections=%u max_step_cm=%.0f",
               kind ? "puppet" : "mirror", interpolate && GetConfig().movementSmoothing ? 1 : 0, g.movingMs, g.glideMs, g.frames, g.stillFrames, g.jumps,
               g.hardSnaps, g.actionSnaps, g.maxStep);
        log(line);
    }
}

void MotionTime(eCEntity* entity, void (*log)(char const*))
{
    char line[260], name[128] = "-";
    auto* animation = entity ? static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
    eCWrapper_emfx2Actor* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) { log("REFUSED motiontime: no body or actor"); return; }
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    MotionName(animation, 0, name, sizeof(name));
    Format(line, "MOTIONTIME name=%.120s running=%d play=%.4f max=%.4f frac=%u", *name ? name : "-", actor->IsMotionRunning(slot0) ? 1 : 0,
           actor->GetPlayTime(slot0), actor->GetMaxTime(slot0), MotionFraction(entity));
    log(line);
}

void MotionLagReport(void (*log)(char const*))
{
    char line[220];
    const bool on = motionSeek < 0 ? GetConfig().motionSeek : motionSeek > 0;
    for (int kind = 0; kind < 2; ++kind) {
        std::vector<float> behind = motionLags[kind], off;
        for (float lag : behind) off.push_back(std::fabs(lag));
        std::sort(behind.begin(), behind.end());
        std::sort(off.begin(), off.end());
        auto at = [](std::vector<float> const& v, size_t percent) { return v.empty() ? 0.0f : v[std::min(v.size() - 1, v.size() * percent / 100)]; };
        unsigned const* why = followSkips[kind];
        Format(line, "MOTIONLAG kind=%s n=%zu p50=%.0fms p95=%.0fms behind_p50=%.0fms seeks=%u paced=%u seek=%d unit=%s combat_targets=%u "
                     "skipped=%u/%u/%u/%u/%u measured=%u", kind ? "puppet" : "mirror", off.size(), at(off, 50), at(off, 95), at(behind, 50),
               seeks[kind], paceSteps[kind], on ? 1 : 0, lagMsUnit ? "ms" : "s", combatTargets[kind], why[0], why[1], why[2], why[3], why[4], why[5]);
        log(line);
    }
}

void NoteTruth(State& state, float const truth[3], uint32_t at)
{
    int best = -1;
    int32_t bestGap = 60;
    for (int i = 0; i < 16; ++i) {
        if (!state.shownAt[i]) continue;
        const int32_t gap = std::abs(static_cast<int32_t>(state.shownAt[i] - at));
        if (gap < bestGap) bestGap = gap, best = i;
    }
    if (best < 0) return;
    auto& errors = glide[state.puppet ? 1 : 0].displayErrors;
    if (errors.size() < 20000) errors.push_back(Distance(state.shownPos[best], truth));
}

void GlideStopOff(void (*log)(char const*)) { stopMotion = false; log("GLIDESTOPOFF ok"); }
void GlideOld(void (*log)(char const*)) { arrivalSpeed = true; settleStance = false; log("GLIDEOLD on"); }

void GlideReset(void (*log)(char const*))
{
    glide[0] = glide[1] = Glide{};
    motionLags[0].clear();
    motionLags[1].clear();
    lagMsUnit = false;
    seeks[0] = seeks[1] = paceSteps[0] = paceSteps[1] = 0;
    for (int kind = 0; kind < 2; ++kind) {
        combatTargets[kind] = otherNamesLogged[kind] = 0;
        for (unsigned& n : followSkips[kind]) n = 0;
    }
    glideMotions[0].clear(); glideMotions[1].clear();
    stillMotions[0].clear(); stillMotions[1].clear();
    log("GLIDERESET ok");
}
}
