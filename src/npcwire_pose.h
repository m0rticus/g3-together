// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_bonepack.h"
#include "npcwire_bones.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

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
    uint32_t keyAt_ = 0;
    bool haveKey_ = false;
    uint32_t anchorAt_ = 0, anchorToken_ = 0;
    bool haveAnchor_ = false;
    uint32_t latestToken_ = 0, incarnation_ = 0;
    uint16_t layout_ = 0;
    uint8_t flags_ = 0;

    Frame const* Find(uint32_t token, uint32_t incarnation, uint16_t layout, uint8_t flags) const
    {
        for (auto const& f : frames_)
            if (f.token == token && f.incarnation == incarnation &&
                f.layout == layout && f.flags == flags) return &f;
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
                                      unsigned ratio, size_t bone, int component)
    {
        const int32_t base = recent.bones[bone].values[component];
        if (!older) return base;
        const int64_t difference = int64_t(base) - older->bones[bone].values[component];
        return PoseSaturateInt16(int64_t(base) + PoseRoundQ8(difference * ratio));
    }

    static unsigned Ratio(Frame const& recent, Frame const* older, uint32_t now)
    {
        if (!older || !PoseNewer(recent.now, older->now) || !PoseNewer(now, recent.now)) return 0;
        const uint32_t span = recent.now - older->now;
        const uint32_t elapsed = now - recent.now;
        if (!span) return 0;
        const uint64_t ratio = (uint64_t(elapsed) * 256u + span / 2u) / span;
        return static_cast<unsigned>((std::min)(uint64_t(1024), ratio));
    }

    static bool QuaternionPrediction(Frame const& recent, Frame const* older, unsigned ratio,
                                     size_t bone, uint8_t omitted, PoseQuat& prediction)
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
                PoseRoundQ8((int64_t(prediction.values[k]) - previous.values[k]) * ratio));
        return true;
    }

public:
    size_t Retained() const { return frames_.size() + anchors_.size() + (firstAnchor_ ? 1 : 0); }
    bool AnchorDue(uint32_t now) const { return !haveAnchor_ || now - anchorAt_ >= PoseKeyMs; }
    bool AnchorDue(uint32_t now, uint32_t incarnation, uint16_t layout, uint8_t flags) const
    {
        return !latestToken_ || incarnation != incarnation_ || layout != layout_ || flags != flags_ ||
            AnchorDue(now);
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
            frames_.clear();
            anchors_.clear();
            firstAnchor_.reset();
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
            Frame frame = existing != frames_.end()
                ? *existing
                : Frame{token, incarnation, layout, flags, now,
                        std::vector<pose::Bone>(bones, bones + count)};
            if (existing != frames_.end()) frames_.erase(existing);
            if (!firstAnchor_) {
                firstAnchor_ = std::move(frame);
            } else {
                auto position = std::find_if(anchors_.begin(), anchors_.end(),
                                             [&](Frame const& f) { return PoseNewer(token, f.token); });
                anchors_.insert(position, std::move(frame));
                if (anchors_.size() > PoseAnchors) anchors_.pop_back();
            }
            if (!haveAnchor_ || PoseNewer(token, anchorToken_)) {
                anchorAt_ = now;
                anchorToken_ = token;
                haveAnchor_ = true;
            }
        } else {
            if (frames_.size() == PoseWindow) frames_.pop_front();
            frames_.push_back({token, incarnation, layout, flags, now,
                               std::vector<pose::Bone>(bones, bones + count)});
        }
        if (newer) {
            latestToken_ = token;
        }
        if (key && (newer || !haveKey_)) { keyAt_ = now; haveKey_ = true; }
        PruneAckRefs();
        return true;
    }

    Status EncodeCompactImpl(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                             pose::Bone const* bones, size_t count, uint32_t now,
                             std::vector<uint8_t>& out, bool& key,
                             std::vector<pose::Bone>& decodedOutput, PoseStats* stats,
                             bool allowMode4, bool forceKey) const
    {
        if (!dictionary.HasLayout(layout) || !incarnation || !layout || !bones ||
            !count || count > pose::MaxBones || (flags & ~pose::Facial))
            return Status::InvalidArgument;

        std::vector<uint8_t> full;
        Status status = dictionary.EncodePose(layout, bones, count, full);
        if (status != Status::Ok) return status;
        std::vector<pose::Bone> reconstructed(bones, bones + count);
        full.insert(full.begin() + 2, 4, 0);
        full.insert(full.begin() + DeltaHeaderBytes, 0);

        std::vector<uint8_t> result = full;
        bool resultKey = true;
        PoseStats resultStats;
        resultStats.headerBytes = 7;
        resultStats.rotationBits = count * 144;
        resultStats.translationBits = count * 48;
        resultStats.includedBones = 0;
        resultStats.predictive = false;

        bool keyDue = forceKey || !haveKey_ || now - keyAt_ >= PoseKeyMs;
        const Frame* base = nullptr;
        if (!keyDue && !ackRefs_.empty())
            base = Find(ackRefs_.front(), incarnation, layout, flags);
        if (!base || base->bones.size() != count) {
            base = nullptr;
            keyDue = true;
        }

        Frame identity{0, incarnation, layout, flags, now, {}};
        if (keyDue) {
            status = dictionary.IdentityPose(layout, identity.bones);
            if (status != Status::Ok) return status;
        } else {
            identity = *base;
        }
        base = &identity;
        if (!base->bones.empty() && base->bones.size() != count) return Status::Malformed;

        std::vector<uint8_t> sparse{static_cast<uint8_t>(layout), static_cast<uint8_t>(layout >> 8)};
        WritePose32(sparse, base->token);
        sparse.push_back(1);
        uint8_t widths[12] = {};
        std::vector<uint16_t> components(count, 0);
        std::size_t includedSparse = 0;
        for (std::size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                const int32_t difference = int32_t(bones[i].values[k]) - base->bones[i].values[k];
                if (!difference) continue;
                uint32_t value = difference < 0
                    ? uint32_t(-int64_t(difference) * 2 - 1)
                    : uint32_t(int64_t(difference) * 2);
                components[i] |= uint16_t(1u << k);
                uint8_t width = 0;
                do { ++width; value >>= 1; } while (value);
                widths[k] = (std::max)(widths[k], width);
            }
            if (components[i]) ++includedSparse;
        }
        sparse.insert(sparse.end(), widths, widths + 12);
        const std::size_t maskAt = sparse.size();
        const std::size_t maskBytes = (count + 7) / 8;
        sparse.resize(maskAt + maskBytes, 0);
        unsigned bit = 0;
        auto put = [&](uint32_t value, unsigned width) {
            for (unsigned n = 0; n < width; ++n) {
                if (!bit) sparse.push_back(0);
                sparse.back() |= static_cast<uint8_t>((value & 1u) << bit);
                value >>= 1;
                bit = (bit + 1) % 8;
            }
        };
        for (std::size_t i = 0; i < count; ++i) {
            if (!components[i]) continue;
            sparse[maskAt + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            put(static_cast<uint32_t>(components[i]), 12);
        }
        for (std::size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                if (!(components[i] & (1u << k))) continue;
                const int32_t difference = int32_t(bones[i].values[k]) - base->bones[i].values[k];
                uint32_t value = difference < 0
                    ? uint32_t(-int64_t(difference) * 2 - 1)
                    : uint32_t(int64_t(difference) * 2);
                put(value, widths[k]);
            }
        }
        if (sparse.size() < result.size()) {
            result = sparse;
            resultKey = base->token == 0;
            resultStats = {};
            resultStats.headerBytes = 19;
            resultStats.maskBits = maskBytes * 8 + includedSparse * 12;
            resultStats.includedBones = includedSparse;
            std::size_t residualRotation = 0, residualTranslation = 0;
            for (std::size_t i = 0; i < count; ++i) for (int k = 0; k < 12; ++k)
                if (components[i] & (1u << k))
                    (k < 9 ? residualRotation : residualTranslation) += widths[k];
            resultStats.rotationBits = residualRotation;
            resultStats.translationBits = residualTranslation;
        }

        {
            const Frame* recent = base;
            const Frame* olderCandidate = !keyDue && ackRefs_.size() >= 2
                ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
            if (olderCandidate && (olderCandidate->bones.size() != count ||
                                   !PoseNewer(recent->token, olderCandidate->token)))
                olderCandidate = nullptr;
            for (bool useOlder : {false, true}) {
                if (useOlder && !olderCandidate) continue;
                const Frame* older = useOlder ? olderCandidate : nullptr;
                const unsigned ratio = older ? Ratio(*recent, older, now) : 0;
                for (uint8_t variant = 0; variant < 3; ++variant) {
                    const uint8_t predictiveMode = variant == 0 ? 2 : 3;
                    const bool secondOrder = variant == 2;
                    std::array<unsigned, 12> kval{};
                    std::vector<uint32_t> residual(count * 12, 0);
                    std::vector<uint8_t> present(count, 0);
                    std::array<int32_t, 12> prior{}, prior2{};
                    for (std::size_t i = 0; i < count; ++i) {
                        for (int k = 0; k < 12; ++k) {
                            const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                            int32_t residualValue = int32_t(bones[i].values[k]) - prediction;
                            if (predictiveMode == 3) {
                                const int32_t spatial = secondOrder && i > 1
                                    ? 2 * prior[k] - prior2[k] : (i ? prior[k] : 0);
                                residualValue -= spatial;
                            }
                            uint32_t zig = PoseZigZag(residualValue);
                            residual[i * 12 + k] = zig;
                            if (zig) present[i] = 1;
                            if (predictiveMode == 3) {
                                prior2[k] = prior[k];
                                prior[k] = int32_t(bones[i].values[k]) - prediction;
                            }
                        }
                    }
                    for (int k = 0; k < 12; ++k) {
                        PoseRiceParameters parameters;
                        for (std::size_t i = 0; i < count; ++i)
                            if (present[i]) parameters.Add(residual[i * 12 + k]);
                        kval[k] = parameters.Best();
                    }
                    const std::size_t presenceBytes = (count + 7) / 8;
                    PoseRiceWriter writer;
                    writer.Put(layout, 16);
                    writer.Put(recent->token, 32);
                    writer.Put(predictiveMode, 8);
                    writer.Put(secondOrder ? 1 : 0, 8);
                    writer.Put(older ? older->token : 0, 32);
                    writer.Put(ratio, 16);
                    for (unsigned k : kval) writer.Put(k, 4);
                    for (std::size_t i = 0; i < count; ++i)
                        writer.Put(present[i] ? 1u : 0u, 1);
                    while (writer.bits & 7) writer.Put(0, 1);
                    std::size_t included = 0, residualRotation = 0, residualTranslation = 0;
                    for (std::size_t i = 0; i < count; ++i) if (present[i]) ++included;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (!present[i]) continue;
                        for (int k = 0; k < 12; ++k) {
                            const std::size_t before = writer.bits;
                            writer.Rice(residual[i * 12 + k], kval[k], 21);
                            (k < 9 ? residualRotation : residualTranslation) += writer.bits - before;
                        }
                    }
                    writer.Align();

                    if (writer.bytes.size() < result.size()) {
                        result = std::move(writer.bytes);
                        resultKey = recent->token == 0;
                        resultStats = {};
                        resultStats.headerBytes = 20;
                        resultStats.maskBits = presenceBytes * 8;
                        resultStats.rotationBits = residualRotation;
                        resultStats.translationBits = residualTranslation;
                        resultStats.includedBones = included;
                        resultStats.predictive = true;
                    }
                }
            }
        }

        if (allowMode4 && !keyDue && ackRefs_.size() >= 1) {
            const Frame *recent = Find(ackRefs_.front(), incarnation, layout, flags);
            const Frame *olderCandidate =
                ackRefs_.size() >= 2 ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
            if (recent && recent->bones.size() == count &&
                (!olderCandidate || (olderCandidate->bones.size() == count &&
                                     PoseNewer(recent->token, olderCandidate->token)))) {
              for (bool useOlder : {false, true}) {
                if (useOlder && !olderCandidate) continue;
                const Frame* older = useOlder ? olderCandidate : nullptr;
                const unsigned ratio = older ? Ratio(*recent, older, now) : 0;
                std::array<PoseQuat, pose::MaxBones> sourceQuaternions{}, predictedQuaternions{};
                std::array<bool, pose::MaxBones> quaternionReady{};
                for (size_t i = 0; i < count; ++i)
                    quaternionReady[i] = PoseExtractQuat(bones[i], sourceQuaternions[i]) &&
                        QuaternionPrediction(*recent, older, ratio, i, sourceQuaternions[i].omitted,
                                             predictedQuaternions[i]);
                for (bool coarse : {false, true}) for (bool secondOrder : {false, true}) {
                    std::array<uint8_t, pose::MaxBones> boneType{};
                    std::array<uint8_t, pose::MaxBones> omitted{};
                    std::vector<uint32_t> residual(count * 12, 0);
                    std::size_t rigidCount = 0;
                    std::size_t included = 0;
                    size_t maxBasisError = 0;
                    int32_t prior[pose::MaxBones][12] = {};
                    uint8_t previousTypesForBone[pose::MaxBones] = {};
                    std::vector<pose::Bone> rigidReconstruction(bones, bones + count);
                    auto spatial = [&](size_t i, int k, uint8_t type, uint8_t omittedIndex) {
                        if (!i || previousTypesForBone[i - 1] != type ||
                            (type == 2 && omitted[i - 1] != omittedIndex)) return int32_t(0);
                        if (secondOrder && i > 1 && previousTypesForBone[i - 2] == type &&
                            (type != 2 || omitted[i - 2] == omittedIndex))
                            return 2 * prior[i - 1][k] - prior[i - 2][k];
                        return prior[i - 1][k];
                    };

                    for (std::size_t i = 0; i < count; ++i) {
                        PoseQuat targetQuat = sourceQuaternions[i], recentQuat = predictedQuaternions[i];
                        bool rigid = quaternionReady[i];
                        pose::Bone rigidMatrix{};
                        size_t boneError = 0;
                        if (rigid) {
                            auto accepts = [&](PoseQuat const &quaternion) {
                                if (!PoseQuatToBone(quaternion, bones[i], rigidMatrix))
                                    return false;
                                boneError = 0;
                                for (int k = 0; k < 9; ++k) {
                                    const size_t error = size_t(std::abs(int32_t(rigidMatrix.values[k]) -
                                                                         int32_t(bones[i].values[k])));
                                    if (error > boneError)
                                        boneError = error;
                                    if (error > 2)
                                        return false;
                                }
                                return true;
                            };
                            if (coarse) {
                                const PoseQuat original = targetQuat;
                                for (auto &value : recentQuat.values)
                                    value = PoseRoundSignedDivide(value, 4) * 4;
                                for (int k = 0; k < 4; ++k)
                                    if (k != targetQuat.omitted)
                                        targetQuat.values[k] =
                                            PoseRoundSignedDivide(targetQuat.values[k], 4) * 4;
                                rigid = PoseCompleteQuat(targetQuat) && accepts(targetQuat);
                                for (unsigned choice = 0; !rigid && choice < 8; ++choice) {
                                    targetQuat = original;
                                    unsigned rank = 0;
                                    for (int k = 0; k < 4; ++k)
                                        if (k != targetQuat.omitted) {
                                            const int value = original.values[k];
                                            const int floor = value - ((value % 4 + 4) % 4);
                                            targetQuat.values[k] =
                                                floor + static_cast<int>((choice >> rank++) & 1u) * 4;
                                        }
                                    rigid = PoseCompleteQuat(targetQuat) && accepts(targetQuat);
                                }
                            } else
                                rigid = accepts(targetQuat);
                        }

                        if (rigid) {
                            maxBasisError = (std::max)(maxBasisError, boneError);
                            rigidReconstruction[i] = rigidMatrix;
                            boneType[i] = 2;
                            omitted[i] = targetQuat.omitted;
                            for (int k = 0; k < 12; ++k)
                                residual[i * 12 + k] = 0;
                            for (int k = 0; k < 12; ++k)
                                prior[i][k] = 0;
                            for (int k = 0; k < 4; ++k) {
                                if (k == targetQuat.omitted)
                                    continue;
                                int32_t difference = targetQuat.values[k] - recentQuat.values[k];
                                prior[i][k] = targetQuat.values[k] - recentQuat.values[k];
                                difference -= spatial(i, k, 2, omitted[i]);
                                residual[i * 12 + k] = PoseZigZag(coarse ? difference / 4 : difference);
                            }
                            for (int k = 9; k < 12; ++k) {
                                int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                                int32_t difference = int32_t(bones[i].values[k]) - prediction;
                                difference -= spatial(i, k, 2, omitted[i]);
                                prior[i][k] = int32_t(bones[i].values[k]) - prediction;
                                residual[i * 12 + k] = PoseZigZag(difference);
                            }
                            ++rigidCount;
                            ++included;
                            previousTypesForBone[i] = boneType[i];
                            continue;
                        }

                        const bool changedAffine = [&]() {
                            bool changed = false;
                            for (int k = 0; k < 12; ++k) {
                                int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                                int32_t difference = int32_t(bones[i].values[k]) - prediction;
                                difference -= spatial(i, k, 1, 0);
                                prior[i][k] = int32_t(bones[i].values[k]) - prediction;
                                residual[i * 12 + k] = PoseZigZag(difference);
                                if (difference)
                                    changed = true;
                            }
                            return changed;
                        }();
                        if (changedAffine) {
                            boneType[i] = 1;
                            ++included;
                        }
                        previousTypesForBone[i] = boneType[i];
                    }

                    if (included) {
                        for (bool grouped : {false, true}) {
                            const size_t groupBones = grouped ? 16 : count;
                            const size_t groups = (count + groupBones - 1) / groupBones;
                            std::vector<std::array<unsigned, 12>> parameters(groups);
                            for (size_t group = 0; group < groups; ++group) {
                                for (int k = 0; k < 12; ++k) {
                                    PoseRiceParameters costs;
                                    for (std::size_t i = group * groupBones;
                                         i < (std::min)(count, (group + 1) * groupBones); ++i)
                                        if (boneType[i] == 1 ||
                                            (boneType[i] == 2 && ((k < 4 && k != omitted[i]) || k >= 9)))
                                            costs.Add(residual[i * 12 + k]);
                                    parameters[group][k] = costs.Best();
                                }
                            }
                            const std::size_t presenceBytes = (count + 7) / 8;
                            const std::size_t rigidBytes = (count + 7) / 8;
                            std::size_t omittedBytes = (rigidCount + 3) / 4;
                            PoseRiceWriter writer;
                            writer.Put(layout, 16);
                            writer.Put(recent->token, 32);
                            writer.Put(4, 8);
                            writer.Put((grouped ? 1 : 0) | (coarse ? 2 : 0) | (secondOrder ? 4 : 0), 8);
                            writer.Put(older ? older->token : 0, 32);
                            writer.Put(ratio, 16);
                            for (auto const &values : parameters)
                                for (unsigned k : values)
                                    writer.Put(k, 4);
                            writer.Align();
                            for (std::size_t i = 0; i < count; ++i)
                                writer.Put(boneType[i] ? 1u : 0u, 1);
                            writer.Align();
                            for (std::size_t i = 0; i < count; ++i)
                                writer.Put(boneType[i] == 2 ? 1u : 0u, 1);
                            writer.Align();
                            for (std::size_t i = 0; i < count; ++i)
                                if (boneType[i] == 2) {
                                    writer.Put(omitted[i], 2);
                                }
                            writer.Align();
                            std::size_t residualRotation = 0, residualTranslation = 0;
                            for (std::size_t i = 0; i < count; ++i)
                                if (boneType[i]) {
                                    auto const &kval = parameters[i / groupBones];
                                    if (boneType[i] == 2) {
                                        for (int k = 0; k < 4; ++k) {
                                            if (k == omitted[i])
                                                continue;
                                            const std::size_t before = writer.bits;
                                            writer.Rice(residual[i * 12 + k], kval[k], 21);
                                            residualRotation += writer.bits - before;
                                        }
                                        for (int k = 9; k < 12; ++k) {
                                            const std::size_t before = writer.bits;
                                            writer.Rice(residual[i * 12 + k], kval[k], 21);
                                            residualTranslation += writer.bits - before;
                                        }
                                    } else {
                                        for (int k = 0; k < 12; ++k) {
                                            const std::size_t before = writer.bits;
                                            writer.Rice(residual[i * 12 + k], kval[k], 21);
                                            (k < 9 ? residualRotation : residualTranslation) +=
                                                writer.bits - before;
                                        }
                                    }
                                }
                            writer.Align();

                            if (writer.bytes.size() < result.size()) {
                                result = std::move(writer.bytes);
                                resultKey = false;
                                resultStats = {};
                                resultStats.headerBytes = 14 + groups * 6;
                                resultStats.maskBits = (presenceBytes + rigidBytes + omittedBytes) * 8;
                                resultStats.rotationBits = residualRotation;
                                resultStats.translationBits = residualTranslation;
                                resultStats.includedBones = included;
                                resultStats.predictive = true;
                                resultStats.maxBasisErrorUnits = maxBasisError;
                                resultStats.rigidBones = rigidCount;

                                reconstructed = rigidReconstruction;
                            }
                        }
                      }
                    }
                }
            }
        }

        out = std::move(result);
        key = resultKey;
        decodedOutput = std::move(reconstructed);
        if (stats)
            *stats = resultStats;
        return Status::Ok;
    }

    Status Encode(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                  pose::Bone const* bones, size_t count, uint32_t now,
                  std::vector<uint8_t>& out, bool& key, PoseStats* stats = nullptr,
                  bool forceKey = false) const
    {
        std::vector<pose::Bone> reconstructed;
        return EncodeCompactImpl(dictionary, layout, incarnation, flags, bones, count, now,
                                 out, key, reconstructed, stats, false, forceKey);
    }

    Status EncodeCompact(Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
                         pose::Bone const* bones, size_t count, uint32_t now,
                         std::vector<uint8_t>& out, bool& key,
                         std::vector<pose::Bone>& reconstructed, PoseStats* stats = nullptr,
                         bool forceKey = false) const
    {
        return EncodeCompactImpl(dictionary, layout, incarnation, flags, bones, count, now,
                                 out, key, reconstructed, stats, true, forceKey);
    }

    Status Decode(Dictionary const& dictionary, uint8_t const* data, size_t size,
                  uint32_t incarnation, uint8_t flags,
                  std::vector<pose::Bone>& out, uint16_t& layout,
                  PoseStats* stats = nullptr) const
    {
        PoseStats decodedStats;
        PoseStats* callerStats = stats;
        stats = &decodedStats;
        if (!data || size <= DeltaHeaderBytes || !incarnation || (flags & ~pose::Facial))
            return Status::Malformed;
        const uint16_t id = uint16_t(data[0]) | uint16_t(uint16_t(data[1]) << 8);
        if (!id) return Status::Malformed;
        if (!dictionary.HasLayout(id)) { layout = id; return Status::UnknownLayout; }
        if (latestToken_ && PoseNewer(incarnation_, incarnation))
            return Status::WrongGeneration;

        const uint32_t ref = ReadPose32(data + 2);
        const uint8_t mode = data[DeltaHeaderBytes];
        if (!mode) {
            if (ref) return Status::Malformed;
            std::vector<uint8_t> full(data, data + 2);
            full.insert(full.end(), data + DeltaHeaderBytes + 1, data + size);
            const Status status = dictionary.DecodePose(full.data(), full.size(), out, layout);
            if (status == Status::Ok && stats) {
                stats->headerBytes = 7;
                stats->rotationBits = out.size() * 144;
                stats->translationBits = out.size() * 48;
                stats->includedBones = 0;
                stats->predictive = false;
            }
            if (status == Status::Ok && callerStats) *callerStats = decodedStats;
            return status;
        }
        if (mode == 1) {
            auto const* base = Find(ref, incarnation, id, flags);
            Frame identity{0, incarnation, id, flags, 0, {}};
            if (!ref) {
                const Status status = dictionary.IdentityPose(id, identity.bones);
                if (status != Status::Ok) return status;
                base = &identity;
            }
            if (!base) { layout = id; return Status::UnknownPose; }
            const size_t count = base->bones.size();
            const size_t maskBytes = (count + 7) / 8;
            const size_t maskAt = DeltaHeaderBytes + 1 + 12;
            if (size < maskAt + maskBytes ||
                (count % 8 && (data[maskAt + maskBytes - 1] >> (count % 8))))
                return Status::Malformed;
            auto const* widths = data + DeltaHeaderBytes + 1;
            for (int k = 0; k < 12; ++k) if (widths[k] > 17) return Status::Malformed;

            std::vector<uint16_t> masks(count, 0);
            std::size_t offset = maskAt + maskBytes;
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
            auto result = base->bones;
            std::size_t included = 0;
            std::size_t residualRotation = 0, residualTranslation = 0;
            for (size_t i = 0; i < count; ++i) {
                if (masks[i]) ++included;
                for (int k = 0; k < 12; ++k) {
                    if (!(masks[i] & (1u << k))) continue;
                    if (!widths[k]) return Status::Malformed;
                    uint32_t value = 0;
                    if (!get(widths[k], value)) return Status::Malformed;
                    if (value > 131070 || !value) return Status::Malformed;
                    const int32_t difference = value & 1
                        ? -int32_t((value + 1) / 2)
                        : int32_t(value / 2);
                    const int64_t decoded = int64_t(base->bones[i].values[k]) + difference;
                    if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                    result[i].values[k] = static_cast<int16_t>(decoded);
                    (k < 9 ? residualRotation : residualTranslation) += widths[k];
                }
            }
            if (bit) {
                if (offset >= size || (data[offset] >> bit)) return Status::Malformed;
                ++offset;
            }
            if (offset != size) return Status::Malformed;
            out = std::move(result);
            layout = id;
            if (stats) {
                stats->headerBytes = 19;
                stats->maskBits = maskBytes * 8 + included * 12;
                stats->rotationBits = residualRotation;
                stats->translationBits = residualTranslation;
                stats->includedBones = included;
                stats->predictive = false;
            }
            if (callerStats) *callerStats = decodedStats;
            return Status::Ok;
        }

        if (mode == 4) {
            if (size < 20) return Status::Malformed;
            if (data[7] & ~7u) return Status::Malformed;
            const bool secondOrder = (data[7] & 4) != 0;
            const uint32_t secondary = ReadPose32(data + 8);
            const unsigned ratio = uint16_t(data[12]) | uint16_t(uint16_t(data[13]) << 8);
            if (ratio > 1024) return Status::Malformed;
            const Frame* recent = Find(ref, incarnation, id, flags);
            if (!recent) { layout = id; return Status::UnknownPose; }
            const size_t count = recent->bones.size();
            if (!count || count > pose::MaxBones) return Status::Malformed;
            if (secondOrder && count < 3) return Status::Malformed;
            const size_t groupBones = (data[7] & 1) ? 16 : count;
            const size_t groups = (count + groupBones - 1) / groupBones;
            const size_t parameterEnd = 14 + groups * 6;
            if (size < parameterEnd) return Status::Malformed;
            std::vector<std::array<unsigned, 12>> parameters(groups);
            for (size_t group = 0; group < groups; ++group)
                for (int k = 0; k < 12; ++k)
                    parameters[group][k] = (data[14 + group * 6 + k / 2] >> ((k & 1) * 4)) & 15u;
            const size_t presenceBytes = (count + 7) / 8;
            const size_t rigidBytes = presenceBytes;
            if (size < parameterEnd + presenceBytes + rigidBytes) return Status::Malformed;
            if (count % 8 && (data[parameterEnd + presenceBytes - 1] >> (count % 8))) return Status::Malformed;
            std::size_t rigidCount = 0;
            for (std::size_t i = 0; i < count; ++i)
                if (data[parameterEnd + presenceBytes + i / 8] & (1u << (i % 8))) ++rigidCount;
            if (count % 8 &&
                (data[parameterEnd + presenceBytes + rigidBytes - 1] >> (count % 8)))
                return Status::Malformed;
            const std::size_t omittedBytes = (rigidCount + 3) / 4;
            const std::size_t headerBytes = parameterEnd + presenceBytes + rigidBytes + omittedBytes;
            if (size < headerBytes) return Status::Malformed;

            Frame const* older = nullptr;
            if (secondary) {
                older = Find(secondary, incarnation, id, flags);
                if (!older) { layout = id; return Status::UnknownPose; }
                if (older->bones.size() != count || !PoseNewer(ref, secondary))
                    return Status::Malformed;
            } else if (ratio != 0) {
                return Status::Malformed;
            }

            unsigned rigidRank = 0;
            std::array<uint8_t, pose::MaxBones> omitted{};
            for (std::size_t i = 0; i < count; ++i) {
                const bool present = data[parameterEnd + i / 8] & (1u << (i % 8));
                const bool rigid = data[parameterEnd + presenceBytes + i / 8] & (1u << (i % 8));
                if (rigid && !present) return Status::Malformed;
                if (rigid) {
                    const std::size_t byte = parameterEnd + presenceBytes + rigidBytes + rigidRank / 4;
                    omitted[i] = static_cast<uint8_t>((data[byte] >> ((rigidRank & 3) * 2)) & 3u);
                    ++rigidRank;
                }
            }
            if (rigidRank != rigidCount) return Status::Malformed;
            if (rigidCount &&
                (rigidCount % 4 &&
                 (data[parameterEnd + presenceBytes + rigidBytes + omittedBytes - 1] >>
                  ((rigidCount % 4) * 2))))
                return Status::Malformed;

            std::vector<uint8_t> payload(data, data + size);
            PoseRiceReader reader{payload};
            reader.bits = headerBytes * 8;
            auto result = recent->bones;
            int32_t priorPrev[12] = {}, priorPrevPrev[12] = {};
            uint8_t prevType = 0, prevPrevType = 0, prevOmitted = 0, prevPrevOmitted = 0;
            bool usedSecondOrder = false;
            auto spatial = [&](size_t i, int k, uint8_t type, uint8_t omittedIndex) -> int64_t {
                if (!i || prevType != type || (type == 2 && prevOmitted != omittedIndex)) return 0;
                if (secondOrder && i > 1 && prevPrevType == type &&
                    (type != 2 || prevPrevOmitted == omittedIndex)) {
                    usedSecondOrder = true;
                    return int64_t(2) * priorPrev[k] - priorPrevPrev[k];
                }
                return priorPrev[k];
            };
            std::size_t included = 0, rigidBones = 0;
            std::size_t residualRotation = 0, residualTranslation = 0;
            for (size_t i = 0; i < count; ++i) {
                auto const& kval = parameters[i / groupBones];
                const bool present = payload[parameterEnd + i / 8] & (1u << (i % 8));
                const bool rigid = payload[parameterEnd + presenceBytes + i / 8] & (1u << (i % 8));
                if (!present) {
                    int32_t priorCur[12] = {};
                    for (int k = 0; k < 12; ++k) {
                        const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                        int64_t decoded = int64_t(prediction) + spatial(i, k, 1, 0);
                        if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                        result[i].values[k] = static_cast<int16_t>(decoded);
                        priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                    }
                    for (int k = 0; k < 12; ++k) {
                        priorPrevPrev[k] = priorPrev[k];
                        priorPrev[k] = priorCur[k];
                    }
                    prevPrevType = prevType;
                    prevPrevOmitted = prevOmitted;
                    prevType = 0;
                    continue;
                }
                ++included;
                int32_t priorCur[12] = {};
                bool boneChanged = false;
                if (!rigid) {
                    for (int k = 0; k < 12; ++k) {
                        const std::size_t before = reader.bits;
                        uint32_t zig = 0;
                        if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                        if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                        if (zig) boneChanged = true;
                        const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                        int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig);
                        decoded += spatial(i, k, 1, 0);
                        if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                        result[i].values[k] = static_cast<int16_t>(decoded);
                        priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                        (k < 9 ? residualRotation : residualTranslation) += reader.bits - before;
                    }
                } else {
                    ++rigidBones;
                    PoseQuat predicted;
                    if (!QuaternionPrediction(*recent, older, ratio, i, omitted[i], predicted))
                        return Status::Malformed;
                    if (data[7] & 2)
                        for (auto& value : predicted.values)
                            value = PoseRoundSignedDivide(value, 4) * 4;

                    PoseQuat target = predicted;
                    for (int k = 0; k < 4; ++k) {
                        if (k == omitted[i]) continue;
                        const std::size_t before = reader.bits;
                        uint32_t zig = 0;
                        if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                        if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                        if (zig) boneChanged = true;
                        const int64_t value = int64_t(predicted.values[k]) +
                            int64_t(PoseUnzigZag(zig)) * ((data[7] & 2) ? 4 : 1) +
                            spatial(i, k, 2, omitted[i]);
                        if (value < -32768 || value > 32767)
                            return Status::Malformed;
                        target.values[k] = static_cast<int32_t>(value);
                        priorCur[k] = target.values[k] - predicted.values[k];
                        residualRotation += reader.bits - before;
                    }
                    target.omitted = omitted[i];
                    if (!PoseCompleteQuat(target)) return Status::Malformed;
                    pose::Bone rigidMatrix;
                    if (!PoseQuatToBone(target, result[i], rigidMatrix)) return Status::Malformed;
                    result[i] = rigidMatrix;

                    for (int k = 9; k < 12; ++k) {
                        const std::size_t before = reader.bits;
                        uint32_t zig = 0;
                        if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                        if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                        if (zig) boneChanged = true;
                        const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                        int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig);
                        decoded += spatial(i, k, 2, omitted[i]);
                        if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                        result[i].values[k] = static_cast<int16_t>(decoded);
                        priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                        residualTranslation += reader.bits - before;
                    }
                }
                if (!rigid && !boneChanged) return Status::Malformed;
                for (int k = 0; k < 12; ++k) {
                    priorPrevPrev[k] = priorPrev[k];
                    priorPrev[k] = priorCur[k];
                }
                prevPrevType = prevType;
                prevPrevOmitted = prevOmitted;
                prevType = rigid ? 2 : 1;
                if (rigid) prevOmitted = omitted[i];
            }
            if (secondOrder && !usedSecondOrder) return Status::Malformed;
            if (!reader.AlignAndFinish()) return Status::Malformed;
            out = std::move(result);
            layout = id;
            if (stats) {
                stats->headerBytes = parameterEnd;
                stats->maskBits = (presenceBytes + rigidBytes + omittedBytes) * 8;
                stats->rotationBits = residualRotation;
                stats->translationBits = residualTranslation;
                stats->includedBones = included;
                stats->predictive = true;
                stats->rigidBones = rigidBones;
            }
            if (callerStats) *callerStats = decodedStats;
            return Status::Ok;
        }

        if (mode != 2 && mode != 3) return Status::Malformed;
        if (size < 20) return Status::Malformed;
        if ((mode == 2 && data[7] != 0) || (mode == 3 && (data[7] & ~1u)))
            return Status::Malformed;
        const bool secondOrder = mode == 3 && (data[7] & 1);
        const uint32_t secondary = ReadPose32(data + 8);
        const unsigned ratio = uint16_t(data[12]) | uint16_t(uint16_t(data[13]) << 8);
        if (ratio > 1024) return Status::Malformed;

        std::array<unsigned, 12> kval{};
        for (int k = 0; k < 12; ++k)
            kval[k] = (data[14 + k / 2] >> ((k & 1) * 4)) & 15u;

        Frame identity{0, incarnation, id, flags, 0, {}};
        const Frame* recent = Find(ref, incarnation, id, flags);
        if (!ref) {
            if (secondary || ratio) return Status::Malformed;
            const Status status = dictionary.IdentityPose(id, identity.bones);
            if (status != Status::Ok) return status;
            recent = &identity;
        }
        if (!recent) { layout = id; return Status::UnknownPose; }
        const size_t count = recent->bones.size();
        if (!count || count > pose::MaxBones) return Status::Malformed;
        if (secondOrder && count < 3) return Status::Malformed;
        const size_t presenceBytes = (count + 7) / 8;
        const size_t headerBytes = 20 + presenceBytes;
        if (size < headerBytes) return Status::Malformed;
        if (count % 8 && (data[headerBytes - 1] >> (count % 8))) return Status::Malformed;

        Frame const* older = nullptr;
        if (secondary) {
            older = Find(secondary, incarnation, id, flags);
            if (!older) { layout = id; return Status::UnknownPose; }
            if (older->bones.size() != count || !PoseNewer(ref, secondary))
                return Status::Malformed;
        } else if (ratio != 0) {
            return Status::Malformed;
        }

        std::vector<uint8_t> payload(data, data + size);
        PoseRiceReader reader{payload};
        reader.bits = headerBytes * 8;
        auto result = recent->bones;
        std::array<int32_t, 12> prior{}, prior2{};
        std::size_t included = 0, residualRotation = 0, residualTranslation = 0;
        for (size_t i = 0; i < count; ++i) {
            const bool present = (payload[20 + i / 8] & (1u << (i % 8))) != 0;
            if (!present) {
                for (int k = 0; k < 12; ++k) {
                    const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                    const int64_t spatial = secondOrder && i > 1
                        ? int64_t(2) * prior[k] - prior2[k] : (i ? prior[k] : 0);
                    const int64_t decoded = mode == 3 ? int64_t(prediction) + spatial : int64_t(prediction);
                    if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                    result[i].values[k] = static_cast<int16_t>(decoded);
                    if (mode == 3) {
                        prior2[k] = prior[k];
                        prior[k] = static_cast<int32_t>(decoded) - prediction;
                    }
                }
                continue;
            }
            ++included;
            bool boneChanged = false;
            for (int k = 0; k < 12; ++k) {
                const std::size_t before = reader.bits;
                uint32_t zig = 0;
                if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                if (zig > (secondOrder ? 524280u : mode == 3 ? 262140u : 131070u))
                    return Status::Malformed;
                if (zig) boneChanged = true;
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig);
                if (mode == 3) decoded += secondOrder && i > 1
                    ? int64_t(2) * prior[k] - prior2[k] : (i ? prior[k] : 0);
                if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                result[i].values[k] = static_cast<int16_t>(decoded);
                if (mode == 3) {
                    prior2[k] = prior[k];
                    prior[k] = static_cast<int32_t>(decoded) - prediction;
                }
                (k < 9 ? residualRotation : residualTranslation) += reader.bits - before;
            }
            if (!boneChanged) return Status::Malformed;
        }
        if (!reader.AlignAndFinish()) return Status::Malformed;
        out = std::move(result);
        layout = id;
        if (stats) {
            stats->headerBytes = 20;
            stats->maskBits = presenceBytes * 8;
            stats->rotationBits = residualRotation;
            stats->translationBits = residualTranslation;
            stats->includedBones = included;
            stats->predictive = true;
        }
        if (callerStats) *callerStats = decodedStats;
        return Status::Ok;
    }
};
}
