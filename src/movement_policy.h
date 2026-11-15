// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace g3t::movement {
inline unsigned GapBucket(uint64_t us)
{
    return us <= 16000 ? 0 : us <= 33000 ? 1 : us <= 66000 ? 2 : us <= 100000 ? 3 : 4;
}

// A candidate, not a verdict: game collisions and native tasks can stop held-key movement.
struct StopWatch {
    bool armed = false, reported = false;
    uint64_t stillSince = 0;
    void Reset() { *this = {}; }
    bool Observe(uint64_t now, float speed, bool held, bool eligible)
    {
        if (!held || !eligible) { Reset(); return false; }
        if (speed >= 40.0f) { armed = true; reported = false; stillSince = 0; return false; }
        if (!armed || speed > 5.0f) { stillSince = 0; return false; }
        if (!stillSince) { stillSince = now; return false; }
        if (reported || now - stillSince < 150000) return false;
        reported = true;
        return true;
    }
};

constexpr uint16_t NoMotionFrac = 65535;
constexpr uint16_t MaxRunningMotionFrac = 65534;

inline bool IsContinuousPuppetMotion(char const* name)
{
    return !name || !*name || std::strstr(name, "_Move_") != nullptr || std::strstr(name, "_Ambient_") != nullptr ||
           std::strstr(name, "_Jump_") != nullptr || std::strstr(name, "_Fall_") != nullptr;
}

inline bool IsLoopingPuppetMotion(char const* name)
{
    if (!name || !*name) return false;
    if (std::strstr(name, "Jump_Up") != nullptr || std::strstr(name, "Jump_End") != nullptr) return false;
    return std::strstr(name, "_Loop") != nullptr || std::strstr(name, "_Move_") != nullptr ||
           std::strstr(name, "_Ambient_") != nullptr;
}

inline bool IsTrackedActionMotion(char const* name)
{
    return name && *name && !IsContinuousPuppetMotion(name);
}

inline bool IsTranslatingLocomotion(char const* name)
{
    return name && std::strstr(name, "_Move_") != nullptr && std::strstr(name, "_Move_Stand_") == nullptr &&
           std::strstr(name, "_Move_Turn") == nullptr;
}

inline int ReplayKind(char const* name)
{
    if (!name) return 2;
    if (std::strstr(name, "KnockDown") || std::strstr(name, "GetUp")) return 1;
    if (std::strstr(name, "Attack") || std::strstr(name, "Parade") || std::strstr(name, "Stumble")) return 0;
    return 2;
}

inline bool ParseCombatMove(char const* name, int& action, char* phase, size_t size)
{
    struct ActionName { char const* name; int value; };
    static constexpr ActionName kCombatActions[] = {
        {"JumpBack", 51}, {"ExitParade", 19},
        {"Attack", 1}, {"PowerAttack", 2}, {"QuickAttack", 3}, {"QuickAttackR", 4}, {"QuickAttackL", 5}, {"SimpleWhirl", 6},
        {"SprintAttack", 9}, {"WhirlAttack", 10}, {"PierceAttack", 11}, {"JumpAttack", 12}, {"RamAttack", 13},
        {"HackAttack", 14}, {"FinishingAttack", 15}, {"Parade", 16}, {"ParadeR", 17}, {"ParadeL", 18},
        {"QuickParadeStumble", 20}, {"ParadeStumble", 21}, {"ParadeStumbleR", 22}, {"ParadeStumbleL", 23},
        {"HeavyParadeStumble", 24}, {"QuickStumble", 25}, {"Stumble", 26}, {"StumbleR", 27}, {"StumbleL", 28},
        {"GetUpAttack", 30}, {"GetUpParade", 31}, {"PierceStumble", 34}, {"AbortAttack", 39}};
    char const* part = name;
    for (int i = 0; i < 5 && part; ++i) {
        part = std::strchr(part, '_');
        if (part) ++part;
    }
    if (!part) return false;
    char const* end = std::strchr(part, '_');
    if (!end) return false;
    const size_t length = static_cast<size_t>(end - part);
    action = 0;
    for (auto const& entry : kCombatActions)
        if (std::strlen(entry.name) == length && !std::strncmp(entry.name, part, length)) action = entry.value;
    if (!action) return false;
    char const* phaseStart = end + 1;
    char const* phaseEnd = std::strchr(phaseStart, '_');
    if (!phaseEnd || static_cast<size_t>(phaseEnd - phaseStart) >= size) return false;
    std::memcpy(phase, phaseStart, phaseEnd - phaseStart);
    phase[phaseEnd - phaseStart] = '\0';
    return true;
}

inline uint16_t EncodeRunningMotionFrac(double part)
{
    if (!std::isfinite(part)) return NoMotionFrac;
    return static_cast<uint16_t>(std::clamp(part, 0.0, 1.0) * static_cast<double>(MaxRunningMotionFrac) + 0.5);
}

inline uint16_t InterpolateMotionFrac(uint16_t a, uint16_t b, float t, bool looping)
{
    if (a >= NoMotionFrac && b >= NoMotionFrac) return NoMotionFrac;
    if (a >= NoMotionFrac) return b;
    if (b >= NoMotionFrac) return a;
    const float clampedT = std::clamp(t, 0.0f, 1.0f);
    const float fa = static_cast<float>(a) / static_cast<float>(MaxRunningMotionFrac);
    const float fb = static_cast<float>(b) / static_cast<float>(MaxRunningMotionFrac);
    float delta = fb - fa;
    if (looping) {
        if (delta < -0.25f) delta += 1.0f;
        else if (delta < 0.0f) delta = 0.0f;
        float f = std::fmod(fa + delta * clampedT, 1.0f);
        if (f < 0.0f) f += 1.0f;
        return EncodeRunningMotionFrac(f);
    }
    if (delta < 0.0f) delta = 0.0f;
    return EncodeRunningMotionFrac(fa + delta * clampedT);
}

inline double ExpectedPuppetPlayTime(double maxSec, int motionFrac, uint32_t ageMs, double unitMs, bool looping)
{
    if (!(maxSec > 0.0) || !(unitMs > 0.0) || motionFrac < 0 || motionFrac >= NoMotionFrac) return 0.0;
    const double base = (static_cast<double>(motionFrac) / static_cast<double>(MaxRunningMotionFrac)) * maxSec;
    const double raw = base + static_cast<double>(ageMs) / unitMs;
    if (!looping) return raw;
    double wrapped = std::fmod(raw, maxSec);
    if (wrapped < 0.0) wrapped += maxSec;
    return wrapped;
}

inline float PuppetPhaseLagMs(double localPlaySec, double expectedSec, double maxSec, double unitMs, bool looping)
{
    if (!(maxSec > 0.0) || !(unitMs > 0.0)) return 0.0f;
    double diff = expectedSec - localPlaySec;
    if (looping) {
        const double half = 0.5 * maxSec;
        while (diff > half) diff -= maxSec;
        while (diff < -half) diff += maxSec;
    }
    return static_cast<float>(diff * unitMs);
}

inline float PuppetPaceSpeed(float lagMs, float deadZoneMs = 8.0f, float paceOverMs = 180.0f)
{
    if (std::fabs(lagMs) <= deadZoneMs || !(paceOverMs > 0.0f)) return 1.0f;
    return std::clamp(1.0f + lagMs / paceOverMs, 0.75f, 1.30f);
}

struct PuppetMotionSample {
    uint32_t sent = 0;
    uint32_t motion = 0;
    uint32_t heldRight = 0;
    uint32_t heldLeft = 0;
    uint16_t motionFrac = NoMotionFrac;
    uint8_t movementMode = 1;
    uint8_t aniState = 0;
    uint8_t actionSeq = 0;
    bool looping = false;
};

struct PuppetMotionHistory {
    static constexpr size_t Capacity = 64;
    std::array<PuppetMotionSample, Capacity> entries{};
    size_t count = 0;
    uint32_t serial = 0;

    void Reset(uint32_t newSerial = 0)
    {
        count = 0;
        serial = newSerial;
    }

    void Push(uint32_t trackSerial, PuppetMotionSample const& sample)
    {
        if (serial != trackSerial) Reset(trackSerial);
        if (count > 0) {
            const int32_t dt = static_cast<int32_t>(sample.sent - entries[count - 1].sent);
            if (dt < 0) return;
            if (dt == 0) {
                entries[count - 1] = sample;
                return;
            }
        }
        if (count == Capacity) {
            for (size_t i = 1; i < Capacity; ++i) entries[i - 1] = entries[i];
            --count;
        }
        entries[count++] = sample;
    }

    bool Sample(uint32_t renderSent, PuppetMotionSample& out, uint32_t& ageMsOut) const
    {
        if (count == 0) return false;
        const int32_t afterFirst = static_cast<int32_t>(renderSent - entries[0].sent);
        if (afterFirst <= 0 || count == 1) {
            out = entries[0];
            ageMsOut = afterFirst > 0 ? static_cast<uint32_t>(afterFirst) : 0u;
            return true;
        }
        const int32_t afterLast = static_cast<int32_t>(renderSent - entries[count - 1].sent);
        if (afterLast >= 0) {
            out = entries[count - 1];
            ageMsOut = static_cast<uint32_t>(afterLast);
            return true;
        }
        for (size_t i = count - 1; i > 0; --i) {
            PuppetMotionSample const& a = entries[i - 1];
            PuppetMotionSample const& b = entries[i];
            const int32_t num = static_cast<int32_t>(renderSent - a.sent);
            if (num < 0) continue;
            const int32_t span = static_cast<int32_t>(b.sent - a.sent);
            if (span <= 0 || num == 0) {
                out = a;
                ageMsOut = 0;
                return true;
            }
            if (a.motion == b.motion && a.motion != 0 && (a.looping || a.actionSeq == b.actionSeq)) {
                out = a;
                const float t = std::clamp(static_cast<float>(num) / static_cast<float>(span), 0.0f, 1.0f);
                out.motionFrac = InterpolateMotionFrac(a.motionFrac, b.motionFrac, t, a.looping);
                ageMsOut = (a.motionFrac < NoMotionFrac && b.motionFrac < NoMotionFrac) ? 0u : static_cast<uint32_t>(num);
                return true;
            }
            out = a;
            ageMsOut = static_cast<uint32_t>(num);
            return true;
        }
        out = entries[0];
        ageMsOut = 0;
        return true;
    }
};
}
