// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

namespace g3t::ragdoll {
struct Vector { float x, y, z; };

inline bool Finite(Vector const& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline Vector RelativePoint(Vector const& world, Vector const& root)
{
    return {world.x - root.x, world.y - root.y, world.z - root.z};
}

inline Vector WorldPoint(Vector const& relative, Vector const& root)
{
    return {root.x + relative.x, root.y + relative.y, root.z + relative.z};
}

// CP 1.75 Script_Game+0x1c985: explosion direction is normalized horizontally, then Y becomes 0.2;
// its distance factor is at most 1 and its force scale is 80000. This is a defensive bound, not a native global clamp.
constexpr double MaximumDeathForce = 80000.0 * 1.019803902718557;
constexpr float MaximumTransformCoordCm = 1.0e7f;
constexpr float MaxLiveCorpseStepCm = 500.0f;
constexpr float MaxUnragdolledDeathClimbCm = 15.0f;
constexpr float MaxSpawnPelvisAnchorRadiusCm = 150.0f;
constexpr float MaxPelvisLiftCm = 130.0f;
constexpr float MaximumDownwardCorpseForce = 4000.0f;
constexpr float MinCorpseForceHeightCm = 50.0f;
constexpr float MaxCorpseForceHeightCm = 100.0f;
constexpr float MaxCorpseForceRadiusCm = 45.0f;
constexpr uint8_t DeadFlag = 1u;
constexpr uint8_t DownFlag = 2u;
constexpr uint8_t RagdollFlag = 128u;
constexpr uint8_t DeadOrRagdollFlags = DeadFlag | RagdollFlag;
constexpr uint8_t RagdollDeadMovementMode = 14u;

inline bool IsRagdollOrDeadState(uint8_t flags, uint8_t movementMode)
{
    return (flags & DeadOrRagdollFlags) != 0 || movementMode == RagdollDeadMovementMode;
}

// Hold the pre-death ground transform while fraction < 1.0f instead of blending feet WorldPosition
// upward toward the host's Bip01 Pelvis ragdoll root on the frame before death takes effect.
inline bool ShouldHoldPreDeathTransform(uint8_t flagsA, uint8_t modeA,
                                        uint8_t flagsB, uint8_t modeB, float fraction)
{
    return fraction < 1.0f && !IsRagdollOrDeadState(flagsA, modeA) && IsRagdollOrDeadState(flagsB, modeB);
}

inline bool IsGroundOriginSample(uint8_t flags, uint8_t movementMode, Vector const& pos)
{
    return !IsRagdollOrDeadState(flags, movementMode) && Finite(pos);
}

inline bool ShouldDeferCorpseRagdoll(bool isHost, bool sessionPaused, bool ragdollInitialized)
{
    return !isHost && sessionPaused && !ragdollInitialized;
}

// Engine.dll RagDollTick (RVA 0x031d40) advances low-velocity settle and max-age timers from real
// wall-clock frame dt while Novodex is paused (velocity == 0), calling StopRagDoll (RVA 0x034550)
// after ~1-2 s. Suppressing OnProcess while paused preserves active and mid-fall ragdolls for unpause.
inline bool ShouldFreezeActiveRagdollPresentation(bool isHost, bool sessionPaused,
                                                  bool ragdollActive, bool corpseUnsettled)
{
    return !isHost && sessionPaused && (ragdollActive || corpseUnsettled);
}

inline bool ShouldRestartUnsettledCorpseAfterPause(bool wasPaused, bool sessionPaused,
                                                   bool ragdollInitialized, bool corpseSettled,
                                                   bool controllerActive)
{
    return wasPaused && !sessionPaused && ragdollInitialized && !corpseSettled && !controllerActive;
}

inline uint32_t ExtendPausedFallbackGrace(uint32_t graceUntil, uint32_t now, uint32_t graceMs = 500u)
{
    if (!graceUntil) return 0;
    const uint32_t extended = now + graceMs;
    return extended ? extended : 1u;
}

inline double Magnitude(Vector const& v)
{
    return std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
}

inline bool LimitForce(Vector const& input, Vector& output)
{
    if (!Finite(input)) return false;
    const double length = Magnitude(input);
    const double scale = length > MaximumDeathForce ? MaximumDeathForce / length : 1.0;
    output = {float(input.x * scale), float(input.y * scale), float(input.z * scale)};
    const double rounded = Magnitude(output);
    if (rounded > MaximumDeathForce) {
        const float correction = std::nextafter(float(MaximumDeathForce / rounded), 0.0f);
        output = {output.x * correction, output.y * correction, output.z * correction};
    }
    return true;
}

// Engine.dll eCVisualAnimation_PS::GetBoneMatrix (RVA 0x2c6a0) tests `index <= mNodes.mLength`
// instead of `<`, reading one 400-byte node past the allocation when index == mNodes.mLength.
inline int ClampedBoneCount(int actorNodeLength, int hardCap = 256)
{
    if (actorNodeLength <= 0 || hardCap <= 0) return 0;
    return actorNodeLength < hardCap ? actorNodeLength : hardCap;
}

inline bool FiniteMatrix12(float const (&m)[12], float maxTranslationCm = MaximumTransformCoordCm)
{
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(m[i])) return false;
    for (int axis = 0; axis < 3; ++axis) {
        const double lenSq = double(m[axis * 3]) * m[axis * 3] +
                             double(m[axis * 3 + 1]) * m[axis * 3 + 1] +
                             double(m[axis * 3 + 2]) * m[axis * 3 + 2];
        if (!std::isfinite(lenSq) || lenSq < 1.0e-8 || lenSq > 1.0e6) return false;
    }
    return std::fabs(m[9]) <= maxTranslationCm &&
           std::fabs(m[10]) <= maxTranslationCm &&
           std::fabs(m[11]) <= maxTranslationCm;
}

inline bool HorizontalNear(Vector const& a, Vector const& b, float maxDistanceCm)
{
    const double dx = double(a.x) - double(b.x);
    const double dz = double(a.z) - double(b.z);
    return dx * dx + dz * dz <= double(maxDistanceCm) * double(maxDistanceCm);
}

struct CorpseOriginInput {
    Vector entityPos{};
    Vector previousPos{};
    Vector shownPos{};
    bool livePresented = false;
    bool havePreviousAlive = false;
    bool hostRagdoll = false;
};

struct CorpseOriginResult {
    Vector origin{};
    bool keepLiveMatrix = false;
};

// Engine.dll eCNovodexPhysicsController::Update (RVA 0x3db90) moves the host entity's WorldPosition
// from feet origin up to the ragdoll root bone (Bip01 Pelvis) as soon as ragdoll ticks. Preserving the
// live client ground matrix avoids lifting an un-ragdolled skeleton ~30..90 cm into the air before ragdoll.
inline bool SelectCorpseOrigin(CorpseOriginInput const& input, CorpseOriginResult& out)
{
    if (input.livePresented && Finite(input.entityPos) &&
        (!Finite(input.shownPos) || HorizontalNear(input.entityPos, input.shownPos, MaxLiveCorpseStepCm))) {
        if (input.hostRagdoll && input.havePreviousAlive && Finite(input.previousPos) &&
            HorizontalNear(input.previousPos, input.entityPos, MaxLiveCorpseStepCm) &&
            (input.entityPos.y - input.previousPos.y) > MaxUnragdolledDeathClimbCm) {
            out = {{input.entityPos.x, input.previousPos.y, input.entityPos.z}, false};
            return true;
        }
        out = {input.entityPos, true};
        return true;
    }
    if (!Finite(input.shownPos)) {
        if (!Finite(input.entityPos)) return false;
        out = {input.entityPos, true};
        return true;
    }
    Vector placed = input.shownPos;
    if (input.havePreviousAlive && Finite(input.previousPos) &&
        HorizontalNear(input.previousPos, input.shownPos, MaxLiveCorpseStepCm)) {
        const bool jumpedUp = input.hostRagdoll
            ? placed.y > input.previousPos.y
            : (placed.y - input.previousPos.y) > MaxUnragdolledDeathClimbCm;
        if (jumpedUp) placed.y = input.previousPos.y;
    } else if (input.hostRagdoll && Finite(input.entityPos) &&
               HorizontalNear(input.entityPos, input.shownPos, MaxSpawnPelvisAnchorRadiusCm)) {
        const float lift = placed.y - input.entityPos.y;
        if (lift > MaxUnragdolledDeathClimbCm && lift <= MaxPelvisLiftCm)
            placed.y = input.entityPos.y;
    }
    out = {placed, false};
    return true;
}

// Script_Game ZS_RagDollDead (RVA 0x1c4a0) aims from attacker feet to victim feet + (0, 150..180, 0),
// producing +10,000..+13,700 upward Y forces applied to the closest single NxActor (head at +180 cm).
// Strip upward lift and clamp contact to torso height relative to the pre-ragdoll ground anchor.
inline bool ShapeCorpseImpulse(Vector const& rawForce, Vector const& rawRelative, Vector const& groundOrigin,
                               Vector& outForce, Vector& outRelative, Vector& outWorldAt)
{
    Vector limited{};
    if (!LimitForce(rawForce, limited) || !Finite(rawRelative) || !Finite(groundOrigin)) return false;
    if (limited.y > 0.0f) limited.y = 0.0f;
    else if (limited.y < -MaximumDownwardCorpseForce) limited.y = -MaximumDownwardCorpseForce;

    Vector rel = rawRelative;
    if (rel.y < MinCorpseForceHeightCm) rel.y = MinCorpseForceHeightCm;
    else if (rel.y > MaxCorpseForceHeightCm) rel.y = MaxCorpseForceHeightCm;
    const double horiz = std::sqrt(double(rel.x) * rel.x + double(rel.z) * rel.z);
    if (horiz > MaxCorpseForceRadiusCm) {
        const float scale = float(MaxCorpseForceRadiusCm / horiz);
        rel.x *= scale;
        rel.z *= scale;
    }
    const Vector worldAt = WorldPoint(rel, groundOrigin);
    if (!Finite(limited) || !Finite(rel) || !Finite(worldAt)) return false;
    outForce = limited;
    outRelative = rel;
    outWorldAt = worldAt;
    return true;
}

inline Vector SafeTraceVelocity(Vector const& current, Vector const& previous, uint32_t dtMs, bool havePrevious)
{
    if (!havePrevious || !dtMs || !Finite(current) || !Finite(previous)) return {0.0f, 0.0f, 0.0f};
    const float scale = 1000.0f / float(dtMs);
    const Vector velocity{(current.x - previous.x) * scale,
                          (current.y - previous.y) * scale,
                          (current.z - previous.z) * scale};
    return Finite(velocity) ? velocity : Vector{0.0f, 0.0f, 0.0f};
}

enum class Source { Pending, Host, Gravity };

struct DeathImpulse {
    Source source = Source::Pending;
    unsigned applied = 0;

    bool Pending() const { return source == Source::Pending; }
    bool HostApplied() const { return source == Source::Host; }
    bool Choose(Source next)
    {
        if (!Pending() || next == Source::Pending) return false;
        source = next;
        applied = next == Source::Host ? 1 : 0;
        return true;
    }
};
}
