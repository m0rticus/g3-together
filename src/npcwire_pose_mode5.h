// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npcwire_pose.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace g3t::npcwire::bones::detail {

struct Mode5Input {
    uint16_t layout;
    uint32_t recentToken;
    uint32_t olderToken;
    uint32_t ratio;
    bool useOlder;
    bool coarse;
    bool secondOrder;
    pose::Bone const* bones;
    size_t count;
    int32_t const* diff;
    uint8_t const* boneType;
    uint8_t const* omitted;
    uint8_t const* transChanged;
    size_t included;
    size_t rigidCount;
    size_t transChangedCount;
    size_t changedOmittedCount;
    size_t maxBasisError;
    pose::Bone const* rigidReconstruction;
    uint32_t const* residual;
    PoseRiceParameters const (*groupCosts)[12];
    PoseRiceParameters const* globalCosts;
    PoseRiceParameters const (*rigidGroupCosts)[3];
    PoseRiceParameters const* rigidGlobalCosts;
    uint8_t const* prepFlags;
    uint8_t const* recentOmitted;
};

inline void EncodeMode5Candidates(
    Mode5Input const& in,
    std::size_t& bestSize, std::vector<uint8_t>& result, bool& resultKey,
    std::vector<pose::Bone>& reconstructed, PoseStats& resultStats,
    bool& outAllPresent, bool& outAllRigid, bool& outSameRigidOmitted, bool& outSparseOmitted)
{
    constexpr std::size_t MaxGroups = (pose::MaxBones + 15) / 16;
    const std::size_t presenceBytes = (in.count + 7) / 8;
    const std::size_t rigidBytes = presenceBytes;
    const std::size_t omittedBytes = (in.rigidCount + 3) / 4;
    const std::size_t maxGroups = (in.count + 15) / 16;

    const bool allPresent = in.included == in.count;
    const bool allRigid = in.rigidCount == in.included;
    bool recentValidAllRigid = true;
    for (std::size_t i = 0; i < in.count; ++i) {
        if (in.boneType[i] == 2 && !(in.prepFlags[i] & 8)) { recentValidAllRigid = false; break; }
    }
    const bool sameRigidOmitted = allRigid && recentValidAllRigid && in.changedOmittedCount == 0;
    const std::size_t sparseOmittedBytes = 1 + in.changedOmittedCount + (in.changedOmittedCount + 3) / 4;
    const bool sparseOmitted = !sameRigidOmitted && recentValidAllRigid &&
        in.changedOmittedCount <= 255 && sparseOmittedBytes < omittedBytes;

    outAllPresent = allPresent;
    outAllRigid = allRigid;
    outSameRigidOmitted = sameRigidOmitted;
    outSparseOmitted = sparseOmitted;

    uint8_t transModes[2];
    unsigned transModeCount = 0;
    if (in.transChangedCount == 0) {
        transModes[transModeCount++] = 0;
    } else {
        if (in.transChangedCount < in.included) transModes[transModeCount++] = 1;
        transModes[transModeCount++] = 2;
    }

    uint32_t sparseTransRes[pose::MaxBones * 3];
    if (in.transChangedCount > 0 && in.transChangedCount < in.included) {
        int32_t tPrior[pose::MaxBones][3];
        for (std::size_t i = 0; i < in.count; ++i) {
            if (!in.boneType[i] || !in.transChanged[i]) {
                tPrior[i][0] = tPrior[i][1] = tPrior[i][2] = 0;
                continue;
            }
            const uint8_t type = in.boneType[i];
            const uint8_t om = type == 2 ? in.omitted[i] : 0;
            for (int t = 0; t < 3; ++t) {
                int32_t d = in.diff[i * 12 + 9 + t];
                tPrior[i][t] = d;
                if (i > 0 && Mode4FirstOrderMatch(i, in.boneType[i - 1], in.omitted[i - 1], type, om)) {
                    if (Mode4SecondOrderMatch(in.secondOrder, i, in.boneType[i - 1],
                            i > 1 ? in.boneType[i - 2] : 0, in.omitted[i - 1],
                            i > 1 ? in.omitted[i - 2] : 0, type, om)) {
                        d -= 2 * tPrior[i - 1][t] - tPrior[i - 2][t];
                    } else {
                        d -= tPrior[i - 1][t];
                    }
                }
                sparseTransRes[i * 3 + t] = PoseZigZag(d);
            }
        }
    }

    for (unsigned tmIdx = 0; tmIdx < transModeCount; ++tmIdx) {
        const uint8_t tm = transModes[tmIdx];
        const std::size_t compCount = allRigid ? (tm == 0 ? 3 : 6) : (tm == 0 ? 9 : 12);
        PoseRiceParameters sparseTransGroup[MaxGroups][3];
        PoseRiceParameters sparseTransGlobal[3];
        if (tm == 1) {
            std::memset(sparseTransGroup, 0, maxGroups * sizeof(sparseTransGroup[0]));
            std::memset(sparseTransGlobal, 0, sizeof(sparseTransGlobal));
            for (size_t group = 0; group < maxGroups; ++group) {
                const std::size_t startBone = group * 16;
                const std::size_t endBone = (std::min)(in.count, startBone + 16);
                for (std::size_t i = startBone; i < endBone; ++i) {
                    if (in.boneType[i] && in.transChanged[i]) {
                        for (int t = 0; t < 3; ++t)
                            sparseTransGroup[group][t].Add(sparseTransRes[i * 3 + t]);
                    }
                }
                for (int t = 0; t < 3; ++t) {
                    sparseTransGlobal[t].count += sparseTransGroup[group][t].count;
                    for (int k = 0; k < 16; ++k) {
                        sparseTransGlobal[t].savings[k] += sparseTransGroup[group][t].savings[k];
                        sparseTransGlobal[t].widths[k] += sparseTransGroup[group][t].widths[k];
                    }
                }
            }
        }

        const std::size_t m5PresBytes = allPresent ? 0 : presenceBytes;
        const std::size_t m5RigidBytes = (sameRigidOmitted || allRigid) ? 0 : rigidBytes;
        const std::size_t m5OmitBytes = sameRigidOmitted ? 0 : (sparseOmitted ? sparseOmittedBytes : omittedBytes);
        const std::size_t m5TransBytes = tm == 1 ? (in.included + 7) / 8 : 0;
        const std::size_t maskBytes = m5PresBytes + m5RigidBytes + m5OmitBytes + m5TransBytes;

        for (bool grouped : {false, true}) {
            if (grouped && in.count <= 16) continue;
            const size_t groupBones = grouped ? 16 : in.count;
            const size_t groups = (in.count + groupBones - 1) / groupBones;
            const std::size_t headerBytes = (in.useOlder ? 14 : 8) + (sameRigidOmitted ? 0 : 1) +
                ((groups * compCount + 1) / 2);
            std::size_t residualRotation = 0, residualTranslation = 0;
            std::array<std::array<unsigned, 12>, MaxGroups> m5Params{};
            for (size_t group = 0; group < groups; ++group) {
                if (allRigid) {
                    for (int s = 0; s < 3; ++s) {
                        std::size_t bits = 0;
                        m5Params[group][s] = (grouped ? in.rigidGroupCosts[group][s] : in.rigidGlobalCosts[s]).Best(bits);
                        residualRotation += bits;
                    }
                    if (tm != 0) {
                        for (int t = 0; t < 3; ++t) {
                            std::size_t bits = 0;
                            auto const& c = tm == 1
                                ? (grouped ? sparseTransGroup[group][t] : sparseTransGlobal[t])
                                : (grouped ? in.groupCosts[group][9 + t] : in.globalCosts[9 + t]);
                            m5Params[group][3 + t] = c.Best(bits);
                            residualTranslation += bits;
                        }
                    }
                } else {
                    for (int k = 0; k < 9; ++k) {
                        std::size_t bits = 0;
                        m5Params[group][k] = (grouped ? in.groupCosts[group][k] : in.globalCosts[k]).Best(bits);
                        residualRotation += bits;
                    }
                    if (tm != 0) {
                        for (int t = 0; t < 3; ++t) {
                            std::size_t bits = 0;
                            auto const& c = tm == 1
                                ? (grouped ? sparseTransGroup[group][t] : sparseTransGlobal[t])
                                : (grouped ? in.groupCosts[group][9 + t] : in.globalCosts[9 + t]);
                            m5Params[group][9 + t] = c.Best(bits);
                            residualTranslation += bits;
                        }
                    }
                }
            }
            const std::size_t exactBytes = headerBytes + maskBytes +
                (residualRotation + residualTranslation + 7) / 8;
            if (exactBytes >= bestSize) continue;

            result.assign(exactBytes, 0);
            PoseBitBufWriter writer{result.data(), 0};
            writer.Put(in.layout, 16);
            writer.Put(in.recentToken, 32);
            writer.Put(5, 8);
            const uint8_t m5Flags = static_cast<uint8_t>(
                (in.useOlder ? 1 : 0) | (in.coarse ? 2 : 0) | (in.secondOrder ? 4 : 0) |
                (grouped ? 8 : 0) | (allPresent ? 16 : 0) | (sameRigidOmitted ? 32 : 0) |
                (tm << 6));
            writer.Put(m5Flags, 8);
            if (in.useOlder) {
                writer.Put(in.olderToken, 32);
                writer.Put(in.ratio, 16);
            }
            if (!sameRigidOmitted) {
                writer.Put((allRigid ? 1u : 0u) | (sparseOmitted ? 2u : 0u), 8);
            }
            for (size_t group = 0; group < groups; ++group)
                for (size_t c = 0; c < compCount; ++c)
                    writer.Put(m5Params[group][c], 4);
            writer.Align();
            if (!allPresent) {
                for (std::size_t i = 0; i < in.count; ++i)
                    writer.Put(in.boneType[i] ? 1u : 0u, 1);
                writer.Align();
            }
            if (!sameRigidOmitted && !allRigid) {
                for (std::size_t i = 0; i < in.count; ++i)
                    writer.Put(in.boneType[i] == 2 ? 1u : 0u, 1);
                writer.Align();
            }
            if (!sameRigidOmitted) {
                if (sparseOmitted) {
                    writer.Put(static_cast<uint32_t>(in.changedOmittedCount), 8);
                    for (std::size_t i = 0; i < in.count; ++i)
                        if (in.boneType[i] == 2 && in.omitted[i] != in.recentOmitted[i])
                            writer.Put(static_cast<uint32_t>(i), 8);
                    for (std::size_t i = 0; i < in.count; ++i)
                        if (in.boneType[i] == 2 && in.omitted[i] != in.recentOmitted[i])
                            writer.Put(in.omitted[i], 2);
                    writer.Align();
                } else {
                    for (std::size_t i = 0; i < in.count; ++i)
                        if (in.boneType[i] == 2) writer.Put(in.omitted[i], 2);
                    writer.Align();
                }
            }
            if (tm == 1) {
                for (std::size_t i = 0; i < in.count; ++i)
                    if (in.boneType[i]) writer.Put(in.transChanged[i] ? 1u : 0u, 1);
                writer.Align();
            }
            for (std::size_t i = 0; i < in.count; ++i) {
                if (!in.boneType[i]) continue;
                auto const& kval = m5Params[i / groupBones];
                if (in.boneType[i] == 2) {
                    int slot = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (k == in.omitted[i]) continue;
                        const unsigned kp = allRigid ? kval[slot++] : kval[k];
                        writer.Rice(in.residual[i * 12 + k], kp, 21);
                    }
                    if (tm == 2 || (tm == 1 && in.transChanged[i])) {
                        for (int t = 0; t < 3; ++t) {
                            const uint32_t tv = tm == 1 ? sparseTransRes[i * 3 + t] : in.residual[i * 12 + 9 + t];
                            writer.Rice(tv, kval[(allRigid ? 3 : 9) + t], 21);
                        }
                    }
                } else {
                    for (int k = 0; k < 9; ++k)
                        writer.Rice(in.residual[i * 12 + k], kval[k], 21);
                    if (tm == 2 || (tm == 1 && in.transChanged[i])) {
                        for (int t = 0; t < 3; ++t) {
                            const uint32_t tv = tm == 1 ? sparseTransRes[i * 3 + t] : in.residual[i * 12 + 9 + t];
                            writer.Rice(tv, kval[9 + t], 21);
                        }
                    }
                }
            }

            bestSize = exactBytes;
            resultKey = false;
            resultStats = {};
            resultStats.headerBytes = headerBytes;
            resultStats.maskBits = maskBytes * 8;
            resultStats.rotationBits = residualRotation;
            resultStats.translationBits = residualTranslation;
            resultStats.includedBones = in.included;
            resultStats.predictive = true;
            resultStats.maxBasisErrorUnits = in.maxBasisError;
            resultStats.rigidBones = in.rigidCount;
            reconstructed.assign(in.rigidReconstruction, in.rigidReconstruction + in.count);
        }
    }
}

} // namespace g3t::npcwire::bones::detail
