// SPDX-License-Identifier: GPL-3.0-or-later
#define G3T_NPCWIRE_POSE_IMPL_INCLUDED 1
#include "npcwire_pose.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace g3t::npcwire::bones {

namespace detail {

G3T_NPCWIRE_POSE_INLINE int64_t Mode3Spatial(bool secondOrder, size_t bone,
                                             int32_t prior, int32_t prior2) noexcept
{
    if (!bone) return 0;
    return secondOrder && bone > 1 ? int64_t(2) * prior - prior2 : int64_t(prior);
}

G3T_NPCWIRE_POSE_INLINE bool Mode4FirstOrderMatch(size_t bone, uint8_t prevType, uint8_t prevOmitted,
                                                  uint8_t type, uint8_t omittedIndex) noexcept
{
    return bone > 0 && prevType == type && (type != 2 || prevOmitted == omittedIndex);
}

G3T_NPCWIRE_POSE_INLINE bool Mode4SecondOrderMatch(bool secondOrder, size_t bone,
                                                   uint8_t prevType, uint8_t prevPrevType,
                                                   uint8_t prevOmitted, uint8_t prevPrevOmitted,
                                                   uint8_t type, uint8_t omittedIndex) noexcept
{
    return secondOrder && bone > 1 &&
           Mode4FirstOrderMatch(bone, prevType, prevOmitted, type, omittedIndex) &&
           prevPrevType == type && (type != 2 || prevPrevOmitted == omittedIndex);
}

G3T_NPCWIRE_POSE_INLINE bool DecoderUsesMode4SecondOrder(
    std::array<uint8_t, pose::MaxBones> const& boneType,
    std::array<uint8_t, pose::MaxBones> const& omitted,
    size_t count) noexcept
{
    for (size_t i = 2; i < count; ++i) {
        const uint8_t wantedType = boneType[i] == 2 ? 2 : 1;
        const uint8_t wantedOmitted = boneType[i] == 2 ? omitted[i] : 0;
        if (Mode4SecondOrderMatch(true, i, boneType[i - 1], boneType[i - 2],
                                  omitted[i - 1], omitted[i - 2], wantedType, wantedOmitted)) {
            return true;
        }
    }
    return false;
}

} // namespace detail

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode1Sparse(
    uint16_t layout, Frame const& base, pose::Bone const* bones, size_t count,
    std::vector<uint8_t>& result, bool& resultKey, PoseStats& resultStats)
{
    std::vector<uint8_t> sparse{static_cast<uint8_t>(layout), static_cast<uint8_t>(layout >> 8)};
    WritePose32(sparse, base.token);
    sparse.push_back(1);
    uint8_t widths[12] = {};
    std::vector<uint16_t> components(count, 0);
    std::size_t includedSparse = 0;
    for (std::size_t i = 0; i < count; ++i) {
        for (int k = 0; k < 12; ++k) {
            const int32_t difference = int32_t(bones[i].values[k]) - base.bones[i].values[k];
            if (!difference) continue;
            uint32_t value = PoseZigZag(difference);
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
            const int32_t difference = int32_t(bones[i].values[k]) - base.bones[i].values[k];
            put(PoseZigZag(difference), widths[k]);
        }
    }
    if (sparse.size() < result.size()) {
        result = std::move(sparse);
        resultKey = base.token == 0;
        resultStats = {};
        resultStats.headerBytes = 19;
        resultStats.maskBits = maskBytes * 8 + includedSparse * 12;
        resultStats.includedBones = includedSparse;
        std::size_t residualRotation = 0, residualTranslation = 0;
        for (std::size_t i = 0; i < count; ++i)
            for (int k = 0; k < 12; ++k)
                if (components[i] & (1u << k))
                    (k < 9 ? residualRotation : residualTranslation) += widths[k];
        resultStats.rotationBits = residualRotation;
        resultStats.translationBits = residualTranslation;
    }
}

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode2And3(
    uint16_t layout, Frame const& recent, Frame const* olderCandidate,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::vector<uint8_t>& result, bool& resultKey, PoseStats& resultStats)
{
    for (bool useOlder : {false, true}) {
        if (useOlder && !olderCandidate) continue;
        const Frame* older = useOlder ? olderCandidate : nullptr;
        const unsigned ratio = older ? Ratio(recent, older, now) : 0;
        for (uint8_t variant = 0; variant < 3; ++variant) {
            const uint8_t predictiveMode = variant == 0 ? 2 : 3;
            const bool secondOrder = variant == 2;
            if (secondOrder && count < 3) continue;
            std::array<unsigned, 12> kval{};
            std::vector<uint32_t> residual(count * 12, 0);
            std::vector<uint8_t> present(count, 0);
            std::array<int32_t, 12> prior{}, prior2{};
            for (std::size_t i = 0; i < count; ++i) {
                for (int k = 0; k < 12; ++k) {
                    const int32_t prediction = TemporalPrediction(recent, older, ratio, i, k);
                    int32_t residualValue = int32_t(bones[i].values[k]) - prediction;
                    if (predictiveMode == 3) {
                        residualValue -= static_cast<int32_t>(
                            detail::Mode3Spatial(secondOrder, i, prior[k], prior2[k]));
                    }
                    const uint32_t zig = PoseZigZag(residualValue);
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
            writer.Put(recent.token, 32);
            writer.Put(predictiveMode, 8);
            writer.Put(secondOrder ? 1 : 0, 8);
            writer.Put(older ? older->token : 0, 32);
            writer.Put(ratio, 16);
            for (unsigned k : kval) writer.Put(k, 4);
            for (std::size_t i = 0; i < count; ++i)
                writer.Put(present[i] ? 1u : 0u, 1);
            writer.Align();
            std::size_t included = 0, residualRotation = 0, residualTranslation = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (!present[i]) continue;
                ++included;
                for (int k = 0; k < 12; ++k) {
                    const std::size_t before = writer.bits;
                    writer.Rice(residual[i * 12 + k], kval[k], 21);
                    (k < 9 ? residualRotation : residualTranslation) += writer.bits - before;
                }
            }
            writer.Align();

            if (writer.bytes.size() < result.size()) {
                result = std::move(writer.bytes);
                resultKey = recent.token == 0;
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

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode4(
    uint16_t layout, Frame const& recent, Frame const* olderCandidate,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::vector<uint8_t>& result, bool& resultKey,
    std::vector<pose::Bone>& reconstructed, PoseStats& resultStats)
{
    for (bool useOlder : {false, true}) {
        if (useOlder && !olderCandidate) continue;
        const Frame* older = useOlder ? olderCandidate : nullptr;
        const unsigned ratio = older ? Ratio(recent, older, now) : 0;
        std::array<PoseQuat, pose::MaxBones> sourceQuaternions{}, predictedQuaternions{};
        std::array<bool, pose::MaxBones> quaternionReady{};
        for (size_t i = 0; i < count; ++i)
            quaternionReady[i] = PoseExtractQuat(bones[i], sourceQuaternions[i]) &&
                QuaternionPrediction(recent, older, ratio, i, sourceQuaternions[i].omitted,
                                     predictedQuaternions[i]);
        for (bool coarse : {false, true}) {
            for (bool secondOrder : {false, true}) {
                if (secondOrder && count < 3) continue;
                std::array<uint8_t, pose::MaxBones> boneType{};
                std::array<uint8_t, pose::MaxBones> omitted{};
                std::vector<uint32_t> residual(count * 12, 0);
                std::size_t rigidCount = 0;
                std::size_t included = 0;
                size_t maxBasisError = 0;
                int32_t prior[pose::MaxBones][12] = {};
                std::vector<pose::Bone> rigidReconstruction(bones, bones + count);
                auto spatial = [&](size_t i, int k, uint8_t type, uint8_t omittedIndex) -> int32_t {
                    if (!detail::Mode4FirstOrderMatch(i, boneType[i - 1], omitted[i - 1], type, omittedIndex))
                        return 0;
                    if (detail::Mode4SecondOrderMatch(secondOrder, i, boneType[i - 1],
                                                      i > 1 ? boneType[i - 2] : 0,
                                                      omitted[i - 1], i > 1 ? omitted[i - 2] : 0,
                                                      type, omittedIndex)) {
                        return 2 * prior[i - 1][k] - prior[i - 2][k];
                    }
                    return prior[i - 1][k];
                };

                for (std::size_t i = 0; i < count; ++i) {
                    bool unchangedPredicted = true;
                    pose::Bone predictedBone = bones[i];
                    int32_t predictedPrior[12] = {};
                    for (int k = 0; k < 12; ++k) {
                        const int32_t prediction = TemporalPrediction(recent, older, ratio, i, k);
                        const int64_t candidate = int64_t(prediction) + spatial(i, k, 1, 0);
                        if (candidate < -32768 || candidate > 32767 ||
                            candidate != int64_t(bones[i].values[k])) {
                            unchangedPredicted = false;
                            break;
                        }
                        predictedBone.values[k] = static_cast<int16_t>(candidate);
                        predictedPrior[k] = static_cast<int32_t>(candidate - prediction);
                    }
                    if (unchangedPredicted) {
                        rigidReconstruction[i] = predictedBone;
                        boneType[i] = 0;
                        for (int k = 0; k < 12; ++k) {
                            prior[i][k] = predictedPrior[k];
                            residual[i * 12 + k] = 0;
                        }
                        continue;
                    }

                    PoseQuat targetQuat = sourceQuaternions[i], recentQuat = predictedQuaternions[i];
                    bool rigid = quaternionReady[i];
                    pose::Bone rigidMatrix{};
                    size_t boneError = 0;
                    if (rigid) {
                        auto accepts = [&](PoseQuat const& quaternion) {
                            if (!PoseQuatToBone(quaternion, bones[i], rigidMatrix))
                                return false;
                            boneError = 0;
                            for (int k = 0; k < 9; ++k) {
                                const size_t error = size_t(std::abs(int32_t(rigidMatrix.values[k]) -
                                                                     int32_t(bones[i].values[k])));
                                if (error > boneError) boneError = error;
                                if (error > 2) return false;
                            }
                            return true;
                        };
                        if (coarse) {
                            const PoseQuat original = targetQuat;
                            for (auto& value : recentQuat.values)
                                value = PoseRoundSignedDivide(value, 4) * 4;
                            for (int k = 0; k < 4; ++k)
                                if (k != targetQuat.omitted)
                                    targetQuat.values[k] = PoseRoundSignedDivide(targetQuat.values[k], 4) * 4;
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
                        } else {
                            rigid = accepts(targetQuat);
                        }
                    }

                    if (rigid) {
                        maxBasisError = (std::max)(maxBasisError, boneError);
                        rigidReconstruction[i] = rigidMatrix;
                        boneType[i] = 2;
                        omitted[i] = targetQuat.omitted;
                        for (int k = 0; k < 12; ++k) {
                            residual[i * 12 + k] = 0;
                            prior[i][k] = 0;
                        }
                        for (int k = 0; k < 4; ++k) {
                            if (k == targetQuat.omitted) continue;
                            int32_t difference = targetQuat.values[k] - recentQuat.values[k];
                            prior[i][k] = difference;
                            difference -= spatial(i, k, 2, omitted[i]);
                            residual[i * 12 + k] = PoseZigZag(coarse ? difference / 4 : difference);
                        }
                        for (int k = 9; k < 12; ++k) {
                            const int32_t prediction = TemporalPrediction(recent, older, ratio, i, k);
                            int32_t difference = int32_t(bones[i].values[k]) - prediction;
                            prior[i][k] = difference;
                            difference -= spatial(i, k, 2, omitted[i]);
                            residual[i * 12 + k] = PoseZigZag(difference);
                        }
                        ++rigidCount;
                        ++included;
                        continue;
                    }

                    bool changedAffine = false;
                    for (int k = 0; k < 12; ++k) {
                        const int32_t prediction = TemporalPrediction(recent, older, ratio, i, k);
                        int32_t difference = int32_t(bones[i].values[k]) - prediction;
                        prior[i][k] = difference;
                        difference -= spatial(i, k, 1, 0);
                        residual[i * 12 + k] = PoseZigZag(difference);
                        if (difference) changedAffine = true;
                    }
                    if (changedAffine) {
                        boneType[i] = 1;
                        ++included;
                    }
                }

                if (!included) continue;
                if (secondOrder && !detail::DecoderUsesMode4SecondOrder(boneType, omitted, count))
                    continue;

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
                    const std::size_t rigidBytes = presenceBytes;
                    const std::size_t omittedBytes = (rigidCount + 3) / 4;
                    PoseRiceWriter writer;
                    writer.Put(layout, 16);
                    writer.Put(recent.token, 32);
                    writer.Put(4, 8);
                    writer.Put((grouped ? 1 : 0) | (coarse ? 2 : 0) | (secondOrder ? 4 : 0), 8);
                    writer.Put(older ? older->token : 0, 32);
                    writer.Put(ratio, 16);
                    for (auto const& values : parameters)
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
                        if (boneType[i] == 2) writer.Put(omitted[i], 2);
                    writer.Align();
                    std::size_t residualRotation = 0, residualTranslation = 0;
                    for (std::size_t i = 0; i < count; ++i) {
                        if (!boneType[i]) continue;
                        auto const& kval = parameters[i / groupBones];
                        if (boneType[i] == 2) {
                            for (int k = 0; k < 4; ++k) {
                                if (k == omitted[i]) continue;
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
                                (k < 9 ? residualRotation : residualTranslation) += writer.bits - before;
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

G3T_NPCWIRE_POSE_INLINE Status PoseCache::EncodeCompactImpl(
    Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
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

    std::vector<uint8_t> result = std::move(full);
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

    EncodeMode1Sparse(layout, *base, bones, count, result, resultKey, resultStats);

    const Frame* olderForPredictive = !keyDue && ackRefs_.size() >= 2
        ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
    if (olderForPredictive && (olderForPredictive->bones.size() != count ||
                               !PoseNewer(base->token, olderForPredictive->token)))
        olderForPredictive = nullptr;
    EncodeMode2And3(layout, *base, olderForPredictive, bones, count, now,
                    result, resultKey, resultStats);

    if (allowMode4 && !keyDue && !ackRefs_.empty()) {
        const Frame* recent = Find(ackRefs_.front(), incarnation, layout, flags);
        const Frame* olderCandidate = ackRefs_.size() >= 2
            ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
        if (recent && recent->bones.size() == count &&
            (!olderCandidate || (olderCandidate->bones.size() == count &&
                                 PoseNewer(recent->token, olderCandidate->token)))) {
            EncodeMode4(layout, *recent, olderCandidate, bones, count, now,
                        result, resultKey, reconstructed, resultStats);
        }
    }

    out = std::move(result);
    key = resultKey;
    decodedOutput = std::move(reconstructed);
    if (stats) *stats = resultStats;
    return Status::Ok;
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::Encode(
    Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::vector<uint8_t>& out, bool& key, PoseStats* stats, bool forceKey) const
{
    std::vector<pose::Bone> reconstructed;
    return EncodeCompactImpl(dictionary, layout, incarnation, flags, bones, count, now,
                             out, key, reconstructed, stats, false, forceKey);
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::EncodeCompact(
    Dictionary const& dictionary, uint16_t layout, uint32_t incarnation, uint8_t flags,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::vector<uint8_t>& out, bool& key,
    std::vector<pose::Bone>& reconstructed, PoseStats* stats, bool forceKey) const
{
    return EncodeCompactImpl(dictionary, layout, incarnation, flags, bones, count, now,
                             out, key, reconstructed, stats, true, forceKey);
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::DecodeMode0(
    Dictionary const& dictionary, uint8_t const* data, size_t size, uint32_t ref,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const
{
    if (ref) return Status::Malformed;
    std::vector<uint8_t> full;
    full.reserve(2 + (size - DeltaHeaderBytes - 1));
    full.push_back(data[0]);
    full.push_back(data[1]);
    full.insert(full.end(), data + DeltaHeaderBytes + 1, data + size);
    const Status status = dictionary.DecodePose(full.data(), full.size(), out, layout);
    if (status == Status::Ok) {
        stats.headerBytes = 7;
        stats.rotationBits = out.size() * 144;
        stats.translationBits = out.size() * 48;
        stats.includedBones = 0;
        stats.predictive = false;
    }
    return status;
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::DecodeMode1(
    Dictionary const& dictionary, uint8_t const* data, size_t size,
    uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const
{
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
            const int32_t difference = PoseUnzigZag(value);
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
    stats.headerBytes = 19;
    stats.maskBits = maskBytes * 8 + included * 12;
    stats.rotationBits = residualRotation;
    stats.translationBits = residualTranslation;
    stats.includedBones = included;
    stats.predictive = false;
    return Status::Ok;
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::DecodeMode2And3(
    Dictionary const& dictionary, uint8_t const* data, size_t size,
    uint8_t mode, uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const
{
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

    PoseRiceReader reader(data, size);
    reader.bits = headerBytes * 8;
    auto result = recent->bones;
    std::array<int32_t, 12> prior{}, prior2{};
    std::size_t included = 0, residualRotation = 0, residualTranslation = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool present = (data[20 + i / 8] & (1u << (i % 8))) != 0;
        if (!present) {
            for (int k = 0; k < 12; ++k) {
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                const int64_t spatial = detail::Mode3Spatial(secondOrder, i, prior[k], prior2[k]);
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
            if (mode == 3) decoded += detail::Mode3Spatial(secondOrder, i, prior[k], prior2[k]);
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
    stats.headerBytes = 20;
    stats.maskBits = presenceBytes * 8;
    stats.rotationBits = residualRotation;
    stats.translationBits = residualTranslation;
    stats.includedBones = included;
    stats.predictive = true;
    return Status::Ok;
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::DecodeMode4(
    uint8_t const* data, size_t size,
    uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const
{
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
    if (rigidCount && (rigidCount % 4) &&
        (data[parameterEnd + presenceBytes + rigidBytes + omittedBytes - 1] >>
         ((rigidCount % 4) * 2)))
        return Status::Malformed;

    PoseRiceReader reader(data, size);
    reader.bits = headerBytes * 8;
    auto result = recent->bones;
    int32_t priorPrev[12] = {}, priorPrevPrev[12] = {};
    uint8_t prevType = 0, prevPrevType = 0, prevOmitted = 0, prevPrevOmitted = 0;
    bool usedSecondOrder = false;
    auto spatial = [&](size_t i, int k, uint8_t type, uint8_t omittedIndex) -> int64_t {
        if (!detail::Mode4FirstOrderMatch(i, prevType, prevOmitted, type, omittedIndex)) return 0;
        if (detail::Mode4SecondOrderMatch(secondOrder, i, prevType, prevPrevType,
                                          prevOmitted, prevPrevOmitted, type, omittedIndex)) {
            usedSecondOrder = true;
            return int64_t(2) * priorPrev[k] - priorPrevPrev[k];
        }
        return priorPrev[k];
    };
    std::size_t included = 0, rigidBones = 0;
    std::size_t residualRotation = 0, residualTranslation = 0;
    for (size_t i = 0; i < count; ++i) {
        auto const& kval = parameters[i / groupBones];
        const bool present = data[parameterEnd + i / 8] & (1u << (i % 8));
        const bool rigid = data[parameterEnd + presenceBytes + i / 8] & (1u << (i % 8));
        if (!present) {
            int32_t priorCur[12] = {};
            for (int k = 0; k < 12; ++k) {
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                const int64_t decoded = int64_t(prediction) + spatial(i, k, 1, 0);
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
                int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig) + spatial(i, k, 1, 0);
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
                if (value < -32768 || value > 32767) return Status::Malformed;
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
                const int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig) + spatial(i, k, 2, omitted[i]);
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
    stats.headerBytes = parameterEnd;
    stats.maskBits = (presenceBytes + rigidBytes + omittedBytes) * 8;
    stats.rotationBits = residualRotation;
    stats.translationBits = residualTranslation;
    stats.includedBones = included;
    stats.predictive = true;
    stats.rigidBones = rigidBones;
    return Status::Ok;
}

G3T_NPCWIRE_POSE_INLINE Status PoseCache::Decode(
    Dictionary const& dictionary, uint8_t const* data, size_t size,
    uint32_t incarnation, uint8_t flags,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats* stats) const
{
    PoseStats decodedStats;
    if (!data || size <= DeltaHeaderBytes || !incarnation || (flags & ~pose::Facial))
        return Status::Malformed;
    const uint16_t id = uint16_t(data[0]) | uint16_t(uint16_t(data[1]) << 8);
    if (!id) return Status::Malformed;
    if (!dictionary.HasLayout(id)) { layout = id; return Status::UnknownLayout; }
    if (latestToken_ && PoseNewer(incarnation_, incarnation))
        return Status::WrongGeneration;

    const uint32_t ref = ReadPose32(data + 2);
    const uint8_t mode = data[DeltaHeaderBytes];
    Status status = Status::Malformed;
    if (mode == 0) {
        status = DecodeMode0(dictionary, data, size, ref, out, layout, decodedStats);
    } else if (mode == 1) {
        status = DecodeMode1(dictionary, data, size, id, ref, incarnation, flags, out, layout, decodedStats);
    } else if (mode == 2 || mode == 3) {
        status = DecodeMode2And3(dictionary, data, size, mode, id, ref, incarnation, flags, out, layout, decodedStats);
    } else if (mode == 4) {
        status = DecodeMode4(data, size, id, ref, incarnation, flags, out, layout, decodedStats);
    }
    if (status == Status::Ok && stats) *stats = decodedStats;
    return status;
}

} // namespace g3t::npcwire::bones
