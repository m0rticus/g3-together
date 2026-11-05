// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include "net.h"

namespace g3t::body {
constexpr int SnapshotCount = 32;
constexpr int32_t BufferDelay = 100;
constexpr int32_t MaxAhead = 100;

struct MotionState
{
    uint32_t motion = 0;
    uint32_t heldRight = 0, heldLeft = 0;
    int aniState = -1, actionSeq = -1, motionFrac = -1;
    uint8_t flags = 0;
    int32_t hitPoints = 0;
    int32_t hitPointsMax = 1;
    uint32_t incarnation = 1, hitSequence = 0;
    uint8_t hitPlayer = 0, partyPlayer = 0;
    uint8_t partyType = 0, waiting = 0, guidePlayer = 0;
    uint8_t aiMode = 0;
    uint8_t action = 0, attackReason = 0, damageType = 0;
    int32_t damageAmount = 0;
    float damageMultiplier = 0.0f;
    uint8_t presentationReady = 0;
    uint8_t contact = 0;
    uint8_t attitude = 0;
    net::NpcMotion motions[net::NpcMotionSlots];
};

struct Track
{
    bool predictive = true;  // player puppets may lead; NPCs only interpolate received host state
    struct Snap {
        uint32_t sent = 0;
        float position[3] = {};
        float rotation[4] = {};
        int movementMode = -1;
        MotionState motion;
    };
    Snap snaps[SnapshotCount];
    int count = 0;
    int32_t offset = 0;
    bool haveOffset = false;
    int32_t windowMin = 0, lastWindowMin = 0;
    uint32_t windowAt = 0;
    uint8_t lastCase = 0;
    uint32_t serial = 1;
    uint32_t renderSent = 0, renderNow = 0;
    int32_t renderShift = -BufferDelay;
    double fractionalAdvance = 0.0;
    bool haveRenderTime = false;
    uint32_t lastArrived = 0, cadenceWindowAt = 0, recoveryUntil = 0, underrunAt = 0;
    uint32_t cadenceMin = 50, previousCadenceMin = 50;
    float jitterMs = 0.0f;
    int32_t delayMs = BufferDelay;
};

struct Presentation
{
    int movementMode = -1;
    float speed = 0.0f;
    uint32_t sent = 0;
    uint32_t stateSent = 0;
    MotionState motion;
};

extern bool offsetWindow;
extern bool arrivalSpeed;
extern unsigned sampledBetween, sampledAhead, sampledHeld, sampledOldest;
float Distance(float const a[3], float const b[3]);
int32_t PlaybackDelay(Track const& track);
bool IsLocomotionMotion(char const* motion);
void Feed(Track& track, float const position[3], float const rotation[4], uint32_t sent, uint32_t arrived, int movementMode = -1,
          MotionState const& motion = MotionState{});
bool Sample(Track& track, uint32_t now, float position[3], float rotation[4], int32_t shift = -BufferDelay,
            bool counted = true, Presentation* presentation = nullptr);
float Speed(float const from[3], float const to[3], uint32_t sentFrom, uint32_t sentTo, uint32_t arrivedFrom, uint32_t arrivedTo);
}
