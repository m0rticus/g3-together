// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_bones.h"
#include <deque>
#include <cstring>

namespace g3t::npcwire::bones {
constexpr uint32_t PoseKeyMs = 1000;
constexpr size_t PoseWindow = 32; // 0.53 s at 60 Hz; longer RTT safely falls back to keys
constexpr size_t DeltaHeaderBytes = 6; // layout:u16, acknowledged reference:u32 (zero = key)
// A mode byte follows: 0 = raw 24-byte affine key per bone; 1 = twelve
// one-byte residual widths (0..17), bone presence bits, packed 12-bit component
// masks and zigzag residual bits. A packed key predicts identity, not bind pose.

inline uint32_t ReadPose32(uint8_t const* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void WritePose32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline bool PoseNewer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

// Every residual is lossless relative to the existing Q12/Q2 affine integers.
// No quaternion projection, cumulative quantization, bind-pose assumption or
// unacknowledged predecessor is involved; arbitrary scale/shear survives.
class PoseCache {
    struct Frame {
        uint32_t token, incarnation;
        uint16_t layout;
        uint8_t flags;
        std::vector<pose::Bone> bones;
    };
    std::deque<Frame> frames_;
    uint32_t ack_ = 0, keyAt_ = 0;
    bool haveKey_ = false;

    Frame const* Find(uint32_t token, uint32_t incarnation, uint16_t layout, uint8_t flags) const
    {
        for (auto const& f : frames_)
            if (f.token == token && f.incarnation == incarnation && f.layout == layout && f.flags == flags) return &f;
        return nullptr;
    }

public:
    size_t Retained() const { return frames_.size(); }
    bool Ack(uint32_t token, uint32_t incarnation)
    {
        if (!token) {
            if (frames_.empty() || frames_.back().incarnation != incarnation) return false;
            ack_ = 0; return true;
        }
        for (auto const& f : frames_)
            if (f.token == token && f.incarnation == incarnation) {
                if (!ack_ || PoseNewer(token, ack_)) ack_ = token;
                return true;
            }
        return false;
    }

    bool Store(uint32_t token, uint32_t incarnation, uint16_t layout, uint8_t flags,
               pose::Bone const* bones, size_t count, uint32_t now, bool key)
    {
        if (!token || !incarnation || !layout || !bones || !count || count > pose::MaxBones) return false;
        if (!frames_.empty()) {
            auto const& last = frames_.back();
            if (PoseNewer(last.incarnation, incarnation) ||
                (last.incarnation == incarnation && !PoseNewer(token, last.token))) return false;
            if (last.incarnation != incarnation || last.layout != layout || last.flags != flags) {
                frames_.clear();
                ack_ = 0;
            }
        }
        if (frames_.size() == PoseWindow) frames_.pop_front();
        frames_.push_back({token, incarnation, layout, flags, {bones, bones + count}});
        if (key) { keyAt_ = now; haveKey_ = true; }
        return true;
    }

    Status Encode(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                  pose::Bone const* bones, size_t count, uint32_t now, std::vector<uint8_t>& out, bool& key) const
    {
        std::vector<uint8_t> full;
        auto status = dictionary.EncodePose(layout, bones, count, full);
        if (status != Status::Ok) return status;
        full.insert(full.begin() + 2, 4, 0);
        auto const* base = Find(ack_, incarnation, layout, flags);
        Frame identity{0, incarnation, layout, flags, {}};
        if (!base || base->bones.size() != count || !haveKey_ || now - keyAt_ >= PoseKeyMs) {
            status = dictionary.IdentityPose(layout, identity.bones);
            if (status != Status::Ok) return status;
            base = &identity;
        }
        std::vector<uint8_t> delta{static_cast<uint8_t>(layout), static_cast<uint8_t>(layout >> 8)};
        WritePose32(delta, base->token);
        delta.push_back(1); // bitpacked residuals; zero is the full affine key
        full.insert(full.begin() + DeltaHeaderBytes, 0);
        uint8_t widths[12] = {};
        std::vector<uint16_t> components(count, 0);
        for (size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                const int32_t d = int32_t(bones[i].values[k]) - base->bones[i].values[k];
                uint32_t value = d < 0 ? uint32_t(-d) * 2 - 1 : uint32_t(d) * 2;
                if (!value) continue;
                components[i] |= uint16_t(1u << k);
                uint8_t width = 0;
                do { ++width; value >>= 1; } while (value);
                widths[k] = (std::max)(widths[k], width);
            }
        }
        delta.insert(delta.end(), widths, widths + 12);
        const size_t maskAt = delta.size(), maskBytes = (count + 7) / 8;
        delta.resize(maskAt + maskBytes, 0);
        unsigned bit = 0;
        auto put = [&](uint32_t value, unsigned width) {
            for (unsigned n = 0; n < width; ++n) {
                if (!bit) delta.push_back(0);
                delta.back() |= static_cast<uint8_t>((value & 1u) << bit);
                value >>= 1;
                bit = (bit + 1) % 8;
            }
        };
        for (size_t i = 0; i < count; ++i) {
            const uint16_t mask = components[i];
            if (!mask) continue;
            delta[maskAt + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            put(mask, 12);
        }
        for (size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                if (!(components[i] & (1u << k))) continue;
                const int32_t difference = int32_t(bones[i].values[k]) - base->bones[i].values[k];
                uint32_t value = difference < 0 ? uint32_t(-difference) * 2 - 1 : uint32_t(difference) * 2;
                put(value, widths[k]);
            }
        }
        key = !base->token || delta.size() >= full.size();
        out = delta.size() >= full.size() ? std::move(full) : std::move(delta);
        return Status::Ok;
    }

    Status Decode(Dictionary const& dictionary, uint8_t const* data, size_t size,
                  uint32_t incarnation, uint8_t flags, std::vector<pose::Bone>& out, uint16_t& layout) const
    {
        if (!data || size <= DeltaHeaderBytes || !incarnation || (flags & ~pose::Facial)) return Status::Malformed;
        const uint16_t id = uint16_t(data[0]) | uint16_t(uint16_t(data[1]) << 8);
        if (!id) return Status::Malformed;
        if (!dictionary.HasLayout(id)) { layout = id; return Status::UnknownLayout; }
        if (!frames_.empty() && PoseNewer(frames_.back().incarnation, incarnation)) return Status::WrongGeneration;
        const uint32_t ref = ReadPose32(data + 2);
        const uint8_t mode = data[DeltaHeaderBytes];
        if (mode > 1) return Status::Malformed;
        if (!mode) {
            if (ref) return Status::Malformed;
            std::vector<uint8_t> full(data, data + 2);
            full.insert(full.end(), data + DeltaHeaderBytes + 1, data + size);
            return dictionary.DecodePose(full.data(), full.size(), out, layout);
        }
        auto const* base = Find(ref, incarnation, id, flags);
        Frame identity{0, incarnation, id, flags, {}};
        if (!ref) {
            auto status = dictionary.IdentityPose(id, identity.bones);
            if (status != Status::Ok) return status;
            base = &identity;
        }
        if (!base) { layout = id; return Status::UnknownPose; }
        const size_t count = base->bones.size(), maskBytes = (count + 7) / 8, maskAt = DeltaHeaderBytes + 1 + 12;
        if (size < maskAt + maskBytes ||
            (count % 8 && (data[maskAt + maskBytes - 1] >> (count % 8)))) return Status::Malformed;
        auto const* widths = data + DeltaHeaderBytes + 1;
        for (int k = 0; k < 12; ++k) if (widths[k] > 17) return Status::Malformed;
        auto result = base->bones;
        size_t offset = maskAt + maskBytes;
        std::vector<uint16_t> masks(count, 0);
        unsigned bit = 0;
        auto get = [&](unsigned width, uint32_t& value) {
            value = 0;
            for (unsigned n = 0; n < width; ++n) {
                if (offset == size) return false;
                value |= uint32_t((data[offset] >> bit) & 1u) << n;
                if (++bit == 8) { bit = 0; ++offset; }
            }
            return true;
        };
        for (size_t i = 0; i < count; ++i) {
            if (!(data[maskAt + i / 8] & (1u << (i % 8)))) continue;
            uint32_t mask;
            if (!get(12, mask) || !mask) return Status::Malformed;
            masks[i] = static_cast<uint16_t>(mask);
        }
        for (size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                if (!(masks[i] & (1u << k))) continue;
                if (!widths[k]) return Status::Malformed;
                uint32_t value = 0;
                if (!get(widths[k], value)) return Status::Malformed;
                if (value > 131070 || !value) return Status::Malformed;
                const int32_t difference = value & 1 ? -int32_t((value + 1) / 2) : int32_t(value / 2);
                const int32_t decoded = base->bones[i].values[k] + difference;
                if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                result[i].values[k] = static_cast<int16_t>(decoded);
            }
        }
        if (bit) {
            if (data[offset] >> bit) return Status::Malformed;
            ++offset;
        }
        if (offset != size) return Status::Malformed;
        out = std::move(result);
        layout = id;
        return Status::Ok;
    }
};
}
