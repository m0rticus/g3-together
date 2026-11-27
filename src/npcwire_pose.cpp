// SPDX-License-Identifier: GPL-3.0-or-later
#define G3T_NPCWIRE_POSE_IMPL_INCLUDED 1
#include "npcwire_pose.h"
#include "npcwire_pose_encode.h"

namespace g3t::npcwire::bones {

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
    if (!ref) return Status::Malformed;
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
                    value = PoseRoundSignedDivide4(value) * 4;

            PoseQuat target = predicted;
            for (int k = 0; k < 4; ++k) {
                if (k == omitted[i]) continue;
                const std::size_t before = reader.bits;
                uint32_t zig = 0;
                if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
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

G3T_NPCWIRE_POSE_INLINE Status PoseCache::DecodeMode5(
    Dictionary const& dictionary, uint8_t const* data, size_t size,
    uint16_t id, uint32_t ref, uint32_t incarnation, uint8_t flags,
    std::vector<pose::Bone>& out, uint16_t& layout, PoseStats& stats) const
{
    if (size < 8) return Status::Malformed;
    const uint8_t m5Flags = data[7];
    const bool hasOlder = (m5Flags & 1) != 0;
    const uint8_t transMode = (m5Flags >> 6) & 3u;

    if (transMode == 3) {
        if (m5Flags & ~0xC1u) return Status::Malformed;
        const size_t expectedSize = hasOlder ? 14 : 8;
        if (size != expectedSize) return Status::Malformed;
        if (!ref) {
            if (hasOlder) return Status::Malformed;
            const auto* idPose = dictionary.IdentityPosePtr(id);
            if (!idPose) return Status::UnknownLayout;
            out = *idPose;
            layout = id;
            stats = {};
            stats.headerBytes = 8;
            stats.predictive = true;
            return Status::Ok;
        }
        const Frame* recent = Find(ref, incarnation, id, flags);
        if (!recent) { layout = id; return Status::UnknownPose; }
        const size_t count = recent->bones.size();
        if (!count || count > pose::MaxBones) return Status::Malformed;
        Frame const* older = nullptr;
        unsigned ratio = 0;
        if (hasOlder) {
            const uint32_t secondary = ReadPose32(data + 8);
            ratio = uint16_t(data[12]) | uint16_t(uint16_t(data[13]) << 8);
            if (!secondary || ratio > 1024) return Status::Malformed;
            older = Find(secondary, incarnation, id, flags);
            if (!older) { layout = id; return Status::UnknownPose; }
            if (older->bones.size() != count || !PoseNewer(ref, secondary)) return Status::Malformed;
        }
        if (!older) {
            out = recent->bones;
        } else {
            auto result = recent->bones;
            for (size_t i = 0; i < count; ++i)
                for (int k = 0; k < 12; ++k)
                    result[i].values[k] = static_cast<int16_t>(TemporalPrediction(*recent, older, ratio, i, k));
            out = std::move(result);
        }
        layout = id;
        stats = {};
        stats.headerBytes = expectedSize;
        stats.predictive = true;
        return Status::Ok;
    }

    if (!ref) return Status::Malformed;
    const bool coarse = (m5Flags & 2) != 0;
    const bool secondOrder = (m5Flags & 4) != 0;
    const bool grouped = (m5Flags & 8) != 0;
    const bool allPresent = (m5Flags & 16) != 0;
    const bool sameRigidOmitted = (m5Flags & 32) != 0;
    const Frame* recent = Find(ref, incarnation, id, flags);
    if (!recent) { layout = id; return Status::UnknownPose; }
    const size_t count = recent->bones.size();
    if (!count || count > pose::MaxBones) return Status::Malformed;
    if (secondOrder && count < 3) return Status::Malformed;
    if (grouped && count <= 16) return Status::Malformed;

    size_t cursor = 8;
    Frame const* older = nullptr;
    unsigned ratio = 0;
    if (hasOlder) {
        if (size < 14) return Status::Malformed;
        const uint32_t secondary = ReadPose32(data + 8);
        ratio = uint16_t(data[12]) | uint16_t(uint16_t(data[13]) << 8);
        if (!secondary || ratio > 1024) return Status::Malformed;
        older = Find(secondary, incarnation, id, flags);
        if (!older) { layout = id; return Status::UnknownPose; }
        if (older->bones.size() != count || !PoseNewer(ref, secondary)) return Status::Malformed;
        cursor = 14;
    }

    bool allRigid = true;
    bool sparseOmitted = false;
    if (!sameRigidOmitted) {
        if (size <= cursor) return Status::Malformed;
        const uint8_t ext = data[cursor++];
        if (ext & ~3u) return Status::Malformed;
        allRigid = (ext & 1) != 0;
        sparseOmitted = (ext & 2) != 0;
    }

    const size_t compCount = allRigid ? (transMode == 0 ? 3 : 6) : (transMode == 0 ? 9 : 12);
    const size_t groupBones = grouped ? 16 : count;
    const size_t groups = (count + groupBones - 1) / groupBones;
    const size_t totalNibbles = groups * compCount;
    const size_t paramBytes = (totalNibbles + 1) / 2;
    const size_t parameterEnd = cursor + paramBytes;
    if (size < parameterEnd) return Status::Malformed;
    if ((totalNibbles & 1) && (data[parameterEnd - 1] >> 4)) return Status::Malformed;

    std::array<std::array<unsigned, 12>, (pose::MaxBones + 15) / 16> parameters{};
    for (size_t n = 0; n < totalNibbles; ++n) {
        const size_t g = n / compCount;
        const size_t c = n % compCount;
        parameters[g][c] = (data[cursor + (n >> 1)] >> ((n & 1) * 4)) & 15u;
    }

    size_t pos = parameterEnd;
    const size_t bitmapBytes = (count + 7) / 8;
    const size_t presenceBytes = allPresent ? 0 : bitmapBytes;
    if (size < pos + presenceBytes) return Status::Malformed;
    uint8_t const* presencePtr = allPresent ? nullptr : data + pos;
    if (presencePtr && (count % 8) && (presencePtr[presenceBytes - 1] >> (count % 8)))
        return Status::Malformed;
    pos += presenceBytes;

    uint8_t present[pose::MaxBones];
    size_t included = 0;
    if (allPresent) {
        std::memset(present, 1, count);
        included = count;
    } else {
        for (size_t i = 0; i < count; ++i) {
            present[i] = (presencePtr[i >> 3] >> (i & 7)) & 1u;
            included += present[i];
        }
        if (!included) return Status::Malformed;
    }

    const size_t rigidBytes = (sameRigidOmitted || allRigid) ? 0 : bitmapBytes;
    if (size < pos + rigidBytes) return Status::Malformed;
    uint8_t const* rigidPtr = rigidBytes ? data + pos : nullptr;
    if (rigidPtr && (count % 8) && (rigidPtr[rigidBytes - 1] >> (count % 8)))
        return Status::Malformed;
    pos += rigidBytes;

    uint8_t isRigid[pose::MaxBones];
    size_t rigidCount = 0;
    if (allRigid) {
        std::memcpy(isRigid, present, count);
        rigidCount = included;
    } else {
        for (size_t i = 0; i < count; ++i) {
            isRigid[i] = (rigidPtr[i >> 3] >> (i & 7)) & 1u;
            if (isRigid[i] && !present[i]) return Status::Malformed;
            rigidCount += isRigid[i];
        }
    }

    uint8_t omitted[pose::MaxBones]{};
    PoseQuat recentQuats[pose::MaxBones];
    bool haveRecentQuat[pose::MaxBones];
    std::memset(haveRecentQuat, 0, count);
    size_t omittedSectionBytes = 0;
    if (sameRigidOmitted) {
        for (size_t i = 0; i < count; ++i) {
            if (!isRigid[i]) continue;
            if (!PoseExtractQuat(recent->bones[i], recentQuats[i])) return Status::Malformed;
            haveRecentQuat[i] = true;
            omitted[i] = recentQuats[i].omitted;
        }
    } else if (sparseOmitted) {
        if (size < pos + 1) return Status::Malformed;
        const size_t changedCount = data[pos++];
        if (changedCount > rigidCount || changedCount > 255) return Status::Malformed;
        const size_t bitBytes = (changedCount + 3) / 4;
        if (size < pos + changedCount + bitBytes) return Status::Malformed;
        for (size_t i = 0; i < count; ++i) {
            if (!isRigid[i]) continue;
            if (!PoseExtractQuat(recent->bones[i], recentQuats[i])) return Status::Malformed;
            haveRecentQuat[i] = true;
            omitted[i] = recentQuats[i].omitted;
        }
        int prevIdx = -1;
        for (size_t c = 0; c < changedCount; ++c) {
            const uint8_t idx = data[pos + c];
            if (idx >= count || !isRigid[idx] || static_cast<int>(idx) <= prevIdx) return Status::Malformed;
            prevIdx = idx;
            omitted[idx] = static_cast<uint8_t>((data[pos + changedCount + (c >> 2)] >> ((c & 3) * 2)) & 3u);
        }
        if (changedCount && (changedCount % 4) &&
            (data[pos + changedCount + bitBytes - 1] >> ((changedCount % 4) * 2)))
            return Status::Malformed;
        omittedSectionBytes = 1 + changedCount + bitBytes;
        pos += changedCount + bitBytes;
    } else {
        const size_t fullOmitBytes = (rigidCount + 3) / 4;
        if (size < pos + fullOmitBytes) return Status::Malformed;
        unsigned rigidRank = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!isRigid[i]) continue;
            omitted[i] = static_cast<uint8_t>((data[pos + (rigidRank >> 2)] >> ((rigidRank & 3) * 2)) & 3u);
            ++rigidRank;
        }
        if (rigidCount && (rigidCount % 4) &&
            (data[pos + fullOmitBytes - 1] >> ((rigidCount % 4) * 2)))
            return Status::Malformed;
        omittedSectionBytes = fullOmitBytes;
        pos += fullOmitBytes;
    }

    const size_t transMaskBytes = transMode == 1 ? (included + 7) / 8 : 0;
    if (size < pos + transMaskBytes) return Status::Malformed;
    uint8_t const* transMaskPtr = transMaskBytes ? data + pos : nullptr;
    if (transMaskPtr) {
        if ((included % 8) && (transMaskPtr[transMaskBytes - 1] >> (included % 8)))
            return Status::Malformed;
        bool anyTransBit = false;
        for (size_t b = 0; b < transMaskBytes; ++b) if (transMaskPtr[b]) { anyTransBit = true; break; }
        if (!anyTransBit) return Status::Malformed;
    }
    pos += transMaskBytes;

    PoseRiceReader reader(data, size);
    reader.bits = pos * 8;
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

    size_t includedRank = 0, rigidBones = 0;
    size_t residualRotation = 0, residualTranslation = 0;
    for (size_t i = 0; i < count; ++i) {
        auto const& kval = parameters[i / groupBones];
        if (!present[i]) {
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

        const bool hasTrans = transMode == 2 ||
            (transMode == 1 && ((transMaskPtr[includedRank >> 3] >> (includedRank & 7)) & 1u));
        ++includedRank;
        int32_t priorCur[12] = {};
        bool boneChanged = false;
        if (!isRigid[i]) {
            for (int k = 0; k < 9; ++k) {
                const size_t before = reader.bits;
                uint32_t zig = 0;
                if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                if (zig) boneChanged = true;
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                const int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig) + spatial(i, k, 1, 0);
                if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                result[i].values[k] = static_cast<int16_t>(decoded);
                priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                residualRotation += reader.bits - before;
            }
            for (int k = 9; k < 12; ++k) {
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                if (!hasTrans) {
                    result[i].values[k] = static_cast<int16_t>(prediction);
                    priorCur[k] = 0;
                } else {
                    const size_t before = reader.bits;
                    uint32_t zig = 0;
                    if (!reader.Rice(zig, kval[k], 21)) return Status::Malformed;
                    if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                    if (zig) boneChanged = true;
                    const int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig) + spatial(i, k, 1, 0);
                    if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                    result[i].values[k] = static_cast<int16_t>(decoded);
                    priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                    residualTranslation += reader.bits - before;
                }
            }
            if (!boneChanged && (transMode != 1 || hasTrans || prevType == 0)) return Status::Malformed;
        } else {
            ++rigidBones;
            PoseQuat predicted;
            if (haveRecentQuat[i] && recentQuats[i].omitted == omitted[i]) {
                predicted = recentQuats[i];
                if (predicted.values[omitted[i]] < 0)
                    for (auto& value : predicted.values) value = -value;
                if (older) {
                    PoseQuat previous;
                    if (!PoseExtractQuat(older->bones[i], previous)) return Status::Malformed;
                    if (previous.values[omitted[i]] < 0)
                        for (auto& value : previous.values) value = -value;
                    for (int k = 0; k < 4; ++k)
                        predicted.values[k] = PoseSaturateInt16(int64_t(predicted.values[k]) +
                            PoseRoundQ8((predicted.values[k] - previous.values[k]) * static_cast<int32_t>(ratio)));
                }
            } else {
                if (!QuaternionPrediction(*recent, older, ratio, i, omitted[i], predicted))
                    return Status::Malformed;
            }
            if (coarse)
                for (auto& value : predicted.values)
                    value = PoseRoundSignedDivide4(value) * 4;

            PoseQuat target = predicted;
            int slot = 0;
            for (int k = 0; k < 4; ++k) {
                if (k == omitted[i]) continue;
                const size_t before = reader.bits;
                uint32_t zig = 0;
                const unsigned kp = allRigid ? kval[slot++] : kval[k];
                if (!reader.Rice(zig, kp, 21)) return Status::Malformed;
                if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                const int64_t value = int64_t(predicted.values[k]) +
                    int64_t(PoseUnzigZag(zig)) * (coarse ? 4 : 1) +
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
                const int32_t prediction = TemporalPrediction(*recent, older, ratio, i, k);
                if (!hasTrans) {
                    result[i].values[k] = static_cast<int16_t>(prediction);
                    priorCur[k] = 0;
                } else {
                    const size_t before = reader.bits;
                    uint32_t zig = 0;
                    const unsigned kp = kval[(allRigid ? 3 : 9) + (k - 9)];
                    if (!reader.Rice(zig, kp, 21)) return Status::Malformed;
                    if (zig > (secondOrder ? 524280u : 262140u)) return Status::Malformed;
                    const int64_t decoded = int64_t(prediction) + PoseUnzigZag(zig) + spatial(i, k, 2, omitted[i]);
                    if (decoded < -32768 || decoded > 32767) return Status::Malformed;
                    result[i].values[k] = static_cast<int16_t>(decoded);
                    priorCur[k] = static_cast<int32_t>(decoded) - prediction;
                    residualTranslation += reader.bits - before;
                }
            }
        }
        for (int k = 0; k < 12; ++k) {
            priorPrevPrev[k] = priorPrev[k];
            priorPrev[k] = priorCur[k];
        }
        prevPrevType = prevType;
        prevPrevOmitted = prevOmitted;
        prevType = isRigid[i] ? 2 : 1;
        if (isRigid[i]) prevOmitted = omitted[i];
    }
    if (secondOrder && !usedSecondOrder) return Status::Malformed;
    if (!reader.AlignAndFinish()) return Status::Malformed;
    out = std::move(result);
    layout = id;
    stats.headerBytes = parameterEnd;
    stats.maskBits = (presenceBytes + rigidBytes + omittedSectionBytes + transMaskBytes) * 8;
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
    } else if (mode == 5) {
        status = DecodeMode5(dictionary, data, size, id, ref, incarnation, flags, out, layout, decodedStats);
    }
    if (status == Status::Ok && stats) *stats = decodedStats;
    return status;
}

} // namespace g3t::npcwire::bones
