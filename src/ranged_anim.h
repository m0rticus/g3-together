// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-independent wire packing and timeline interpolation for player upper-body overlays
// (emfx2 slots 1..7, e.g. bow/crossbow draw-aim-hold-shoot-reload) and left-hand weapon bend
// animations (<Bow>_PowerRaise_00 / <CrossBow>_Cock_00) on remote puppets.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "player_state.h"

namespace g3t::ranged_anim {

struct OverlayChannelMeta {
    std::uint8_t slot = 0;       // 0 = inactive, 1..7 = emfx2 slot
    std::uint8_t blendMode = 0;  // 0..7 (eEMotionBlendMode)
    std::uint8_t playMode = 0;   // 0..3
    bool looping = false;
    bool defaults = false;
    std::uint8_t seq = 0;        // 0..3 restart counter
};

struct OverlayMeta {
    OverlayChannelMeta ch0{};
    OverlayChannelMeta ch1{};
    bool weaponActive = false;
    std::uint8_t weaponSeq = 0;  // 0..3 restart counter
};

struct OverlayHot {
    std::uint8_t phase0 = 0;
    std::uint8_t weight0 = 0;
    std::uint8_t phase1 = 0;
    std::uint8_t weight1 = 0;
    std::uint8_t weaponPhase = 0;
};

constexpr std::uint16_t HotActionMarker = 0x8000u;

inline std::uint8_t QuantizeUnit(float value)
{
    if (!std::isfinite(value)) return 0;
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

inline float DequantizeUnit(std::uint8_t value)
{
    return static_cast<float>(value) / 255.0f;
}

inline std::uint32_t PackChannelMeta(OverlayChannelMeta const& ch)
{
    if (ch.slot == 0 || ch.slot > 7) return 0u;
    return static_cast<std::uint32_t>(ch.slot & 0x7u) |
           (static_cast<std::uint32_t>(ch.blendMode & 0x7u) << 3) |
           (static_cast<std::uint32_t>(ch.playMode & 0x3u) << 6) |
           (static_cast<std::uint32_t>(ch.looping ? 1u : 0u) << 8) |
           (static_cast<std::uint32_t>(ch.defaults ? 1u : 0u) << 9) |
           (static_cast<std::uint32_t>(ch.seq & 0x3u) << 10);
}

inline OverlayChannelMeta UnpackChannelMeta(std::uint32_t raw)
{
    OverlayChannelMeta ch{};
    ch.slot = static_cast<std::uint8_t>(raw & 0x7u);
    if (ch.slot == 0) return OverlayChannelMeta{};
    ch.blendMode = static_cast<std::uint8_t>((raw >> 3) & 0x7u);
    ch.playMode = static_cast<std::uint8_t>((raw >> 6) & 0x3u);
    ch.looping = ((raw >> 8) & 0x1u) != 0;
    ch.defaults = ((raw >> 9) & 0x1u) != 0;
    ch.seq = static_cast<std::uint8_t>((raw >> 10) & 0x3u);
    return ch;
}

inline std::uint32_t PackOverlayMeta(OverlayMeta const& meta)
{
    const std::uint32_t c0 = PackChannelMeta(meta.ch0);
    const std::uint32_t c1 = PackChannelMeta(meta.ch1);
    const std::uint32_t w = meta.weaponActive
        ? (1u | (static_cast<std::uint32_t>(meta.weaponSeq & 0x3u) << 1))
        : 0u;
    return c0 | (c1 << 12) | (w << 24);
}

inline OverlayMeta UnpackOverlayMeta(std::uint32_t packed)
{
    OverlayMeta out{};
    out.ch0 = UnpackChannelMeta(packed & 0xfffu);
    out.ch1 = UnpackChannelMeta((packed >> 12) & 0xfffu);
    const std::uint32_t w = (packed >> 24) & 0x7u;
    out.weaponActive = (w & 0x1u) != 0;
    out.weaponSeq = out.weaponActive ? static_cast<std::uint8_t>((w >> 1) & 0x3u) : 0u;
    return out;
}

inline bool CanPackOverlayHot(std::uint8_t flags, std::uint32_t overlayMeta)
{
    return overlayMeta != 0u && (flags & net::PlayerFlags::Robbable) == 0u;
}

inline void PackOverlayHot(OverlayHot const& hot, std::uint8_t flags, std::uint32_t overlayMeta,
                           std::int32_t& goldInOut, std::uint16_t& actionMsInOut)
{
    if (!CanPackOverlayHot(flags, overlayMeta)) return;
    // Bit 31 of gold stays 0 so gold is always in [0, INT32_MAX] as required by playerwire.
    const std::uint32_t g = static_cast<std::uint32_t>(hot.phase0) |
                            (static_cast<std::uint32_t>(hot.weight0) << 8) |
                            (static_cast<std::uint32_t>(hot.phase1) << 16) |
                            (static_cast<std::uint32_t>(hot.weight1 & 0x7fu) << 24);
    goldInOut = static_cast<std::int32_t>(g);
    actionMsInOut = static_cast<std::uint16_t>(
        HotActionMarker |
        static_cast<std::uint16_t>(hot.weaponPhase) |
        (static_cast<std::uint16_t>((hot.weight1 >> 7) & 0x1u) << 8));
}

inline OverlayHot UnpackOverlayHot(std::uint8_t flags, std::uint32_t overlayMeta,
                                   std::int32_t gold, std::uint16_t actionMs)
{
    OverlayHot out{};
    if (!CanPackOverlayHot(flags, overlayMeta) || gold < 0 || (actionMs & HotActionMarker) == 0u) {
        const OverlayMeta meta = UnpackOverlayMeta(overlayMeta);
        out.weight0 = meta.ch0.slot ? 255u : 0u;
        out.weight1 = meta.ch1.slot ? 255u : 0u;
        return out;
    }
    const std::uint32_t g = static_cast<std::uint32_t>(gold);
    out.phase0 = static_cast<std::uint8_t>(g & 0xffu);
    out.weight0 = static_cast<std::uint8_t>((g >> 8) & 0xffu);
    out.phase1 = static_cast<std::uint8_t>((g >> 16) & 0xffu);
    out.weight1 = static_cast<std::uint8_t>(((g >> 24) & 0x7fu) | (((actionMs >> 8) & 0x1u) << 7));
    out.weaponPhase = static_cast<std::uint8_t>(actionMs & 0xffu);
    return out;
}

inline std::uint8_t InterpolateLinearByte(std::uint8_t a, std::uint8_t b, float t)
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * clamped;
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

inline std::uint8_t InterpolatePhaseByte(std::uint8_t a, std::uint8_t b, float t, bool looping)
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    const float fa = DequantizeUnit(a);
    const float fb = DequantizeUnit(b);
    float delta = fb - fa;
    if (looping) {
        if (delta < -0.25f) delta += 1.0f;
        else if (delta < 0.0f) delta = 0.0f;
        float f = std::fmod(fa + delta * clamped, 1.0f);
        if (f < 0.0f) f += 1.0f;
        return QuantizeUnit(f);
    }
    if (delta < 0.0f) delta = 0.0f;
    return QuantizeUnit(fa + delta * clamped);
}

inline bool SameChannelInstance(OverlayChannelMeta const& a, OverlayChannelMeta const& b)
{
    return a.slot != 0 && a.slot == b.slot && (a.looping || a.seq == b.seq);
}

inline OverlayHot InterpolateOverlayHot(std::uint32_t metaA, std::uint32_t anim0A, std::uint32_t anim1A,
                                        std::uint32_t weaponA, OverlayHot const& hotA,
                                        std::uint32_t metaB, std::uint32_t anim0B, std::uint32_t anim1B,
                                        std::uint32_t weaponB, OverlayHot const& hotB, float t)
{
    OverlayHot out = hotA;
    const OverlayMeta ua = UnpackOverlayMeta(metaA);
    const OverlayMeta ub = UnpackOverlayMeta(metaB);
    if (anim0A != 0 && anim0A == anim0B && SameChannelInstance(ua.ch0, ub.ch0)) {
        out.phase0 = InterpolatePhaseByte(hotA.phase0, hotB.phase0, t, ua.ch0.looping);
        out.weight0 = InterpolateLinearByte(hotA.weight0, hotB.weight0, t);
    }
    if (anim1A != 0 && anim1A == anim1B && SameChannelInstance(ua.ch1, ub.ch1)) {
        out.phase1 = InterpolatePhaseByte(hotA.phase1, hotB.phase1, t, ua.ch1.looping);
        out.weight1 = InterpolateLinearByte(hotA.weight1, hotB.weight1, t);
    }
    if (weaponA != 0 && weaponA == weaponB && ua.weaponActive && ub.weaponActive && ua.weaponSeq == ub.weaponSeq) {
        out.weaponPhase = InterpolatePhaseByte(hotA.weaponPhase, hotB.weaponPhase, t, false);
    }
    return out;
}

} // namespace g3t::ranged_anim
