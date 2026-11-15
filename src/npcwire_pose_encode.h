// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

struct Mode4BonePrep {
    PoseQuat sourceQuat{};
    PoseQuat recentQuat{};
    PoseQuat olderQuat{};
    PoseQuat coarseTarget{};
    pose::Bone exactMatrix{};
    pose::Bone coarseMatrix{};
    size_t exactError = 0;
    size_t coarseError = 0;
    uint8_t sourceState = 0; // 0 = unevaluated, 1 = invalid, 2 = valid
    uint8_t recentState = 0;
    uint8_t olderState = 0;
    uint8_t exactState = 0;
    uint8_t coarseState = 0;

    bool EnsureSource(pose::Bone const& bone)
    {
        if (!sourceState) sourceState = PoseExtractQuat(bone, sourceQuat) ? 2 : 1;
        return sourceState == 2;
    }

    bool PredictQuat(pose::Bone const& recentBone, pose::Bone const* olderBone,
                     unsigned ratio, uint8_t omitted, PoseQuat& out)
    {
        if (!recentState) recentState = PoseExtractQuat(recentBone, recentQuat) ? 2 : 1;
        if (recentState != 2) return false;
        out = recentQuat;
        if (out.values[omitted] < 0)
            for (auto& value : out.values) value = -value;
        if (!olderBone) return true;
        if (!olderState) olderState = PoseExtractQuat(*olderBone, olderQuat) ? 2 : 1;
        if (olderState != 2) return false;
        PoseQuat previous = olderQuat;
        if (previous.values[omitted] < 0)
            for (auto& value : previous.values) value = -value;
        for (int k = 0; k < 4; ++k)
            out.values[k] = PoseSaturateInt16(int64_t(out.values[k]) +
                PoseRoundQ8((int64_t(out.values[k]) - previous.values[k]) * ratio));
        return true;
    }

    static bool Accepts(PoseQuat const& quaternion, pose::Bone const& bone,
                        pose::Bone& rigidMatrix, size_t& boneError)
    {
        if (!PoseQuatToBone(quaternion, bone, rigidMatrix)) return false;
        boneError = 0;
        for (int k = 0; k < 9; ++k) {
            const size_t error = size_t(std::abs(int32_t(rigidMatrix.values[k]) -
                                                 int32_t(bone.values[k])));
            if (error > boneError) boneError = error;
            if (error > 2) return false;
        }
        return true;
    }

    bool EvaluateExact(pose::Bone const& bone)
    {
        if (!exactState) exactState = Accepts(sourceQuat, bone, exactMatrix, exactError) ? 2 : 1;
        return exactState == 2;
    }

    bool EvaluateCoarse(pose::Bone const& bone)
    {
        if (!coarseState) {
            PoseQuat candidate = sourceQuat;
            for (int k = 0; k < 4; ++k)
                if (k != candidate.omitted)
                    candidate.values[k] = PoseRoundSignedDivide(candidate.values[k], 4) * 4;
            bool ok = PoseCompleteQuat(candidate) && Accepts(candidate, bone, coarseMatrix, coarseError);
            for (unsigned choice = 0; !ok && choice < 8; ++choice) {
                candidate = sourceQuat;
                unsigned rank = 0;
                for (int k = 0; k < 4; ++k) {
                    if (k == candidate.omitted) continue;
                    const int value = sourceQuat.values[k];
                    const int floor = value - ((value % 4 + 4) % 4);
                    candidate.values[k] = floor + static_cast<int>((choice >> rank++) & 1u) * 4;
                }
                ok = PoseCompleteQuat(candidate) && Accepts(candidate, bone, coarseMatrix, coarseError);
            }
            if (ok) coarseTarget = candidate;
            coarseState = ok ? 2 : 1;
        }
        return coarseState == 2;
    }
};

} // namespace detail

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode1Sparse(
    uint16_t layout, Frame const& base, pose::Bone const* bones, size_t count,
    std::size_t& bestSize, std::vector<uint8_t>& result, bool& resultKey, PoseStats& resultStats)
{
    uint8_t widths[12] = {};
    std::array<uint16_t, pose::MaxBones> components{};
    std::size_t includedSparse = 0;
    for (std::size_t i = 0; i < count; ++i) {
        for (int k = 0; k < 12; ++k) {
            const int32_t difference = int32_t(bones[i].values[k]) - base.bones[i].values[k];
            if (!difference) continue;
            const uint8_t width = static_cast<uint8_t>(PoseBitWidth(PoseZigZag(difference)));
            components[i] |= uint16_t(1u << k);
            widths[k] = (std::max)(widths[k], width);
        }
        if (components[i]) ++includedSparse;
    }

    std::size_t residualRotation = 0, residualTranslation = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const uint16_t mask = components[i];
        if (!mask) continue;
        for (int k = 0; k < 12; ++k)
            if (mask & (1u << k))
                (k < 9 ? residualRotation : residualTranslation) += widths[k];
    }

    const std::size_t maskBytes = (count + 7) / 8;
    const std::size_t packedBits = includedSparse * 12 + residualRotation + residualTranslation;
    const std::size_t exactBytes = 19 + maskBytes + (packedBits + 7) / 8;
    if (exactBytes >= bestSize) return;

    result.assign(exactBytes, 0);
    result[0] = static_cast<uint8_t>(layout);
    result[1] = static_cast<uint8_t>(layout >> 8);
    result[2] = static_cast<uint8_t>(base.token);
    result[3] = static_cast<uint8_t>(base.token >> 8);
    result[4] = static_cast<uint8_t>(base.token >> 16);
    result[5] = static_cast<uint8_t>(base.token >> 24);
    result[6] = 1;
    std::memcpy(result.data() + 7, widths, 12);

    uint8_t* const maskDst = result.data() + 19;
    uint8_t* const packedDst = maskDst + maskBytes;
    std::size_t bitPos = 0;
    auto put = [&](uint32_t value, unsigned width) {
        const std::size_t byteAt = bitPos >> 3;
        const unsigned shift = static_cast<unsigned>(bitPos & 7u);
        const uint64_t shifted = static_cast<uint64_t>(value) << shift;
        const unsigned totalBits = shift + width;
        for (unsigned b = 0; b * 8 < totalBits; ++b)
            packedDst[byteAt + b] |= static_cast<uint8_t>(shifted >> (b * 8));
        bitPos += width;
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (!components[i]) continue;
        maskDst[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        put(static_cast<uint32_t>(components[i]), 12);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const uint16_t mask = components[i];
        if (!mask) continue;
        for (int k = 0; k < 12; ++k) {
            if (!(mask & (1u << k))) continue;
            const int32_t difference = int32_t(bones[i].values[k]) - base.bones[i].values[k];
            put(PoseZigZag(difference), widths[k]);
        }
    }

    bestSize = exactBytes;
    resultKey = base.token == 0;
    resultStats = {};
    resultStats.headerBytes = 19;
    resultStats.maskBits = maskBytes * 8 + includedSparse * 12;
    resultStats.includedBones = includedSparse;
    resultStats.rotationBits = residualRotation;
    resultStats.translationBits = residualTranslation;
}

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode2And3(
    uint16_t layout, Frame const& recent, Frame const* olderCandidate,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::size_t& bestSize, std::vector<uint8_t>& result, bool& resultKey, PoseStats& resultStats)
{
    std::array<uint32_t, pose::MaxBones * 12> residual{};
    std::array<uint8_t, pose::MaxBones> present{};
    const std::size_t presenceBytes = (count + 7) / 8;

    for (bool useOlder : {false, true}) {
        if (useOlder && !olderCandidate) continue;
        const Frame* older = useOlder ? olderCandidate : nullptr;
        const unsigned ratio = older ? Ratio(recent, older, now) : 0;
        if (useOlder && !ratio) continue;

        for (uint8_t variant = 0; variant < 3; ++variant) {
            const uint8_t predictiveMode = variant == 0 ? 2 : 3;
            const bool secondOrder = variant == 2;
            if (secondOrder && count < 3) continue;

            std::fill_n(present.data(), count, uint8_t(0));
            std::array<int32_t, 12> prior{}, prior2{};
            std::size_t included = 0;
            for (std::size_t i = 0; i < count; ++i) {
                for (int k = 0; k < 12; ++k) {
                    const int32_t prediction = TemporalPrediction(recent, older, ratio, i, k);
                    const int32_t delta = int32_t(bones[i].values[k]) - prediction;
                    int32_t residualValue = delta;
                    if (predictiveMode == 3) {
                        residualValue -= static_cast<int32_t>(
                            detail::Mode3Spatial(secondOrder, i, prior[k], prior2[k]));
                        prior2[k] = prior[k];
                        prior[k] = delta;
                    }
                    const uint32_t zig = PoseZigZag(residualValue);
                    residual[i * 12 + k] = zig;
                    if (zig) present[i] = 1;
                }
                if (present[i]) ++included;
            }

            std::array<unsigned, 12> kval{};
            std::size_t residualRotation = 0, residualTranslation = 0;
            for (int k = 0; k < 12; ++k) {
                PoseRiceParameters parameters;
                if (included) {
                    for (std::size_t i = 0; i < count; ++i)
                        if (present[i]) parameters.Add(residual[i * 12 + k]);
                }
                std::size_t compBits = 0;
                kval[k] = parameters.Best(compBits);
                (k < 9 ? residualRotation : residualTranslation) += compBits;
            }

            const std::size_t exactBytes = 20 + presenceBytes +
                (residualRotation + residualTranslation + 7) / 8;
            if (exactBytes >= bestSize) continue;

            PoseRiceWriter writer;
            writer.bytes = std::move(result);
            writer.bytes.clear();
            writer.bytes.reserve(exactBytes);
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
            for (std::size_t i = 0; i < count; ++i) {
                if (!present[i]) continue;
                for (int k = 0; k < 12; ++k)
                    writer.Rice(residual[i * 12 + k], kval[k], 21);
            }
            writer.Align();

            result = std::move(writer.bytes);
            bestSize = result.size();
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

G3T_NPCWIRE_POSE_INLINE void PoseCache::EncodeMode4(
    uint16_t layout, Frame const& recent, Frame const* olderCandidate,
    pose::Bone const* bones, size_t count, uint32_t now,
    std::size_t& bestSize, std::vector<uint8_t>& result, bool& resultKey,
    std::vector<pose::Bone>& reconstructed, PoseStats& resultStats)
{
    std::array<detail::Mode4BonePrep, pose::MaxBones> prep{};
    std::array<uint32_t, pose::MaxBones * 12> residual{};
    std::array<pose::Bone, pose::MaxBones> rigidReconstruction{};
    constexpr std::size_t MaxGroups = (pose::MaxBones + 15) / 16;
    std::array<std::array<unsigned, 12>, MaxGroups> parameters{};
    const std::size_t presenceBytes = (count + 7) / 8;
    const std::size_t rigidBytes = presenceBytes;

    for (bool useOlder : {false, true}) {
        if (useOlder && !olderCandidate) continue;
        const Frame* older = useOlder ? olderCandidate : nullptr;
        const unsigned ratio = older ? Ratio(recent, older, now) : 0;
        if (useOlder && !ratio) continue;

        std::array<PoseQuat, pose::MaxBones> predictedQuaternions{};
        std::array<uint8_t, pose::MaxBones> readyState{}; // 0 = unevaluated, 1 = false, 2 = true
        auto ensureReady = [&](std::size_t i) -> bool {
            if (!readyState[i]) {
                const bool ok = prep[i].EnsureSource(bones[i]) &&
                    prep[i].PredictQuat(recent.bones[i],
                                        older ? &older->bones[i] : nullptr,
                                        ratio, prep[i].sourceQuat.omitted,
                                        predictedQuaternions[i]);
                readyState[i] = ok ? 2 : 1;
            }
            return readyState[i] == 2;
        };

        for (bool coarse : {false, true}) {
            for (bool secondOrder : {false, true}) {
                if (secondOrder && count < 3) continue;
                std::array<uint8_t, pose::MaxBones> boneType{};
                std::array<uint8_t, pose::MaxBones> omitted{};
                std::size_t rigidCount = 0;
                std::size_t included = 0;
                size_t maxBasisError = 0;
                int32_t prior[pose::MaxBones][12] = {};
                std::copy_n(bones, count, rigidReconstruction.data());

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

                    bool rigid = ensureReady(i);
                    PoseQuat targetQuat{};
                    PoseQuat recentQuat{};
                    pose::Bone const* rigidMatrix = nullptr;
                    size_t boneError = 0;
                    if (rigid) {
                        recentQuat = predictedQuaternions[i];
                        if (coarse) {
                            for (auto& value : recentQuat.values)
                                value = PoseRoundSignedDivide(value, 4) * 4;
                            rigid = prep[i].EvaluateCoarse(bones[i]);
                            if (rigid) {
                                targetQuat = prep[i].coarseTarget;
                                rigidMatrix = &prep[i].coarseMatrix;
                                boneError = prep[i].coarseError;
                            }
                        } else {
                            rigid = prep[i].EvaluateExact(bones[i]);
                            if (rigid) {
                                targetQuat = prep[i].sourceQuat;
                                rigidMatrix = &prep[i].exactMatrix;
                                boneError = prep[i].exactError;
                            }
                        }
                    }

                    if (rigid) {
                        maxBasisError = (std::max)(maxBasisError, boneError);
                        rigidReconstruction[i] = *rigidMatrix;
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

                const std::size_t omittedBytes = (rigidCount + 3) / 4;
                for (bool grouped : {false, true}) {
                    if (grouped && count <= 16) continue;
                    const size_t groupBones = grouped ? 16 : count;
                    const size_t groups = (count + groupBones - 1) / groupBones;
                    std::size_t residualRotation = 0, residualTranslation = 0;
                    for (size_t group = 0; group < groups; ++group) {
                        const std::size_t endBone = (std::min)(count, (group + 1) * groupBones);
                        for (int k = 0; k < 12; ++k) {
                            PoseRiceParameters costs;
                            for (std::size_t i = group * groupBones; i < endBone; ++i)
                                if (boneType[i] == 1 ||
                                    (boneType[i] == 2 && ((k < 4 && k != omitted[i]) || k >= 9)))
                                    costs.Add(residual[i * 12 + k]);
                            std::size_t compBits = 0;
                            parameters[group][k] = costs.Best(compBits);
                            (k < 9 ? residualRotation : residualTranslation) += compBits;
                        }
                    }

                    const std::size_t exactBytes = 14 + groups * 6 +
                        presenceBytes + rigidBytes + omittedBytes +
                        (residualRotation + residualTranslation + 7) / 8;
                    if (exactBytes >= bestSize) continue;

                    PoseRiceWriter writer;
                    writer.bytes = std::move(result);
                    writer.bytes.clear();
                    writer.bytes.reserve(exactBytes);
                    writer.Put(layout, 16);
                    writer.Put(recent.token, 32);
                    writer.Put(4, 8);
                    writer.Put((grouped ? 1 : 0) | (coarse ? 2 : 0) | (secondOrder ? 4 : 0), 8);
                    writer.Put(older ? older->token : 0, 32);
                    writer.Put(ratio, 16);
                    for (size_t group = 0; group < groups; ++group)
                        for (unsigned k : parameters[group])
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
                    for (std::size_t i = 0; i < count; ++i) {
                        if (!boneType[i]) continue;
                        auto const& kval = parameters[i / groupBones];
                        if (boneType[i] == 2) {
                            for (int k = 0; k < 4; ++k) {
                                if (k == omitted[i]) continue;
                                writer.Rice(residual[i * 12 + k], kval[k], 21);
                            }
                            for (int k = 9; k < 12; ++k)
                                writer.Rice(residual[i * 12 + k], kval[k], 21);
                        } else {
                            for (int k = 0; k < 12; ++k)
                                writer.Rice(residual[i * 12 + k], kval[k], 21);
                        }
                    }
                    writer.Align();

                    result = std::move(writer.bytes);
                    bestSize = result.size();
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
                    reconstructed.assign(rigidReconstruction.data(), rigidReconstruction.data() + count);
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
    Status status = dictionary.ValidatePose(layout, bones, count);
    if (status != Status::Ok) return status;

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
        base = &identity;
    }
    if (!base->bones.empty() && base->bones.size() != count) return Status::Malformed;

    const std::size_t mode0Bytes = 7 + count * TransformBytes;
    std::size_t bestSize = mode0Bytes;
    out.clear();
    decodedOutput.assign(bones, bones + count);
    bool resultKey = true;
    PoseStats resultStats;
    resultStats.headerBytes = 7;
    resultStats.rotationBits = count * 144;
    resultStats.translationBits = count * 48;
    resultStats.includedBones = 0;
    resultStats.predictive = false;

    EncodeMode1Sparse(layout, *base, bones, count, bestSize, out, resultKey, resultStats);

    const Frame* olderForPredictive = !keyDue && ackRefs_.size() >= 2
        ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
    if (olderForPredictive && (olderForPredictive->bones.size() != count ||
                               !PoseNewer(base->token, olderForPredictive->token)))
        olderForPredictive = nullptr;
    EncodeMode2And3(layout, *base, olderForPredictive, bones, count, now,
                    bestSize, out, resultKey, resultStats);

    if (allowMode4 && !keyDue && !ackRefs_.empty()) {
        const Frame* recent = Find(ackRefs_.front(), incarnation, layout, flags);
        const Frame* olderCandidate = ackRefs_.size() >= 2
            ? Find(ackRefs_[1], incarnation, layout, flags) : nullptr;
        if (recent && recent->bones.size() == count &&
            (!olderCandidate || (olderCandidate->bones.size() == count &&
                                 PoseNewer(recent->token, olderCandidate->token)))) {
            EncodeMode4(layout, *recent, olderCandidate, bones, count, now,
                        bestSize, out, resultKey, decodedOutput, resultStats);
        }
    }

    if (bestSize == mode0Bytes) {
        out.resize(mode0Bytes);
        out[0] = static_cast<uint8_t>(layout);
        out[1] = static_cast<uint8_t>(layout >> 8);
        std::memset(out.data() + 2, 0, 5);
        uint8_t* dst = out.data() + 7;
        for (std::size_t i = 0; i < count; ++i) {
            for (int k = 0; k < 12; ++k) {
                const uint16_t raw = static_cast<uint16_t>(bones[i].values[k]);
                *dst++ = static_cast<uint8_t>(raw);
                *dst++ = static_cast<uint8_t>(raw >> 8);
            }
        }
    }

    key = resultKey;
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

} // namespace g3t::npcwire::bones
