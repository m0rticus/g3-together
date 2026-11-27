// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_bonepack.h"
#include "npcwire_bones.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#if defined(G3T_NPCWIRE_POSE_SEPARATE_TU)
#define G3T_NPCWIRE_POSE_INLINE
#else
#define G3T_NPCWIRE_POSE_INLINE inline
#endif

namespace g3t::npcwire::bones {

// Reliable independent anchors are sent periodically; deltas reference acknowledged frames only.
constexpr uint32_t PoseKeyMs = 5000;
constexpr size_t PoseWindow = 32;
constexpr size_t PoseAnchors = 4; // recent anchors, in addition to the first
constexpr size_t PoseRetainedAnchors = PoseAnchors + 1;
constexpr size_t DeltaHeaderBytes = 6; // layout:u16, acknowledged reference:u32 (zero = key)
// A mode byte follows: 0 = raw 24-byte affine key per bone; 1 = twelve
// one-byte residual widths (0..17), bone presence bits, packed 12-bit component
// masks and zigzag residual bits. A packed key predicts identity, not bind pose.
// Modes 2/3: flags:u8, older ACK:u32, ratio:Q8 u16 (0..1024),
// twelve Rice k nibbles, padded bone presence bits, exact component residuals.
// Zero references and ratio predict identity independently. Mode 3 predicts
// the adjacent bone's temporal residual as well; its flag bit 0 selects a
// second-order predictor (2 * previous residual - second previous residual).
// Mode 4 adds a rigid-bone bitmap and packed 2-bit quaternion omitted indices.
// Rigid bones carry three Q14 quaternion coordinates and exact translations;
// other bones retain all twelve affine integers. Every Rice escape is bounded.
// Mode-4 flags: bit 0 selects separate Rice parameters per 16 bones; bit 1
// selects a Q12 grid (multiples of four Q14 units) for rigid coordinates only;
// bit 2 selects second-order spatial prediction for compatible bones.

inline uint32_t ReadPose32(uint8_t const* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void WritePose32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline bool PoseNewer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

// Encode is exact; EncodeCompact also considers guarded near-rigid rotations
// (at most two Q12 units per basis entry). Both use explicitly ACKed canonical
// references. Store the returned reconstruction, never an untransmitted source.
// Mode 5 compacts Mode 4 further: omits secondary/ratio when unused, omits
// presence/rigid/omitted masks when unchanged or all-present/all-rigid, uses
// compact 3/6/9/12 Rice parameters, skips zero translation deltas, and emits
// an 8-byte (or 14-byte with older ref) zero-residual frame when all bones
// match temporal prediction.
class PoseCache {
    struct Frame {
        uint32_t token, incarnation;
        uint16_t layout;
        uint8_t flags;
        uint32_t now;
        std::vector<pose::Bone> bones;
    };
    std::deque<Frame> frames_;
    std::deque<Frame> anchors_; // newest token first, independent of the transient window
    std::optional<Frame> firstAnchor_;
    std::deque<uint32_t> ackRefs_;
    std::vector<std::vector<pose::Bone>> spareBones_;
    uint32_t keyAt_ = 0;
    bool haveKey_ = false;
    uint32_t anchorAt_ = 0, anchorToken_ = 0;
    bool haveAnchor_ = false;
    uint32_t latestToken_ = 0, incarnation_ = 0;
    uint16_t layout_ = 0;
    uint8_t flags_ = 0;

    void Recycle(Frame& frame)
    {
        if (spareBones_.size() < 8) {
            frame.bones.clear();
            spareBones_.push_back(std::move(frame.bones));
        }
    }

    Frame const* Find(uint32_t token, uint32_t incarnation, uint16_t layout, uint8_t flags) const
    {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
            if (it->token == token && it->incarnation == incarnation &&
                it->layout == layout && it->flags == flags) return &*it;
        for (auto const& f : anchors_)
            if (f.token == token && f.incarnation == incarnation &&
                f.layout == layout && f.flags == flags) return &f;
        if (firstAnchor_ && firstAnchor_->token == token && firstAnchor_->incarnation == incarnation &&
            firstAnchor_->layout == layout && firstAnchor_->flags == flags) return &*firstAnchor_;
        return nullptr;
    }

    void PruneAckRefs()
    {
        ackRefs_.erase(std::remove_if(ackRefs_.begin(), ackRefs_.end(), [&](uint32_t token) {
            return !Find(token, incarnation_, layout_, flags_);
        }), ackRefs_.end());
    }

    static int32_t TemporalPrediction(Frame const& recent, Frame const* older,
                                      unsigned ratio, size_t bone, int component) noexcept
    {
        const int32_t base = recent.bones[bone].values[component];
        if (!older) return base;
        const int32_t difference = base - older->bones[bone].values[component];
        return PoseSaturateInt16(int64_t(base) + PoseRoundQ8(difference * static_cast<int32_t>(ratio)));
    }

    static unsigned Ratio(Frame const& recent, Frame const* older, uint32_t now) noexcept
    {
        if (!older || !PoseNewer(recent.now, older->now) || !PoseNewer(now, recent.now)) return 0;
        const uint32_t span = recent.now - older->now;
        const uint32_t elapsed = now - recent.now;
        if (!span) return 0;
        const uint64_t ratio = (uint64_t(elapsed) * 256u + span / 2u) / span;
        return static_cast<unsigned>((std::min)(uint64_t(1024), ratio));
    }

    static bool QuaternionPrediction(Frame const& recent, Frame const* older, unsigned ratio,
                                     size_t bone, uint8_t omitted, PoseQuat& prediction) noexcept
    {
        if (!PoseExtractQuat(recent.bones[bone], prediction)) return false;
        if (prediction.values[omitted] < 0)
            for (auto& value : prediction.values) value = -value;
        if (!older) return true;
        PoseQuat previous;
        if (!PoseExtractQuat(older->bones[bone], previous)) return false;
        if (previous.values[omitted] < 0)
            for (auto& value : previous.values) value = -value;
        for (int k = 0; k < 4; ++k)
            prediction.values[k] = PoseSaturateInt16(int64_t(prediction.values[k]) +
                PoseRoundQ8((prediction.values[k] - previous.values[k]) * static_cast<int32_t>(ratio)));
        return true;
    }

    static void EncodeMode1Sparse(uint16_t layout, Frame const& base,
                                  pose::Bone const* bones, size_t count,
                                  std::size_t& bestSize, std::vector<uint8_t>& result,
                                  bool& resultKey, PoseStats& resultStats);
    static void EncodeMode2And3(uint16_t layout, Frame const& recent, Frame const* olderCandidate,
                                pose::Bone const* bones, size_t count, uint32_t now,
                                std::size_t& bestSize, std::vector<uint8_t>& result,
                                bool& resultKey, PoseStats& resultStats);
    static void EncodeMode4And5(uint16_t layout, Frame const& recent, Frame const* olderCandidate,
                                pose::Bone const* bones, size_t count, uint32_t now,
                                std::size_t& bestSize, std::vector<uint8_t>& result, bool& resultKey,
                                std::vector<pose::Bone>& reconstructed, PoseStats& resultStats);
    Status EncodeCompactImpl(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                             pose::Bone const* bones, size_t count, uint32_t now,
                             std::vector<uint8_t>& out, bool& key,
                             std::vector<pose::Bone>& decodedOutput, PoseStats* stats,
                             bool allowMode4, bool forceKey, bool prevalidated) const;

    Status DecodeMode0(Dictionary const& dictionary, uint8_t const* data, size_t size, uint32_t ref,
                       std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const;
    Status DecodeMode1(Dictionary const& dictionary, uint8_t const* data, size_t size,
                       uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
                       std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const;
    Status DecodeMode2And3(Dictionary const& dictionary, uint8_t const* data, size_t size,
                           uint8_t mode, uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
                           std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const;
    Status DecodeMode4(uint8_t const* data, size_t size,
                       uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
                       std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const;
    Status DecodeMode5(Dictionary const& dictionary, uint8_t const* data, size_t size,
                       uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
                       std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const;

public:
    size_t Retained() const { return frames_.size() + anchors_.size() + (firstAnchor_ ? 1 : 0); }
    bool AnchorDue(uint32_t now) const { return !haveAnchor_ || now - anchorAt_ >= PoseKeyMs; }
    bool AnchorDue(uint32_t now, uint32_t incarnation, uint16_t layout, uint8_t flags) const
    {
        return !latestToken_ || incarnation != incarnation_ || layout != layout_ || flags != flags_ ||
            AnchorDue(now);
    }
    uint32_t LatestAckRef() const noexcept { return ackRefs_.empty() ? 0 : ackRefs_.front(); }
    uint32_t LatestAckTime() const noexcept
    {
        if (ackRefs_.empty()) return 0;
        const Frame* f = Find(ackRefs_.front(), incarnation_, layout_, flags_);
        return f ? f->now : 0;
    }

    bool Ack(uint32_t token, uint32_t incarnation)
    {
        if (!token) {
            if (!latestToken_ || incarnation_ != incarnation) return false;
            ackRefs_.clear();
            return true;
        }
        if (incarnation != incarnation_ || !Find(token, incarnation, layout_, flags_)) return false;
        if (std::find(ackRefs_.begin(), ackRefs_.end(), token) != ackRefs_.end()) return true;
        auto position = std::find_if(ackRefs_.begin(), ackRefs_.end(),
                                     [&](uint32_t ref) { return PoseNewer(token, ref); });
        ackRefs_.insert(position, token);
        while (ackRefs_.size() > PoseAnchors) ackRefs_.pop_back();
        return true;
    }

    bool Store(uint32_t token, uint32_t incarnation, uint16_t layout, uint8_t flags,
               pose::Bone const* bones, size_t count, uint32_t now, bool key, bool pin = false)
    {
        if (!token || !incarnation || !layout || !bones || !count || count > pose::MaxBones ||
            (flags & ~pose::Facial) || (pin && !key)) return false;
        const bool sameLife = latestToken_ && incarnation == incarnation_;
        if (latestToken_ && !sameLife && !PoseNewer(incarnation, incarnation_)) return false;
        const bool sameContext = sameLife && layout == layout_ && flags == flags_;
        const bool newer = !sameLife || PoseNewer(token, latestToken_);
        if (sameLife && !newer && (!sameContext || !pin)) return false;
        if (sameContext && pin) {
            const Frame* existing = Find(token, incarnation, layout, flags);
            if (existing && (existing->bones.size() != count ||
                std::memcmp(existing->bones.data(), bones, count * sizeof(pose::Bone)) != 0)) return false;
            if (firstAnchor_ && firstAnchor_->token == token) return true;
            for (auto const& anchor : anchors_)
                if (anchor.token == token) return true;
            if (anchors_.size() == PoseAnchors && !PoseNewer(token, anchors_.back().token))
                return false;
        }
        if (!sameContext) {
            for (auto& f : frames_) Recycle(f);
            frames_.clear();
            for (auto& f : anchors_) Recycle(f);
            anchors_.clear();
            if (firstAnchor_) { Recycle(*firstAnchor_); firstAnchor_.reset(); }
            ackRefs_.clear();
            keyAt_ = anchorAt_ = anchorToken_ = 0;
            haveKey_ = haveAnchor_ = false;
            incarnation_ = incarnation;
            layout_ = layout;
            flags_ = flags;
            latestToken_ = 0;
        }
        if (pin) {
            auto existing = std::find_if(frames_.begin(), frames_.end(),
                                         [&](Frame const& f) { return f.token == token; });
            Frame frame;
            if (existing != frames_.end()) {
                frame = std::move(*existing);
                frames_.erase(existing);
            } else {
                frame = {token, incarnation, layout, flags, now, {}};
                if (!spareBones_.empty()) {
                    frame.bones = std::move(spareBones_.back());
                    spareBones_.pop_back();
                }
                frame.bones.assign(bones, bones + count);
            }
            if (!firstAnchor_) {
                firstAnchor_ = std::move(frame);
            } else {
                auto position = std::find_if(anchors_.begin(), anchors_.end(),
                                             [&](Frame const& f) { return PoseNewer(token, f.token); });
                anchors_.insert(position, std::move(frame));
                if (anchors_.size() > PoseAnchors) {
                    Recycle(anchors_.back());
                    anchors_.pop_back();
                }
            }
            if (!haveAnchor_ || PoseNewer(token, anchorToken_)) {
                anchorAt_ = now;
                anchorToken_ = token;
                haveAnchor_ = true;
            }
        } else {
            if (frames_.size() == PoseWindow) {
                Recycle(frames_.front());
                frames_.pop_front();
            }
            Frame frame{token, incarnation, layout, flags, now, {}};
            if (!spareBones_.empty()) {
                frame.bones = std::move(spareBones_.back());
                spareBones_.pop_back();
            }
            frame.bones.assign(bones, bones + count);
            frames_.push_back(std::move(frame));
        }
        if (newer) latestToken_ = token;
        if (key && (newer || !haveKey_)) { keyAt_ = now; haveKey_ = true; }
        PruneAckRefs();
        return true;
    }

    Status Encode(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                  pose::Bone const* bones, size_t count, uint32_t now,
                  std::vector<uint8_t>& out, bool& key, PoseStats* stats = nullptr,
                  bool forceKey = false) const;

    Status EncodeCompact(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                         pose::Bone const* bones, size_t count, uint32_t now,
                         std::vector<uint8_t>& out, bool& key,
                         std::vector<pose::Bone>& reconstructed, PoseStats* stats = nullptr,
                         bool forceKey = false, bool prevalidated = false) const;

    Status Decode(Dictionary const& dictionary, uint8_t const* data, size_t size,
                  uint32_t incarnation, uint8_t flags,
                  std::vector<pose::Bone>& out, uint16_t& layout,
                  PoseStats* stats = nullptr) const;
};

} // namespace g3t::npcwire::bones

#if !defined(G3T_NPCWIRE_POSE_SEPARATE_TU) && !defined(G3T_NPCWIRE_POSE_IMPL_INCLUDED)
#include "npcwire_pose.cpp"
#endif
